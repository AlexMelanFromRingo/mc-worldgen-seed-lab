#!/usr/bin/env python3
"""Дискретная модель планировщика генерации чанков 26.3 (ChunkMap + ChunkGenerationTask + ChunkTaskDispatcher + ChunkPyramid) — порядок шагов FEATURES.

    sched_sim.py [--radius 6] [--create xz|zx|rand<seed>] [--async-ms 40] [--features-ms 27] [--compare run/gt/jfr_a.jfr …]

Что моделируется (исходники 26.3, docs/blender/nondeterminism.md §2.1):
  * зависимости слоёв — точный порт ChunkStep.Builder (direct/accumulated dependencies) и ChunkLevel (уровни билета forceload 31 + расстояние);
  * задача на чанк (ChunkGenerationTask.runUntilWait): слои статусов по очереди, на каждом слое — applyStep для всех чанков радиуса (x внешний, z внутренний);
    первый дошедший до (чанк, статус) выполняет шаг, остальные ждут его future;
  * синхронные шаги (STRUCTURE_*, FEATURES) выполняются в последовательном исполнителе worldgen; BIOMES/TERRAIN/свет — асинхронно (задержка async-ms, jitter);
  * очередь ChunkTaskPriorityQueue: меньший уровень билета первым, внутри уровня — порядок вставки ключа чанка; готовая снова задача уходит в хвост очереди.
Порядок создания задач (DistanceManager.chunksToUpdateFutures — ReferenceOpenHashSet, порядок идентификационных хэшей) в игре неизвестен — параметр --create.
Результат — порядок чанков шага FEATURES; сравнение с записями JFR — доля пар соседних чанков (≤ 2 по Чебышёву) с разным порядком.
"""
import argparse, heapq, itertools, os, random, statistics, sys
from collections import OrderedDict

ST = ['EMPTY', 'STRUCTURE_STARTS', 'STRUCTURE_REFERENCES', 'BIOMES', 'TERRAIN', 'FEATURES', 'INITIALIZE_LIGHT', 'LIGHT', 'SPAWN', 'FULL']
I = {n: i for i, n in enumerate(ST)}
# прямые требования шагов (ChunkPyramid.GENERATION_PYRAMID): [(статус, радиус)]
REQ = {
    'EMPTY': [], 'STRUCTURE_STARTS': [], 'STRUCTURE_REFERENCES': [('STRUCTURE_STARTS', 8)], 'BIOMES': [('STRUCTURE_STARTS', 8)],
    'TERRAIN': [('STRUCTURE_STARTS', 8), ('BIOMES', 1)], 'FEATURES': [('STRUCTURE_STARTS', 8), ('TERRAIN', 1)], 'INITIALIZE_LIGHT': [],
    'LIGHT': [('INITIALIZE_LIGHT', 1)], 'SPAWN': [('BIOMES', 1)], 'FULL': [],
}


def smax(a, b):
    return a if I[a] >= I[b] else b


def build_pyramid():
    """{статус: accumulatedDependencies (список статусов по радиусу)} — порт ChunkStep.Builder."""
    acc = {}
    for k, name in enumerate(ST):
        if k == 0:
            acc[name] = []; continue
        parent = ST[k - 1]
        direct = [parent]
        for (s, r) in REQ[name]:                       # addRequirement
            n = r + 1; prev = direct
            if n > len(prev): direct = [s] * n
            for i in range(min(n, len(prev))): direct[i] = smax(prev[i], s)
        rop = 0                                        # getRadiusOfParent
        for i in range(len(direct) - 1, -1, -1):
            if I[direct[i]] >= I[parent]: rop = i; break
        pa = acc[parent]
        out = []
        for d in range(max(rop + len(pa), len(direct))):
            dp = d - rop
            if dp < 0 or dp >= len(pa): out.append(direct[d])
            elif d >= len(direct): out.append(pa[dp])
            else: out.append(smax(direct[d], pa[dp]))
        acc[name] = out
    return acc


ACC = build_pyramid()
MAXD = len(ACC['FULL']) - 1                            # ChunkLevel.RADIUS_AROUND_FULL_CHUNK


def radius_of(target, status):
    """ChunkStep.getAccumulatedRadiusOf: радиус, до которого для цели target нужен статус status."""
    if status == target: return 0
    r = 0
    for rad, s in enumerate(ACC[target]):
        if I[s] >= I[status]: r = rad
    return r


def status_around_full(d):
    return 'FULL' if d <= 0 else (ACC['FULL'][d] if d <= MAXD else None)


class Chunk:
    __slots__ = ('pos', 'level', 'started', 'done', 'task')

    def __init__(self, pos, level):
        self.pos, self.level, self.started, self.done, self.task = pos, level, set(), set(), None


class Task:
    __slots__ = ('pos', 'target', 'sched', 'layer', 'cancel')

    def __init__(self, pos, target):
        self.pos, self.target, self.sched, self.layer, self.cancel = pos, target, None, [], False


def simulate(radius=6, create='xz', async_ms=40.0, features_ms=27.0, jitter=0.0, seed=1):
    rnd = random.Random(seed)
    R = radius
    chunks = {}
    for x in range(-R - MAXD - 3, R + MAXD + 4):
        for z in range(-R - MAXD - 3, R + MAXD + 4):
            d = max(max(-R - x, x - R, 0), max(-R - z, z - R, 0))
            chunks[(x, z)] = Chunk((x, z), 31 + d)
    holders = [c for c in chunks if chunks[c].level <= 33]
    if create == 'xz': holders.sort()
    elif create == 'zx': holders.sort(key=lambda c: (c[1], c[0]))
    elif create.startswith('rand'): random.Random(int(create[4:] or 1)).shuffle(holders)
    tasks = []
    for h in holders:                                  # prepareAccessibleChunk: окно 3×3 вокруг держателя (z внешний), статус по расстоянию
        for dz in (-1, 0, 1):
            for dx in (-1, 0, 1):
                p = (h[0] + dx, h[1] + dz)
                st = status_around_full(max(abs(dx), abs(dz)))
                c = chunks[p]
                if c.task is None or I[st] > I[c.task.target]:
                    if c.task: c.task.cancel = True
                    t = Task(p, st); c.task = t; tasks.append(t)
    now = 0.0; seq = itertools.count()
    events = []                                        # завершения асинхронных шагов: (время, №, (pos, статус))
    queue = {}                                         # уровень -> OrderedDict(pos -> [задачи])
    waiting = {}                                       # (pos, статус) -> задачи, ждущие этот future
    features = []

    def submit(t):
        queue.setdefault(chunks[t.pos].level, OrderedDict()).setdefault(t.pos, []).append(t)

    def complete(pos, status):
        chunks[pos].done.add(status)
        for t in waiting.pop((pos, status), []): submit(t)

    def exec_step(c, status):
        """возвращает затраченное время исполнителя (синхронный шаг) или 0 (асинхронный — завершится позже)"""
        if status in ('EMPTY', 'STRUCTURE_STARTS', 'STRUCTURE_REFERENCES'):
            complete(c.pos, status); return 0.2
        if status == 'FEATURES':
            features.append(c.pos); complete(c.pos, status); return features_ms * (1 + rnd.uniform(-jitter, jitter))
        dur = async_ms * (1 + rnd.uniform(-jitter, jitter)) * (0.05 if status in ('INITIALIZE_LIGHT', 'LIGHT', 'FULL', 'SPAWN') else (0.1 if status == 'BIOMES' else 1.0))
        heapq.heappush(events, (now + dur, next(seq), (c.pos, status)))
        return 0.0

    def run_until_wait(t):
        spent = 0.0
        while True:
            while t.layer:                             # waitForScheduledLayer
                p, s = t.layer[-1]
                if s not in chunks[p].done:
                    waiting.setdefault((p, s), []).append(t); return spent
                t.layer.pop()
            if t.cancel or t.sched == t.target: return spent
            nxt = ST[0] if t.sched is None else ST[I[t.sched] + 1]
            r = radius_of(t.target, nxt)
            for x in range(t.pos[0] - r, t.pos[0] + r + 1):
                for z in range(t.pos[1] - r, t.pos[1] + r + 1):
                    c = chunks.get((x, z))
                    if c is None: continue
                    if nxt not in c.started:
                        c.started.add(nxt); spent += exec_step(c, nxt)
                    if nxt not in c.done: t.layer.append(((x, z), nxt))
            t.sched = nxt

    for t in tasks: submit(t)
    for _ in range(2_000_000):
        while events and events[0][0] <= now:
            _, _, (pos, status) = heapq.heappop(events); complete(pos, status)
        item = None
        for lv in sorted(queue):
            q = queue[lv]
            if q:
                item = q.popitem(last=False)
                if not q: del queue[lv]
                break
        if item is None:
            if not events: break
            now = events[0][0]; continue
        for t in item[1]:
            now += run_until_wait(t)
    return features


def pair_disagreement(order, run):
    P = {c: i for i, c in enumerate(order)}; Q = {c: i for i, c in enumerate(run)}
    common = sorted(set(P) & set(Q)); S = set(common); tot = diff = 0
    for (x, z) in common:
        for dx in range(-2, 3):
            for dz in range(-2, 3):
                q = (x + dx, z + dz)
                if q in S and q > (x, z):
                    tot += 1; diff += (P[(x, z)] < P[q]) != (Q[(x, z)] < Q[q])
    return 100.0 * diff / max(tot, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--radius', type=int, default=6); ap.add_argument('--create', default='xz')
    ap.add_argument('--async-ms', type=float, default=40.0); ap.add_argument('--features-ms', type=float, default=27.0)
    ap.add_argument('--jitter', type=float, default=0.0); ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--compare', nargs='*', default=[]); ap.add_argument('--out'); ap.add_argument('--print-pyramid', action='store_true')
    a = ap.parse_args()
    if a.print_pyramid:
        for n in ST: print(f'{n:22s} {ACC[n]}')
        print('FULL: радиус статуса:', {s: radius_of('FULL', s) for s in ST[1:]}); print('статус по расстоянию от FULL:', [status_around_full(d) for d in range(MAXD + 1)])
        return
    order = simulate(a.radius, a.create, a.async_ms, a.features_ms, a.jitter, a.seed)
    if a.out: open(a.out, 'w').write(''.join(f'{x} {z}\n' for x, z in order))
    print(f'шагов FEATURES: {len(order)}; первые: {order[:12]}')
    if a.compare:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import jfr_order as J
        res = []
        for f in a.compare:
            rows = J.load(f)
            run = [(r['cx'], r['cz']) for r in sorted([r for r in rows if r['status'].endswith('features') and r['level'].endswith('overworld')], key=lambda r: r['start'])]
            res.append(pair_disagreement(order, run))
        print(f'расхождение порядка пар соседних чанков с записями: среднее {statistics.mean(res):.2f} % (мин {min(res):.2f}, макс {max(res):.2f})')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Разбор JFR-записи сервера (событие minecraft.ChunkGeneration): порядок выполнения шагов генерации чанков.

    jfr_order.py run/gt/jfr_a.jfr [--step features] [--out order.json] [-v]

Печатает число событий по шагам/потокам, проверяет, пересекаются ли по времени шаги FEATURES (одновременность),
и пишет порядок чанков для шага (по времени начала) в JSON: [[cx, cz], …] — для libmcgen: MCGEN_FEATURES_ORDER=файл.
"""
import argparse, collections, json, subprocess, sys


def ns_of(ts):
    """'2026-10-04T00:26:53.130172817Z' -> целые наносекунды с эпохи"""
    import calendar, time
    base, frac = ts.rstrip('Z').split('.') if '.' in ts else (ts.rstrip('Z'), '0')
    sec = calendar.timegm(time.strptime(base, '%Y-%m-%dT%H:%M:%S'))
    return sec * 10**9 + int((frac + '000000000')[:9])


def dur_ns(d):
    """'PT0.000007155S' -> наносекунды"""
    x = d.replace('PT', '').replace('S', '')
    if '.' in x:
        a, b = x.split('.')
        return int(a) * 10**9 + int((b + '000000000')[:9])
    return int(x) * 10**9


def load(path):
    out = subprocess.check_output(['jfr', 'print', '--json', '--events', 'minecraft.ChunkGeneration', path], text=True)
    ev = json.loads(out)['recording']['events']
    rows = []
    for e in ev:
        v = e['values']
        rows.append({'cx': v['chunkPosX'], 'cz': v['chunkPosZ'], 'status': v['status'], 'level': v['level'],
                     'start': ns_of(v['startTime']), 'dur': dur_ns(v['duration']), 'thread': (v.get('eventThread') or {}).get('javaName', '?')})
    return rows


STATUS_RANK = ['empty', 'structure_starts', 'structure_references', 'noise_biomes', 'biomes', 'noise', 'surface', 'carvers', 'terrain', 'features', 'initialize_light', 'light', 'spawn', 'full']


def load_reads(path, dim):
    """чтения чанков с диска (minecraft.ChunkRegionRead, включено в tools/gt/jfr/chunkgen.jfc): [(время_нс, cx, cz)] по возрастанию времени"""
    try:
        out = subprocess.check_output(['jfr', 'print', '--json', '--events', 'minecraft.ChunkRegionRead', path], text=True)
        ev = json.loads(out)['recording']['events']
    except Exception:       # noqa: BLE001 — запись без этих событий (старый jfc)
        return []
    rows = [(ns_of(e['values']['startTime']), e['values']['chunkPosX'], e['values']['chunkPosZ']) for e in ev
            if e['values'].get('type', 'chunk') == 'chunk' and str(e['values'].get('dimension', '')).endswith(dim)]
    return sorted(rows)


def chunk_events(rows, reads, dim):
    """события состояния чанков для расписания: [(время_нс, 'W'|'U', cx, cz)].
    'W' — чанк прочитан с диска со статусом от CARVERS (26.1/26.2) / TERRAIN (26.3+) до SPAWN: карты *_WG не пишутся на диск (heightmapsAfter), игра строит их заново по блокам при первом запросе;
    'U' — чанк FULL (прочитан как FULL или шаг full завершён): запросы *_WG отдаются финальными картами (ImposterProtoChunk.fixType)."""
    names = {r['status'].split(':')[-1] for r in rows}
    thr = STATUS_RANK.index('carvers' if 'carvers' in names else 'terrain')
    done = {}
    for r in rows:
        if r['level'].endswith(dim):
            done.setdefault((r['cx'], r['cz']), []).append((r['start'] + r['dur'], STATUS_RANK.index(r['status'].split(':')[-1]) if r['status'].split(':')[-1] in STATUS_RANK else 0))
    for v in done.values(): v.sort()
    ev = []
    for t, cx, cz in reads:
        rank = -1
        for e, rk in done.get((cx, cz), []):
            if e <= t: rank = max(rank, rk)
        if rank == STATUS_RANK.index('full'): ev.append((t, 'U', cx, cz))
        elif rank >= thr: ev.append((t, 'W', cx, cz))
    for (cx, cz), v in done.items():
        for e, rk in v:
            if rk == STATUS_RANK.index('full'): ev.append((e, 'U', cx, cz))
    return sorted(ev)


def interleave(steps, events):
    """[(тип, строка)] — шаги FEATURES в порядке времени начала; события состояния — перед первым шагом, начавшимся после них"""
    out = []; k = 0
    for r in steps:
        while k < len(events) and events[k][0] <= r['start']:
            out.append(('E', events[k])); k += 1
        out.append(('F', r))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('jfr'); ap.add_argument('--step', default='features'); ap.add_argument('--dim', default='overworld', help='измерение (имя в событии level: overworld | the_nether | the_end)'); ap.add_argument('--out'); ap.add_argument('--txt', help='порядок шага --step как «cx cz» по строкам'); ap.add_argument('--sched', help='единый файл расписания .mcsched (порядок FEATURES с масками света + порядок пост-обработки) — для libmcgen/аддона'); ap.add_argument('--fluid-txt', help='порядок постобработки жидкостей: чанки по моменту готовности 3×3 (max конца шага full у 9 чанков)'); ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    rows = load(a.jfr)
    print('событий:', len(rows))
    c = collections.Counter((r['status'], r['thread']) for r in rows)
    for (s, t), n in sorted(c.items()): print(f'  {s:22s} {t:34s} {n}')
    st = sorted([r for r in rows if r['status'].split(':')[-1] == a.step and r['level'].endswith(a.dim)], key=lambda r: r['start'])
    # одновременность: перекрытие интервалов [start, start+dur) у шагов РАЗНЫХ чанков; отдельно — у соседних (≤1 по Чебышёву) и «опасных» (≤2: общие записываемые окна)
    over = near1 = near2 = 0
    for i in range(len(st)):
        for j in range(i + 1, len(st)):
            if st[j]['start'] >= st[i]['start'] + st[i]['dur']:
                break
            over += 1
            d = max(abs(st[i]['cx'] - st[j]['cx']), abs(st[i]['cz'] - st[j]['cz']))
            near1 += d <= 1
            near2 += d <= 2
    print(f'шаг {a.step}: {len(st)} событий, {len({(r["cx"], r["cz"]) for r in st})} разных чанков; пересекающихся по времени пар: {over}, из них соседей (d≤1): {near1}, d≤2: {near2}')
    tt = [r['dur'] for r in st]
    if tt: print(f'  длительность шага: медиана {sorted(tt)[len(tt)//2]/1e6:.2f} мс, макс {max(tt)/1e6:.1f} мс')
    order = [[r['cx'], r['cz']] for r in st]
    if a.txt:
        # третье поле — маска 5×5 вокруг чанка (бит (dz+2)*5+dx+2): чанк уже прошёл INITIALIZE_LIGHT к началу шага (конец события initialize_light ≤ начало шага);
        # libmcgen (fc_sky_light) по ней знает, какие секции света «зарегистрированы» к этому моменту (свет читает MushroomBlock.canSurvive)
        init_end = {(r['cx'], r['cz']): r['start'] + r['dur'] for r in rows if r['status'].split(':')[-1] == 'initialize_light' and r['level'].endswith(a.dim)}
        lines = []
        events = chunk_events(rows, load_reads(a.jfr, a.dim), a.dim)
        for kind, r in interleave(st, events):
            if kind == 'E':
                lines.append(f"{r[1]} {r[2]} {r[3]}\n"); continue
            m = 0
            for dz in range(-2, 3):
                for dx in range(-2, 3):
                    e = init_end.get((r['cx'] + dx, r['cz'] + dz))
                    if e is not None and e <= r['start'] and (dx or dz): m |= 1 << ((dz + 2) * 5 + dx + 2)
            lines.append(f"{r['cx']} {r['cz']} {m:x}\n")
        open(a.txt, 'w').write(''.join(lines))
        print('порядок (txt):', a.txt, f'(событий состояния чанков: {len(events)})')
    if a.fluid_txt:
        full = {}
        for r in rows:
            if r['status'].split(':')[-1] == 'full' and r['level'].endswith(a.dim):
                full[(r['cx'], r['cz'])] = r['start'] + r['dur']
        ready = {}
        for (cx, cz) in full:
            nb = [full.get((cx + i, cz + j)) for i in (-1, 0, 1) for j in (-1, 0, 1)]
            if all(v is not None for v in nb):
                ready[(cx, cz)] = max(nb)
        seq = sorted(ready, key=lambda k: (ready[k], k))
        open(a.fluid_txt, 'w').write(''.join(f'{x} {z}\n' for x, z in seq))
        print(f'порядок жидкостей: {len(seq)} чанков (из {len(full)} full) -> {a.fluid_txt}')
    if a.sched:
        init_end = {(r['cx'], r['cz']): r['start'] + r['dur'] for r in rows if r['status'].split(':')[-1] == 'initialize_light' and r['level'].endswith(a.dim)}
        full = {(r['cx'], r['cz']): r['start'] + r['dur'] for r in rows if r['status'].split(':')[-1] == 'full' and r['level'].endswith(a.dim)}
        ready = {}
        for (cx, cz) in full:
            nb = [full.get((cx + i, cz + j)) for i in (-1, 0, 1) for j in (-1, 0, 1)]
            if all(v is not None for v in nb): ready[(cx, cz)] = max(nb)
        out = ['# MCSCHED 1', f'# dim {a.dim}', '# F cx cz mask — шаг FEATURES (mask: окно 5×5, чанки с уже выполненным INITIALIZE_LIGHT); P cx cz — пост-обработка (FULL у всех 8 соседей)']
        events = chunk_events(rows, load_reads(a.jfr, a.dim), a.dim)
        out[2] += '; W cx cz — чанк перезагружен с диска (карты *_WG потеряны); U cx cz — чанк стал FULL'
        for kind, r in interleave(st, events):
            if kind == 'E':
                out.append(f"{r[1]} {r[2]} {r[3]}"); continue
            m = 0
            for dz in range(-2, 3):
                for dx in range(-2, 3):
                    e = init_end.get((r['cx'] + dx, r['cz'] + dz))
                    if e is not None and e <= r['start'] and (dx or dz): m |= 1 << ((dz + 2) * 5 + dx + 2)
            out.append(f"F {r['cx']} {r['cz']} {m:x}")
        for (x, z) in sorted(ready, key=lambda k: (ready[k], k)): out.append(f'P {x} {z}')
        open(a.sched, 'w').write('\n'.join(out) + '\n')
        print(f'расписание: {len(st)} шагов FEATURES, {len(ready)} чанков пост-обработки -> {a.sched}')
    if a.out:
        json.dump(order, open(a.out, 'w'))
        print('порядок записан:', a.out)
    if a.v:
        print(order[:40])


if __name__ == '__main__':
    main()

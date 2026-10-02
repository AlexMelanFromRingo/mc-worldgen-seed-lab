#!/usr/bin/env python3
"""Ворота G1: каждая зарегистрированная density-функция libmcgen против настоящего кода игры (oracle `df`), бит-в-бит.

Сравниваются биты результата: float (26.3+, DensitySampler возвращает float) или double (26.1/26.2).
Точки случайные: 70 % |x|,|z| < 2·10⁴, 20 % < 10⁶, 10 % < 3·10⁷ (обёртка координат шума); y — по высоте измерения ±32.

Режимы покрытия:
  «домашний» (--points, по умолчанию 10⁶): каждая функция — в своём измерении/пресете (overworld/* → overworld normal,
      overworld_large_biomes/* → large_biomes, overworld_amplified/* → amplified, nether/* → nether, end/* → end;
      общие shift_x, shift_z, y, zero — во всех трёх измерениях);
  «перекрёстный» (--cross N): все остальные сочетания функция × измерение × пресет по N точек
      (в Nether/End RandomState другой — legacy-случайность, — это дополнительная проверка).
Oracle — одна JVM (`oracle/run.sh <V> serve`); libmcgen считается параллельно (mcgen-cli df, все функции за запуск).

    python3 libmcgen/tests/g1_df.py --version 26.3 --points 1000000 --cross 20000 --report g1-26.3.json
"""
import argparse, json, os, random, subprocess, sys, time, threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')

DIM_Y = {'overworld': (-64, 384), 'nether': (0, 256), 'end': (0, 256)}
DIM_FULL = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
SHARED = {'minecraft:shift_x', 'minecraft:shift_z', 'minecraft:y', 'minecraft:zero'}


class Oracle:
    def __init__(self, version):
        env = dict(os.environ)
        env.setdefault('ORACLE_JAVA_OPTS', '-Xmx3g')
        self.p = subprocess.Popen([os.path.join(ROOT, 'oracle', 'run.sh'), version, 'serve'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)

    def cmd(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        if not r.get('ok'):
            raise RuntimeError(f'oracle: {r.get("error")} ({line[:120]})')
        return r

    def close(self):
        try:
            self.p.stdin.write('quit\n'); self.p.stdin.flush(); self.p.wait(10)
        except Exception:
            self.p.kill()


def gen_points(n, dim, rng):
    lo, h = DIM_Y[dim]
    pts = []
    for _ in range(n):
        r = rng.random()
        R = 20000 if r < 0.7 else (1000000 if r < 0.9 else 30000000)
        pts.append((rng.randint(-R, R), rng.randint(lo - 32, lo + h + 32), rng.randint(-R, R)))
    return pts


def our_bits_multi(pack, version, dim, preset, seed, ids, pts):
    """{id: [биты]} за один запуск mcgen-cli"""
    inp = '\n'.join(f'{x} {y} {z}' for x, y, z in pts) + '\n'
    r = subprocess.run([CLI, 'df', '--pack', pack, '--version', version, '--dim', DIM_FULL[dim], '--preset', preset,
                        '--seed', str(seed), '--id', ','.join(ids)], input=inp, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f'mcgen-cli df: {r.stderr.strip()}')
    lines = r.stdout.split('\n')
    n = len(pts)
    return {i: [int(lines[k * n + j].split()[1], 16) for j in range(n)] for k, i in enumerate(ids)}


def oracle_bits(orc, dim, preset, seed, dfid, pts, is_float, batch):
    out = []
    for i in range(0, len(pts), batch):
        chunk = pts[i:i + batch]
        coords = ' '.join(f'{x} {y} {z}' for x, y, z in chunk)
        r = orc.cmd(f'df {dim} {seed} {dfid} {coords} --preset {preset}')
        bits = r['fbits'] if is_float else r['dbits']
        out.extend((b & 0xFFFFFFFF) if is_float else (b & 0xFFFFFFFFFFFFFFFF) for b in bits)
    return out


def list_dfs(pack):
    base = os.path.join(pack, 'data', 'minecraft', 'worldgen', 'density_function')
    ids = []
    for d, _, fs in os.walk(base):
        for f in fs:
            if f.endswith('.json'):
                rel = os.path.relpath(os.path.join(d, f), base)[:-5].replace(os.sep, '/')
                ids.append('minecraft:' + rel)
    return sorted(ids)


def home_of(dfid):
    """список (dim, preset), где функция «дома»"""
    if dfid in SHARED:
        return [('overworld', 'normal'), ('nether', 'normal'), ('end', 'normal')]
    p = dfid.split(':', 1)[1]
    if p.startswith('overworld_large_biomes/'): return [('overworld', 'large_biomes')]
    if p.startswith('overworld_amplified/'): return [('overworld', 'amplified')]
    if p.startswith('overworld/'): return [('overworld', 'normal')]
    if p.startswith('nether/'): return [('nether', 'normal')]
    if p.startswith('end/'): return [('end', 'normal')]
    return [('overworld', 'normal')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--pack')
    ap.add_argument('--ids', nargs='+', default=None)
    ap.add_argument('--points', type=int, default=1000000, help='точек на функцию в «домашнем» сочетании')
    ap.add_argument('--cross', type=int, default=0, help='точек на прочие сочетания функция×измерение×пресет (0 — не проверять)')
    ap.add_argument('--seeds', nargs='+', type=int, default=[12345, -4172144997902289642, 1])
    ap.add_argument('--batch', type=int, default=4000)
    ap.add_argument('--report')
    ap.add_argument('--rng', type=int, default=20261002)
    a = ap.parse_args()
    pack = a.pack or os.path.join(ROOT, 'run', f'pack-{a.version}')
    is_float = not (a.version.startswith('26.1') or a.version.startswith('26.2'))
    ids = a.ids and [i if ':' in i else 'minecraft:' + i for i in a.ids] or list_dfs(pack)
    combos = [('overworld', 'normal'), ('overworld', 'large_biomes'), ('overworld', 'amplified'), ('nether', 'normal'), ('end', 'normal')]
    # задания: (dim, preset) → [(id, points)]
    tasks = {c: [] for c in combos}
    for i in ids:
        homes = home_of(i)
        for c in combos:
            n = a.points if c in homes else a.cross
            if n > 0:
                tasks[c].append((i, n))
    orc = Oracle(a.version)
    rng = random.Random(a.rng)
    stats = {}   # (dim, preset, id) → [ok, bad, examples, kind]
    t0 = time.time()
    try:
        for (dim, preset), lst in tasks.items():
            if not lst:
                continue
            for seed in a.seeds:
                # точки: общий набор максимального размера, функциям — префиксы
                nmax = max(n for _, n in lst) // len(a.seeds) + 1
                pts = gen_points(nmax, dim, rng)
                res = {}
                def run_ours():
                    try:
                        # группы по числу точек, чтобы не считать лишнего
                        by_n = {}
                        for i, n in lst:
                            by_n.setdefault(n // len(a.seeds) + 1, []).append(i)
                        for n, grp in by_n.items():
                            res.update(our_bits_multi(pack, a.version, dim, preset, seed, grp, pts[:n]))
                    except Exception as e:
                        res['__err__'] = e
                th = threading.Thread(target=run_ours); th.start()
                ob = {}
                for i, n in lst:
                    ob[i] = oracle_bits(orc, dim, preset, seed, i, pts[:n // len(a.seeds) + 1], is_float, a.batch)
                th.join()
                if '__err__' in res:
                    raise res['__err__']
                for i, n in lst:
                    st = stats.setdefault((dim, preset, i), [0, 0, [], 'home' if (dim, preset) in home_of(i) else 'cross'])
                    for p, x, y in zip(pts, ob[i], res[i]):
                        if x == y:
                            st[0] += 1
                        else:
                            st[1] += 1
                            if len(st[2]) < 5:
                                st[2].append({'seed': seed, 'pos': p, 'oracle': hex(x), 'ours': hex(y)})
            for i, _ in lst:
                st = stats[(dim, preset, i)]
                print(f'{dim:9s} {preset:12s} {st[3]:5s} {i:58s} {st[0] + st[1]:9d} '
                      f'{"OK" if st[1] == 0 else "РАСХОЖДЕНИЙ " + str(st[1])}' + ('' if not st[2] else '  ' + json.dumps(st[2][0])), flush=True)
    finally:
        orc.close()
    dt = time.time() - t0
    rows = [{'dim': k[0], 'preset': k[1], 'id': k[2], 'kind': v[3], 'points': v[0] + v[1], 'mismatch': v[1], 'examples': v[2]} for k, v in stats.items()]
    home_pts = sum(r['points'] for r in rows if r['kind'] == 'home'); cross_pts = sum(r['points'] for r in rows if r['kind'] == 'cross')
    bad = sum(r['mismatch'] for r in rows)
    minhome = min((r['points'] for r in rows if r['kind'] == 'home'), default=0)
    print(f'\nИТОГО {a.version}: функций {len(ids)}; домашних сочетаний {sum(1 for r in rows if r["kind"] == "home")} '
          f'(точек {home_pts}, минимум на сочетание {minhome}); перекрёстных {sum(1 for r in rows if r["kind"] == "cross")} '
          f'(точек {cross_pts}); расхождений {bad}; {dt:.0f} с')
    if a.report:
        json.dump({'version': a.version, 'seeds': a.seeds, 'home_points': home_pts, 'cross_points': cross_pts, 'mismatch_total': bad,
                   'seconds': dt, 'rows': rows}, open(a.report, 'w'), indent=1, ensure_ascii=False)
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Дифф-тест: биомы движка (tools/mcquery) против эталона (oracle serve) на сетках квартов.

Использование:
  tests/diff_biomes.py [--versions 26.1 26.2 26.3] [--dims overworld nether end] [--seeds N] [--size 64] [--preset normal]
Печатает число точек, число расхождений и первые примеры. Код возврата 1 при расхождениях.
"""
import argparse, json, os, random, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXED_SEEDS = [0, 1, -1, 12345, 8675309, 9223372036854775807, -9223372036854775808, 1234567890123456789, -4172144997902289642]


def seed_list(n, rnd):
    s = list(FIXED_SEEDS)
    while len(s) < n:
        s.append(rnd.randint(-2**63, 2**63 - 1))
    return s[:n]


class Oracle:
    def __init__(self, version):
        self.p = subprocess.Popen([f'{ROOT}/oracle/run.sh', version, 'serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def call(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        while True:
            out = self.p.stdout.readline()
            if not out:
                raise RuntimeError('oracle died')
            if out.startswith('{'):
                return json.loads(out)

    def close(self):
        try:
            self.p.stdin.close(); self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def engine_grid(version, dim, preset, seed, qx0, qz0, n, qy):
    d = {'overworld': 'ow', 'nether': 'nether', 'end': 'end'}[dim]
    cmd = [f'{ROOT}/tools/mcquery', version, d]
    if dim == 'overworld':
        cmd += ['--preset', preset]
    cmd += [str(seed), str(qx0), str(qz0), str(n), str(n), str(qy)]
    env = dict(os.environ, MCGEN_ROOT=ROOT)
    out = subprocess.run(cmd, capture_output=True, text=True, env=env, check=True).stdout.split('\n')
    return [l.split(' ')[2] for l in out if l]


_TABLES = {}


def load_table(version, dim):
    key = (version, dim)
    if key not in _TABLES:
        rows = []
        name = 'overworld' if dim == 'overworld' else 'nether'
        for line in open(f'{ROOT}/data/params/{version}/{name}.tsv'):
            if line.startswith('#'):
                continue
            f = line.rstrip('\n').split('\t'); v = [int(x) for x in f[:13]]
            hi_adj = -1 if version.startswith('26.4') else 0   # 26.4: верхняя граница исключающая (hi-1)
            rows.append(([(v[2 * i], v[2 * i + 1] + hi_adj) for i in range(6)] + [(v[12], v[12] + hi_adj)], f[13]))
        _TABLES[key] = rows
    return _TABLES[key]


def engine_target(version, dim, preset, seed, qx, qz, qy):
    d = {'overworld': 'ow', 'nether': 'nether'}[dim]
    cmd = [f'{ROOT}/tools/mcquery', version, d] + (['--preset', preset] if dim == 'overworld' else []) + [str(seed), str(qx), str(qz), '1', '1', str(qy), '--climate']
    out = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, MCGEN_ROOT=ROOT), check=True).stdout.split()
    return [int(x) for x in out[2:8]] + [0]


def is_tie(version, dim, preset, seed, qx, qz, qy, a, b):
    """Оба биома (a, b) имеют минимальный fitness => ничья (в Java выбор зависит от lastResult предыдущего поиска)."""
    if dim == 'end':
        return False
    tg = engine_target(version, dim, preset, seed, qx, qz, qy)
    best = None; names = set()
    for box, name in load_table(version, dim):
        d = 0
        for (lo, hi), x in zip(box, tg):
            e = x - hi if x > hi else (lo - x if lo > x else 0)
            d += e * e
        if best is None or d < best:
            best = d; names = {name}
        elif d == best:
            names.add(name)
    return a in names and b in names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--versions', nargs='*', default=['26.1', '26.2', '26.3'])
    ap.add_argument('--dims', nargs='*', default=['overworld', 'nether', 'end'])
    ap.add_argument('--seeds', type=int, default=12)
    ap.add_argument('--size', type=int, default=48)
    ap.add_argument('--preset', default='normal')
    ap.add_argument('--rng', type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.rng)
    total = bad = 0
    for v in a.versions:
        orc = Oracle(v)
        for dim in a.dims:
            dtot = dbad = ties = 0
            for seed in seed_list(a.seeds, rnd):
                # окна: у начала координат, далеко (+-1e5 кварт = 4e5 блоков) и очень далеко
                wins = [(-a.size // 2, -a.size // 2), (rnd.randint(-50000, 50000), rnd.randint(-50000, 50000)),
                        (rnd.randint(-3000000, 3000000) // 4, rnd.randint(-3000000, 3000000) // 4)]
                if dim == 'end':
                    wins = [(rnd.randint(-2000, 2000), rnd.randint(-2000, 2000)), (300, -300), (rnd.randint(-50000, 50000), rnd.randint(-50000, 50000))]
                qys = [16] if dim == 'end' else ([-16, 0, 16, 40] if dim == 'overworld' else [4, 16, 24])
                for (qx0, qz0) in wins:
                    for qy in qys[:2] if (qx0, qz0) != wins[0] else qys:
                        r = orc.call(f'biome {dim} {seed} {qx0} {qz0} {a.size} {a.size} {qy}' + (f' --preset {a.preset}' if dim == 'overworld' and a.preset != 'normal' else ''))
                        if not r.get('ok'):
                            print('oracle error', r); return 2
                        ref = [r['palette'][i] for i in r['idx']]
                        got = engine_grid(v, dim, a.preset, seed, qx0, qz0, a.size, qy)
                        diff = [(i, ref[i], got[i]) for i in range(len(ref)) if ref[i] != got[i]]
                        dtot += len(ref)
                        real = []
                        for (i, e, g) in diff:
                            if is_tie(v, dim, a.preset if a.preset != 'normal' else 'overworld', seed, qx0 + i % a.size, qz0 + i // a.size, qy, e, g):
                                ties += 1
                            else:
                                real.append((i, e, g))
                        diff = real; dbad += len(diff)
                        if diff:
                            i, e, g = diff[0]
                            print(f'  MISMATCH v={v} {dim} seed={seed} win=({qx0},{qz0}) qy={qy}: {len(diff)}/{len(ref)}; first at q=({qx0 + i % a.size},{qz0 + i // a.size}) oracle={e} engine={g}')
            print(f'{v} {dim}: {dtot} points, {dbad} real mismatches, {ties} fitness-ties (vanilla non-deterministic)')
            total += dtot; bad += dbad
        orc.close()
    print(f'TOTAL: {total} points, {bad} mismatches')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

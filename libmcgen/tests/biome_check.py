#!/usr/bin/env python3
"""Биомы libmcgen против настоящего кода игры (oracle):
  * «сырые» биомы клеток 4×4×4 (BiomeSource.getNoiseBiome; oracle `biome … --mode point`),
  * биомы с зумом BiomeManager (mcgen_biome_at / mcgen_biome_grid; oracle `blockbiome`),
  * биомы стадии BIOMES чанка (26.3+: пакетный путь sampleVolume; oracle `biome … --mode chunk --fullcolumn`).
Ничьи R-дерева (равный fitness двух листьев) в самой игре зависят от предыдущего запроса потока (lastResult) —
расхождение считается «ничьей», если расстояния до лучших листьев обоих биомов равны (по таблице климата libmcgen).

    python3 libmcgen/tests/biome_check.py --version 26.3
"""
import argparse, json, os, random, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')
DUMP = os.path.join(ROOT, 'libmcgen', 'build', 'tests', 'biome_params_dump')
sys.path.insert(0, HERE)
from g1_df import Oracle, DIM_FULL  # noqa: E402

QY = {'overworld': [-14, -4, 4, 16, 30, 60], 'nether': [2, 16, 28], 'end': [4, 16]}


def cli(args, pack, version, dim, preset, seed):
    r = subprocess.run([CLI] + args + ['--pack', pack, '--version', version, '--dim', DIM_FULL[dim], '--preset', preset, '--seed', str(seed)],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.split()


def load_params(pack, version, dim):
    r = subprocess.run([DUMP, pack, version, 'nether' if dim == 'nether' else 'overworld'], capture_output=True, text=True, check=True)
    pts = []
    for line in r.stdout.splitlines():
        f = line.split('\t')
        v = [int(x) for x in f[:13]]
        pts.append((v, f[13]))
    return pts


def best_dist(params, biome, q, excl):
    """минимальный fitness листа с данным биомом (как Climate.Parameter.distance; 26.4 — исключающая верхняя граница)"""
    best = None
    for v, b in params:
        if b != biome:
            continue
        d = 0
        for k in range(7):
            lo, hi = v[2 * k], v[2 * k + 1] if k < 6 else v[12]
            if k == 6:
                lo = hi = v[12]
            t = q[k] if k < 6 else 0
            above = t - hi + (1 if excl else 0)
            below = lo - t
            x = above if above > 0 else (below if below > 0 else 0)
            d += x * x
        best = d if best is None or d < best else best
    return best


M64 = (1 << 64) - 1


def to_s64(v):
    v &= M64
    return v - (1 << 64) if v >> 63 else v


def zoom_quart(seed_hashed, x, y, z):
    """BiomeManager.getBiome: какая клетка 4×4×4 выбрана для блока (x,y,z)"""
    def nxt(r, c):
        r = to_s64(r * (r * 6364136223846793005 + 1442695040888963407))
        return to_s64(r + c)
    def fiddle(r):
        return (((r >> 24) % 1024) / 1024.0 - 0.5) * 0.9
    ax, ay, az = x - 2, y - 2, z - 2
    px, py, pz = ax >> 2, ay >> 2, az >> 2
    fx, fy, fz = (ax & 3) / 4.0, (ay & 3) / 4.0, (az & 3) / 4.0
    best, bi = float('inf'), 0
    for i in range(8):
        cx = px if (i & 4) == 0 else px + 1; cy = py if (i & 2) == 0 else py + 1; cz = pz if (i & 1) == 0 else pz + 1
        dx = fx if (i & 4) == 0 else fx - 1.0; dy = fy if (i & 2) == 0 else fy - 1.0; dz = fz if (i & 1) == 0 else fz - 1.0
        r = seed_hashed
        for c in (cx, cy, cz, cx, cy, cz):
            r = nxt(r, c)
        f1 = fiddle(r); r = nxt(r, seed_hashed); f2 = fiddle(r); r = nxt(r, seed_hashed); f3 = fiddle(r)
        d = (dz + f3) ** 2 + (dy + f2) ** 2 + (dx + f1) ** 2
        if best > d:
            best, bi = d, i
    return (px if (bi & 4) == 0 else px + 1, py if (bi & 2) == 0 else py + 1, pz if (bi & 1) == 0 else pz + 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--seeds', nargs='+', type=int, default=[12345, -4172144997902289642, 1, 987654321])
    ap.add_argument('--size', type=int, default=48)
    ap.add_argument('--report')
    a = ap.parse_args()
    pack = os.path.join(ROOT, 'run', f'pack-{a.version}')
    newf = not (a.version.startswith('26.1') or a.version.startswith('26.2'))
    excl = a.version.startswith('26.4')
    orc = Oracle(a.version)
    combos = [('overworld', 'normal'), ('overworld', 'large_biomes'), ('overworld', 'amplified'), ('nether', 'normal'), ('end', 'normal')]
    params = {d: load_params(pack, a.version, d) for d in ('overworld', 'nether')}
    tot = {'raw': [0, 0, 0], 'zoom': [0, 0, 0], 'chunk': [0, 0, 0]}   # точек, ничьих, реальных расхождений
    examples = []
    rng = random.Random(7)
    t0 = time.time()
    n = a.size
    try:
        for dim, preset in combos:
            for seed in a.seeds:
                for win in (0, 50000, 750000):
                    qx0 = rng.randint(-win, win) - n // 2; qz0 = rng.randint(-win, win) - n // 2
                    for qy in QY[dim]:
                        o = orc.cmd(f'biome {dim} {seed} {qx0} {qz0} {n} {n} {qy} --preset {preset}')
                        ob = [o['palette'][i] for i in o['idx']]
                        mb = cli(['qbiome', '--x0', str(qx0), '--z0', str(qz0), '--nx', str(n), '--nz', str(n), '--y', str(qy)], pack, a.version, dim, preset, seed)
                        bad = [i for i in range(n * n) if ob[i] != mb[i]]
                        q = None
                        if bad and dim != 'end':
                            c = orc.cmd(f'climategrid {dim} {seed} {qx0} {qz0} {n} {n} {qy} --preset {preset}')['q']
                            q = c
                        ties = real = 0
                        for i in bad:
                            if q is not None:
                                tq = [q['t'][i], q['h'][i], q['c'][i], q['e'][i], q['d'][i], q['w'][i]]
                                P = params['nether' if dim == 'nether' else 'overworld']
                                if best_dist(P, ob[i], tq, excl) == best_dist(P, mb[i], tq, excl):
                                    ties += 1; continue
                            real += 1
                            if len(examples) < 10:
                                examples.append({'kind': 'raw', 'dim': dim, 'preset': preset, 'seed': seed, 'q': [qx0 + i % n, qy, qz0 + i // n], 'oracle': ob[i], 'ours': mb[i]})
                        tot['raw'][0] += n * n; tot['raw'][1] += ties; tot['raw'][2] += real
                    # зум: блоки
                    bx0, bz0, by = qx0 * 4, qz0 * 4, (QY[dim][len(QY[dim]) // 2]) * 4
                    o = orc.cmd(f'blockbiome {dim} {seed} {bx0} {by} {bz0} {n} {n} 3 --preset {preset}')
                    ob = [o['palette'][i] for i in o['idx']]
                    mb = cli(['biome', '--x0', str(bx0), '--z0', str(bz0), '--nx', str(n), '--nz', str(n), '--step', '3', '--y', str(by)], pack, a.version, dim, preset, seed)
                    tot['zoom'][0] += n * n
                    hs = o['hashed_seed']
                    for i in range(n * n):
                        if ob[i] == mb[i]:
                            continue
                        bx, bz = bx0 + 3 * (i % n), bz0 + 3 * (i // n)
                        qx, qy2, qz = zoom_quart(hs, bx, by, bz)
                        tie = False
                        if dim != 'end':
                            c = orc.cmd(f'climategrid {dim} {seed} {qx} {qz} 1 1 {qy2} --preset {preset}')['q']
                            tq = [c['t'][0], c['h'][0], c['c'][0], c['e'][0], c['d'][0], c['w'][0]]
                            P = params['nether' if dim == 'nether' else 'overworld']
                            tie = best_dist(P, ob[i], tq, excl) == best_dist(P, mb[i], tq, excl)
                        if tie:
                            tot['zoom'][1] += 1
                        else:
                            tot['zoom'][2] += 1
                            if len(examples) < 10:
                                examples.append({'kind': 'zoom', 'dim': dim, 'preset': preset, 'seed': seed, 'block': [bx, by, bz], 'oracle': ob[i], 'ours': mb[i]})
                    # чанки (26.3+: пакетный путь)
                    if newf and dim != 'end':
                        cx0, cz0 = qx0 // 4, qz0 // 4
                        nc = 3
                        mb = cli(['chunkbiomes', '--cx0', str(cx0), '--cz0', str(cz0), '--nx', str(nc), '--nz', str(nc), '--threads', '1'], pack, a.version, dim, preset, seed)
                        H = 384 if dim == 'overworld' else 256
                        miny = -64 if dim == 'overworld' else 0
                        per = H // 4 * 16
                        for qy in QY[dim]:
                            o = orc.cmd(f'biome {dim} {seed} {cx0 * 4} {cz0 * 4} {nc * 4} {nc * 4} {qy} --preset {preset} --mode chunk --fullcolumn')
                            ob = [o['palette'][i] for i in o['idx']]
                            for iz in range(nc * 4):
                                for ix in range(nc * 4):
                                    ci = (iz // 4) * nc + ix // 4
                                    li = ((qy - miny // 4) * 4 + iz % 4) * 4 + ix % 4
                                    ours = mb[ci * per + li]
                                    tot['chunk'][0] += 1
                                    if ours != ob[iz * nc * 4 + ix]:
                                        tot['chunk'][2] += 1
                                        if len(examples) < 10:
                                            examples.append({'kind': 'chunk', 'dim': dim, 'seed': seed, 'q': [cx0 * 4 + ix, qy, cz0 * 4 + iz], 'oracle': ob[iz * nc * 4 + ix], 'ours': ours})
            print(f'{dim} {preset}: raw {tot["raw"]}, zoom {tot["zoom"]}, chunk {tot["chunk"]}', flush=True)
    finally:
        orc.close()
    print(f'\nИТОГО {a.version}: [точек, ничьих, расхождений] raw {tot["raw"]}, zoom {tot["zoom"]}, chunk {tot["chunk"]}; {time.time() - t0:.0f} с')
    for e in examples:
        print('  пример', json.dumps(e, ensure_ascii=False))
    if a.report:
        json.dump({'version': a.version, 'totals': tot, 'examples': examples}, open(a.report, 'w'), indent=1, ensure_ascii=False)
    return 0 if all(v[2] == 0 for v in tot.values()) else 1


if __name__ == '__main__':
    sys.exit(main())

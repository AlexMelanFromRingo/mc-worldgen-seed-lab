#!/usr/bin/env python3
"""Подбор областей 11×11 чанков с максимальной долей заданных биомов (для эталонов feature:<id> деревьев, где область плана «пустая»).
    g5_tree_areas.py <биом[,биом…]> [--seeds 12345,8675309,-7048155917072976836] [--span 160] [--y 20]  → лучшие центры (seed, cx, cz, доля)"""
import argparse, os, subprocess
import numpy as np
ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser(); ap.add_argument('biomes'); ap.add_argument('--seeds', default='12345,8675309,-7048155917072976836'); ap.add_argument('--span', type=int, default=160)
ap.add_argument('--y', type=int, default=72, help='блочная y'); ap.add_argument('--dim', default='minecraft:overworld'); ap.add_argument('--top', type=int, default=3)
a = ap.parse_args()
want = {b if ':' in b else 'minecraft:' + b for b in a.biomes.split(',')}
best = []
for seed in a.seeds.split(','):
    n = 2 * a.span
    # одна точка на чанк: команда biome — блочные координаты и шаг (16 блоков)
    r = subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', 'biome', '--pack', f'{ROOT}/run/pack-26.3', '--version', '26.3', '--dim', a.dim, '--seed', seed, '--x0', str(-16 * a.span),
                        '--z0', str(-16 * a.span), '--nx', str(n), '--nz', str(n), '--step', '16', '--y', str(a.y)], capture_output=True, text=True)
    names = r.stdout.split()
    if len(names) != n * n: print('seed', seed, 'вывод', len(names), 'ожидалось', n * n); continue
    g = np.array([x in want for x in names], dtype=np.float32).reshape(n, n)    # [z][x]? порядок вывода: x внешний или z — определим по ячейке
    k = 11
    cs = np.cumsum(np.cumsum(np.pad(g, ((1, 0), (1, 0))), 0), 1)
    win = cs[k:, k:] - cs[:-k, k:] - cs[k:, :-k] + cs[:-k, :-k]
    idx = np.dstack(np.unravel_index(np.argsort(-win, axis=None)[:200], win.shape))[0]
    used = []
    for i, j in idx:
        if any(abs(i - u) < 11 and abs(j - v) < 11 for u, v in used): continue
        used.append((i, j)); best.append((float(win[i, j]) / (k * k), seed, int(j) + k // 2 - a.span, int(i) + k // 2 - a.span))
        if len(used) >= a.top: break
best.sort(reverse=True)
for f, seed, cx, cz in best[:a.top * 2]: print(f'доля {f:.2f}  seed {seed}  cx {cx}  cz {cz}')

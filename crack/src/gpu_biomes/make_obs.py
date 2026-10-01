#!/usr/bin/env python3
"""Генератор файла наблюдений из oracle (настоящий код Mojang) — для тестов и демонстраций.
  make_obs.py --version 26.3 --dim overworld --seed 8675309 --k 12 [--blocks] [--radius 5000] [--preset normal] [--rng 1] --out obs.txt
Точки — случайные в квадрате ±radius КВАРТОВ (Overworld y = 64 (qy=16)); End — вне центрального острова."""
import argparse, os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from known_answer import Oracle

ap = argparse.ArgumentParser()
ap.add_argument('--version', required=True); ap.add_argument('--dim', default='overworld'); ap.add_argument('--preset', default='normal')
ap.add_argument('--seed', type=int, required=True); ap.add_argument('--k', type=int, default=12); ap.add_argument('--radius', type=int, default=5000)
ap.add_argument('--blocks', action='store_true'); ap.add_argument('--rng', type=int, default=1); ap.add_argument('--out', required=True)
a = ap.parse_args()
rnd = random.Random(a.rng); orc = Oracle(a.version)
with open(a.out, 'w') as f:
    f.write(f'# seed={a.seed} {a.version} {a.dim} {a.preset} {"blocks" if a.blocks else "quarts"} (oracle)\n')
    for _ in range(a.k):
        qx, qz = rnd.randint(-a.radius, a.radius), rnd.randint(-a.radius, a.radius)
        while a.dim == 'end' and qx * qx + qz * qz < 400 * 400:
            qx, qz = rnd.randint(-a.radius, a.radius), rnd.randint(-a.radius, a.radius)
        qy = 16 if a.dim != 'nether' else rnd.randint(2, 30)
        if a.blocks:
            x, y, z = qx * 4 + rnd.randint(0, 3), qy * 4 + rnd.randint(0, 3), qz * 4 + rnd.randint(0, 3)
            b = orc.blockbiome(a.dim, a.seed, x, y, z, a.preset); f.write(f'{x} {y} {z} {b}\n')
        else:
            b = orc.biome(a.dim, a.seed, qx, qy, qz, a.preset); f.write(f'{qx} {qy} {qz} {b}\n')
orc.close()
print('written', a.out)

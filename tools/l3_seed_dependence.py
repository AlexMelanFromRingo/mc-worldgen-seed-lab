#!/usr/bin/env python3
"""Зависимость смещения «центр диска глины -> начало алмазной жилы» от младших 4 бит seed мира (версии 1.13–1.17.1, seed фичи = decSeed+index+10000*step).
decSeed = (X*a + Z*b) ^ worldSeed, X,Z кратны 16 => младшие 4 бита decSeed = младшие 4 бита worldSeed во всех чанках мира; поэтому (dx,dz) почти
детерминированы для данного (worldSeed mod 16, idx_clay, idx_diamond).
  tools/l3_seed_dependence.py <idx_clay> <idx_diamond> [step=6] [n=4000]
Печатает для s0 = 0..15 доминирующие (dx,dz) = (x_d - x_c, z_d - z_c) mod 16 и их вероятности (случайные старшие биты seed и случайные чанки)."""
import sys, numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import l3_replay as R
ic, idd = int(sys.argv[1]), int(sys.argv[2])
step = int(sys.argv[3]) if len(sys.argv) > 3 else 6
n = int(sys.argv[4]) if len(sys.argv) > 4 else 4000
rng = np.random.default_rng(3)
print(f'idx_clay={ic}, idx_diamond={idd}, step={step}: для s0 = worldSeed mod 16:')
for s0 in range(16):
    T = np.zeros((16, 16), np.int64)
    for w in range(40):
        seed = int(rng.integers(0, 1 << 62)) * 16 + s0
        cx = rng.integers(-500, 500, n // 40); cz = rng.integers(-500, 500, n // 40)
        dec = R.decoration_seeds(seed, cx, cz)
        xc, zc = R.feature_draws(dec, ic, step); xd, zd = R.feature_draws(dec, idd, step)
        np.add.at(T, ((xd - xc) % 16, (zd - zc) % 16), 1)
    fl = np.argsort(T.ravel())[::-1][:4]
    print(f'  s0={s0:2d}: ' + '  '.join(f'({k // 16},{k % 16}) {T.ravel()[k] / T.sum():.2f}' for k in fl))

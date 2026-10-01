#!/usr/bin/env python3
"""Зависимость смещения «центр диска глины -> начало алмазной жилы» от seed мира (версии 1.13–1.17.1: seed фичи = decSeed + index + 10000*step).
decSeed = (X*a + Z*b) ^ worldSeed, X,Z кратны 16 => младшие 4 бита decSeed = (worldSeed & 15) во ВСЕХ чанках мира, бит 4 = бит 4 seed XOR чётность (cx+cz).
Поэтому (dx,dz) почти детерминированы для данных (worldSeed & 15, чётность cx+cz, idx_clay, idx_diamond, порядок вызовов).
  tools/l3_seed_dependence.py <idx_clay> <idx_diamond> [step=6] [order=xzy|xyz] [n=6000]
Печатает для каждого s0 = worldSeed & 15 и чётности (cx+cz)&1 доминирующие (dx,dz) = (x_d - x_c, z_d - z_c) mod 16 с вероятностями
(случайные старшие биты seed и случайные чанки). x_c,z_c — первые два nextInt(16) глины; для алмаза: xzy -> x,z = 1-й и 2-й; xyz -> x,z = 1-й и 3-й."""
import sys, numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import l3_replay as R


def table(ic, idd, step, order, s0, parity, n=6000, seed=3):
    rng = np.random.default_rng(seed + s0 * 2 + parity)
    T = np.zeros((16, 16), np.int64)
    for w in range(60):
        S = int(rng.integers(0, 1 << 62)) * 16 + s0
        cx = rng.integers(-500, 500, n // 60); cz = rng.integers(-500, 500, n // 60)
        cz = cz - ((cx + cz - parity) & 1)            # подгоняем чётность (cx+cz)&1 = parity
        dec = R.decoration_seeds(S, cx, cz)
        xc, zc = R.feature_draws(dec, ic, step)[:2]
        d = R.feature_draws(dec, idd, step, n=3)
        xd = d[0]; zd = d[1] if order == 'xzy' else d[2]
        np.add.at(T, ((xd - xc) % 16, (zd - zc) % 16), 1)
    return T


if __name__ == '__main__':
    ic, idd = int(sys.argv[1]), int(sys.argv[2])
    step = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    order = sys.argv[4] if len(sys.argv) > 4 else 'xzy'
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 6000
    print(f'idx_clay={ic}, idx_diamond={idd}, step={step}, порядок алмаза {order}: для s0 = worldSeed & 15 и чётности (cx+cz)&1:')
    for s0 in range(16):
        parts = []
        Tall = np.zeros((16, 16), np.int64)
        for par in (0, 1):
            T = table(ic, idd, step, order, s0, par, n); Tall += T
            fl = np.argsort(T.ravel())[::-1][:3]
            parts.append(f'чёт{par}: ' + ' '.join(f'({k // 16},{k % 16}) {T.ravel()[k] / T.sum():.2f}' for k in fl))
        fl = np.argsort(Tall.ravel())[::-1][:2]
        print(f'  s0={s0:2d}: ' + ' | '.join(parts) + '   [всего: ' + ' '.join(f'({k // 16},{k % 16}) {Tall.ravel()[k] / Tall.sum():.2f}' for k in fl) + ']')

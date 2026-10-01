#!/usr/bin/env python3
"""Строгий тест «глина → алмазы на фиксированном смещении»: наблюдаемые счётчики пар по смещениям (dx,dz) против нулевого
распределения, полученного случайными сдвигами глины (сохраняют распределение по y и локальную кластеризацию)."""
import sys, numpy as np
from scipy.spatial import cKDTree
seed = sys.argv[1]; K = int(sys.argv[2]) if len(sys.argv) > 2 else 20
z = np.load(f'data/l3-w{seed}.npz'); clay, dia = z['clay'], z['diamond']
rng = np.random.default_rng(7)
tree = cKDTree(dia)
R = 14                       # окно по x,z
DY = (-135, -25)             # глина выше алмаза на 25..135 блоков
def hist(src):
    H = np.zeros((2 * R + 1, 2 * R + 1), dtype=np.int64); Hy = np.zeros(DY[1] - DY[0] + 1, dtype=np.int64)
    for (x, y, zz) in src:
        idx = tree.query_ball_point([x, y + (DY[0] + DY[1]) / 2, zz], r=max(R, (DY[1] - DY[0]) / 2) + 1, p=np.inf)
        if not idx: continue
        q = dia[idx] - np.array([x, y, zz])
        m = (np.abs(q[:, 0]) <= R) & (np.abs(q[:, 2]) <= R) & (q[:, 1] >= DY[0]) & (q[:, 1] <= DY[1])
        q = q[m]
        np.add.at(H, (q[:, 0] + R, q[:, 2] + R), 1); np.add.at(Hy, q[:, 1] - DY[0], 1)
    return H, Hy
for name, mask in (('все блоки глины y>30', np.ones(len(clay), bool)), ('глина на дне рек/океанов (y 40..64)', (clay[:, 1] >= 40) & (clay[:, 1] <= 64))):
    cl = clay[mask]
    if len(cl) < 100: print(name, 'мало'); continue
    sub = cl[rng.choice(len(cl), min(len(cl), 3000), replace=False)]
    H, Hy = hist(sub)
    # нулевое распределение: сдвиги по x,z на 200..2000 блоков
    maxz_null = []; tot_null = []
    Hnull = []
    for k in range(K):
        # циклический сдвиг (тор) внутри области: глина остаётся над областью с алмазами, но локальная связь разрушена
        x0, x1 = dia[:, 0].min(), dia[:, 0].max(); z0, z1 = dia[:, 2].min(), dia[:, 2].max()
        sx, sz = rng.integers(60, x1 - x0 - 60), rng.integers(60, z1 - z0 - 60)
        mv = sub.copy(); mv[:, 0] = (sub[:, 0] - x0 + sx) % (x1 - x0 + 1) + x0; mv[:, 2] = (sub[:, 2] - z0 + sz) % (z1 - z0 + 1) + z0
        Hk, _ = hist(mv); Hnull.append(Hk)
    Hn = np.array(Hnull); mu = Hn.mean(axis=0); sd = Hn.std(axis=0) + 1e-9
    zz = (H - mu) / np.sqrt(np.maximum(mu, 1))                # пуассоновская z по среднему контролю
    # z каждого контроля относительно среднего остальных — для максимума при отсутствии связи
    maxn = [np.max(np.abs((Hn[k] - (Hn.sum(axis=0) - Hn[k]) / (K - 1)) / np.sqrt(np.maximum((Hn.sum(axis=0) - Hn[k]) / (K - 1), 1)))) for k in range(K)]
    print(f'\n== {name}: глины в выборке {len(sub)}')
    print(f'   пар в окне: наблюдено {H.sum()}, контроль в среднем {Hn.sum(axis=(1,2)).mean():.0f} ± {Hn.sum(axis=(1,2)).std():.0f}')
    print(f'   макс |z| по ячейкам (dx,dz): наблюдено {np.abs(zz).max():.2f}; в контролях (нулевое): среднее {np.mean(maxn):.2f}, макс {np.max(maxn):.2f} (из {K})')
    i = np.unravel_index(np.argmax(np.abs(zz)), zz.shape)
    print(f'   лучшая ячейка dx={i[0]-R}, dz={i[1]-R}: наблюдено {H[i]}, ожидалось {mu[i]:.1f}')

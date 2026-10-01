#!/usr/bin/env python3
"""Рисунок docs/img/l3_clay_diamond_offsets.png: z-карты 16x16 смещений (dx mod 16, dz mod 16) между центром диска глины (под водой) и центрами жил алмаза
в ТОЙ ЖЕ ячейке (чанке) по версиям; z = (наблюдено - среднее контролей) / sqrt(среднее контролей); контроль — релокация диска в другой чанк.
  tools/l3_plot.py"""
import sys, os, glob
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_pairs as P

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERS = [('1.12.2', 8), ('1.13.2', 0), ('1.15.2', 0), ('1.16.5', 0), ('1.17.1', 0), ('1.18.2', 0)]
CM = LinearSegmentedColormap.from_list('div', ['#2a6fb0', '#9bbbe0', '#eeeeee', '#f0b48a', '#c8501a'])   # две оттенка + нейтральная середина
CLIP = 8.0

fig, axs = plt.subplots(2, 3, figsize=(10.5, 7.4), constrained_layout=True)
for ax, (v, off) in zip(axs.ravel(), VERS):
    files = sorted(glob.glob(f'{ROOT}/data/l3v/{v}-w*.npz'))
    res, sets = P.analyze(files, off, 24, blocks_sample=10)
    s = sets['wet']; mu = s['TBk'].mean(axis=0)
    z = P.zmap(s['TB'], mu)
    im = ax.imshow(np.clip(z, -CLIP, CLIP), cmap=CM, vmin=-CLIP, vmax=CLIP, origin='upper', interpolation='nearest')
    ax.set_title(f"{v}: пар {int(s['TB'].sum())}, макс |z| = {np.abs(z).max():.0f}", fontsize=10)
    ax.set_xlabel('dz = z_алмаза - z_глины (mod 16)', fontsize=8); ax.set_ylabel('dx (mod 16)', fontsize=8)
    ax.set_xticks(range(0, 16, 3)); ax.set_yticks(range(0, 16, 3)); ax.tick_params(labelsize=8)
cb = fig.colorbar(im, ax=axs, shrink=0.6, location='right', label='z (обрезано до ±8; серый = связи нет)')
fig.suptitle('Связь «центр диска глины -> жила алмаза» внутри чанка (набор: глина под водой)', fontsize=11)
os.makedirs(f'{ROOT}/docs/img', exist_ok=True)
fig.savefig(f'{ROOT}/docs/img/l3_clay_diamond_offsets.png', dpi=130)
print('saved')

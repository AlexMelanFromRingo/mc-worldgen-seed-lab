"""Общие ресурсы тестов: таблица состояний (из кэша), цвета биомов, мир."""
import os
import numpy as np

import _boot
from mcgen_addon.assets import state_table

_T = None
_BIOMES = None


def biome_names():
    global _BIOMES
    if _BIOMES is None:
        d = os.path.join(_boot.PACK_DIR, 'data', 'minecraft', 'worldgen', 'biome')
        _BIOMES = sorted('minecraft:' + f[:-5] for f in os.listdir(d) if f.endswith('.json'))
    return _BIOMES


def table():
    """StateTable для 26.3 (кэш в SCRATCH/cache), цвета биомов привязаны."""
    global _T
    if _T is None:
        os.makedirs(os.path.join(_boot.SCRATCH, 'cache'), exist_ok=True)
        _T = state_table.load(_boot.ASSETS_DIR, _boot.PACK_DIR, os.path.join(_boot.SCRATCH, 'cache'), version=_boot.VERSION)
        _T.set_biomes(biome_names(), _boot.ASSETS_DIR, _boot.PACK_DIR)
    return _T


def random_world(t, rng, height=32, p_air=0.45, p_cube=0.2):
    """3×3 случайных чанков: смесь воздуха, непрозрачных кубов и случайных состояний. -> (nb[9], nbio[9])"""
    n = t.names.__len__()
    air = t.state_id('minecraft:air')
    cubes = np.nonzero((t.st_flags & state_table.F.OPAQUE) != 0)[0]
    nb, nbio = [], []
    for _ in range(9):
        a = rng.integers(0, n, size=height * 256).astype(np.uint16)
        r = rng.random(height * 256)
        a[r < p_air] = air
        m = (r >= p_air) & (r < p_air + p_cube)
        a[m] = cubes[rng.integers(0, len(cubes), size=int(m.sum()))]
        nb.append(a)
        nbio.append(rng.integers(0, len(biome_names()), size=(height // 4) * 16).astype(np.uint8))
    return nb, nbio

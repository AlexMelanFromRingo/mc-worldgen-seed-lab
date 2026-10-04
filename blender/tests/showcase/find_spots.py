#!/usr/bin/env python3
"""Поиск выразительных мест для показательных кадров (обычный python3, без Blender; нужна собранная libmcgen и pack-каталог).

    python3 blender/tests/showcase/find_spots.py --scene forest --seeds 1-60 [--top 8] [--pack run/pack-26.3] [--version 26.3]

Сцены: forest (берёзово-цветочный лес), snow (снежная тайга + горные пики), island (остров с пляжем в океане),
desert (пирамида в пустыне), village (деревня), nether (бастион / крепость), end (главный остров).
Печатает JSON-строки кандидатов: seed, cx0, cz0 (начало области в чанках), оценка и пояснение. Библиотека: $MCGEN_LIB или
libmcgen/build (см. core/paths.py). Биомная карта берётся через mcgen_biome_grid, постройки — через mcgen_structure_starts;
генерация блоков не нужна (быстро: секунды на сид).
"""
import argparse
import json
import multiprocessing as mp
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, 'blender', 'tests', 'addon'))
import _boot  # noqa: E402

_boot.load_core()
from mcgen_addon.core import lib  # noqa: E402

FOREST = ('forest', 'birch_forest', 'old_growth_birch_forest', 'flower_forest', 'dark_forest')
PEAKS = ('jagged_peaks', 'frozen_peaks', 'stony_peaks')
SNOWY = ('snowy_taiga', 'snowy_slopes', 'grove', 'snowy_plains', 'frozen_river', 'ice_spikes')
OCEANS = ('ocean', 'deep_ocean', 'cold_ocean', 'deep_cold_ocean', 'lukewarm_ocean', 'deep_lukewarm_ocean', 'warm_ocean', 'frozen_ocean',
          'deep_frozen_ocean')
SHORE = ('beach', 'snowy_beach', 'stony_shore')


def window_sums(mask, wy, wx):
    """Сумма маски по всем окнам wy×wx (интегральное изображение) -> массив форм (H-wy+1, W-wx+1)."""
    ii = np.pad(mask.astype(np.int32).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return ii[wy:, wx:] - ii[:-wy, wx:] - ii[wy:, :-wx] + ii[:-wy, :-wx]


class Ctx:
    def __init__(self, pack, version, dim, seed):
        self.g = lib.McGen.open(pack, version)
        self.names = [n.replace('minecraft:', '') for n in self.g.biome_names()]
        self.w = self.g.world(dim, 'normal', seed)
        self.seed = seed

    def cls(self, names):
        return np.isin(np.arange(len(self.names)), [i for i, n in enumerate(self.names) if n in names])


def search_biome_scene(c, scene, half=2048, step=16, wcx=16, wcz=12):
    if scene == 'island':
        step = 8
    """Окно wcx×wcz чанков, оценка по долям биомов; сетка шага step блоков."""
    n = half * 2 // step
    grid = c.w.biome_grid(-half, -half, n, n, step, 80)            # [iz, ix]
    ww, wh = wcx * 16 // step, wcz * 16 // step
    tot = ww * wh
    frac = lambda names: window_sums(c.cls(names)[grid], wh, ww) / tot   # noqa: E731
    if scene == 'forest':
        f = frac(('flower_forest', 'birch_forest', 'old_growth_birch_forest'))
        fl = frac(('flower_forest',))
        fo = frac(FOREST)
        bad = frac(OCEANS + ('river', 'frozen_river', 'beach', 'desert', 'badlands'))
        score = np.minimum(fo, 1.0) * 1.0 + f * 0.8 + fl * 1.5 - bad * 2
        score[(fo < 0.9) | (f < 0.5)] = -9
        why = lambda iy, ix: dict(birch_flower=round(float(f[iy, ix]), 2), flower_forest=round(float(fl[iy, ix]), 2), forest=round(float(fo[iy, ix]), 2))   # noqa: E731
    elif scene == 'snow':
        p = frac(PEAKS)
        s = frac(('snowy_taiga', 'snowy_slopes', 'grove'))
        bad = frac(OCEANS + ('desert', 'badlands', 'plains', 'beach'))
        score = np.minimum(p, 0.35) * 2 + np.minimum(s, 0.5) * 1.5 - bad * 3
        score[(p < 0.15) | (s < 0.25)] = -9
        why = lambda iy, ix: dict(peaks=round(float(p[iy, ix]), 2), snowy=round(float(s[iy, ix]), 2))   # noqa: E731
    elif scene == 'island':
        warm = frac(('warm_ocean', 'lukewarm_ocean', 'deep_lukewarm_ocean', 'ocean'))
        o = frac(OCEANS)
        sh = frac(('beach',))
        veg = frac(('jungle', 'sparse_jungle', 'bamboo_jungle', 'forest', 'plains', 'savanna', 'mushroom_fields', 'swamp', 'mangrove_swamp'))
        score = warm * 1.5 + np.minimum(sh, 0.05) * 8 + np.minimum(veg, 0.08) * 8 - np.abs(1 - o - 0.12)
        score[(warm < 0.6) | (sh < 0.012) | (veg < 0.015)] = -9
        why = lambda iy, ix: dict(warm=round(float(warm[iy, ix]), 2), beach=round(float(sh[iy, ix]), 3), veg=round(float(veg[iy, ix]), 3))   # noqa: E731
    else:
        raise SystemExit('нет такой сцены: ' + scene)
    out = []
    flat = score.copy()
    for _ in range(3):
        iy, ix = np.unravel_index(int(np.argmax(flat)), flat.shape)
        if flat[iy, ix] <= -9:
            break
        cx0 = (-half + ix * step) // 16
        cz0 = (-half + iy * step) // 16
        d = why(iy, ix)
        out.append(dict(seed=c.seed, cx0=int(cx0), cz0=int(cz0), nx=wcx, nz=wcz, score=round(float(flat[iy, ix]), 3), **d))
        flat[max(0, iy - wh):iy + wh, max(0, ix - ww):ix + ww] = -9   # не повторять соседние окна
    return out


def search_island(c, half=2048, step=8):
    """Острова: связные компоненты суши (не океан/река) площадью 40..500 клеток шага step, с пляжем и растительностью; окно 16x12 чанков
    центрируется на острове."""
    from scipy import ndimage
    n = half * 2 // step
    grid = c.w.biome_grid(-half, -half, n, n, step, 80)
    ocean = c.cls(OCEANS + ('river', 'frozen_river'))[grid]
    warm = c.cls(('warm_ocean', 'lukewarm_ocean', 'deep_lukewarm_ocean'))[grid]
    beach = c.cls(('beach',))[grid]
    veg = c.cls(('jungle', 'sparse_jungle', 'bamboo_jungle', 'forest', 'plains', 'savanna', 'swamp', 'mangrove_swamp', 'birch_forest'))[grid]
    lab, k = ndimage.label(~ocean)
    out = []
    for i, sl in enumerate(ndimage.find_objects(lab), start=1):
        m = lab[sl] == i
        area = int(m.sum())
        if not 40 <= area <= 500:
            continue
        h, w = m.shape
        if h > 30 or w > 40:
            continue
        nb = int((beach[sl] & m).sum())
        nv = int((veg[sl] & m).sum())
        if nb < 6 or nv < 8:
            continue
        # доля тёплой воды вокруг (рамка 24 клетки)
        y0, y1 = max(0, sl[0].start - 24), min(n, sl[0].stop + 24)
        x0, x1 = max(0, sl[1].start - 24), min(n, sl[1].stop + 24)
        ring_w = float(warm[y0:y1, x0:x1].mean())
        if ring_w < 0.5:
            continue
        cy, cx = (sl[0].start + sl[0].stop) // 2, (sl[1].start + sl[1].stop) // 2
        bx, bz = -half + cx * step, -half + cy * step
        out.append(dict(seed=c.seed, cx0=int(bx // 16 - 8), cz0=int(bz // 16 - 6), nx=16, nz=12, center=[int(bx), int(bz)], area_blocks=area * step * step,
                        score=round(ring_w + min(nb, 40) / 80 + min(nv, 60) / 120, 3), beach=nb, veg=nv, warm=round(ring_w, 2)))
    return out


def search_structure_scene(c, scene, dim, half=96):
    """Постройки: окно центрируется на старте; оценка — число частей и однородность биомов вокруг."""
    ids = {'desert': ('minecraft:desert_pyramid',), 'village': ('minecraft:village_plains', 'minecraft:village_taiga', 'minecraft:village_savanna',
                                                                  'minecraft:village_snowy', 'minecraft:village_desert'),
           'nether': ('minecraft:bastion_remnant', 'minecraft:fortress'), 'bastion': ('minecraft:bastion_remnant',),
           'fortress': ('minecraft:fortress',)}[scene]
    out = []
    for st in c.w.structure_starts(-half, -half, 2 * half, 2 * half):
        if st.id not in ids:
            continue
        x0, y0, z0, x1, y1, z1 = st.bb
        cx, cz = (x0 + x1) // 2, (z0 + z1) // 2
        if dim == 'minecraft:overworld':
            rad = 112
            g = c.w.biome_grid(cx - rad, cz - rad, 2 * rad // 8, 2 * rad // 8, 8, 80)
            names = [c.names[i] for i in g.reshape(-1)]
            want = {'desert': ('desert',), 'village': ('plains', 'meadow', 'forest', 'birch_forest', 'sunflower_plains', 'flower_forest', 'taiga',
                                                       'savanna', 'snowy_plains', 'snowy_taiga')}[scene]
            frac = sum(1 for n in names if n in want) / len(names)
            bad = sum(1 for n in names if n in OCEANS or n == 'river') / len(names)
            score = frac * 2 - bad * 4 + min(st.piece_count, 60) / 60.0
            if scene == 'desert':       # пирамида должна торчать из песка: высота поверхности ВНУТРИ её bounding box по настоящему рельефу (TERRAIN+SURFACE)
                c0x, c0z, c1x, c1z = x0 // 16, z0 // 16, x1 // 16, z1 // 16
                r = c.w.generate_region(c0x, c0z, c1x - c0x + 1, c1z - c0z + 1, stages=7)
                cols = []
                for ccz in range(c0z, c1z + 1):
                    for ccx in range(c0x, c1x + 1):
                        h = r.heightmap(ccx, ccz, 0)
                        for zz in range(16):
                            for xx in range(16):
                                if x0 <= ccx * 16 + xx <= x1 and z0 <= ccz * 16 + zz <= z1:
                                    cols.append(int(h[zz, xx]) - 1)
                hmin, hmax = min(cols), max(cols)
                score += 3.0 - (hmax - y0) / 2.0 - (hmax - hmin) / 2.0    # земля у основания пирамиды и ровная
        else:
            score = min(st.piece_count, 80) / 80.0
            frac = 1
        out.append(dict(seed=c.seed, id=st.id, cx0=int(cx // 16 - 8), cz0=int(cz // 16 - 6), nx=16, nz=12, bb=list(st.bb), pieces=st.piece_count,
                        score=round(float(score), 3), biome_frac=round(float(frac), 2)))
    return out


def work(args):
    scene, seed, pack, version = args
    dim = {'nether': 'minecraft:the_nether', 'bastion': 'minecraft:the_nether', 'fortress': 'minecraft:the_nether',
           'end': 'minecraft:the_end'}.get(scene, 'minecraft:overworld')
    try:
        c = Ctx(pack, version, dim, seed)
        if scene == 'island':
            return search_island(c)
        if scene in ('forest', 'snow'):
            return search_biome_scene(c, scene)
        return search_structure_scene(c, scene, dim)
    except Exception as e:   # noqa: BLE001
        return [dict(seed=seed, error=str(e))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', required=True)
    ap.add_argument('--seeds', default='1-40')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--pack', default=os.path.join(REPO, 'run', 'pack-26.3'))
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    lo, _, hi = a.seeds.partition('-')
    seeds = range(int(lo), int(hi or lo) + 1)
    with mp.Pool(a.jobs) as p:
        res = [r for sub in p.imap_unordered(work, [(a.scene, s, a.pack, a.version) for s in seeds]) for r in sub]
    err = [r for r in res if 'error' in r]
    if err:
        print('ошибки:', err[:3], file=sys.stderr)
    res = sorted((r for r in res if 'error' not in r), key=lambda r: -r['score'])
    for r in res[:a.top]:
        print(json.dumps(r))


if __name__ == '__main__':
    main()

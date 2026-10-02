#!/usr/bin/env python3
"""Подбор областей эталонных миров по биомам (tools/mcquery поверх engine/) -> tools/gt/matrix.json.

Для каждого seed сканируется квадрат +-R блоков на уровне y=68 (клетки 4 блока), окна 21x21 чанков (шаг 21 чанк) классифицируются по долям
океана / гор / пустынь+бесплодных земель / джунглей; для каждого класса берётся окно с максимальной долей (при равенстве — ближе к началу).
Результат — детерминированная матрица конфигураций (dim, seed, cx, cz, radius, label), которую читает queue.py.
"""
import argparse, json, os, subprocess, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT

SEEDS = [12345, 8675309, -7048155917072976836]
# редкие биомы для изоляции фич (feature_plan.py): метка -> (измерение, набор биомов)
EXTRAS = {
    'old_growth_taiga': ('overworld', {'old_growth_pine_taiga', 'old_growth_spruce_taiga'}),
    'pale_garden': ('overworld', {'pale_garden'}), 'dappled_forest': ('overworld', {'dappled_forest'}), 'flower_forest': ('overworld', {'flower_forest'}),
    'mangrove_swamp': ('overworld', {'mangrove_swamp'}), 'ice_spikes': ('overworld', {'ice_spikes'}), 'warm_ocean': ('overworld', {'warm_ocean'}),
    'cold_ocean': ('overworld', {'cold_ocean', 'deep_cold_ocean'}), 'deep_lukewarm_ocean': ('overworld', {'deep_lukewarm_ocean'}),
    'mushroom_fields': ('overworld', {'mushroom_fields'}), 'wooded_badlands': ('overworld', {'wooded_badlands'}),
    'windswept_forest': ('overworld', {'windswept_forest'}), 'windswept_savanna': ('overworld', {'windswept_savanna'}),
    'soul_sand_valley': ('nether', {'soul_sand_valley'}),
}
CLASSES = {
    'ocean': lambda n: n.endswith('ocean'),
    'mountains': lambda n: n in ('jagged_peaks', 'frozen_peaks', 'stony_peaks', 'snowy_slopes', 'grove', 'windswept_hills', 'windswept_forest',
                                 'windswept_gravelly_hills', 'windswept_savanna'),
    'desert': lambda n: n in ('desert', 'badlands', 'eroded_badlands', 'wooded_badlands'),
    'jungle': lambda n: n in ('jungle', 'bamboo_jungle', 'sparse_jungle'),
}


def scan(seed, R, qy=17, dim='ow'):
    n = (2 * R) // 4
    q0 = -R // 4
    out = subprocess.run([f'{ROOT}/tools/mcquery', '26.3', dim, str(seed), str(q0), str(q0), str(n), str(n), str(qy)],
                         capture_output=True, text=True, check=True).stdout.split('\n')
    names = [l.rsplit(' ', 1)[1][10:] for l in out if l]       # убрать 'minecraft:'
    arr = np.array(names).reshape(n, n)                        # [z][x]
    return arr, q0


def pick(arr, q0, win_chunks=21, stride_chunks=21):
    w = win_chunks * 4          # окно в клетках
    n = arr.shape[0]
    res = {}
    for cls, f in CLASSES.items():
        uniq = {u: f(u) for u in np.unique(arr)}
        m = np.vectorize(uniq.get)(arr).astype(np.int32)
        ps = np.zeros((n + 1, n + 1), dtype=np.int64)
        ps[1:, 1:] = m.cumsum(0).cumsum(1)
        best = None
        for z in range(0, n - w + 1, stride_chunks * 4 // 2):
            for x in range(0, n - w + 1, stride_chunks * 4 // 2):
                s = ps[z + w, x + w] - ps[z, x + w] - ps[z + w, x] + ps[z, x]
                frac = s / (w * w)
                # центр окна в чанках
                cx = (q0 + x + w // 2) // 4
                cz = (q0 + z + w // 2) // 4
                dist = abs(cx) + abs(cz)
                key = (round(frac, 3), -dist)
                if cls == 'ocean' and frac > 0.85:      # хотим и берег
                    key = (round(0.85 - (frac - 0.85), 3), -dist)
                if best is None or key > best[0]:
                    best = (key, cx, cz, frac, x, z)
        _, cx, cz, frac, x, z = best
        sub = arr[z:z + w, x:x + w]
        u, c = np.unique(sub, return_counts=True)
        top = sorted(zip(c.tolist(), u.tolist()), reverse=True)[:6]
        res[cls] = {'cx': int(cx), 'cz': int(cz), 'frac': round(float(frac), 3), 'top_biomes': [f'{b}:{k}' for k, b in top]}
    return res


def pick_extra(arr, q0, targets, win_chunks=11):
    w = win_chunks * 4
    n = arr.shape[0]
    m = np.isin(arr, list(targets)).astype(np.int32)
    ps = np.zeros((n + 1, n + 1), dtype=np.int64)
    ps[1:, 1:] = m.cumsum(0).cumsum(1)
    best = None
    step = win_chunks * 2
    for z in range(0, n - w + 1, step):
        for x in range(0, n - w + 1, step):
            sm = ps[z + w, x + w] - ps[z, x + w] - ps[z + w, x] + ps[z, x]
            cx, cz = (q0 + x + w // 2) // 4, (q0 + z + w // 2) // 4
            key = (int(sm), -(abs(cx) + abs(cz)))
            if best is None or key > best[0]:
                best = (key, cx, cz, int(sm))
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--range', type=int, default=4000, help='полуширина сканирования, блоков')
    ap.add_argument('--out', default=f'{ROOT}/tools/gt/matrix.json')
    a = ap.parse_args()
    cfgs = []
    info = {}
    arrs = {}
    for seed in SEEDS:
        arr, q0 = scan(seed, a.range)
        arrs[seed] = (arr, q0)
        regs = pick(arr, q0)
        info[str(seed)] = regs
        cfgs.append({'dim': 'overworld', 'seed': seed, 'cx': 0, 'cz': 0, 'radius': 10, 'label': 'spawn'})
        for cls, r in regs.items():
            cfgs.append({'dim': 'overworld', 'seed': seed, 'cx': r['cx'], 'cz': r['cz'], 'radius': 10, 'label': cls})
        cfgs.append({'dim': 'nether', 'seed': seed, 'cx': 0, 'cz': 0, 'radius': 10, 'label': 'center'})
        cfgs.append({'dim': 'nether', 'seed': seed, 'cx': 60, 'cz': -60, 'radius': 10, 'label': 'offset'})
        cfgs.append({'dim': 'end', 'seed': seed, 'cx': 0, 'cz': 0, 'radius': 10, 'label': 'main_island'})
        cfgs.append({'dim': 'end', 'seed': seed, 'cx': 90, 'cz': 0, 'radius': 10, 'label': 'outer_east'})
        cfgs.append({'dim': 'end', 'seed': seed, 'cx': -40, 'cz': -90, 'radius': 10, 'label': 'outer_north'})
    extras = []
    for label, (dim, targets) in EXTRAS.items():
        best = None
        for seed in SEEDS:
            if dim == 'overworld':
                arr, q0 = arrs[seed]
            else:
                arr, q0 = scan(seed, 1600, 8, 'nether')
            r = pick_extra(arr, q0, targets)
            if r and (best is None or r[0] > best[0]):
                best = (r[0], seed, r[1], r[2], r[3])
        if best and best[4] > 0:
            extras.append({'dim': dim, 'seed': best[1], 'cx': best[2], 'cz': best[3], 'radius': 5, 'label': label, 'cells': best[4]})
        print(f'extra {label:20s}', best)
    json.dump({'doc': 'Матрица ворот G2-G6: сгенерировано tools/gt/pick_regions.py (mcquery, y=68). label — класс ландшафта области. '
                      'extras — области (r=5) с редкими биомами для изоляции фич.',
               'seeds': SEEDS, 'picked': info, 'configs': cfgs, 'extras': extras}, open(a.out, 'w'), indent=1, ensure_ascii=False)
    for s, regs in info.items():
        print(s)
        for k, v in regs.items():
            print(f'   {k:10s} cx={v["cx"]:5d} cz={v["cz"]:5d} доля {v["frac"]}  {v["top_biomes"]}')


if __name__ == '__main__':
    main()

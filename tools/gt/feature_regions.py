#!/usr/bin/env python3
"""Области для фич, которых нет в матрице/плане (редкие биомы, подземные биомы, большие радиусы): tools/gt/features_plan_extra.json.

  feature_regions.py            (mcquery по трём seed; ~5 мин)  ->  gen_queue.py --plan features_extra [--sets id,id]

Для каждой цели ищется окно (2r+1 чанков) с максимумом клеток нужных биомов; для окон выбирается лучший из трёх seed. Подземные биомы — на заданном уровне qy.
"""
import json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT
from pick_regions import scan, pick_extra, SEEDS

FROZEN = {'frozen_ocean', 'deep_frozen_ocean'}
OCEANS = {'cold_ocean', 'lukewarm_ocean', 'deep_ocean', 'deep_cold_ocean', 'deep_lukewarm_ocean', 'ocean'}
SNOW = {'snowy_plains', 'ice_spikes', 'frozen_river', 'snowy_taiga'}
# группа: (измерение, биомы, qy, радиус, [фичи])
GROUPS = {
    'frozen': ('overworld', FROZEN, 17, 5, ['blue_ice', 'iceberg_packed', 'iceberg_blue']),
    'snow': ('overworld', SNOW, 17, 5, ['freeze_top_layer']),
    'ocean_magma': ('overworld', OCEANS, 17, 5, ['underwater_magma']),
    'desert': ('overworld', {'desert'}, 17, 20, ['desert_well']),
    'deep_dark': ('overworld', {'deep_dark'}, -10, 5, ['sculk_vein', 'sculk_patch_deep_dark']),
    'sulfur': ('overworld', {'sulfur_caves'}, -2, 5, ['sulfur_pool', 'sulfur_spike', 'sulfur_spike_cluster', 'rooted_sulfur_spring']),
    'basalt': ('nether', {'basalt_deltas'}, 8, 10, ['basalt_pillar', 'delta', 'small_basalt_columns', 'large_basalt_columns', 'basalt_blobs', 'blackstone_blobs']),
}


def main():
    plan = []
    for name, (dim, biomes, qy, r, feats) in GROUPS.items():
        best = None
        for seed in SEEDS:
            arr, q0 = scan(seed, 3000 if dim == 'overworld' else 1600, qy, 'ow' if dim == 'overworld' else 'nether')
            res = pick_extra(arr, q0, biomes, 2 * r + 1)
            if res and (best is None or res[0] > best[0]):
                best = (res[0], seed, res[1], res[2], res[3])
        cells = best[4] if best else 0
        print(f'{name:12s} seed {best[1] if best else None} ({best[2] if best else None},{best[3] if best else None}) клеток {cells}', flush=True)
        if best and cells:
            for f in feats:
                plan.append({'feature': f'minecraft:{f}', 'dim': dim, 'seed': best[1], 'cx': best[2], 'cz': best[3], 'radius': r, 'group': name, 'cells': cells})
    for f in ('glowstone', 'glowstone_extra'):
        for seed in (12345, 8675309):
            plan.append({'feature': f'minecraft:{f}', 'dim': 'nether', 'seed': seed, 'cx': 0, 'cz': 0, 'radius': 10, 'group': 'nether_center'})
    plan.append({'feature': 'minecraft:end_gateway_return', 'dim': 'end', 'seed': 12345, 'cx': 90, 'cz': 0, 'radius': 20, 'group': 'end_outer'})
    plan.append({'feature': 'minecraft:end_platform', 'dim': 'end', 'seed': 12345, 'cx': 6, 'cz': 0, 'radius': 4, 'group': 'end_platform'})
    json.dump({'doc': 'tools/gt/feature_regions.py', 'plan': plan}, open(f'{ROOT}/tools/gt/features_plan_extra.json', 'w'), indent=1, ensure_ascii=False)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Генератор описаний одиночных деревьев для эталонов `custom:<имя>` (tools/gt/custom/<имя>.json).

    python3 libmcgen/tests/g5_tree_custom_gen.py [--version 26.3]

Для каждой настроенной фичи дерева (tree/fallen_tree) пишет placed_feature с цепочкой [rarity_filter 2, in_square, surface_water_depth_filter, heightmap OCEAN_FLOOR]
+ блок-предикаты (would_survive саженца) из существующих *_checked, в область матрицы, где растёт это дерево: деревья разрежены — не более одного на чанк, так что
расхождение локализуется внутри одного дерева. Файл: {"placed_feature": {...}, "dim", "seed", "cx", "cz", "radius", "feature": "<id настроенной фичи>"}.
"""
import argparse, glob, json, os
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
R = {
    'forest': (8675309, 0, 0), 'taiga': (-7048155917072976836, 0, 0), 'ogtaiga': (-7048155917072976836, 19, -30), 'savanna': (8675309, 180, -103),
    'cherry': (8675309, 180, -103), 'dark': (12345, 0, 0), 'jungle': (12345, -103, 2), 'mangrove': (-7048155917072976836, 107, -8), 'swamp': (12345, -19, -208),
    'pale': (12345, -118, -157), 'poplar': (12345, 41, -69),
}


def region(n):
    if n.startswith(('mega_spruce', 'mega_pine')): return 'ogtaiga'
    if n.startswith(('spruce', 'pine', 'fallen_spruce')): return 'taiga'
    if n.startswith('acacia'): return 'savanna'
    if n.startswith('cherry'): return 'cherry'
    if n.startswith('dark_oak'): return 'dark'
    if 'jungle' in n: return 'jungle'
    if 'mangrove' in n: return 'mangrove'
    if n.startswith('swamp'): return 'swamp'
    if n.startswith('pale_oak'): return 'pale'
    if 'poplar' in n: return 'poplar'
    return 'forest'


SKIP = {'azalea_tree', 'birch_bees_005', 'fancy_oak_bees_005', 'jungle_tree_no_vine', 'oak_bees_005', 'pale_oak_bonemeal', 'crimson_fungus_planted', 'warped_fungus_planted',
        'huge_brown_mushroom', 'huge_red_mushroom', 'crimson_fungus', 'warped_fungus', 'rooted_azalea_tree', 'rooted_sulfur_spring', 'cherry_bees_005_'}


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--version', default='26.3')
    a = ap.parse_args()
    base = f'{ROOT}/run/pack-{a.version}/data/minecraft/worldgen'
    cfg_dir = 'feature' if os.path.isdir(f'{base}/feature') else 'configured_feature'
    ref = {}
    for f in glob.glob(f'{base}/placed_feature/*.json'):
        d = json.load(open(f))
        if isinstance(d['feature'], str): ref.setdefault(d['feature'].split(':')[1], d)
    out = f'{ROOT}/tools/gt/custom' + ('' if a.version == '26.3' else '-' + a.version)
    os.makedirs(out, exist_ok=True)
    n = 0
    for f in sorted(glob.glob(f'{base}/{cfg_dir}/*.json')):
        name = os.path.basename(f)[:-5]
        d = json.load(open(f))
        if d['type'] not in ('minecraft:tree', 'minecraft:fallen_tree') or name in SKIP: continue
        filt = [m for m in (ref[name]['placement'] if name in ref else []) if m['type'] == 'minecraft:block_predicate_filter']
        if name == 'swamp_oak' or not filt:
            if name == 'mangrove' or name == 'tall_mangrove': st = 'minecraft:mangrove_propagule'
            else: st = 'minecraft:oak_sapling'
            if not filt: filt = [{'type': 'minecraft:block_predicate_filter', 'predicate': {'type': 'minecraft:would_survive', 'state': st}}]
        water = 5 if 'mangrove' in name else 0
        pl = [{'type': 'minecraft:rarity_filter', 'chance': 2}, {'type': 'minecraft:in_square'}, {'type': 'minecraft:surface_water_depth_filter', 'max_water_depth': water},
              {'type': 'minecraft:heightmap', 'heightmap': 'OCEAN_FLOOR'}] + filt
        seed, cx, cz = R[region(name)]
        spec = {'doc': 'W11: одиночное дерево, tools/gt/custom (libmcgen/tests/g5_tree_custom_gen.py)', 'feature': f'minecraft:{name}',
                'placed_feature': {'feature': f'minecraft:{name}', 'placement': pl}, 'dim': 'overworld', 'seed': seed, 'cx': cx, 'cz': cz, 'radius': 5}
        json.dump(spec, open(f'{out}/{name}.json', 'w'), indent=1, ensure_ascii=False)
        n += 1
    print(f'записано {n} файлов в {out}')


if __name__ == '__main__':
    main()

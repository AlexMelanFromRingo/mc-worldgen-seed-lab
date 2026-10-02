#!/usr/bin/env python3
"""План изоляции фич (G5, «каждая фича проверена в изоляции»): для каждой placed_feature — область матрицы, где её биомов больше всего.

  feature_plan.py [--version 26.3] [--radius 5] [--limit N]    ->  tools/gt/features_plan.json
  gen_queue.py --version 26.3 --variants feature --plan features [--plan-radius 5]

Биомы берутся из готовых raw-миров (биомы в raw совпадают с ванильными): гистограмма 4x4x4-клеток по области каждого мира; фича попадает в тот мир своего
измерения (по биомам: Nether/End-биомы известны списком), где сумма клеток её биомов максимальна. Фичи без биомов (используются только из конфигов) пропускаются.
"""
import argparse, collections, glob, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, GT, pack_dir
import anvil

NETHER = {'nether_wastes', 'soul_sand_valley', 'crimson_forest', 'warped_forest', 'basalt_deltas'}
END = {'the_end', 'end_highlands', 'end_midlands', 'small_end_islands', 'end_barrens'}


def hist(wd, dim, version, area):
    cache = f'{wd}/biome-hist.json'
    if os.path.exists(cache):
        return json.load(open(cache))
    w = anvil.World(f'{wd}/world', dim, version)
    cnt = np.zeros(256, dtype=np.int64)
    x0, z0, x1, z1 = area
    for cz in range(z0, z1 + 1):
        for cx in range(x0, x1 + 1):
            c = w.chunk(cx, cz)
            if c is not None and c.status == 'minecraft:full':
                cnt += np.bincount(c.biomes.ravel(), minlength=256)[:256]
    h = {w.biomes.names[i].split(':')[1]: int(cnt[i]) for i in range(256) if cnt[i] and i < len(w.biomes.names)}
    json.dump(h, open(cache, 'w'))
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='26.3'); ap.add_argument('--radius', type=int, default=5); ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    pk = f'{pack_dir(a.version)}/data/minecraft/worldgen'
    feat_biomes = collections.defaultdict(set)
    for f in glob.glob(f'{pk}/biome/*.json'):
        b = os.path.basename(f)[:-5]
        for step in json.load(open(f)).get('features', []):
            for pf in step:
                if isinstance(pf, str):
                    feat_biomes[pf].add(b)
    worlds = []
    for mp in glob.glob(f'{GT}/{a.version}/raw/*/manifest.json'):
        m = json.load(open(mp))
        if m.get('ok') and not mp.endswith('_rep/manifest.json') and '_t' not in os.path.basename(os.path.dirname(mp)).rsplit('-r', 1)[-1]:
            worlds.append((os.path.dirname(mp), m))
    hs = {wd: hist(wd, m['dim'], a.version, m['area_chunks']) for wd, m in worlds}
    plan, skipped = [], []
    for pf in sorted(feat_biomes):
        bs = feat_biomes[pf]
        dim = 'the_nether' if bs <= NETHER else 'the_end' if bs <= END else 'overworld'
        best = None
        for wd, m in worlds:
            if m['dim'] != dim:
                continue
            sc = sum(hs[wd].get(b, 0) for b in bs)
            if best is None or sc > best[0]:
                best = (sc, wd, m)
        if best is None or best[0] == 0:
            skipped.append(pf); continue
        _, wd, m = best
        plan.append({'feature': pf, 'dim': {'overworld': 'overworld', 'the_nether': 'nether', 'the_end': 'end'}[dim], 'seed': m['seed'],
                     'cx': m['center_chunk'][0], 'cz': m['center_chunk'][1], 'radius': min(m['radius'], a.radius), 'cells': best[0], 'biomes': sorted(bs)})
    if a.limit:
        plan = plan[:a.limit]
    json.dump({'doc': 'tools/gt/feature_plan.py: области для изолированных фич', 'version': a.version, 'radius': a.radius, 'plan': plan, 'skipped': skipped},
              open(f'{ROOT}/tools/gt/features_plan.json', 'w'), indent=1, ensure_ascii=False)
    print(f'фич в биомах: {len(feat_biomes)}, в плане: {len(plan)}, пропущено: {len(skipped)}')
    c = collections.Counter(p['dim'] for p in plan)
    print(dict(c))


if __name__ == '__main__':
    main()

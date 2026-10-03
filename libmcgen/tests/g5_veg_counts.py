#!/usr/bin/env python3
"""G5-veg: статистическая сверка «в сборе»: число блоков каждого типа растительности в нашем дампе и в эталоне featureset:veg_* на одной области.
Если логика совпадает, а расходятся лишь позиции из-за порядка чанков, суммы по типам совпадают с точностью до шума.
    python3 libmcgen/tests/g5_veg_counts.py <каталог мира featureset_veg_*> [--version 26.3] [--min 200]"""
import argparse, collections, json, os, shutil, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g5_features as G
sys.path.insert(0, f'{G.ROOT}/tools/gt')
import anvil
from mcr import Mcr
ap = argparse.ArgumentParser(); ap.add_argument('world'); ap.add_argument('--version', default='26.3'); ap.add_argument('--min', type=int, default=200); ap.add_argument('--rep', action='store_true', help='добавить столбец: повтор эталона (<мир>_rep1)'); a = ap.parse_args()
wd = os.path.abspath(a.world); m = json.load(open(f'{wd}/manifest.json'))
name = os.path.basename(os.path.dirname(wd))[len('featureset_'):]
lst = json.load(open(f'{G.ROOT}/tools/gt/featuresets/{name}.json'))
G.CLI = os.environ.get('MCGEN_CLI') or f'{G.ROOT}/libmcgen/build/mcgen-cli'
out = tempfile.mktemp(suffix='.mcr', dir='/tmp')
G.run_one(a.version, wd, ','.join(lst), 1, 0, '0x17', keep=out)
ours = Mcr(out); ref = anvil.World(wd + '/world', m['dim'], a.version)
cnt_o, cnt_r, cnt_p = collections.Counter(), collections.Counter(), collections.Counter()
rep = anvil.World(wd + '_rep1/world', m['dim'], a.version, states=ref.states, biomes=ref.biomes) if a.rep and os.path.isdir(wd + '_rep1') else None
x0, z0, x1, z1 = m['area_chunks']
on = np.array(ours.state_names, dtype=object)
for cx in range(x0, x1 + 1):
    for cz in range(z0, z1 + 1):
        rc = ref.chunk(cx, cz)
        if rc is None: continue
        u, c = np.unique(ours.blocks(cx, cz), return_counts=True)
        for k, v in zip(u, c): cnt_o[on[k].split('[')[0]] += int(v)
        u, c = np.unique(rc.blocks, return_counts=True)
        for k, v in zip(u, c): cnt_r[ref.states.names[int(k)].split('[')[0]] += int(v)
        pc = rep.chunk(cx, cz) if rep else None
        if pc is not None:
            u, c = np.unique(pc.blocks, return_counts=True)
            for k, v in zip(u, c): cnt_p[ref.states.names[int(k)].split('[')[0]] += int(v)
veg = [k for k in set(cnt_o) | set(cnt_r) if abs(cnt_o[k] - cnt_r[k]) > 0 or cnt_o[k] >= a.min]
print(f'{"блок":38s} {"наш":>9s} {"эталон":>9s} {"разность":>9s}  отн.' + ('   повтор  (повтор-эталон)' if rep else ''))
tot_o = tot_r = 0
for k in sorted(veg, key=lambda k: -max(cnt_o[k], cnt_r[k])):
    if max(cnt_o[k], cnt_r[k]) < a.min or k in ('minecraft:air', 'minecraft:stone', 'minecraft:deepslate', 'minecraft:water', 'minecraft:cave_air', 'minecraft:dirt', 'minecraft:netherrack'): continue
    d = cnt_o[k] - cnt_r[k]; print(f'{k:38s} {cnt_o[k]:>9,} {cnt_r[k]:>9,} {d:>+9,}  {d / max(cnt_r[k], 1) * 100:+.2f} %' + (f'  {cnt_p[k]:>9,} {cnt_p[k] - cnt_r[k]:>+9,}' if rep else ''))
os.remove(out)

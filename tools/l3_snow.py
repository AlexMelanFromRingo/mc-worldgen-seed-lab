#!/usr/bin/env python3
"""L3: снежная линия. По каждому биому: на каких высотах верхний блок колонки — снег (snow/snow_block/powder_snow/ice),
и сравнение с формулой Biome.getHeightAdjustedTemperature (снег при T_adj < 0.15; для T_base ≥ 0.15 порог высоты ≈ 80 + 800·(T_base − 0.15) ∓ 8)."""
import sys, os, json, collections, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_anvil as A
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
seed = int(sys.argv[1]); rad = int(sys.argv[2])
reg = f'{ROOT}/run/server-26.3/w{seed}/dimensions/minecraft/overworld/region'
BT = {}
for f in os.listdir(f'{ROOT}/src/data-26.3/data/minecraft/worldgen/biome'):
    d = json.load(open(f'{ROOT}/src/data-26.3/data/minecraft/worldgen/biome/{f}'))
    BT[f[:-5]] = (d['temperature'], d.get('has_precipitation', True), d.get('temperature_modifier'))
SNOWY = {A.bid(x) for x in ('minecraft:snow', 'minecraft:snow_block', 'minecraft:powder_snow', 'minecraft:ice', 'minecraft:packed_ice')}
SNOWTOP = {A.bid(x) for x in ('minecraft:snow', 'minecraft:snow_block', 'minecraft:powder_snow')}
AIRS = [A.bid(x) for x in ('minecraft:air', 'minecraft:cave_air')]
LEAVES = None
stat = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))   # biome -> y_bin(8) -> [всего, со снегом]
def bgrid(n):
    out = {}
    for s in n['sections']:
        b = s.get('biomes');
        if not b: continue
        names = [q if isinstance(q, str) else (q.get('Name') or q.get('') or '') for q in b['palette']]
        arr = np.zeros(64, dtype=int) if (len(names) == 1 or 'data' not in b) else A.unpack(b['data'], max(1, (len(names) - 1).bit_length()), 64)
        out[s['Y']] = (names, arr.reshape(4, 4, 4))
    return out
nch = 0
for cx in range(-rad, rad + 1):
    for cz in range(-rad, rad + 1):
        p = f'{reg}/r.{cx >> 5}.{cz >> 5}.mca'
        if not os.path.exists(p): continue
        n = A.read_chunk_nbt(p, cx, cz)
        if n is None or str(n.get('Status')) != 'minecraft:full': continue
        y0, a = A.chunk_blocks(n); nch += 1
        bg = bgrid(n)
        nonair = ~np.isin(a, AIRS)
        H = a.shape[0]
        top = H - 1 - np.argmax(nonair[::-1], axis=0)          # индекс верхнего непустого блока [z,x]
        for zz in range(0, 16, 2):
            for xx in range(0, 16, 2):
                yi = int(top[zz, xx]); y = yi + y0
                if y < 60: continue
                sy = y // 16
                if sy not in bg: continue
                names, arr = bg[sy]; b = names[arr[(y % 16) // 4, zz // 4, xx // 4]].replace('minecraft:', '')
                t = int(a[yi, zz, xx])
                s = stat[b][min(y // 8, 80)]; s[0] += 1; s[1] += (t in SNOWTOP)
print('чанков:', nch)
print('\nбиом | T_base | модификатор | высота над которой снег на верху (≥ 5 % колонок бина) | доля снега по высотам (бин 8 блоков: y:доля)')
rows = []
for b, bins in stat.items():
    tot = sum(v[0] for v in bins.values())
    if tot < 300: continue
    t, prec, mod = BT.get(b, (None, None, None))
    thr = next((k * 8 for k in sorted(bins) if bins[k][0] >= 20 and bins[k][1] / bins[k][0] >= 0.05), None)
    pred = None if t is None or t < 0.15 else (80 + 800 * (t - 0.15))
    prof = ' '.join(f'{k * 8}:{bins[k][1] / bins[k][0]:.0%}' for k in sorted(bins) if bins[k][0] >= 20)[:110]
    rows.append((t if t is not None else 9, b, t, mod, thr, pred, tot, prof))
for _, b, t, mod, thr, pred, tot, prof in sorted(rows):
    print(f'{b:26s} T={t:5} mod={str(mod):8s} снег с y≥{thr!s:5s} предсказано(≥0.15)={pred if pred is None else round(pred)!s:5s} n={tot:6d} | {prof}')

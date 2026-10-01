#!/usr/bin/env python3
"""«Под деревом с азалией всегда пещера?» — анализ корневых систем (кластеры rooted_dirt) на настоящих чанках."""
import sys, os, collections, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_anvil as A
from scipy.spatial import cKDTree
from scipy import ndimage as ndi
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
seed = int(sys.argv[1]); rad = int(sys.argv[2])
reg = f'{ROOT}/run/server-26.3/w{seed}/dimensions/minecraft/overworld/region'
RD = A.bid('minecraft:rooted_dirt')
chunks = {}
for cx in range(-rad, rad + 1):
    for cz in range(-rad, rad + 1):
        p = f'{reg}/r.{cx >> 5}.{cz >> 5}.mca'
        if not os.path.exists(p): continue
        n = A.read_chunk_nbt(p, cx, cz)
        if n is None: continue
        has = False
        for s in n['sections']:
            for q in s['block_states']['palette']:
                nm = q if isinstance(q, str) else (q.get('Name') or q.get('') or '')
                if nm == 'minecraft:rooted_dirt': has = True
        if not has: continue
        y0, a = A.chunk_blocks(n); chunks[(cx, cz)] = (y0, a)
print('чанков с rooted_dirt:', len(chunks))
pts = []
for (cx, cz), (y0, a) in chunks.items():
    for (yy, zz, xx) in np.argwhere(a == RD):
        pts.append((cx * 16 + int(xx), int(yy) + y0, cz * 16 + int(zz)))
pts = np.array(pts)
# кластеры: связность по расстоянию <= 3 (корни разрежены)
tree = cKDTree(pts); pairs = tree.query_pairs(3.0, output_type='ndarray')
import scipy.sparse as sp, scipy.sparse.csgraph as cg
g = sp.coo_matrix((np.ones(len(pairs)), (pairs[:, 0], pairs[:, 1])), shape=(len(pts), len(pts)))
nc, lab = cg.connected_components(g, directed=False)
print('блоков rooted_dirt:', len(pts), 'кластеров (корневых систем):', nc)
def block_at(x, y, z):
    c = (x >> 4, z >> 4)
    if c not in chunks: return None
    y0, a = chunks[c]
    yi = y - y0
    if yi < 0 or yi >= a.shape[0]: return None
    return A.NAME_LIST[a[yi, z & 15, x & 15]]
AIR = {'minecraft:air', 'minecraft:cave_air'}
LUSH = {'minecraft:moss_block', 'minecraft:moss_carpet', 'minecraft:hanging_roots', 'minecraft:cave_vines', 'minecraft:cave_vines_plant', 'minecraft:spore_blossom', 'minecraft:big_dripleaf', 'minecraft:big_dripleaf_stem', 'minecraft:small_dripleaf', 'minecraft:azalea', 'minecraft:flowering_azalea', 'minecraft:glow_lichen', 'minecraft:clay'}
stat = collections.Counter(); sizes = []
for k in range(nc):
    m = pts[lab == k]
    if len(m) < 8: stat['мелкие (<8 блоков)'] += 1; continue
    sizes.append(len(m))
    low = m[:, 1].min(); top = m[:, 1].max()
    # воздух непосредственно под ЛЮБЫМ блоком кластера нижнего слоя (±2)
    air_below = any(block_at(x, y - 1, z) in AIR for (x, y, z) in m[m[:, 1] <= low + 2])
    # «лаш»-признаки в радиусе 4 под нижним слоем
    lush = False
    for (x, y, z) in m[m[:, 1] <= low + 1]:
        for dy in range(1, 5):
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    if block_at(x + dx, y - dy, z + dz) in LUSH: lush = True
    stat['крупные кластеры'] += 1
    stat['  воздух (пещера) непосредственно под нижним слоем'] += air_below
    stat['  лаш-признаки (мох/лианы/корни/глина) под нижним слоем'] += lush
    stat['  пещера ИЛИ лаш-признаки'] += (air_below or lush)
    stat['  вертикальная протяжённость > 6'] += (top - low > 6)
for k, v in stat.items(): print(f'{k}: {v}')
print('размеры кластеров (блоков): медиана', int(np.median(sizes)) if sizes else 0, 'макс', max(sizes) if sizes else 0)

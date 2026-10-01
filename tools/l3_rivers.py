#!/usr/bin/env python3
"""Карта биомов (движок) + топология рек: компоненты связности, концы, петли.
  tools/l3_rivers.py <map.txt (вывод mcquery)> <n> <out.png>
"""
import sys, numpy as np
from PIL import Image
from scipy import ndimage as ndi

path, n, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
names = {}
ids = np.zeros(n * n, dtype=np.int32)
i = 0
for line in open(path):
    b = line.split()[2]
    ids[i] = names.setdefault(b, len(names)); i += 1
ids = ids.reshape(n, n)            # [z, x]
inv = {v: k for k, v in names.items()}
col = {}
def color(b):
    b = b.replace('minecraft:', '')
    if b in ('river',): return (30, 120, 255)
    if b in ('frozen_river',): return (150, 210, 255)
    if 'ocean' in b: return (10, 20, 110) if 'deep' in b else (25, 50, 150)
    if 'beach' in b or 'shore' in b: return (240, 230, 160)
    if b in ('swamp', 'mangrove_swamp'): return (80, 110, 70)
    if 'desert' in b or 'badlands' in b or 'savanna' in b: return (225, 190, 110)
    if 'snow' in b or 'frozen' in b or 'ice' in b or 'peaks' in b or 'grove' in b or 'slopes' in b: return (235, 240, 245)
    if 'forest' in b or 'taiga' in b or 'jungle' in b or 'cherry' in b or 'garden' in b: return (40, 120, 50)
    return (110, 190, 80)
lut = np.array([color(inv[i]) for i in range(len(inv))], dtype=np.uint8)
img = lut[ids]
Image.fromarray(img).resize((n, n), Image.NEAREST).save(out)
river = np.isin(ids, [names[k] for k in names if k in ('minecraft:river', 'minecraft:frozen_river')])
lab, nc = ndi.label(river, structure=np.ones((3, 3)))
sizes = ndi.sum(river, lab, range(1, nc + 1))
print('river pixels', int(river.sum()), 'components', nc, 'largest', int(sizes.max()) if nc else 0)
# соседство компонента с «не сушей-рекой»: океан / болото / край карты
ocean = np.isin(ids, [names[k] for k in names if 'ocean' in k])
swamp = np.isin(ids, [names[k] for k in names if 'swamp' in k])
H, W = river.shape
big = [c for c in range(1, nc + 1) if sizes[c - 1] >= 200]
print('components >= 200 px:', len(big))
touch_ocean = touch_swamp = touch_edge = 0; free = 0
for c in big:
    m = lab == c
    d = ndi.binary_dilation(m, iterations=3)
    to, ts = bool((d & ocean).any()), bool((d & swamp).any())
    ys, xs = np.where(m); te = ys.min() == 0 or xs.min() == 0 or ys.max() == H - 1 or xs.max() == W - 1
    touch_ocean += to; touch_swamp += ts; touch_edge += te
    if not (to or ts or te): free += 1
print('touch ocean', touch_ocean, 'touch swamp', touch_swamp, 'touch map edge', touch_edge, '| components touching none (closed loops / isolated):', free)
# Эйлерова характеристика: петли = число «дыр» (внутренних фонов) в сумме по компонентам
holes = 0
for c in big:
    m = lab == c
    filled = ndi.binary_fill_holes(m)
    hl, hn = ndi.label(filled & ~m)
    holes += hn
print('loops (holes) in big river components:', holes)

#!/usr/bin/env python3
"""Сводка по эталонному миру: гистограмма блоков/биомов, признаки течения жидкостей, статусы чанков.

  stats.py <каталог запуска или мир> [--dim overworld] [--version 26.3] [--top 25] [--area cx0 cz0 nx nz]
Область по умолчанию — из manifest.json (area_chunks). Нужна для проверки изоляции стадий (что именно осталось в мире)
и для оценки «грязи» от тиков (жидкости с level != 0 — результат растекания, не чистого заполнения).
"""
import argparse, collections, json, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anvil, diff


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('path'); ap.add_argument('--dim'); ap.add_argument('--version')
    ap.add_argument('--top', type=int, default=25); ap.add_argument('--area', nargs=4, type=int)
    a = ap.parse_args()
    mp = f'{a.path}/manifest.json'
    man = json.load(open(mp)) if os.path.exists(mp) else {}
    dim = a.dim or man.get('dim', 'overworld'); ver = a.version or man.get('version', '26.3')
    w = anvil.World(diff.find_world(a.path), dim, ver)
    if a.area:
        x0, z0, nx, nz = a.area
        box = (x0, z0, x0 + nx - 1, z0 + nz - 1)
    else:
        box = tuple(man['area_chunks']) if 'area_chunks' in man else None
    cnt = np.zeros(w.states.count, dtype=np.int64); bio = np.zeros(256, dtype=np.int64); n = 0
    for (cx, cz) in sorted(w.statuses()):
        if box and not (box[0] <= cx <= box[2] and box[1] <= cz <= box[3]):
            continue
        c = w.chunk(cx, cz)
        if c.status != 'minecraft:full':
            continue
        cnt += np.bincount(c.blocks.ravel(), minlength=w.states.count); bio += np.bincount(c.biomes.ravel(), minlength=256)[:256]; n += 1
    print(f'{a.path}: {n} чанков full в области {box}, блоков {cnt.sum():,}')
    order = np.argsort(-cnt)
    for i in order[:a.top]:
        if cnt[i]:
            print(f'  {cnt[i]:14,d}  {w.states.names[i]}')
    flow = {i: cnt[i] for i in range(w.states.count) if cnt[i] and (w.states.names[i].startswith(('minecraft:water[level=', 'minecraft:lava[level='))) and 'level=0]' not in w.states.names[i]}
    print(f'жидкости с level != 0 (растекание): {sum(flow.values()):,}')
    print('биомы клеток: ' + ', '.join(f'{w.biomes.names[i].split(":")[1]}={bio[i]:,}' for i in np.argsort(-bio)[:12] if bio[i]))
    print(f'различных состояний: {int((cnt > 0).sum())}')


if __name__ == '__main__':
    main()

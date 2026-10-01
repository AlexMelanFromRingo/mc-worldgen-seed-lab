#!/usr/bin/env python3
"""L3: эмпирический анализ настоящих чанков (сервер 26.3). Вход: run/server-26.3/w<seed>/dimensions/minecraft/overworld/region.
Проверяет утверждения о взаимосвязях:
  A. «рядом с глиной на некотором расстоянии внизу появляются алмазы» — кросс-корреляция смещений (глина → алмаз) против контроля;
  B. «под азалией всегда пещера» — у каждой колонки rooted_dirt нижний блок граничит с воздухом снизу?
  C. «руда, вскрытая воздухом, отбрасывается» — доля алмазов, касающихся воздуха, против доли твёрдых блоков того же слоя.
Использование: tools/l3_analyze.py <seed> [радиус_чанков]
"""
import sys, os, collections, glob, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_anvil as A
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
seed = int(sys.argv[1]); rad = int(sys.argv[2]) if len(sys.argv) > 2 else 40
reg = f'{ROOT}/run/server-26.3/w{seed}/dimensions/minecraft/overworld/region'

want = {'clay': 'minecraft:clay', 'diamond': ['minecraft:diamond_ore', 'minecraft:deepslate_diamond_ore'],
        'rooted_dirt': 'minecraft:rooted_dirt', 'azalea_leaves': ['minecraft:azalea_leaves', 'minecraft:flowering_azalea_leaves'],
        'hanging_roots': 'minecraft:hanging_roots', 'moss': 'minecraft:moss_block', 'gravel': 'minecraft:gravel'}
pts = collections.defaultdict(list)       # имя -> список (x,y,z)
air_ids = None
exposed_diamond = total_diamond = 0
exposed_solid = total_solid = 0
rooted_cols = []                           # (x,z,min_y_rooted_dirt, block_below_id)
t0 = time.time(); nch = 0
rng = np.random.default_rng(1)
for cx in range(-rad, rad + 1):
    for cz in range(-rad, rad + 1):
        p = f'{reg}/r.{cx >> 5}.{cz >> 5}.mca'
        if not os.path.exists(p): continue
        try:
            n = A.read_chunk_nbt(p, cx, cz)
        except Exception as e:
            continue
        if n is None or str(n.get('Status')) != 'minecraft:full': continue
        y0, a = A.chunk_blocks(n); nch += 1
        names = A.NAME_LIST
        if air_ids is None:
            air_ids = None
        air = np.isin(a, [A.bid('minecraft:air'), A.bid('minecraft:cave_air'), A.bid('minecraft:void_air')])
        dia_ids = [A.bid(x) for x in want['diamond']]
        dia = np.isin(a, dia_ids)
        # соседство с воздухом (6 направлений; за границу чанка — «нет воздуха»)
        adj = np.zeros_like(air)
        adj[1:] |= air[:-1]; adj[:-1] |= air[1:]
        adj[:, 1:] |= air[:, :-1]; adj[:, :-1] |= air[:, 1:]
        adj[:, :, 1:] |= air[:, :, :-1]; adj[:, :, :-1] |= air[:, :, 1:]
        total_diamond += int(dia.sum()); exposed_diamond += int((dia & adj).sum())
        # контроль: все твёрдые «каменные» блоки в том же диапазоне y [-64..16]
        ylo, yhi = -64 - y0, 16 - y0
        stone = np.isin(a, [A.bid(x) for x in ('minecraft:stone', 'minecraft:deepslate', 'minecraft:tuff', 'minecraft:granite', 'minecraft:diorite', 'minecraft:andesite')])
        sl = stone[ylo:yhi + 1]; al = adj[ylo:yhi + 1]
        total_solid += int(sl.sum()); exposed_solid += int((sl & al).sum())
        for k, v in want.items():
            ids = [A.bid(x) for x in (v if isinstance(v, list) else [v])]
            m = np.isin(a, ids)
            if k == 'clay':
                m &= (np.arange(a.shape[0])[:, None, None] + y0 > 30)          # речная/морская глина и ore_clay выше 30; нижнюю отдельно ниже
            idx = np.argwhere(m)
            if len(idx):
                g = idx + np.array([y0, cz * 16, cx * 16])           # (y, z, x) глобально
                pts[k].append(g[:, [2, 0, 1]])                        # -> (x, y, z)
        # колонки rooted_dirt
        rd = np.isin(a, [A.bid('minecraft:rooted_dirt')])
        if rd.any():
            cols = np.argwhere(rd.any(axis=0))
            for (zz, xx) in cols:
                ys = np.where(rd[:, zz, xx])[0]
                low = ys.min()
                below = a[low - 1, zz, xx] if low > 0 else -1
                rooted_cols.append((cx * 16 + int(xx), cz * 16 + int(zz), int(low) + y0, int(below)))
print(f'chunks parsed: {nch}, {time.time() - t0:.0f}s')
P = {k: (np.concatenate(v) if v else np.zeros((0, 3), int)) for k, v in pts.items()}
for k, v in P.items(): print(f'  {k}: {len(v)}')

# ---------- C. воздух и руда ----------
fd = exposed_diamond / max(1, total_diamond); fs = exposed_solid / max(1, total_solid)
print(f'\n[C] алмазы: {total_diamond}, касаются воздуха: {exposed_diamond} ({fd:.3%}); твёрдые блоки y∈[-64,16]: {total_solid}, касаются воздуха: {exposed_solid} ({fs:.3%}); отношение = {fd / max(fs, 1e-12):.3f}')

# ---------- B. азалия ----------
if rooted_cols:
    below_air = sum(1 for (_, _, _, b) in rooted_cols if A.NAME_LIST[b] in ('minecraft:air', 'minecraft:cave_air', 'minecraft:hanging_roots', 'minecraft:moss_carpet', 'minecraft:glow_lichen') ) if True else 0
    tot = len(rooted_cols)
    cnt = collections.Counter(A.NAME_LIST[b] if b >= 0 else 'edge' for (_, _, _, b) in rooted_cols)
    print(f'\n[B] колонок rooted_dirt: {tot}; блок под нижним rooted_dirt: {cnt.most_common(6)}')
    print(f'    нижний rooted_dirt над пещерой (воздух/корни/мох-ковёр): {below_air}/{tot} = {below_air / tot:.1%}')
    ys = np.array([c[2] for c in rooted_cols]); print(f'    высота нижнего rooted_dirt: медиана {np.median(ys):.0f}, min {ys.min()}, max {ys.max()}')
else:
    print('\n[B] rooted_dirt не найден в выборке')

# ---------- A. глина ↔ алмазы ----------
clay = P['clay']; dia = P['diamond']
print(f'\n[A] глина выше y=30: {len(clay)} блоков; алмазов: {len(dia)}')
if len(clay) and len(dia):
    tree = cKDTree(dia)
    # 1) расстояние от алмаза до ближайшей глины по горизонтали (x,z) против контроля
    ct = cKDTree(clay[:, [0, 2]])
    d_h, _ = ct.query(dia[:, [0, 2]])
    ctrl = np.column_stack([rng.integers(-rad * 16, rad * 16 + 16, len(dia)), dia[:, 1], rng.integers(-rad * 16, rad * 16 + 16, len(dia))])
    d_c, _ = ct.query(ctrl[:, [0, 2]])
    print(f'    горизонтальное расстояние алмаз→ближайшая глина: медиана {np.median(d_h):.1f}, контроль (случайные x,z, те же y) {np.median(d_c):.1f}')
    for r in (5, 10, 20):
        print(f'    доля с глиной в радиусе {r} по xz: алмазы {np.mean(d_h <= r):.3%}, контроль {np.mean(d_c <= r):.3%}')
    # 2) гистограмма смещений (глина → алмаз) в окне |dx|,|dz| <= 12, dy ∈ [-110, -20]
    sub = clay[rng.choice(len(clay), min(len(clay), 4000), replace=False)]
    def offsets(src):
        H = np.zeros((25, 25, 91), dtype=np.int64)       # dx, dz, dy+110
        for (x, y, z) in src:
            # алмазы в параллелепипеде
            idx = tree.query_ball_point([x, y - 65, z], r=60, p=np.inf)      # грубый куб, далее фильтр
            if not idx: continue
            q = dia[idx] - np.array([x, y, z])
            m = (np.abs(q[:, 0]) <= 12) & (np.abs(q[:, 2]) <= 12) & (q[:, 1] >= -110) & (q[:, 1] <= -20)
            q = q[m]
            np.add.at(H, (q[:, 0] + 12, q[:, 2] + 12, q[:, 1] + 110), 1)
        return H
    H = offsets(sub)
    # контроль: те же «глины», сдвинутые на случайные векторы (разрушает связь, сохраняя распределение по y)
    shift = np.column_stack([rng.integers(-300, 300, len(sub)), np.zeros(len(sub), int), rng.integers(-300, 300, len(sub))])
    Hc = offsets(sub + shift)
    tot, totc = H.sum(), Hc.sum()
    print(f'    пар (глина→алмаз) в окне: {tot}, контроль: {totc}, отношение {tot / max(totc, 1):.3f}')
    # поиск пиков: сумма по y → (dx,dz); и по (dx,dz) → dy
    Hs, Hcs = H.sum(axis=2), Hc.sum(axis=2)
    ratio = (Hs + 1) / (Hcs + 1)
    i = np.unravel_index(np.argmax(ratio), ratio.shape)
    print(f'    макс. отношение по (dx,dz): {ratio.max():.2f} при dx={i[0] - 12}, dz={i[1] - 12} (медиана {np.median(ratio):.2f}; пар {Hs[i]} против {Hcs[i]})')
    Hy, Hcy = H.sum(axis=(0, 1)), Hc.sum(axis=(0, 1))
    ry = (Hy + 1) / (Hcy + 1); j = int(np.argmax(ry))
    print(f'    макс. отношение по dy: {ry.max():.2f} при dy={j - 110} (медиана {np.median(ry):.2f})')
    # z-оценка по пуассону для лучшего (dx,dz)
    z = (Hs[i] - Hcs[i]) / np.sqrt(max(Hs[i] + Hcs[i], 1))
    print(f'    z-оценка лучшей ячейки (dx,dz): {z:.2f}; всего ячеек {ratio.size}, ожидаемый максимум |z| при отсутствии связи ≈ 3.5–4')
np.savez(f'{ROOT}/data/l3-w{seed}.npz', clay=clay, diamond=dia)

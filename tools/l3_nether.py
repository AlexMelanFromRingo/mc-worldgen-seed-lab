#!/usr/bin/env python3
"""L3 для Незера (26.3): вертикальная вытянутость пустот («столбы»), лава выше моря и распределение по биомам."""
import sys, os, collections, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_anvil as A
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
seed = int(sys.argv[1]); rad = int(sys.argv[2])
reg = f'{ROOT}/run/server-26.3/w{seed}/dimensions/minecraft/the_nether/region'
NONSOLID = [A.bid(x) for x in ('minecraft:air', 'minecraft:cave_air', 'minecraft:void_air', 'minecraft:lava', 'minecraft:water', 'minecraft:fire', 'minecraft:soul_fire')]
LAVA = A.bid('minecraft:lava')
rng = np.random.default_rng(3)
Y0, Y1 = 34, 120
acc = {ax: np.zeros(17) for ax in 'xyz'}; cnt = {ax: np.zeros(17) for ax in 'xyz'}
sum_s = 0.0; n_s = 0; sum_ss = 0.0
col_empty = col_full = col_total = 0
runs_obs = []; runs_shuf = []
lava_by_biome = collections.Counter(); lava_total_by_y = collections.Counter(); sol_by_biome = collections.Counter()
top_lava = collections.Counter()
chunks = 0
def biome_grid(n):
    secs = n['sections']; out = {}
    for s in secs:
        b = s.get('biomes')
        if not b: continue
        pal = b['palette']; names = [q if isinstance(q, str) else (q.get('Name') or q.get('') or '') for q in pal]
        if len(names) == 1 or 'data' not in b: arr = np.zeros(64, dtype=int)
        else:
            bits = max(1, (len(names) - 1).bit_length()); arr = A.unpack(b['data'], bits, 64)
        out[s['Y']] = (names, arr.reshape(4, 4, 4))     # [y, z, x]
    return out
def longest_run(m):           # m: bool [y, ...] -> длиннейшая серия True по y для каждого столбца
    best = np.zeros(m.shape[1:], dtype=int); cur = np.zeros(m.shape[1:], dtype=int)
    for y in range(m.shape[0]):
        cur = np.where(m[y], cur + 1, 0); best = np.maximum(best, cur)
    return best
for cx in range(-rad, rad + 1):
    for cz in range(-rad, rad + 1):
        p = f'{reg}/r.{cx >> 5}.{cz >> 5}.mca'
        if not os.path.exists(p): continue
        n = A.read_chunk_nbt(p, cx, cz)
        if n is None or str(n.get('Status')) != 'minecraft:full': continue
        y0, a = A.chunk_blocks(n); chunks += 1
        solid = ~np.isin(a, NONSOLID)
        sub = solid[Y0:Y1 + 1].astype(np.float32)              # [y, z, x]
        # автокорреляция (Пирсон) по осям внутри чанка
        m = sub.mean(); v = sub.var()
        if v > 0:
            for ax, axis in (('y', 0), ('z', 1), ('x', 2)):
                for d in range(1, 17):
                    if d >= sub.shape[axis]: break
                    s1 = np.take(sub, range(0, sub.shape[axis] - d), axis=axis); s2 = np.take(sub, range(d, sub.shape[axis]), axis=axis)
                    acc[ax][d] += ((s1 - m) * (s2 - m)).mean() / v; cnt[ax][d] += 1
        # пустые столбцы
        empty = ~sub.astype(bool)
        col_empty += int(empty.all(axis=0).sum()); col_full += int((~empty).all(axis=0).sum()); col_total += 256
        runs_obs.append(longest_run(empty).ravel())
        sh = sub.copy()
        for zz in range(16):
            for xx in range(16): rng.shuffle(sh[:, zz, xx])
        runs_shuf.append(longest_run(~sh.astype(bool)).ravel())
        # лава выше моря по биомам
        bg = biome_grid(n)
        lav = (a == LAVA)
        for y in range(34 - y0, a.shape[0]):
            ys = lav[y]
            if ys.any():
                lava_total_by_y[y + y0] += int(ys.sum())
        lv = lav[33 - y0:]                                      # y >= 33
        if lv.any():
            for (yy, zz, xx) in np.argwhere(lv):
                sy = (yy + 33) // 16
                if sy in bg:
                    names, arr = bg[sy]; lava_by_biome[names[arr[((yy + 33) % 16) // 4, zz // 4, xx // 4]]] += 1
        # верхняя лава в колонке
        topl = np.where(lav.any(axis=0), a.shape[0] - 1 - np.argmax(lav[::-1], axis=0) + y0, -1)
        for v_ in topl.ravel().tolist(): top_lava[v_] += 1
        for (yy, zz, xx) in np.argwhere(~np.isin(a[40 - y0:100 - y0], NONSOLID)) if False else []: pass
print('чанков Незера:', chunks)
print('\nАвтокорреляция твёрдости (Пирсон) по осям, y∈[%d;%d] внутри чанка: лаг d → корреляция' % (Y0, Y1))
def ac(ax): return [acc[ax][d] / max(cnt[ax][d], 1) for d in range(1, 17)]
for ax in 'xyz':
    c = ac(ax); half = next((d + 1 for d, v in enumerate(c) if v < 0.5), '>16')
    print(f'  {ax}: ' + ' '.join(f'{v:.2f}' for v in c[:12]) + f'  | длина корреляции (падение < 0.5): {half}')
ro = np.concatenate(runs_obs); rs = np.concatenate(runs_shuf)
print(f'\nКолонок: {col_total}; полностью пустых (y {Y0}..{Y1}, ни одного твёрдого блока): {col_empty} ({col_empty / col_total:.2%}); полностью твёрдых: {col_full} ({col_full / col_total:.2%})')
for L in (20, 40, 60, 80):
    print(f'  самая длинная вертикальная пустая серия ≥ {L}: колонок {np.mean(ro >= L):.2%}, при случайной перестановке по y: {np.mean(rs >= L):.2%}')
print('\nЛава: блоков на уровнях (y ≥ 34), топ:', lava_total_by_y.most_common(8))
tl = sum(top_lava.values())
print('верхний уровень лавы в колонке: ', [(k, f'{v / tl:.1%}') for k, v in top_lava.most_common(6)])
print('лава выше y=33 по биомам:', lava_by_biome.most_common(6))

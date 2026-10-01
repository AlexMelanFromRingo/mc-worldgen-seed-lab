#!/usr/bin/env python3
"""Внутричанковая связь «диск глины -> жила алмаза» и «трюк»: точное правило смещения по биомам (версии 1.13 – 1.17.1: seed фичи = decSeed+index+10000*step).

  tools/l3_chunk_rule.py <версия> [--bio center|00|next00] [--off 0|8] [--replay] [--json out.json] data/l3v/<версия>-w*.npz

Что делает:
  1. Наблюдаемые смещения (dx,dz) = (центр жилы алмаза) - (центр диска глины) для пар в ОДНОЙ ячейке (чанке) — отдельно по биомам и классам
     (swamp / river / ocean / прочие), таблица 16x16 по модулю 16 и знаковая;
  2. (--replay) для каждого биома точное воспроизведение decorationSeed по seed мира: подбор индексов фич (idx_алмаза, idx_глины) по
     совпадению наблюдаемых центров с предсказанными nextInt(16) x2; предсказанное распределение смещений по ВСЕМ чанкам группы;
  3. успех «трюка»: доля дисков, у которых в колонке (x_c+dx, z_c+dz) есть алмазная руда, для лучшего смещения, выбранного на обучающей половине
     данных и проверенного на тестовой (по чётности cx+cz); база — случайная колонка того же окна.
"""
import sys, os, re, json, argparse, collections
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_pairs as P
import l3_replay as R

SWAMP = {6, 134}; RIVER = {7, 11}; OCEAN = {0, 10, 24, 44, 45, 46, 47, 48, 49, 50}


def bclass(b):
    if b in SWAMP: return 'swamp'
    if b in RIVER: return 'river'
    if b in OCEAN: return 'ocean'
    return 'regular'


def cell_biome(w, k, field):
    if field == 'next00':
        v = w.bio.get((k[0] + 1, k[1] + 1)); return v[1] if v else -1
    v = w.bio.get(k)
    if not v: return -1
    return v[1] if field == '00' else v[0]


def world_seed(path):
    m = re.search(r'-w(-?\d+)\.npz$', path)
    return int(m.group(1))


def group_cells(w, field, wet_only=True):
    cl = w.cc[w.cc_wet] if wet_only else w.cc
    kc = w.cell(cl[:, :2]); kd = w.cell(w.dc[:, :2])
    cmap = collections.defaultdict(list); dmap = collections.defaultdict(list)
    for i in range(len(cl)):
        k = (int(kc[i, 0]), int(kc[i, 1]))
        if k in w.interior: cmap[k].append(cl[i, :2])
    for i in range(len(w.dc)):
        k = (int(kd[i, 0]), int(kd[i, 1]))
        if k in w.interior: dmap[k].append(w.dc[i, :2])
    return cmap, dmap


def find_idx(seed, cx, cz, fx, fz, steps=range(0, 11), idxs=range(0, 90), tol=2):
    """лучший (step,index): доля предсказанных (x,z) в пределах tol от наблюдаемых центров (по кругу mod 16)"""
    dec = R.decoration_seeds(seed, cx, cz)
    best = (0.0, -1, -1)
    for st in steps:
        for ix in idxs:
            x, z = R.feature_draws(dec, ix, st)
            dx = np.abs(((fx - (x + 0.5) + 8) % 16) - 8); dz = np.abs(((fz - (z + 0.5) + 8) % 16) - 8)
            h = float(((dx <= tol) & (dz <= tol)).mean())
            if h > best[0]: best = (h, st, ix)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('version'); ap.add_argument('files', nargs='+')
    ap.add_argument('--bio', default='center'); ap.add_argument('--off', type=int, default=0)
    ap.add_argument('--replay', action='store_true'); ap.add_argument('--order', default='xzy', help='порядок вызовов nextInt(16) у алмаза: xzy (1.16–1.17: square, range) либо xyz (1.13–1.15: COUNT_RANGE)'); ap.add_argument('--json'); ap.add_argument('--minveins', type=int, default=40)
    a = ap.parse_args()
    worlds = [(world_seed(p), P.World(p, a.off)) for p in a.files]
    print(f'== {a.version}: миров {len(worlds)}, off={a.off}, биом по полю "{a.bio}"')

    # --- 1. наблюдаемые смещения по биомам ---
    obs = collections.defaultdict(lambda: np.zeros((16, 16), np.int64))      # ключ: ('b', id) / ('c', класс) / ('all',)
    sgn = collections.defaultdict(lambda: np.zeros((31, 31), np.int64))
    ncells = collections.Counter(); npairs = collections.Counter()
    for seed, w in worlds:
        cmap, dmap = group_cells(w, a.bio)
        for k, cl in cmap.items():
            b = cell_biome(w, k, a.bio)
            ncells[('b', b)] += 1; ncells[('c', bclass(b))] += 1; ncells[('all',)] += 1
            for d in dmap.get(k, []):
                for c in cl:
                    dd = np.rint(d - c).astype(int)
                    for key in (('b', b), ('c', bclass(b)), ('all',)):
                        obs[key][dd[0] % 16, dd[1] % 16] += 1; npairs[key] += 1
                        if abs(dd[0]) <= 15 and abs(dd[1]) <= 15: sgn[key][dd[0] + 15, dd[1] + 15] += 1
    print('\n-- 1. наблюдаемые смещения (центр алмазной жилы) - (центр диска глины) в одной ячейке; таблица по модулю 16')
    rows = []
    for key in sorted(obs, key=lambda k: (-npairs[k])):
        n = npairs[key]
        if n < 15: continue
        T = obs[key]; fl = np.argsort(T.ravel())[::-1][:3]
        row = dict(key=str(key), cells_with_disk=ncells[key], pairs=int(n), top=[(int(k // 16), int(k % 16), round(float(T.ravel()[k]) / n, 3)) for k in fl],
                   dx_zero_frac=float(T[[0, 1, 15], :].sum() / n))
        rows.append(row)
        print(f"   {str(key):22s} пар {n:5d} (дисков-ячеек {ncells[key]:5d}); топ (dx,dz mod16, доля): " + '  '.join(f'({t[0]},{t[1]}) {t[2]:.2f}' for t in row['top']) + f";  доля |dx|<=1: {row['dx_zero_frac']:.2f}")

    result = dict(version=a.version, obs=rows)
    if a.replay:
        print('\n-- 2. воспроизведение decorationSeed: индексы фич по биомам')
        # индекс алмаза по биомам: по ВСЕМ внутренним ячейкам биома считаем долю ячеек, где в колонке (x_d-1,z_d-1) предсказанного начала жилы
        # есть алмазная руда (правильный индекс ≈ 0.6, неверный ≈ 0.01) — это снимает «псевдонимы» индексов, у которых z почти совпадает
        keymap = {}
        for si, (seed, w) in enumerate(worlds):
            dcols = np.unique(np.stack([w.dia_blocks[:, 0], w.dia_blocks[:, 2]], axis=1), axis=0)
            keymap[si] = np.sort(dcols[:, 0].astype(np.int64) * (1 << 21) + dcols[:, 1] + (1 << 20))

        def colhit(si, X, Z):
            keys = keymap[si]; kq = X.astype(np.int64) * (1 << 21) + Z.astype(np.int64) + (1 << 20)
            pos = np.searchsorted(keys, kq); pos[pos >= len(keys)] = len(keys) - 1
            return keys[pos] == kq
        cells_by_b = collections.defaultdict(list)           # b -> list of (si, kx, kz)
        for si, (seed, w) in enumerate(worlds):
            for k in w.interior:
                cells_by_b[cell_biome(w, k, a.bio)].append((si, k[0], k[1]))
        idx_d = {}
        for b, lst in sorted(cells_by_b.items(), key=lambda kv: -len(kv[1])):
            if len(lst) < a.minveins: continue
            arr = np.array(lst)
            hits = {}
            for st in range(0, 11):
                for ix in range(0, 90):
                    tot = 0
                    for si in set(arr[:, 0]):
                        m = arr[:, 0] == si
                        dec = R.decoration_seeds(worlds[si][0], arr[m, 1].astype(np.int64), arr[m, 2].astype(np.int64))
                        x, z, yy = R.feature_draws(dec, ix, st, n=3)
                        if a.order == 'xyz': z = yy
                        tot += int(colhit(si, 16 * arr[m, 1] + a.off + x - 1, 16 * arr[m, 2] + a.off + z - 1).sum())
                    hits[(st, ix)] = tot / len(arr)
            (st, ix), h = max(hits.items(), key=lambda kv: kv[1])
            idx_d[b] = (st, ix, h, len(arr))
            print(f'   биом {b:4d} ({bclass(b):7s}): ячеек {len(arr):5d}  лучший (step,index) для алмаза = ({st},{ix}), колонка (x_d-1,z_d-1) содержит алмаз в {h:.2f} ячеек')
        result['idx_diamond'] = {str(b): dict(step=v[0], index=v[1], hit=v[2], n=v[3]) for b, v in idx_d.items()}
        # глина: индексы по ТОЧНЫМ дискам (полный шаблон радиуса 2/3, центр известен точно), по биомам; затем группы по (idx_d, idx_c)
        groups = collections.defaultdict(list)
        for b, (st, ix, h, n) in idx_d.items():
            if h > 0.3: groups[(st, ix)].append(b)
        print('   группы биомов по индексу алмаза:', {k: v for k, v in groups.items()})
        result['groups'] = {f'{k[0]}:{k[1]}': v for k, v in groups.items()}
        ex_by_b = collections.defaultdict(list)           # b -> (world idx, x0, z0)
        for si, (seed, w) in enumerate(worlds):
            for j in range(len(w.cc)):
                if np.isfinite(w.cc_exact[j, 0]):
                    x0, z0 = int(w.cc_exact[j, 0]), int(w.cc_exact[j, 1])
                    k = (int(np.floor((x0 - a.off) / 16)), int(np.floor((z0 - a.off) / 16)))
                    if k in w.interior: ex_by_b[cell_biome(w, k, a.bio)].append((si, x0, z0, k[0], k[1]))
        clay_by_b = {}
        for b, lst in sorted(ex_by_b.items(), key=lambda kv: -len(kv[1])):
            arr = np.array(lst)
            if len(arr) < 4: continue
            hits = {}
            for st in range(0, 11):
                for ix in range(0, 90):
                    tot = 0
                    for sidx in set(arr[:, 0].astype(int)):
                        m = arr[:, 0] == sidx
                        dec = R.decoration_seeds(worlds[sidx][0], arr[m, 3].astype(np.int64), arr[m, 4].astype(np.int64))
                        x, z = R.feature_draws(dec, ix, st)
                        tot += int(((16 * arr[m, 3] + a.off + x == arr[m, 1]) & (16 * arr[m, 4] + a.off + z == arr[m, 2])).sum())
                    hits[(st, ix)] = tot / len(arr)
            top = sorted(hits.items(), key=lambda kv: -kv[1])[:2]
            clay_by_b[b] = (top[0][0], top[0][1], len(arr))
            print(f'   биом {b:4d} ({bclass(b):7s}): точных дисков {len(arr):4d}; индекс глины (step,index)={top[0][0]}, точных совпадений {top[0][1]:.2f}; следующий {top[1][0]} {top[1][1]:.2f}')
        result['clay_idx_by_biome'] = {str(b): dict(step=v[0][0], index=v[0][1], hit=v[1], n=v[2]) for b, v in clay_by_b.items()}
        # индекс глины для каждого биома: собственная подгонка по точным дискам; иначе — типовой (для болот — болотный)
        fit_ok = {b: v[0][1] for b, v in clay_by_b.items() if v[1] > 0.7}
        nonswamp = collections.Counter(); swampc = collections.Counter()
        for b, v in clay_by_b.items():
            if v[1] > 0.7: (swampc if b in SWAMP else nonswamp)[(v[0][0], v[0][1])] += v[2]
        default_c = nonswamp.most_common(1)[0][0] if nonswamp else None
        swamp_c = swampc.most_common(1)[0][0] if swampc else default_c
        pair_biomes = collections.defaultdict(list)
        for b, (st, ix, h, n) in idx_d.items():
            if h <= 0.3: continue
            if b in clay_by_b and clay_by_b[b][1] > 0.7: c = clay_by_b[b][0]
            else: c = swamp_c if b in SWAMP else default_c
            if c is None: continue
            pair_biomes[((st, ix), c)].append(b)
        grp_res = {}
        for (dpair, cpair), blist in pair_biomes.items():
            nd = sum(clay_by_b[b][2] for b in blist if b in clay_by_b)
            g = (dpair[0], dpair[1], cpair[1])
            grp_res[g] = dict(biomes=blist, n_disks=nd, clay=(cpair[0], cpair[1], 1.0), dia=dpair)
            print(f'   группа биомов {blist}: idx_diamond={dpair[1]}, idx_clay={cpair[1]} (step {cpair[0]}), разность clay-diamond = {cpair[1] - dpair[1]}, точных дисков для подгонки {nd}')
        result['pairs'] = [dict(biomes=v['biomes'], step=v['dia'][0], idx_diamond=v['dia'][1], idx_clay=v['clay'][1], exact_disks=v['n_disks']) for v in grp_res.values()]

        # --- предсказанные смещения и успех трюка ---
        print('\n-- 3. предсказание по ГСЧ и успех «трюка» (точный центр диска из ГСЧ; диск «существует», если наблюдаемая глина лежит в круге радиуса 3 вокруг предсказанного центра)')
        for g, gr in grp_res.items():
            (st_d, ix_d) = gr['dia']; (st_c, ix_c, hc) = gr['clay']
            pred = np.zeros((16, 16), np.int64); vtot = [0, 0, 0]
            recs = []          # (world, parity, exact_cx, exact_cz, est_cx, est_cz, dx_pred, dz_pred, dy)
            for si, (seed, w) in enumerate(worlds):
                cells = [k for k in w.interior if cell_biome(w, k, a.bio) in gr['biomes']]
                if not cells: continue
                kk = np.array(cells, dtype=np.int64)
                dec = R.decoration_seeds(seed, kk[:, 0], kk[:, 1])
                xc, zc = R.feature_draws(dec, ix_c, st_c)[:2]; xd, zd, y3 = R.feature_draws(dec, ix_d, st_d, n=3)
                if a.order == 'xyz': zd = y3
                np.add.at(pred, ((xd - xc) % 16, (zd - zc) % 16), 1)
                OX = 16 * kk[:, 0] + a.off + xd; OZ = 16 * kk[:, 1] + a.off + zd
                h4 = colhit(si, OX - 1, OZ - 1) | colhit(si, OX, OZ - 1) | colhit(si, OX - 1, OZ) | colhit(si, OX, OZ)
                h1 = colhit(si, OX - 1, OZ - 1)
                vtot[0] += int(h4.sum()); vtot[1] += len(h4); vtot[2] += int(h1.sum())
                cmap_i = collections.defaultdict(list)
                for j in range(len(w.cc)):
                    if not w.cc_wet[j]: continue
                    k = (int(np.floor((w.cc[j, 0] - a.off) / 16)), int(np.floor((w.cc[j, 1] - a.off) / 16)))
                    cmap_i[k].append(j)
                for j, k in enumerate(cells):
                    cx_abs = 16 * k[0] + a.off + int(xc[j]); cz_abs = 16 * k[1] + a.off + int(zc[j])
                    found = None
                    for ci in cmap_i.get(tuple(k), []):
                        cols = w.cc_cols[ci]
                        if len(cols) >= 3 and (np.abs(cols[:, 0] - cx_abs) <= 3).all() and (np.abs(cols[:, 1] - cz_abs) <= 3).all() and (((cols[:, 0] - cx_abs) ** 2 + (cols[:, 1] - cz_abs) ** 2) <= 9).all():
                            found = ci; break
                    if found is None: continue
                    recs.append((si, (k[0] + k[1]) & 1, cx_abs, cz_abs, int(round(w.cc[found, 0] - 0.5)), int(round(w.cc[found, 1] - 0.5)), int(xd[j] - xc[j]), int(zd[j] - zc[j])))
            nrec = len(recs)
            print(f'   группа {g} (idx_d={ix_d}, idx_c={ix_c}, d={ix_c - ix_d}; биомы {gr["biomes"]}): ячеек {int(pred.sum())}, из них с реальным диском {nrec}; проверка: в 2x2 колонок у предсказанного начала жилы алмаз есть в {vtot[0] / max(vtot[1], 1):.3f} ячеек (в колонке (x_d-1,z_d-1): {vtot[2] / max(vtot[1], 1):.3f}; база ~0.02)')
            result.setdefault('origin_check', {})[':'.join(str(v) for v in g)] = dict(cells=vtot[1], hit2x2=vtot[0] / max(vtot[1], 1))
            fl = np.argsort(pred.ravel())[::-1][:6]
            print('     предсказание ГСЧ (dx,dz mod 16) = (x_d - x_c, z_d - z_c) по всем ячейкам группы: ' + '  '.join(f'({k // 16},{k % 16}) {pred.ravel()[k] / pred.sum():.2f}' for k in fl))
            if nrec < 20: continue
            R_ = np.array(recs)
            def hitmap(cxcol, czcol):
                """bool (n,31,31): есть ли алмаз в колонке (c_x+dx, c_z+dz)"""
                H = np.zeros((len(R_), 31, 31), bool)
                for si in set(R_[:, 0].astype(int)):
                    m = np.where(R_[:, 0] == si)[0]
                    keys = keymap[si]
                    for dx in range(-15, 16):
                        for dz in range(-15, 16):
                            kq = (R_[m, cxcol] + dx).astype(np.int64) * (1 << 21) + (R_[m, czcol] + dz).astype(np.int64) + (1 << 20)
                            pos = np.searchsorted(keys, kq); pos[pos >= len(keys)] = len(keys) - 1
                            H[m, dx + 15, dz + 15] = keys[pos] == kq
                return H
            tr = R_[:, 1] == 0; te = ~tr
            res_g = {}
            for label, cxc, czc in (('точный центр (из ГСЧ)', 2, 3), ('оценка центра по блокам глины', 4, 5)):
                H = hitmap(cxc, czc)
                H2 = H[:, :-1, :-1] | H[:, 1:, :-1] | H[:, :-1, 1:] | H[:, 1:, 1:]            # любая из колонок 2x2 с якорем (dx,dz)
                out = {}
                for nm, HH in (('1 колонка', H), ('2x2', H2)):
                    mt = HH[tr].mean(axis=0); i = np.unravel_index(np.argmax(mt), mt.shape)
                    out[nm] = dict(best=(int(i[0] - 15), int(i[1] - 15)), train=float(mt[i]), test=float(HH[te][:, i[0], i[1]].mean()), base=float(HH.mean()))
                res_g[label] = out
                print(f'     [{label}] ' + '; '.join(f"{nm}: лучшее смещение по обучению (dx,dz)={o['best']}, успех обуч. {o['train']:.3f} ({int(tr.sum())}), тест {o['test']:.3f} ({int(te.sum())}), база {o['base']:.4f}" for nm, o in out.items()))
            allp = hitmap(2, 3).mean(axis=0)
            top5 = np.argsort(allp.ravel())[::-1][:6]
            print('     топ смещений (dx,dz) от точного центра: успех по всем дискам: ' + '  '.join(f'({k // 31 - 15},{k % 31 - 15}) {allp.ravel()[k]:.2f}' for k in top5))
            result.setdefault('trick', {})[':'.join(str(v) for v in g)] = dict(biomes=gr['biomes'], idx_d=ix_d, idx_c=ix_c, disks=nrec, results=res_g,
                                                                 pred_top=[(int(k // 16), int(k % 16), float(pred.ravel()[k] / pred.sum())) for k in fl],
                                                                 top=[(int(k // 31 - 15), int(k % 31 - 15), float(allp.ravel()[k])) for k in top5])
    if a.json:
        json.dump(result, open(a.json, 'w'), ensure_ascii=False, indent=1, default=str)


if __name__ == '__main__':
    main()

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
    ap.add_argument('--replay', action='store_true'); ap.add_argument('--json'); ap.add_argument('--minveins', type=int, default=40)
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
        # диамантовые жилы в чанках: ячейки off=0
        byb = collections.defaultdict(lambda: ([], [], [], [], [], []))      # b -> (seedidx, cx, cz, fx, fz, y)
        for si, (seed, w) in enumerate(worlds):
            kd = w.cell(w.dc[:, :2])
            for i in range(len(w.dc)):
                k = (int(kd[i, 0]), int(kd[i, 1]))
                if k not in w.interior: continue
                b = cell_biome(w, k, a.bio)
                t = byb[b]; t[0].append(si); t[1].append(k[0]); t[2].append(k[1]); t[3].append(w.dc[i, 0] - 16 * k[0] - a.off); t[4].append(w.dc[i, 1] - 16 * k[1] - a.off); t[5].append(w.dc[i, 2])
        idx_d = {}
        for b, t in sorted(byb.items(), key=lambda kv: -len(kv[1][0])):
            if len(t[0]) < a.minveins: continue
            si = np.array(t[0]); best = (0, -1, -1)
            # по мирам раздельно (seed разные): суммируем попадания
            hits = {}
            for st in range(0, 11):
                for ix in range(0, 90):
                    tot = 0; n = 0
                    for s in set(si):
                        m = si == s
                        dec = R.decoration_seeds(worlds[s][0], np.array(t[1])[m], np.array(t[2])[m])
                        x, z, yy = R.feature_draws(dec, ix, st, n=3)
                        fx = np.array(t[3])[m]; fz = np.array(t[4])[m]; fy = np.array(t[5])[m]
                        dx = np.abs(((fx - (x + 0.5) + 8) % 16) - 8); dz = np.abs(((fz - (z + 0.5) + 8) % 16) - 8)
                        tot += int(((dx <= 2) & (dz <= 2) & (np.abs(fy - yy) <= 2.5)).sum()); n += int(m.sum())
                    hits[(st, ix)] = tot / n
            (st, ix), h = max(hits.items(), key=lambda kv: kv[1])
            idx_d[b] = (st, ix, h, len(t[0]))
            print(f'   биом {b:4d} ({bclass(b):7s}): жил {len(t[0]):5d}  лучший (step,index) для алмаза = ({st},{ix}), совпадений {h:.2f}')
        result['idx_diamond'] = {str(b): dict(step=v[0], index=v[1], hit=v[2], n=v[3]) for b, v in idx_d.items()}
        # глина: индексы по ТОЧНЫМ дискам (полный шаблон радиуса 2/3, центр известен точно), по биомам; затем группы по (idx_d, idx_c)
        groups = collections.defaultdict(list)
        for b, (st, ix, h, n) in idx_d.items():
            if h > 0.5: groups[(st, ix)].append(b)
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
        grp_res = {}
        for g, blist in groups.items():
            votes = collections.Counter()
            for b in blist:
                if b in clay_by_b and clay_by_b[b][1] > 0.7: votes[clay_by_b[b][0]] += clay_by_b[b][2]
            if not votes:
                print(f'   группа {g}: нет надёжного индекса глины'); continue
            (cst, cix), nv = votes.most_common(1)[0]
            grp_res[g] = dict(biomes=blist, n_disks=nv, clay=(cst, cix, 1.0), dia=g)
            print(f'   группа алмаза {g}: индекс глины (step,index) = ({cst},{cix}) по {nv} точным дискам; разность индексов clay-diamond = {cix - g[1]}')
        result['clay_idx'] = {f'{k[0]}:{k[1]}': dict(biomes=v['biomes'], n_disks=v['n_disks'], clay_step=v['clay'][0], clay_index=v['clay'][1]) for k, v in grp_res.items()}
        # отдельно: биомы вне групп с точными дисками (напр. болото) — собственные пары индексов
        for b, (st, ix, h, n) in idx_d.items():
            if b in clay_by_b and clay_by_b[b][1] > 0.7 and not any(b in v['biomes'] and v['clay'][1] == clay_by_b[b][0][1] for v in grp_res.values()):
                g = (st, ix)
                grp_res[(g[0], g[1], 'b', b)] = dict(biomes=[b], n_disks=clay_by_b[b][2], clay=(clay_by_b[b][0][0], clay_by_b[b][0][1], 1.0), dia=g)

        # --- предсказанные смещения и успех трюка ---
        print('\n-- 3. предсказание по ГСЧ и успех «трюка» (точный центр диска из ГСЧ; диск «существует», если наблюдаемый центр глины в 3 блоках от предсказанного)')
        for g, gr in grp_res.items():
            (st_d, ix_d) = gr['dia']; (st_c, ix_c, hc) = gr['clay']
            if hc < 0.4:
                print(f'   группа {g}: индекс глины не определён надёжно (совпадений {hc:.2f}) — пропуск'); continue
            pred = np.zeros((16, 16), np.int64)
            recs = []                                                    # (parity, dx_c, dz_c abs) для успеха
            succ_cols = []
            for si, (seed, w) in enumerate(worlds):
                cells = [k for k in w.interior if cell_biome(w, k, a.bio) in gr['biomes']]
                if not cells: continue
                kk = np.array(cells, dtype=np.int64)
                dec = R.decoration_seeds(seed, kk[:, 0], kk[:, 1])
                xc, zc = R.feature_draws(dec, ix_c, st_c); xd, zd = R.feature_draws(dec, ix_d, st_d)
                np.add.at(pred, ((xd - xc) % 16, (zd - zc) % 16), 1)
                # существование диска: наблюдаемый центр глины в пределах 3 блоков от предсказанного
                cmap, dmap = group_cells(w, a.bio)
                dcols = np.unique(np.stack([w.dia_blocks[:, 0], w.dia_blocks[:, 2]], axis=1), axis=0)
                keys = np.sort(dcols[:, 0].astype(np.int64) * (1 << 21) + dcols[:, 1] + (1 << 20))
                for j, k in enumerate(cells):
                    cx_abs = 16 * k[0] + a.off + xc[j]; cz_abs = 16 * k[1] + a.off + zc[j]
                    ok = False
                    for c in cmap.get(tuple(k), []):
                        if abs(c[0] - (cx_abs + 0.5)) <= 3 and abs(c[1] - (cz_abs + 0.5)) <= 3: ok = True; break
                    if not ok: continue
                    recs.append((si, (k[0] + k[1]) & 1, cx_abs, cz_abs, 16 * k[0] + a.off, 16 * k[1] + a.off, int(xd[j] - xc[j]), int(zd[j] - zc[j])))
                succ_cols.append((si, keys))
            nrec = len(recs)
            print(f'   группа {g} (idx_d={ix_d}, idx_c={ix_c}, d={ix_c - ix_d}; биомы {gr["biomes"]}): ячеек {int(pred.sum())}, из них с реальным диском {nrec}')
            fl = np.argsort(pred.ravel())[::-1][:6]
            print('     предсказание ГСЧ (dx,dz mod 16) = (x_d - x_c, z_d - z_c) по всем ячейкам группы: ' + '  '.join(f'({k // 16},{k % 16}) {pred.ravel()[k] / pred.sum():.2f}' for k in fl))
            if nrec < 20: continue
            # успех: колонка (x_c+dx, z_c+dz)
            kmap = {si: keys for si, keys in succ_cols}
            R_ = np.array(recs)
            S = {0: np.zeros((31, 31)), 1: np.zeros((31, 31))}; Nn = {0: 0, 1: 0}
            for par in (0, 1):
                sel = R_[R_[:, 1] == par]; Nn[par] = len(sel)
                for si in set(sel[:, 0].astype(int)):
                    ss = sel[sel[:, 0] == si]
                    keys = kmap[si]
                    for dx in range(-15, 16):
                        for dz in range(-15, 16):
                            kq = (ss[:, 2] + dx).astype(np.int64) * (1 << 21) + (ss[:, 3] + dz).astype(np.int64) + (1 << 20)
                            pos = np.searchsorted(keys, kq)
                            pos[pos >= len(keys)] = len(keys) - 1
                            S[par][dx + 15, dz + 15] += int((keys[pos] == kq).sum())
            tr = S[0] / max(Nn[0], 1); te = S[1] / max(Nn[1], 1)
            i = np.unravel_index(np.argmax(tr), tr.shape)
            base = float(np.mean(te))
            print(f'     лучшее смещение по обучению (чётные cx+cz, {Nn[0]} дисков): (dx,dz)=({i[0] - 15},{i[1] - 15}), успех {tr[i]:.3f}; на тесте (нечётные, {Nn[1]} дисков): {te[i]:.3f}; база (среднее по окну 31x31): {base:.4f}')
            allp = (S[0] + S[1]) / max(Nn[0] + Nn[1], 1)
            top5 = np.argsort(allp.ravel())[::-1][:6]
            print('     топ смещений (dx,dz): успех по всем дискам: ' + '  '.join(f'({k // 31 - 15},{k % 31 - 15}) {allp.ravel()[k]:.2f}' for k in top5))
            result.setdefault('trick', {})[':'.join(str(v) for v in g)] = dict(biomes=gr['biomes'], disks=nrec, best=(int(i[0] - 15), int(i[1] - 15)), train=float(tr[i]), test=float(te[i]), base=base,
                                                                 pred_top=[(int(k // 16), int(k % 16), float(pred.ravel()[k] / pred.sum())) for k in fl],
                                                                 top=[(int(k // 31 - 15), int(k % 31 - 15), float(allp.ravel()[k])) for k in top5])
    if a.json:
        json.dump(result, open(a.json, 'w'), ensure_ascii=False, indent=1, default=str)


if __name__ == '__main__':
    main()

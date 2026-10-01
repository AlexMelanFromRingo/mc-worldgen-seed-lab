#!/usr/bin/env python3
"""«Глина -> алмаз по смещению» на нескольких мирах одной версии: центры месторождений, окна смещений, внутричанковые таблицы.

  tools/l3_pairs.py <версия> [--off 0|8] [--k 48] [--json out.json] data/l3v/<версия>-w*.npz

Вход: .npz из tools/l3_anvil_legacy.py extract (clay: x,y,z,wet; diamond: x,y,z; chunks: cx,cz,full).
Нулевое распределение («контроль») строится РЕЛОКАЦИЕЙ: каждый центр глины переносится на вектор, кратный 16, в случайную ДРУГУЮ внутреннюю
ячейку (расстояние >= 3 ячеек) — сохраняется положение внутри чанка, локальная плотность алмазов и распределение по y; ломается только
связь «этот чанк — этот алмаз». Это исправляет ловушку первого варианта (циклический сдвиг за границу области).
Три теста:
  (A) окно смещений (dx,dz) in [-16,16]^2 между центрами глины и центрами жил алмаза: макс |z| по ячейкам против максимумов в контролях;
  (B) таблица 16x16 (dx mod 16, dz mod 16) для пар ВНУТРИ одной ячейки популяции: хи-квадрат против контролей;
  (C) блочное окно как в l3_clay_diamond.py: dx,dz in [-16,16], dy in [-140,-20], случайные блоки глины.
"""
import sys, os, json, argparse
import numpy as np
from scipy.spatial import cKDTree
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

W = 16                      # половина окна
NB = 2 * W + 1


def load(path):
    z = np.load(path)
    return z['clay'], z['diamond'], z['chunks']


def comps(pts, r=1.75):
    """компоненты связности 26-соседством -> (labels, ncomp)"""
    n = len(pts)
    if n == 0: return np.zeros(0, int), 0
    t = cKDTree(pts)
    pr = t.query_pairs(r, output_type='ndarray')
    g = coo_matrix((np.ones(len(pr), np.int8), (pr[:, 0], pr[:, 1])), shape=(n, n))
    nc, lab = connected_components(g, directed=False)
    return lab, nc


def summarize(pts, lab, nc, wet=None):
    cnt = np.bincount(lab, minlength=nc)
    out = {}
    for ax, nm in ((0, 'x'), (1, 'y'), (2, 'z')):
        out['mean' + nm] = np.bincount(lab, weights=pts[:, ax], minlength=nc) / cnt
        mn = np.full(nc, 1 << 30); mx = np.full(nc, -(1 << 30))
        np.minimum.at(mn, lab, pts[:, ax]); np.maximum.at(mx, lab, pts[:, ax])
        out['ext' + nm] = mx - mn + 1
    out['n'] = cnt
    if wet is not None: out['wet'] = np.bincount(lab, weights=wet, minlength=nc)
    return out


TEMPL = {r: np.array([(dx, dz) for dx in range(-r, r + 1) for dz in range(-r, r + 1) if dx * dx + dz * dz <= r * r]) for r in (2, 3)}


def exact_disk(cols):
    """cols: (k,2) уникальные колонки глины. Если это ПОЛНЫЙ диск радиуса 2 или 3 (шаблон dx^2+dz^2<=r^2) -> (x0,z0,r), иначе None"""
    k = len(cols)
    r = {13: 2, 29: 3}.get(k)
    if r is None: return None
    m = cols.mean(axis=0)
    c = np.rint(m).astype(int)
    if np.abs(m - c).max() > 1e-9: return None
    exp = {(int(c[0] + a), int(c[1] + b)) for a, b in TEMPL[r]}
    return (int(c[0]), int(c[1]), r) if {(int(a), int(b)) for a, b in cols} == exp else None


def clay_centers(clay, want_cols=False):
    """центры «дисков глины»: компоненты глины (y>30) с габаритом <= 7x7 по xz и <= 4 по y (диск радиуса 2–3, высота 3).
    -> (centers (n,3: x,z,y), wet_flags[, список колонок, точные диски (n,3) или NaN])"""
    if len(clay) == 0:
        return (np.zeros((0, 3)), np.zeros(0, bool)) + ((([], np.zeros((0, 3))),) if want_cols else ())
    lab, nc = comps(clay[:, :3].astype(float))
    s = summarize(clay[:, :3], lab, nc, clay[:, 3].astype(float))
    keep = (s['extx'] <= 7) & (s['extz'] <= 7) & (s['exty'] <= 4) & (s['n'] >= 3)
    c = np.stack([s['meanx'], s['meanz'], s['meany']], axis=1)[keep]
    wet = (s['wet'][keep] >= 1)
    if not want_cols: return c, wet
    keep_ids = np.where(keep)[0]
    order = np.argsort(lab, kind='stable'); lab_sorted = lab[order]
    starts = np.searchsorted(lab_sorted, keep_ids, 'left'); ends = np.searchsorted(lab_sorted, keep_ids, 'right')
    cols_l = []; ex = np.full((len(keep_ids), 3), np.nan)
    for j, (a, b) in enumerate(zip(starts, ends)):
        pts = clay[order[a:b]][:, [0, 2]]
        cols = np.unique(pts, axis=0)
        cols_l.append(cols)
        e = exact_disk(cols)
        if e: ex[j] = e
    return c, wet, cols_l, ex


def dia_centers(dia):
    if len(dia) == 0: return np.zeros((0, 3)), np.zeros(0)
    lab, nc = comps(dia.astype(float))
    s = summarize(dia, lab, nc)
    return np.stack([s['meanx'], s['meanz'], s['meany']], axis=1), s['n']


class World:
    def __init__(self, path, off):
        clay, dia, ch = load(path)
        self.path = path; self.off = off
        ok = {(int(r[0]), int(r[1])) for r in ch if r[2]}
        self.bio = {(int(r[0]), int(r[1])): (int(r[3]), int(r[4])) for r in ch if len(r) >= 5}
        self.nfull = len(ok)
        # внутренние ячейки (kx,kz): все чанки kx-1..kx+2 x kz-1..kz+2 имеют статус full
        xs = [c[0] for c in ok]; zs = [c[1] for c in ok]
        self.interior = set()
        if ok:
            for (a, b) in ok:
                if all((a + i, b + j) in ok for i in range(-1, 3) for j in range(-1, 3)):
                    self.interior.add((a, b))
        self.clay_blocks = clay; self.dia_blocks = dia
        self.cc, self.cc_wet, self.cc_cols, self.cc_exact = clay_centers(clay, True)
        self.dc, self.dn = dia_centers(dia)
        self.ncells_int = len(self.interior)

    def cell(self, xz):
        return (np.floor((xz - self.off) / 16)).astype(np.int64)


def pick_other(rng, cells_arr, k, mind=3):
    """случайные другие ячейки для каждой исходной: cells_arr (M,2), k (N,2) -> (N,2)"""
    out = np.empty_like(k)
    for i in range(len(k)):
        while True:
            c = cells_arr[rng.integers(len(cells_arr))]
            if max(abs(c[0] - k[i, 0]), abs(c[1] - k[i, 1])) >= mind: break
        out[i] = c
    return out


def window_hist(src_xz, dtree, dxz):
    """гистограмма смещений (диапазон [-W,W]^2) между src (N,2) и алмазами dxz (M,2) в дереве dtree"""
    H = np.zeros((NB, NB), np.int64)
    if len(src_xz) == 0 or len(dxz) == 0: return H
    idx = dtree.query_ball_point(src_xz, r=W + 0.5, p=np.inf)
    n = np.fromiter((len(i) for i in idx), int, len(idx))
    if n.sum() == 0: return H
    flat = np.concatenate([np.asarray(i, int) for i in idx if len(i)])
    srcrep = np.repeat(src_xz, n, axis=0)
    off = np.rint(dxz[flat] - srcrep).astype(np.int64)
    m = (np.abs(off[:, 0]) <= W) & (np.abs(off[:, 1]) <= W)
    off = off[m]
    return np.bincount((off[:, 0] + W) * NB + off[:, 1] + W, minlength=NB * NB).reshape(NB, NB)


def same_cell_table(w, cc_xz, shift_cells, dc_cell_map):
    """таблица 16x16 (dx mod 16, dz mod 16) для пар (глина, алмаз) в одной ячейке; shift_cells (N,2) — перенос глины на другую ячейку или None"""
    T = np.zeros((16, 16), np.int64)
    kc = w.cell(cc_xz)
    for i in range(len(cc_xz)):
        k = tuple(kc[i]) if shift_cells is None else tuple(shift_cells[i])
        lst = dc_cell_map.get(k)
        if lst is None: continue
        c = cc_xz[i] + (0 if shift_cells is None else 16 * (np.array(k) - kc[i]))
        d = np.rint(lst - c).astype(np.int64) % 16
        np.add.at(T, (d[:, 0], d[:, 1]), 1)
    return T


def zmap(obs, mu):
    return (obs - mu) / np.sqrt(np.maximum(mu, 1.0))


def analyze(paths, off, K, seed=7, blocks_sample=6000):
    rng = np.random.default_rng(seed)
    worlds = [World(p, off) for p in paths]
    res = {'worlds': len(worlds), 'chunks_full': sum(w.nfull for w in worlds), 'interior_cells': sum(w.ncells_int for w in worlds),
           'clay_blocks': int(sum(len(w.clay_blocks) for w in worlds)), 'clay_blocks_wet': int(sum((w.clay_blocks[:, 3] == 1).sum() for w in worlds)),
           'dia_blocks': int(sum(len(w.dia_blocks) for w in worlds)), 'off': off}
    out_sets = {}
    for setname in ('all', 'wet'):
        HA = np.zeros((NB, NB), np.int64); HAk = np.zeros((K, NB, NB), np.int64)
        TB = np.zeros((16, 16), np.int64); TBk = np.zeros((K, 16, 16), np.int64)
        HC = np.zeros((NB, NB), np.int64); HCk = np.zeros((K, NB, NB), np.int64)
        nclay = ndia_int = 0; ncl_int = 0
        for w in worlds:
            sel = np.ones(len(w.cc), bool) if setname == 'all' else w.cc_wet
            cc = w.cc[sel][:, :2]
            kc = w.cell(cc)
            interior = w.interior
            ink = np.array([tuple(k) in interior for k in kc], bool) if len(kc) else np.zeros(0, bool)
            cc = cc[ink]; kc = kc[ink]
            nclay += len(cc)
            # алмазные центры внутри внутренних ячеек
            dxz = w.dc[:, :2]; dk = w.cell(dxz)
            dmap = {}
            for i in range(len(dxz)):
                dmap.setdefault((int(dk[i, 0]), int(dk[i, 1])), []).append(dxz[i])
            dmap = {k: np.array(v) for k, v in dmap.items()}
            ndia_int += sum(len(v) for k, v in dmap.items() if k in interior)
            dtree = cKDTree(dxz) if len(dxz) else None
            if len(cc) and dtree is not None:
                HA += window_hist(cc, dtree, dxz)
                TB += same_cell_table(w, cc, None, dmap)
            cells = np.array(sorted(interior)) if interior else np.zeros((0, 2), int)
            for j in range(K):
                if len(cc) == 0 or len(cells) < 10 or dtree is None: continue
                k2 = pick_other(rng, cells, kc)
                cs = cc + 16 * (k2 - kc)
                HAk[j] += window_hist(cs, dtree, dxz)
                TBk[j] += same_cell_table(w, cc, k2, dmap)
            # (C) блочный тест: случайные ДИСКИ глины (все их блоки, компоненты связности) в окне, dy in [-140,-20]; контроль переносит диск целиком
            cb = w.clay_blocks
            if setname == 'wet': cb = cb[cb[:, 3] == 1]
            if len(cb) and len(w.dia_blocks):
                cbk = w.cell(cb[:, [0, 2]].astype(float))
                inb = np.array([tuple(k) in interior for k in cbk], bool)
                cb = cb[inb]; cbk = cbk[inb]
                if len(cb) == 0: continue
                lab, nc = comps(cb[:, :3].astype(float))
                order = rng.permutation(nc)
                sizes = np.bincount(lab, minlength=nc)
                cum = np.cumsum(sizes[order]); take = order[:max(1, int(np.searchsorted(cum, blocks_sample)) + 1)]
                sel = np.isin(lab, take)
                cb = cb[sel]; cbk = cbk[sel]; lab = lab[sel]
                ncl_int += len(cb)
                db = w.dia_blocks
                dxz = db[:, [0, 2]].astype(float)
                tr = cKDTree(dxz)

                def bh(src):
                    idx = tr.query_ball_point(src[:, [0, 2]].astype(float), r=W + 0.5, p=np.inf)
                    n = np.fromiter((len(i) for i in idx), int, len(idx))
                    H = np.zeros((NB, NB), np.int64)
                    if n.sum() == 0: return H
                    flat = np.concatenate([np.asarray(i, int) for i in idx if len(i)])
                    q = db[flat] - np.repeat(src[:, :3], n, axis=0)
                    m = (np.abs(q[:, 0]) <= W) & (np.abs(q[:, 2]) <= W) & (q[:, 1] >= -140) & (q[:, 1] <= -20)
                    q = q[m]
                    return np.bincount((q[:, 0] + W) * NB + q[:, 2] + W, minlength=NB * NB).reshape(NB, NB)
                HC += bh(cb)
                cells = np.array(sorted(interior))
                # ссылочная ячейка компоненты — ячейка её первого блока
                first = {}
                for i in range(len(lab)): first.setdefault(int(lab[i]), i)
                labs = np.array(sorted(first)); refk = np.array([cbk[first[int(l)]] for l in labs])
                pos_of = {int(l): j for j, l in enumerate(labs)}
                li = np.array([pos_of[int(l)] for l in lab])
                for j in range(min(K, 24)):
                    k2 = pick_other(rng, cells, refk)
                    shift = 16 * (k2 - refk)
                    cs = cb.copy(); cs[:, 0] += shift[li, 0]; cs[:, 2] += shift[li, 1]
                    HCk[j] += bh(cs)
        out_sets[setname] = dict(nclay=nclay, ndia=ndia_int, n_clay_blocks_used=ncl_int, HA=HA, HAk=HAk, TB=TB, TBk=TBk, HC=HC, HCk=HCk)
    return res, out_sets


def stats(Hobs, Hk, ncontrols):
    """-> словарь: сумма, контроль±σ, макс|z| (набл., контроли), лучшая ячейка"""
    Hk = Hk[:ncontrols]
    if Hk.sum() == 0: return None
    mu = Hk.mean(axis=0)
    tot = Hk.reshape(len(Hk), -1).sum(axis=1)
    z = zmap(Hobs, mu)
    # контроль: каждый против среднего остальных
    mz = []
    for k in range(len(Hk)):
        mu_o = (Hk.sum(axis=0) - Hk[k]) / (len(Hk) - 1)
        mz.append(np.max(np.abs(zmap(Hk[k], mu_o))))
    i = np.unravel_index(np.argmax(np.abs(z)), z.shape)
    return dict(obs=int(Hobs.sum()), ctrl_mean=float(tot.mean()), ctrl_sd=float(tot.std()), maxz_obs=float(np.abs(z).max()),
                maxz_ctrl_mean=float(np.mean(mz)), maxz_ctrl_max=float(np.max(mz)), best_cell=(int(i[0] - (Hobs.shape[0] // 2)), int(i[1] - (Hobs.shape[1] // 2))),
                best_obs=int(Hobs[i]), best_exp=float(mu[i]))


def chi2_stats(Tobs, Tk):
    mu = Tk.mean(axis=0)
    mu = np.maximum(mu, 0.5)
    def chi(T, m): return float(((T - m) ** 2 / m).sum())
    c_obs = chi(Tobs, mu)
    c_ctrl = []
    for k in range(len(Tk)):
        mo = np.maximum((Tk.sum(axis=0) - Tk[k]) / (len(Tk) - 1), 0.5)
        c_ctrl.append(chi(Tk[k], mo))
    c_ctrl = np.array(c_ctrl)
    z = zmap(Tobs, mu)
    i = np.unravel_index(np.argmax(np.abs(z)), z.shape)
    p_rank = float((np.sum(c_ctrl >= c_obs) + 1) / (len(c_ctrl) + 1))
    return dict(pairs=int(Tobs.sum()), ctrl_pairs=float(Tk.reshape(len(Tk), -1).sum(axis=1).mean()), chi2_obs=c_obs, chi2_ctrl_mean=float(c_ctrl.mean()),
                chi2_ctrl_sd=float(c_ctrl.std()), chi2_z=float((c_obs - c_ctrl.mean()) / max(c_ctrl.std(), 1e-9)), p_rank=p_rank,
                maxz_obs=float(np.abs(z).max()), maxz_ctrl_mean=float(np.mean([np.abs(zmap(Tk[k], np.maximum((Tk.sum(axis=0) - Tk[k]) / (len(Tk) - 1), 0.5))).max() for k in range(len(Tk))])),
                best_cell=(int(i[0]), int(i[1])), best_obs=int(Tobs[i]), best_exp=float(mu[i]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('version'); ap.add_argument('files', nargs='+')
    ap.add_argument('--off', type=int, default=0); ap.add_argument('--k', type=int, default=48)
    ap.add_argument('--json'); ap.add_argument('--blocks', type=int, default=6000)
    a = ap.parse_args()
    res, sets = analyze(a.files, a.off, a.k, blocks_sample=a.blocks)
    out = dict(version=a.version, **res, sets={})
    print(f"== {a.version}: миров {res['worlds']}, чанков full {res['chunks_full']}, внутр. ячеек {res['interior_cells']}, блоков глины y>30: {res['clay_blocks']} (над водой {res['clay_blocks_wet']}), блоков алмаза {res['dia_blocks']}, off={a.off}")
    for nm, s in sets.items():
        A_ = stats(s['HA'], s['HAk'], a.k); B_ = chi2_stats(s['TB'], s['TBk']) if s['TBk'].sum() else None
        C_ = stats(s['HC'], s['HCk'], min(a.k, 24))
        print(f"-- набор глины '{nm}': центров глины {s['nclay']}, центров жил алмаза во внутр. ячейках {s['ndia']}")
        if A_: print(f"   (A) окно ±16 центры: пар {A_['obs']} против контроля {A_['ctrl_mean']:.0f} ± {A_['ctrl_sd']:.0f}; макс|z| {A_['maxz_obs']:.2f} (контроли: среднее {A_['maxz_ctrl_mean']:.2f}, макс {A_['maxz_ctrl_max']:.2f}); лучшая ячейка {A_['best_cell']}: {A_['best_obs']} против {A_['best_exp']:.1f}")
        if B_: print(f"   (B) внутри ячейки mod16: пар {B_['pairs']} (контроль {B_['ctrl_pairs']:.0f}); chi2 {B_['chi2_obs']:.0f} против {B_['chi2_ctrl_mean']:.0f} ± {B_['chi2_ctrl_sd']:.0f} (z={B_['chi2_z']:.2f}, p_rank={B_['p_rank']:.3f}); макс|z| {B_['maxz_obs']:.2f} (контроли {B_['maxz_ctrl_mean']:.2f}); лучшая {B_['best_cell']}: {B_['best_obs']} против {B_['best_exp']:.1f}")
        if C_: print(f"   (C) блочное окно: пар {C_['obs']} против контроля {C_['ctrl_mean']:.0f} ± {C_['ctrl_sd']:.0f}; макс|z| {C_['maxz_obs']:.2f} (контроли: среднее {C_['maxz_ctrl_mean']:.2f}, макс {C_['maxz_ctrl_max']:.2f}); блоков глины использовано {s['n_clay_blocks_used']}")
        out['sets'][nm] = dict(nclay=s['nclay'], ndia=s['ndia'], A=A_, B=B_, C=C_)
    if a.json:
        json.dump(out, open(a.json, 'w'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()


def power_fraction(Tobs, Tk, cell=(0, 3), target_z=5.0, seed=1):
    """минимальная доля f пар таблицы B, «сдвинутых» в одну ячейку cell (инъекция искусственной связи), при которой chi2_z > target_z.
    Показывает чувствительность теста для данной версии (если связь реально есть с долей >= f, она была бы замечена)."""
    rng = np.random.default_rng(seed)
    n = int(Tobs.sum())
    if n < 50: return None
    base = Tk.mean(axis=0).astype(float).ravel(); p = base / base.sum()
    for f in np.concatenate([np.arange(0.002, 0.05, 0.002), np.arange(0.05, 0.5, 0.02)]):
        zs = []
        for _ in range(5):
            T = np.zeros(256, np.int64)
            m = int(round(f * n))
            T += rng.multinomial(n - m, p)
            T[cell[0] * 16 + cell[1]] += m
            zs.append(chi2_stats(T.reshape(16, 16), Tk)['chi2_z'])
        if np.mean(zs) > target_z: return float(f)
    return None

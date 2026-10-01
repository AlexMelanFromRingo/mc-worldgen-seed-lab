#!/usr/bin/env python3
"""Успех «трюка» по мирам с разными seed & 15 (1.15.2 – 1.17.1): для каждого мира и класса биома (обычный / болото):
  m = мода Δz = (z_d - z_c) mod 16 по ВСЕМ ячейкам класса (то, что игрок найдёт калибровкой: «отойди на m блоков к югу»; m в диапазоне -8..7);
  успех правил на реальных дисках (диск «существует», если наблюдаемая глина в круге радиуса 3 вокруг предсказанного центра):
    R1 одна колонка (x_c-1, z_c+m-1);  R2 две колонки (x_c-1..x_c, z_c+m-1);  R3 квадрат 2x2 (x_c-1..x_c, z_c+m-1..z_c+m)
  — для ТОЧНОГО центра (из ГСЧ) и для ОЦЕНКИ центра по блокам глины (центроид).
  tools/l3_s0_table.py <версия> data/l3v/<версия>-w*.npz
"""
import sys, os, re, json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_pairs as P, l3_replay as R, l3_trick as T

SWAMP = {6, 134}


def main():
    ver = sys.argv[1]; files = sys.argv[2:]
    p = T.PARAMS[ver]
    rows = []
    for f in files:
        seed = int(re.search(r'-w(-?\d+)\.npz$', f).group(1))
        w = P.World(f, 0)
        dcols = np.unique(np.stack([w.dia_blocks[:, 0], w.dia_blocks[:, 2]], axis=1), axis=0)
        keys = np.sort(dcols[:, 0].astype(np.int64) * (1 << 21) + dcols[:, 1] + (1 << 20))

        def hit(X, Z):
            kq = np.asarray(X, np.int64) * (1 << 21) + np.asarray(Z, np.int64) + (1 << 20)
            pos = np.searchsorted(keys, kq); pos[pos >= len(keys)] = len(keys) - 1
            return keys[pos] == kq
        cells = sorted(w.interior)
        cmap = {}
        for j in range(len(w.cc)):
            if w.cc_wet[j]:
                cmap.setdefault((int(np.floor(w.cc[j, 0] / 16)), int(np.floor(w.cc[j, 1] / 16))), []).append(j)
        for cls in ('regular', 'swamp'):
            sel = [k for k in cells if k in w.bio and ((w.bio[k][0] in SWAMP) == (cls == 'swamp'))]
            if cls == 'regular': sel = [k for k in sel if w.bio[k][0] not in {3, 18, 19, 131, 133} or p['dia_by_biome'] == {}]
            if len(sel) < 20: continue
            kk = np.array(sel)
            ic = p['clay_swamp'] if cls == 'swamp' else p['clay']
            idd_arr = np.array([p['dia_by_biome'].get(w.bio[k][0], p['dia_default']) for k in sel])
            dec = R.decoration_seeds(seed, kk[:, 0], kk[:, 1])
            xc, zc = R.feature_draws(dec, ic, p['step'])[:2]
            xd = np.zeros(len(sel), np.int64); zd = np.zeros(len(sel), np.int64)
            for idv in set(idd_arr):
                m = idd_arr == idv
                d = R.feature_draws(dec[m], int(idv), p['step'], n=3)
                xd[m] = d[0]; zd[m] = d[1] if p['order'] == 'xzy' else d[2]
            dz = (zd - zc) % 16
            mode = int(np.bincount(dz, minlength=16).argmax()); mode_share = float(np.mean(dz == mode))
            m_signed = mode - 16 if mode > 8 else mode
            cx_abs = 16 * kk[:, 0] + xc; cz_abs = 16 * kk[:, 1] + zc
            rec = []
            for i, k in enumerate(sel):
                for ci in cmap.get(k, []):
                    cols = w.cc_cols[ci]
                    if len(cols) >= 3 and (((cols[:, 0] - cx_abs[i]) ** 2 + (cols[:, 1] - cz_abs[i]) ** 2) <= 9).all():
                        rec.append((i, int(round(w.cc[ci, 0] - 0.5)), int(round(w.cc[ci, 1] - 0.5)))); break
            if len(rec) < 10: continue
            rec = np.array(rec); i = rec[:, 0]
            res = {}
            for lab, X, Z in (('точный', cx_abs[i], cz_abs[i]), ('оценка', rec[:, 1], rec[:, 2])):
                r1 = hit(X - 1, Z + m_signed - 1)
                r2 = r1 | hit(X, Z + m_signed - 1)
                r3 = r2 | hit(X - 1, Z + m_signed) | hit(X, Z + m_signed)
                # с учётом границы чанка: цель z_t = z_c_local + мода (mod 16) внутри того же чанка (игрок видит chunk-координаты по F3)
                kz16 = 16 * np.floor(Z / 16).astype(np.int64) if lab == 'точный' else None
                if kz16 is not None:
                    zt = kz16 + ((Z - kz16 + mode) % 16)
                    c1 = hit(X - 1, zt - 1); c3 = c1 | hit(X, zt - 1) | hit(X - 1, zt) | hit(X, zt)
                    res['чанк-учёт'] = (float(c1.mean()), float(c3.mean()))
                res[lab] = (float(r1.mean()), float(r2.mean()), float(r3.mean()))
            row = dict(world=seed, s0=seed & 15, cls=cls, disks=int(len(rec)), cells=len(sel), mode=m_signed, mode_share=mode_share, res=res)
            rows.append(row)
            print(f"{ver} w{seed} s0={seed & 15:2d} {cls:7s}: дисков {len(rec):4d}; мода dz={m_signed:+d} ({mode_share:.2f}); точный центр R1/R2/R3 = "
                  f"{res['точный'][0]:.2f}/{res['точный'][1]:.2f}/{res['точный'][2]:.2f}; с учётом границы чанка (R1/R3): {res['чанк-учёт'][0]:.2f}/{res['чанк-учёт'][1]:.2f}; оценка центра: {res['оценка'][0]:.2f}/{res['оценка'][1]:.2f}/{res['оценка'][2]:.2f}", flush=True)
    json.dump(rows, open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), f'data/l3v/{ver}-s0table.json'), 'w'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()

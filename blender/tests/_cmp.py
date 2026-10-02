import numpy as np
def key_rows(d):
    n=d.n_quads
    keys=[]
    for i in range(n):
        keys.append((int(d.block[i]), int(d.mat[i]), int(d.dir[i]), tuple(np.round(d.pos[i].reshape(-1),3).tolist()), tuple(np.round(d.uv[i].reshape(-1),3).tolist())))
    order=sorted(range(n), key=lambda i:keys[i])
    return keys, order
def compare(a,b, verbose=True, atol=2e-4):
    ka,oa=key_rows(a); kb,ob=key_rows(b)
    ok=True
    if a.n_quads!=b.n_quads:
        ok=False
        if verbose: print('count mismatch',a.n_quads,b.n_quads)
    sa=set(ka[i] for i in oa); sb=set(kb[i] for i in ob)
    only_a=sa-sb; only_b=sb-sa
    if only_a or only_b:
        ok=False
        if verbose:
            print('only in A',len(only_a),'only in B',len(only_b))
            for k in sorted(only_a)[:6]: print(' A',k)
            for k in sorted(only_b)[:6]: print(' B',k)
    if a.n_quads==b.n_quads:
        pa=a.pos[oa]; pb=b.pos[ob]
        if not np.allclose(pa,pb,atol=atol):
            ok=False
            if verbose: print('pos differ', np.abs(pa-pb).max())
        if not np.allclose(a.uv[oa],b.uv[ob],atol=atol):
            ok=False
            if verbose: print('uv differ', np.abs(a.uv[oa]-b.uv[ob]).max())
        ca=a.col[oa].astype(int); cb=b.col[ob].astype(int)
        if np.abs(ca-cb).max()>1:
            ok=False
            if verbose:
                bad=np.nonzero(np.abs(ca-cb).max(axis=(1,2))>1)[0]
                print('col differ', len(bad), [ (ka[oa[i]][:3], ca[i,0], cb[i,0]) for i in bad[:5]])
    return ok


def expand_merged(d):
    """Разворачивает слитые грани обратно в единичные (по локальным UV и прямоугольнику) -> MeshData без merged (для сравнения)."""
    from mcgen_addon.mesh.mesher import MeshData
    pos, uv, col, mat, blk, dr = [], [], [], [], [], []
    for i in range(d.n_quads):
        if not d.merged[i]:
            pos.append(d.pos[i]); uv.append(d.uv[i]); col.append(d.col[i]); mat.append(d.mat[i]); blk.append(d.block[i]); dr.append(d.dir[i])
            continue
        P = d.pos[i].astype(np.float64)
        L = d.uv[i].astype(np.float64)
        R = d.rect[i].astype(np.float64)
        eu, ev = L[:, 0].max(), L[:, 1].max()
        su = (L[:, 0] > 0.5 * eu).astype(int)
        sv = (L[:, 1] > 0.5 * ev).astype(int)
        k00 = [k for k in range(4) if su[k] == 0 and sv[k] == 0][0]
        k10 = [k for k in range(4) if su[k] == 1 and sv[k] == 0][0]
        k01 = [k for k in range(4) if su[k] == 0 and sv[k] == 1][0]
        e1 = P[k10] - P[k00]
        e2 = P[k01] - P[k00]
        for ci in range(int(round(eu))):
            for cj in range(int(round(ev))):
                q = np.zeros((4, 3)); u = np.zeros((4, 2))
                for k in range(4):
                    s = (ci + su[k]) / eu
                    t = (cj + sv[k]) / ev
                    q[k] = P[k00] + s * e1 + t * e2
                    u[k] = (R[0] + su[k] * R[2], R[1] + sv[k] * R[3])
                pos.append(q.astype(np.float32)); uv.append(u.astype(np.float32)); col.append(d.col[i]); mat.append(d.mat[i]); dr.append(d.dir[i])
                # блок исходной ячейки восстановить трудно — не сравниваем (ставим 0)
                blk.append(0)
    n = len(pos)
    return MeshData(np.array(pos).reshape(n, 4, 3), np.array(uv).reshape(n, 4, 2), np.array(col).reshape(n, 4, 4), np.array(mat, dtype=np.uint8),
                    np.array(blk, dtype=np.uint32), np.array(dr, dtype=np.uint8))


def key_set_nob(d, nd=3):
    """Мультимножество граней без поля блока: (материал, направление, позиции, uv, цвет) — для сравнения слитых и неслитых мешей."""
    from collections import Counter
    c = Counter()
    for i in range(d.n_quads):
        c[(int(d.mat[i]), int(d.dir[i]), tuple(np.round(d.pos[i].reshape(-1), nd).tolist()), tuple(np.round(d.uv[i].reshape(-1), nd).tolist()),
           tuple(d.col[i, 0].tolist()))] += 1
    return c

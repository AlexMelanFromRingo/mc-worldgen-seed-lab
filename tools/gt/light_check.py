#!/usr/bin/env python3
"""Проверка модели небесного света: сравнение расчёта по блокам эталонного мира с SkyLight, который игра сохранила в чанках.

    light_check.py <каталог мира gen_world> [--cx 0 --cz 0] [--version 26.3] [--dim overworld] [--shapes]

Модель (SkyLightEngine / ChunkSkyLightSources): lowestSourceY колонки = y самого высокого блока с затуханием ≠ 0, плюс 1; клетки с y ≥ lowestSourceY — источники (15);
уровень соседа = уровень − max(1, затухание клетки-приёмника). Затухание блока — из reports/block_flags.json (damp).
"""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anvil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_window(w, cx, cz, r=1):
    rf = {}
    out = {}
    for dz in range(-r, r + 1):
        for dx in range(-r, r + 1):
            x, z = cx + dx, cz + dz
            key = (x >> 5, z >> 5)
            if key not in rf:
                rf[key] = anvil.RegionFile(f'{w.rdir}/r.{key[0]}.{key[1]}.mca')
            raw = rf[key].raw(x, z)
            nbt = anvil.parse_nbt(raw)
            ch = anvil.parse_chunk(nbt, w.states, w.biomes, w.extent)
            sky = {s['Y']: s.get('SkyLight') for s in nbt['sections']}
            out[(x, z)] = (ch, sky, nbt)
    for f in rf.values(): f.close()
    return out


def nib(b):
    a = np.frombuffer(b, dtype=np.uint8)
    lo, hi = a & 15, a >> 4
    out = np.empty(4096, dtype=np.uint8); out[0::2] = lo; out[1::2] = hi
    return out.reshape(16, 16, 16)      # [y][z][x]


def compute_sky(D, shape_mask=None, iters=15):
    """D: затухание [y][z][x] (uint8). Возвращает уровни неба (uint8)."""
    H = D.shape[0]
    opaque = D != 0
    # lowestSourceY: y самого высокого блока с затуханием≠0, плюс 1 (если нет — все клетки источники)
    top = np.where(opaque.any(axis=0), H - 1 - np.argmax(opaque[::-1], axis=0), -1) + 1
    yy = np.arange(H)[:, None, None]
    src = yy >= top[None]
    L = np.where(src, 15, 0).astype(np.int16)
    op = np.maximum(1, D.astype(np.int16))
    for _ in range(iters):
        best = np.zeros_like(L)
        for ax, sh in ((0, 1), (0, -1), (1, 1), (1, -1), (2, 1), (2, -1)):
            r = np.roll(L, sh, axis=ax)
            if ax == 0:
                if sh == 1: r[0] = 0
                else: r[-1] = 0
            elif ax == 1:
                if sh == 1: r[:, 0] = 0
                else: r[:, -1] = 0
            else:
                if sh == 1: r[:, :, 0] = 0
                else: r[:, :, -1] = 0
            best = np.maximum(best, r)
        cand = best - op
        L2 = np.maximum(L, cand)
        L2 = np.where(src, 15, L2)
        if (L2 == L).all(): break
        L = L2
    return np.clip(L, 0, 15).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('world'); ap.add_argument('--cx', type=int, default=0); ap.add_argument('--cz', type=int, default=0)
    ap.add_argument('--version', default='26.3'); ap.add_argument('--dim', default='overworld'); ap.add_argument('--n', type=int, default=1)
    a = ap.parse_args()
    w = anvil.World(a.world + '/world' if os.path.isdir(a.world + '/world') else a.world, a.dim, a.version, use_cache=False)
    fl = json.load(open(f'{ROOT}/run/pack-{a.version}/reports/block_flags.json'))
    damp = np.array(fl['damp'], dtype=np.uint8)
    tot = bad = 0
    for k in range(a.n):
        cx, cz = a.cx + k, a.cz
        win = load_window(w, cx, cz)
        min_y, H = w.extent
        D = np.zeros((H, 48, 48), dtype=np.uint8)
        for (x, z), (ch, sky, nbt) in win.items():
            ox, oz = (x - (cx - 1)) * 16, (z - (cz - 1)) * 16
            D[:, oz:oz + 16, ox:ox + 16] = damp[ch.blocks]
        L = compute_sky(D)
        ch, sky, nbt = win[(cx, cz)]
        ours = L[:, 16:32, 16:32]
        ref = np.zeros((H, 16, 16), dtype=np.uint8); have = np.zeros(H // 16, bool)
        stored = [sy for sy, b in sky.items() if b is not None]
        top_stored = max(stored) if stored else -999
        for s in range(H // 16):
            sy = min_y // 16 + s
            b = sky.get(sy)
            if b is not None:
                ref[s * 16:(s + 1) * 16] = nib(b)
            elif sy > top_stored:
                ref[s * 16:(s + 1) * 16] = 15          # выше верхней хранимой секции — небо (игра света там не хранит)
            have[s] = True                              # не сохранённая нижняя секция — все нули (пустой слой не пишется)
        m = np.repeat(have, 16)[:, None, None] & np.ones((1, 16, 16), bool)
        diff = (ours != ref) & m
        tot += int(m.sum()); bad += int(diff.sum())
        print(f'chunk ({cx},{cz}): сравнено {int(m.sum())}, расхождений {int(diff.sum())}, секций со светом {int(have.sum())}/{H // 16}')
        if diff.any():
            ys, zs, xs = np.nonzero(diff)
            for i in range(min(8, len(ys))):
                y, z, x = ys[i], zs[i], xs[i]
                print(f'   x={cx*16+x} y={min_y+y} z={cz*16+z} ours={ours[y,z,x]} ref={ref[y,z,x]} block={w.states.names[ch.blocks[y,z,x]] if hasattr(w.states,"names") else ch.blocks[y,z,x]}')
    print(f'итого: {tot} клеток, расхождений {bad} ({100.0 * (tot - bad) / max(tot, 1):.5f} % совпадения)')


if __name__ == '__main__':
    main()

"""Дальний LOD по карте высот: вместо полной геометрии чанка — «ступенчатая» поверхность из столбцов (по одному на ячейку stride×stride
блоков): верхняя грань цвета верхнего блока (средний цвет текстуры × оттенок биома) и боковые грани при перепаде высот. Без UV и текстур —
цвет вершин. Чистый numpy, без bpy. Выход — в осях Blender (x, -z, y), координаты относительно начала группы чанков.

Поверхность столбца — самый верхний блок «видимого твёрдого» класса (куб, частичный, полупрозрачный, листва, жидкость; растения и воздух пропускаются).
"""
import numpy as np

from ..assets import tint as tint_mod
from ..assets.models import FACE_VERTS

__all__ = ['column_heights', 'build_lod_group']

SIDE_SHADE = 0.72
WATER_TOP = 8.0 / 9.0


def column_heights(table, blocks_flat, height, y_lo=0, y_hi=None, hide_water=False):
    """Карта высот чанка: (ytop int16 [16,16] — индекс верхнего блока поверхности (-1 нет), sid uint16 [16,16] — его состояние).
    y_lo/y_hi — диапазон высот (локальные индексы, включительно); hide_water — вода не считается поверхностью."""
    arr_all = np.asarray(blocks_flat).reshape(height, 16, 16)
    y_hi = height - 1 if y_hi is None else min(y_hi, height - 1)
    y_lo = max(0, y_lo)
    arr = arr_all[y_lo:y_hi + 1]
    kind = table.st_lod_kind[arr]
    surf = kind > 0
    if hide_water:
        surf &= (table.st_flags[arr] & (1 << 4)) == 0
    h = arr.shape[0]
    anyv = surf.any(axis=0) if h else np.zeros((16, 16), bool)
    ytop = (h - 1 - surf[::-1].argmax(axis=0) + y_lo).astype(np.int16) if h else np.zeros((16, 16), np.int16)
    ytop[~anyv] = -1
    arr = arr_all
    yc = np.maximum(ytop, 0).astype(np.intp)
    sid = np.take_along_axis(arr, yc[None, :, :], axis=0)[0]
    sid = np.where(anyv, sid, 0).astype(np.uint16)
    return ytop, sid


def _mul(c, k):
    return (c.astype(np.float32) * k).astype(np.float32)


def _cell_colors(table, biome_colors, sid, ytop, biomes_flat, height, ox):
    """RGB uint8 [16,16,3] цвета верхней грани столбцов (с оттенком биома)."""
    base = table.st_lod_rgb[sid].astype(np.uint32)
    rgb = np.stack([(base >> 16) & 255, (base >> 8) & 255, base & 255], axis=-1).astype(np.float32)
    kind = table.st_lod_tint[sid]
    if biomes_flat is None or biome_colors is None:
        return rgb.astype(np.uint8)
    bio = np.asarray(biomes_flat).reshape(height // 4, 4, 4)
    qy = np.maximum(ytop, 0) >> 2
    zz, xx = np.meshgrid(np.arange(16) >> 2, np.arange(16) >> 2, indexing='ij')
    b = bio[qy, zz, xx]
    b = np.minimum(b, biome_colors.rgb.shape[0] - 1)
    for k in (tint_mod.GRASS, tint_mod.FOLIAGE, tint_mod.DRY_FOLIAGE, tint_mod.WATER):
        m = kind == k
        if m.any():
            col = biome_colors.rgb[b[m], k - tint_mod.GRASS].astype(np.uint32)
            tc = np.stack([(col >> 16) & 255, (col >> 8) & 255, col & 255], axis=-1).astype(np.float32)
            rgb[m] = rgb[m] * tc / 255.0
    return np.clip(rgb, 0, 255).astype(np.uint8)


def build_lod_group(table, biome_colors, blocks_by_chunk, biomes_by_chunk, chunk_keys, gk, n_per_obj, min_y, height, stride=2, scale=1.0,
                    heights_cache=None, y_lo=0, y_hi=None, hide_water=False):
    """LOD-меш группы чанков `chunk_keys` (группа gk размером n_per_obj×n_per_obj). -> (pos float32 [n,4,3], col uint8 [n,4,4]).

    heights_cache — словарь ck -> (ytop, sid), общий для вызовов (чтобы не пересчитывать карты высот соседей)."""
    cache = heights_cache if heights_cache is not None else {}
    s = int(stride)
    assert 16 % s == 0
    cpc = 16 // s                     # ячеек на чанк по стороне
    gx0, gz0 = gk[0] * n_per_obj, gk[1] * n_per_obj
    ncell = n_per_obj * cpc

    def heights(ck):
        r = cache.get(ck)
        if r is None:
            a = blocks_by_chunk.get(ck)
            if a is None:
                return None
            r = column_heights(table, a, height, y_lo, y_hi, hide_water)
            cache[ck] = r
        return r

    # карта высот ячеек с рамкой в 1 ячейку: top[j+1, i+1] — высота верхней грани (в блоках от min_y), -1 — нет
    top = np.full((ncell + 2, ncell + 2), -1.0, dtype=np.float32)
    col = np.zeros((ncell, ncell, 3), dtype=np.uint8)
    water = np.zeros((ncell, ncell), dtype=bool)
    have = np.zeros((ncell + 2, ncell + 2), dtype=bool)

    def cell_block(ck):
        h = heights(ck)
        if h is None:
            return None
        ytop, sid = h
        yt = ytop.astype(np.float32).reshape(cpc, s, cpc, s)
        # представитель ячейки — столбец с максимальной высотой
        flat = yt.transpose(0, 2, 1, 3).reshape(cpc, cpc, s * s)
        arg = flat.argmax(axis=2)
        topc = flat.max(axis=2)
        return ytop, sid, arg, topc

    for ck in chunk_keys:
        r = cell_block(ck)
        if r is None:
            continue
        ytop, sid, arg, topc = r
        ci, cj = ck[0] - gx0, ck[1] - gz0          # положение чанка в группе
        # цвет по представителю
        zz, xx = np.meshgrid(np.arange(cpc), np.arange(cpc), indexing='ij')
        rz = zz * s + arg // s
        rx = xx * s + arg % s
        rep_sid = sid[rz, rx]
        rep_y = ytop[rz, rx]
        full = _cell_colors(table, biome_colors, sid, ytop, biomes_by_chunk.get(ck) if biomes_by_chunk else None, height, 0)
        cc = full[rz, rx]
        is_water = (table.st_flags[rep_sid] & (1 << 4)) != 0
        h = np.where(topc >= 0, topc + 1.0, -1.0)
        h = np.where(is_water & (topc >= 0), topc + WATER_TOP, h)
        sl = (slice(1 + cj * cpc, 1 + (cj + 1) * cpc), slice(1 + ci * cpc, 1 + (ci + 1) * cpc))
        top[sl] = h
        have[sl] = True
        col[cj * cpc:(cj + 1) * cpc, ci * cpc:(ci + 1) * cpc] = cc
        water[cj * cpc:(cj + 1) * cpc, ci * cpc:(ci + 1) * cpc] = is_water
    # рамка: соседние чанки группы
    for dj in range(-1, ncell // cpc + 1):
        for di in range(-1, ncell // cpc + 1):
            if 0 <= di < n_per_obj and 0 <= dj < n_per_obj:
                continue
            ck = (gx0 + di, gz0 + dj)
            r = cell_block(ck)
            if r is None:
                continue
            ytop, sid, arg, topc = r
            hh = np.where(topc >= 0, topc + 1.0, -1.0)
            # куда класть: ячейки рамки
            for cj_ in range(cpc):
                for ci_ in range(cpc):
                    gi = di * cpc + ci_
                    gj = dj * cpc + cj_
                    if -1 <= gi <= ncell and -1 <= gj <= ncell:
                        top[gj + 1, gi + 1] = hh[cj_, ci_]
                        have[gj + 1, gi + 1] = True

    pos_l, col_l = [], []
    ys0 = 0.0
    inner = top[1:-1, 1:-1]
    present = inner >= 0
    jj, ii = np.nonzero(present)
    if len(jj):
        # верхние грани
        x0 = ii * float(s)
        z0 = jj * float(s)
        y = inner[jj, ii]
        P = np.zeros((len(jj), 4, 3), dtype=np.float32)
        P[:, 0] = np.stack([x0, y, z0], axis=1)
        P[:, 1] = np.stack([x0, y, z0 + s], axis=1)
        P[:, 2] = np.stack([x0 + s, y, z0 + s], axis=1)
        P[:, 3] = np.stack([x0 + s, y, z0], axis=1)
        pos_l.append(P)
        c4 = np.zeros((len(jj), 4, 4), dtype=np.uint8)
        c4[:, :, :3] = col[jj, ii][:, None, :]
        c4[:, :, 3] = 255
        col_l.append(c4)
        # боковые грани
        for d, (dj, di) in ((2, (-1, 0)), (3, (1, 0)), (4, (0, -1)), (5, (0, 1))):
            nb = top[1 + jj + dj, 1 + ii + di]
            hn = np.where(have[1 + jj + dj, 1 + ii + di], np.maximum(nb, 0.0), 0.0)
            m = y > hn + 1e-4
            if not m.any():
                continue
            xa, za, yt, yb = x0[m], z0[m], y[m], hn[m]
            frm = np.stack([xa, yb, za], axis=1)
            to = np.stack([xa + s, yt, za + s], axis=1)
            Q = np.zeros((int(m.sum()), 4, 3), dtype=np.float32)
            for v in range(4):
                sel = FACE_VERTS[d][v]
                Q[:, v, 0] = np.where(sel[0], to[:, 0], frm[:, 0])
                Q[:, v, 1] = np.where(sel[1], to[:, 1], frm[:, 1])
                Q[:, v, 2] = np.where(sel[2], to[:, 2], frm[:, 2])
            pos_l.append(Q)
            cs = np.zeros((int(m.sum()), 4, 4), dtype=np.uint8)
            cs[:, :, :3] = (col[jj, ii][m].astype(np.float32) * SIDE_SHADE).astype(np.uint8)[:, None, :]
            cs[:, :, 3] = 255
            col_l.append(cs)
    if not pos_l:
        return np.zeros((0, 4, 3), np.float32), np.zeros((0, 4, 4), np.uint8)
    P = np.concatenate(pos_l)
    C = np.concatenate(col_l)
    # оси Blender: (x, -z, y - 0) в блоках, затем масштаб
    out = np.empty_like(P)
    out[:, :, 0] = P[:, :, 0] * scale
    out[:, :, 1] = -P[:, :, 2] * scale
    out[:, :, 2] = P[:, :, 1] * scale
    return out, C

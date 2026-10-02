"""Эталонная (простая, медленная) реализация меширования чанка на Python/numpy — для проверки C-ядра (mesh.c) на случайных и реальных
чанках: набор граней, UV, цвета, материалы и «грань → блок» должны совпасть. Написана заново прямо по поведению игры 26.x
(Block.shouldRenderFace, ModelBlockRenderer.tesselateBlock, FluidRenderer.tesselate, BlockTintCache), а не по коду C.
Без bpy."""
import math

import numpy as np

from ..assets import jrand
from ..assets import tint as tint_mod
from ..assets.state_table import F, SKIP_BARS, SKIP_LEAVES, SKIP_LIQUID, SKIP_NONE, SKIP_POWDER, SKIP_ROOTS, SKIP_SAME

DIR_VEC = ((0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1), (-1, 0, 0), (1, 0, 0))
OPP = (1, 0, 3, 2, 5, 4)
HORIZ_FLOW = (2, 5, 3, 4)      # Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST
SHADE_DEFAULT = (0.5, 1.0, 0.8, 0.8, 0.6, 0.6)
F32 = np.float32


class _World:
    """3×3 чанков вокруг центрального: состояния по локальным координатам (x, y, z) центра; вне — по соседям; y вне диапазона — None."""

    def __init__(self, nb, nbio, height):
        self.nb = nb
        self.nbio = nbio
        self.H = height
        self.arr = [None if b is None else np.asarray(b).reshape(height, 16, 16) for b in nb]
        self.bio = [None if b is None else np.asarray(b).reshape(height // 4, 4, 4) for b in nbio]

    def state(self, x, y, z):
        if y < 0 or y >= self.H:
            return -1
        cx = 0 if x < 0 else (2 if x >= 16 else 1)
        cz = 0 if z < 0 else (2 if z >= 16 else 1)
        a = self.arr[cz * 3 + cx]
        if a is None:
            return -1
        return int(a[y, z & 15, x & 15])

    def biome(self, x, qy, z, n_biomes):
        cx = 0 if x < 0 else (2 if x >= 16 else 1)
        cz = 0 if z < 0 else (2 if z >= 16 else 1)
        b = self.bio[cz * 3 + cx]
        if b is None:
            b = self.bio[4]
            if b is None:
                return 0
            x = min(max(x, 0), 15)
            z = min(max(z, 0), 15)
        v = int(b[qy, (z & 15) >> 2, (x & 15) >> 2])
        return v if v < n_biomes else 0


def _mask_bits(masks, mid):
    return np.unpackbits(masks[mid]).reshape(16, 16).astype(bool)


class _Ref:
    def __init__(self, table, biome_colors, opt, cx, cz, min_y, height, nb, nbio):
        self.t = table
        self.bc = biome_colors
        self.opt = opt
        self.cx, self.cz, self.min_y, self.H = cx, cz, min_y, height
        self.w = _World(nb, nbio, height)
        self.flags = table.st_flags
        self.rows = []   # (block, mat, dir, pos(4,3), uv(4,2), rgb, shade)
        self.tint_maps = {}
        self.nbio_n = biome_colors.rgb.shape[0]
        self.masks = table.occ_masks
        self.perm = table.swamp_perm

    # --- вспомогательное ---
    def fl(self, s):
        return 0 if s < 0 else int(self.flags[s])

    def covers(self, a, b):
        """Маска a целиком покрыта маской b (побитово, независимо от occ_cover)."""
        ma, mb = self.masks[a], self.masks[b]
        return not np.any(ma & ~mb)

    def skip(self, sa, sb, d):
        t = self.t
        fa = self.fl(sa)
        cls = (fa >> F.SKIP_SHIFT) & 7
        fb = self.fl(sb)
        if cls == SKIP_NONE:
            return False
        if cls in (SKIP_SAME, SKIP_POWDER):
            return sb >= 0 and int(t.st_block[sb]) == int(t.st_block[sa])
        if cls == SKIP_LEAVES:
            return (not self.opt.cutout_leaves) and bool(fb & F.LEAVES)
        if cls == SKIP_ROOTS:
            roots = t.block_names.index('minecraft:mangrove_roots') if 'minecraft:mangrove_roots' in t.block_names else -1
            return sb >= 0 and int(t.st_block[sb]) == roots and d < 2
        if cls == SKIP_LIQUID:
            return bool(fb & (F.WATER | F.LAVA)) and bool(fa & F.WATER) == bool(fb & F.WATER)
        if cls == SKIP_BARS:
            if sb < 0:
                return False
            if int(t.st_block[sb]) == int(t.st_block[sa]) or (fa & F.BARS and fb & F.BARS):
                if d < 2:
                    return True
                ca = (fa >> (F.CONN_SHIFT + d - 2)) & 1
                cb = (fb >> (F.CONN_SHIFT + OPP[d] - 2)) & 1
                if ca and cb:
                    return True
            return False
        return False

    def should_render(self, sa, sb, d):
        t = self.t
        occ = int(t.st_occ[sb][OPP[d]]) if sb >= 0 else 0
        full = bool(np.all(self.masks[occ] == 255)) if occ else False
        if full:
            return False
        if self.skip(sa, sb, d):
            return False
        if occ == 0 or not np.any(self.masks[occ]):
            return True
        shape = int(t.st_occ[sa][d])
        if not np.any(self.masks[shape]):
            return True
        return not self.covers(shape, occ)

    # --- тонировка ---
    def tint_map(self, qy, kind):
        key = (qy, kind)
        m = self.tint_maps.get(key)
        if m is not None:
            return m
        r = self.opt.blend_radius
        bc = self.bc
        W = 16 + 2 * r
        samp = np.zeros((W, W, 3), dtype=np.int64)
        for zz in range(-r, 16 + r):
            for xx in range(-r, 16 + r):
                b = self.w.biome(xx, qy, zz, self.nbio_n)
                if kind == tint_mod.GRASS:
                    if bc.mod[b] == tint_mod.MOD_SWAMP:
                        col = 0x4C763C if tint_mod.swamp_is_dark(self.perm, self.cx * 16 + xx, self.cz * 16 + zz) else 0x6A7039
                    else:
                        col = int(bc.rgb[b][0])
                else:
                    col = int(bc.rgb[b][kind - tint_mod.GRASS])
                samp[zz + r, xx + r] = ((col >> 16) & 255, (col >> 8) & 255, col & 255)
        n = (2 * r + 1) ** 2
        out = np.zeros((16, 16), dtype=np.uint32)
        for z in range(16):
            for x in range(16):
                s = samp[z:z + 2 * r + 1, x:x + 2 * r + 1].reshape(-1, 3).sum(axis=0)
                rr, gg, bb = int(s[0]) // n & 255, int(s[1]) // n & 255, int(s[2]) // n & 255
                out[z, x] = (rr << 16) | (gg << 8) | bb
        self.tint_maps[key] = out
        return out

    def tint(self, kind, x, y, z):
        return int(self.tint_map(y >> 2, kind)[z, x])

    # --- вывод ---
    def emit(self, pos, uv, rgb, shade, mat, block, d):
        self.rows.append((block, mat, d, [tuple(p) for p in pos], [tuple(u) for u in uv], rgb, shade))

    # --- блоки с моделью ---
    def model_block(self, x, y, z, s):
        t = self.t
        f = int(self.flags[s])
        ax, az, ay = self.cx * 16 + x, self.cz * 16 + z, y + self.min_y
        g0, g1 = int(t.st_grp_off[s]), int(t.st_grp_off[s + 1])
        seed = None
        if f & F.RANDOM:
            seed0 = jrand.mth_get_seed(ax, ay, az)
            if f & F.MULTIPART:
                r = jrand.LegacyRandom(seed0)
                seed = r.next_long()
            else:
                seed = seed0
        ox = oy = oz = 0.0
        if f & (F.OFF_XZ | F.OFF_XYZ):
            sd = jrand.mth_get_seed(ax, 0, az)
            mh = float(t.st_off_h[s])
            mv = float(t.st_off_v[s])
            ox = float(F32(min(max(((float(F32(sd & 15) / F32(15.0))) - 0.5) * 0.5, -mh), mh)))
            oz = float(F32(min(max(((float(F32((sd >> 8) & 15) / F32(15.0))) - 0.5) * 0.5, -mh), mh)))
            if f & F.OFF_XYZ:
                oy = float(F32((float(F32((sd >> 4) & 15) / F32(15.0)) - 1.0) * mv))
        cull_cache = {}
        force_solid = (not self.opt.cutout_leaves) and bool(f & F.LEAVES)
        block = (y * 16 + z) * 16 + x
        shade = self.opt.shade
        for gi in range(g0, g1):
            g = int(t.grp_list[gi])
            v0, v1 = int(t.grp_var_off[g]), int(t.grp_var_off[g + 1])
            vi = v0
            total = int(t.grp_total[g])
            if total > 1 and seed is not None and not (self.opt.no_variants or self.opt.merge):
                r = jrand.LegacyRandom(seed)
                sel = r.next_int(total)
                vi = v0
                while vi < v1:
                    sel -= int(t.var_weight[vi])
                    if sel < 0:
                        break
                    vi += 1
                if vi >= v1:
                    vi = v1 - 1
            b = int(t.var_baked[vi])
            for q in range(int(t.baked_q_off[b]), int(t.baked_q_off[b + 1])):
                cull = int(t.q_cull[q])
                if cull >= 0:
                    if cull not in cull_cache:
                        dv = DIR_VEC[cull]
                        sb = self.w.state(x + dv[0], y + dv[1], z + dv[2])
                        cull_cache[cull] = self.should_render(s, sb, cull)
                    if not cull_cache[cull]:
                        continue
                qp = t.q_pos[q]
                pos = [(x + ox + float(qp[i][0]), y + oy + float(qp[i][1]), z + oz + float(qp[i][2])) for i in range(4)]
                kind = int(t.q_tint[q])
                rgb = 0xFFFFFF
                if kind == tint_mod.CONST:
                    rgb = int(t.q_rgb[q])
                elif kind >= tint_mod.GRASS:
                    yy = y - 1 if (f & F.TINT_BELOW) and y > 0 else y
                    rgb = self.tint(kind, x, yy, z)
                layer = 0 if force_solid else int(t.q_layer[q])
                self.emit(pos, t.q_uv[q], rgb, shade[int(t.q_shade[q])], layer, block, int(t.q_dir[q]))

    # --- жидкости ---
    @staticmethod
    def fkind(f):
        return 1 if f & F.WATER else (2 if f & F.LAVA else 0)

    @staticmethod
    def famount(f):
        return (f >> F.FLUID_AMOUNT_SHIFT) & 15

    def occluded_by_state(self, dirx, height, sx):
        """isFaceOccludedByState: грань sx (обращённая к жидкости) закрывает брусок жидкости высотой height?"""
        if sx < 0:
            return False
        occ = int(self.t.st_occ[sx][OPP[dirx]])
        m = self.masks[occ]
        if not np.any(m):
            return False
        if np.all(m == 255):
            return dirx != 1 or height == 1.0
        bits = _mask_bits(self.masks, occ)       # [u][v]
        if dirx == 1:
            return height >= 1.0 - 1e-7 and bool(bits.all())
        if dirx == 0:
            return bool(bits.all())
        rows = min(16, int(math.ceil(height * 16.0 - 1e-4)))
        if dirx >= 4:       # ось X: u = y, v = z
            return bool(bits[:rows, :].all())
        return bool(bits[:, :rows].all())   # ось Z: u = x, v = y

    def fheight(self, x, y, z, kind):
        s = self.w.state(x, y, z)
        f = self.fl(s)
        if self.fkind(f) == kind:
            sa = self.w.state(x, y + 1, z)
            return 1.0 if self.fkind(self.fl(sa)) == kind else float(F32(self.famount(f)) / F32(9.0))
        return -1.0 if f & F.SOLID else 0.0

    def avg_height(self, kind, hself, h2, h1, cx, cy, cz):
        if h1 >= 1.0 or h2 >= 1.0:
            return 1.0
        w = [0.0, 0.0]

        def add(h):
            if h >= 0.8:
                w[0] += h * 10.0
                w[1] += 10.0
            elif h >= 0.0:
                w[0] += h
                w[1] += 1.0
        if h1 > 0.0 or h2 > 0.0:
            hc = self.fheight(cx, cy, cz, kind)
            if hc >= 1.0:
                return 1.0
            add(hc)
        add(hself)
        add(h1)
        add(h2)
        return float(F32(w[0]) / F32(w[1]))

    def flow(self, x, y, z, kind, selfh, falling):
        fx = fz = 0.0
        for d in HORIZ_FLOW:
            dv = DIR_VEC[d]
            nx, nz = x + dv[0], z + dv[2]
            sn = self.w.state(nx, y, nz)
            fn = self.fl(sn)
            kn = self.fkind(fn)
            if kn == 0 or kn == kind:
                nh = float(F32(self.famount(fn)) / F32(9.0)) if kn else 0.0
                dist = 0.0
                if nh == 0.0:
                    if not (fn & F.BLOCKS_FLOW):
                        sb = self.w.state(nx, y - 1, nz)
                        fb = self.fl(sb)
                        kb = self.fkind(fb)
                        if kb == 0 or kb == kind:
                            nh = float(F32(self.famount(fb)) / F32(9.0)) if kb else 0.0
                            if nh > 0.0:
                                dist = float(F32(selfh) - (F32(nh) - F32(0.8888889)))
                elif nh > 0.0:
                    dist = float(F32(selfh) - F32(nh))
                if dist != 0.0:
                    fx += dv[0] * dist
                    fz += dv[2] * dist

        def norm(a, b, c):
            ln = math.sqrt(a * a + b * b + c * c)
            return (0.0, 0.0, 0.0) if ln < 1e-5 else (a / ln, b / ln, c / ln)
        v = norm(fx, 0.0, fz)
        if falling:
            for d in HORIZ_FLOW:
                dv = DIR_VEC[d]
                found = False
                for up in (0, 1):
                    sp = self.w.state(x + dv[0], y + up, z + dv[2])
                    fp = self.fl(sp)
                    if self.fkind(fp) == kind:
                        continue
                    if sp >= 0 and (int(self.t.st_sturdy[sp]) >> d) & 1:
                        found = True
                        break
                if found:
                    v = norm(v[0], v[1] - 6.0, v[2])
                    return v[0], v[2]
        v = norm(*v)
        return v[0], v[2]

    def fluid_block(self, x, y, z, s):
        t = self.t
        f = int(self.flags[s])
        kind = self.fkind(f)
        block = (y * 16 + z) * 16 + x
        st = {d: self.w.state(x + DIR_VEC[d][0], y + DIR_VEC[d][1], z + DIR_VEC[d][2]) for d in range(6)}
        fs = {d: self.fl(st[d]) for d in range(6)}
        same = {d: self.fkind(fs[d]) == kind for d in range(6)}
        self_occ = {d: self.occluded_by_state(OPP[d], 1.0, s) for d in range(6)}
        renderUp = not same[1]
        renderDown = (not same[0]) and (not self_occ[0]) and (not self.occluded_by_state(0, float(F32(0.8888889)), st[0]))
        render = {d: (not same[d]) and (not self_occ[d]) for d in (2, 3, 4, 5)}
        if not (renderUp or renderDown or any(render.values())):
            return
        mat = 3 if kind == 1 else int(t.fluid_layer[1])
        tint = self.tint(tint_mod.WATER, x, y, z) if kind == 1 else 0xFFFFFF
        hself = self.fheight(x, y, z, kind)
        if hself >= 1.0:
            hNE = hNW = hSE = hSW = 1.0
        else:
            hN = self.fheight(x, y, z - 1, kind)
            hS = self.fheight(x, y, z + 1, kind)
            hE = self.fheight(x + 1, y, z, kind)
            hW = self.fheight(x - 1, y, z, kind)
            hNE = self.avg_height(kind, hself, hN, hE, x + 1, y, z - 1)
            hNW = self.avg_height(kind, hself, hN, hW, x - 1, y, z - 1)
            hSE = self.avg_height(kind, hself, hS, hE, x + 1, y, z + 1)
            hSW = self.avg_height(kind, hself, hS, hW, x - 1, y, z + 1)
        sh = self.opt.shade
        OFFS = 0.001
        bottom = OFFS if renderDown else 0.0
        base = 0 if kind == 1 else 3
        rect = t.fluid_rect
        if renderUp and not self.occluded_by_state(1, min(hNW, hSW, hSE, hNE), st[1]):
            hNW, hSW, hSE, hNE = (float(F32(h) - F32(OFFS)) for h in (hNW, hSW, hSE, hNE))
            amount = self.famount(f)
            flx, flz = self.flow(x, y, z, kind, float(F32(amount) / F32(9.0)), bool(f & F.FALLING))
            if flx == 0.0 and flz == 0.0:
                r = rect[base]
                u00, v00 = r[0], r[1]
                u01, v01 = u00, r[3]
                u10, v10 = r[2], v01
                u11, v11 = u10, v00
            else:
                r = rect[base + 1]
                angle = float(F32(math.atan2(flz, flx)) - F32(1.5707964))
                sn = float(F32(math.sin(angle)) * F32(0.25))
                cs = float(F32(math.cos(angle)) * F32(0.25))

                def GU(fr):
                    return r[0] + (r[2] - r[0]) * fr

                def GV(fr):
                    return r[1] + (r[3] - r[1]) * fr
                u00, v00 = GU(0.5 + (-cs - sn)), GV(0.5 + (-cs + sn))
                u01, v01 = GU(0.5 + (-cs + sn)), GV(0.5 + (cs + sn))
                u10, v10 = GU(0.5 + (cs + sn)), GV(0.5 + (cs - sn))
                u11, v11 = GU(0.5 + (cs - sn)), GV(0.5 + (-cs - sn))
            back = False
            for ox in (-1, 0, 1):
                for oz in (-1, 0, 1):
                    sp = self.w.state(x + ox, y + 1, z + oz)
                    fp = self.fl(sp)
                    if self.fkind(fp) != kind and not (fp & F.OPAQUE):
                        back = True
            P = [(x, y + hNW, z), (x, y + hSW, z + 1), (x + 1, y + hSE, z + 1), (x + 1, y + hNE, z)]
            UV = [(u00, v00), (u01, v01), (u10, v10), (u11, v11)]
            self.fluid_face(P, UV, tint, sh[1], mat, block, 1, back)
        if renderDown:
            r = rect[base]
            P = [(x, y + bottom, z), (x + 1, y + bottom, z), (x + 1, y + bottom, z + 1), (x, y + bottom, z + 1)]
            UV = [(r[0], r[1]), (r[2], r[1]), (r[2], r[3]), (r[0], r[3])]
            self.fluid_face(P, UV, tint, sh[0], mat, block, 0, False)
        for d in (2, 3, 4, 5):
            if d == 2:
                hh0, hh1, x0, x1, z0, z1 = hNW, hNE, x, x + 1, z + OFFS, z + OFFS
            elif d == 3:
                hh0, hh1, x0, x1, z0, z1 = hSE, hSW, x + 1, x, z + 1 - OFFS, z + 1 - OFFS
            elif d == 4:
                hh0, hh1, x0, x1, z0, z1 = hSW, hNW, x + OFFS, x + OFFS, z + 1, z
            else:
                hh0, hh1, x0, x1, z0, z1 = hNE, hSE, x + 1 - OFFS, x + 1 - OFFS, z, z + 1
            if not render[d] or self.occluded_by_state(d, max(hh0, hh1), st[d]):
                continue
            r = rect[base + 1]
            overlay = False
            if kind == 1 and st[d] >= 0:
                nf = fs[d]
                if (nf & F.LEAVES) or (((nf >> F.SKIP_SHIFT) & 7) == SKIP_SAME):
                    overlay = True
            if overlay:
                r = rect[2]
            u0 = r[0]
            u1 = r[0] + (r[2] - r[0]) * 0.5
            v01 = r[1] + (r[3] - r[1]) * ((1.0 - hh0) * 0.5)
            v02 = r[1] + (r[3] - r[1]) * ((1.0 - hh1) * 0.5)
            v1 = r[1] + (r[3] - r[1]) * 0.5
            shade_side = sh[2 if d <= 3 else 4] * sh[1]
            P = [(x0, y + hh0, z0), (x1, y + hh1, z1), (x1, y + bottom, z1), (x0, y + bottom, z0)]
            UV = [(u0, v01), (u1, v02), (u1, v1), (u0, v1)]
            self.fluid_face(P, UV, tint, shade_side, mat, block, d, not overlay)

    def fluid_face(self, P, UV, rgb, shade, mat, block, d, back):
        self.emit(P, UV, rgb, shade, mat, block, d)
        if back:
            o = (0, 3, 2, 1)
            self.emit([P[i] for i in o], [UV[i] for i in o], rgb, shade, mat, block, d)

    # --- главный цикл ---
    def run(self):
        t = self.t
        n_states = t.st_flags.shape[0]
        want = 0
        if not self.opt.no_models:
            want |= F.GEOM
        if not self.opt.no_fluids:
            want |= F.WATER | F.LAVA
        a = self.w.arr[4]
        for y in range(self.H):
            sl = a[y]
            ys, xs = np.nonzero((self.flags[np.minimum(sl, n_states - 1)] & want) != 0)
            for z, x in zip(ys.tolist(), xs.tolist()):
                s = int(sl[z, x])
                f = int(self.flags[s])
                if f & want & (F.WATER | F.LAVA):
                    self.fluid_block(x, y, z, s)
                if f & want & F.GEOM:
                    self.model_block(x, y, z, s)


def reference_mesh_chunk(table, biome_colors, options, cx, cz, min_y, height, nb, nbio):
    """nb, nbio — по 9 массивов (или None) окрестности 3×3 (индекс (dz+1)*3 + dx+1). Возвращает MeshData с теми же опциями, что у C-ядра."""
    from .mesher import MeshData
    r = _Ref(table, biome_colors, options, cx, cz, min_y, height, nb, nbio)
    r.run()
    n = len(r.rows)
    pos = np.zeros((n, 4, 3), dtype=np.float32)
    uv = np.zeros((n, 4, 2), dtype=np.float32)
    col = np.zeros((n, 4, 4), dtype=np.uint8)
    mat = np.zeros(n, dtype=np.uint8)
    blk = np.zeros(n, dtype=np.uint32)
    dr = np.zeros(n, dtype=np.uint8)
    for i, (block, m, d, P, UV, rgb, shade) in enumerate(r.rows):
        p = np.asarray(P, dtype=np.float32)
        if options.blender_axes:
            q = np.empty_like(p)
            q[:, 0] = p[:, 0]
            q[:, 1] = -p[:, 2]
            q[:, 2] = p[:, 1] + options.y_offset
            p = q
        else:
            p = p.copy()
            p[:, 1] += options.y_offset
        pos[i] = p * np.float32(options.scale)
        u = np.asarray(UV, dtype=np.float32)
        if options.blender_uv:
            u = u.copy()
            u[:, 1] = 1.0 - u[:, 1]
        uv[i] = u
        rr, gg, bb = (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255
        if options.bake_shade:
            rr, gg, bb = (int(np.float32(c) * np.float32(shade) + np.float32(0.5)) for c in (rr, gg, bb))
        col[i, :, 0], col[i, :, 1], col[i, :, 2], col[i, :, 3] = rr, gg, bb, 255
        mat[i], blk[i], dr[i] = m, block, d
    return MeshData(pos, uv, col, mat, blk, dr, {'blocks_visited': -1})

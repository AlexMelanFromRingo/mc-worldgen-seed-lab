"""Временный запасной предпросмотр по карте высот (заменится render.scene.SceneBuilder потока W4).

Строит из региона «блочный ландшафт»: по одному квадрату на колонку (верх) + боковые стенки там, где сосед ниже. Цвет вершин — по
имени верхнего блока (трава, камень, песок, вода, снег …), опционально с оттенком биома (траву/листву красит цвет биома из JSON
датапака, воду — water_color). Без текстур и без внутренних блоков. Объекты группируются по chunks_per_object (1/2/4/8 чанков).

Работает на numpy; bpy используется только в begin()/step()/clear() (главный поток).
"""
import hashlib
import itertools
import time

import numpy as np

from . import biomes as biomes_mod
from .scene_iface import SceneSink

TAG = 'mcgen_preview'
STAMP = 'mcgen_stamp'                  # порядковый номер сборки объекта (по нему видно, какие объекты пересобраны)
_stamps = itertools.count(1)
VCOL = 'Col'
MAX_COLUMNS_PER_GROUP = 512 * 512

# (подстрока имени блока, RGB sRGB 0..1, тонируется ли биомом: 'grass' | 'water' | None)
_COLOR_RULES = [
    ('grass_block', (0.46, 0.70, 0.27), 'grass'), ('podzol', (0.35, 0.24, 0.12), None), ('mycelium', (0.44, 0.38, 0.45), None),
    ('water', (0.20, 0.34, 0.85), 'water'), ('lava', (0.95, 0.45, 0.10), None), ('ice', (0.60, 0.75, 0.95), None),
    ('snow', (0.95, 0.97, 1.0), None), ('powder_snow', (0.95, 0.97, 1.0), None),
    ('leaves', (0.30, 0.55, 0.20), 'grass'), ('fern', (0.30, 0.55, 0.20), 'grass'), ('grass', (0.40, 0.65, 0.25), 'grass'),
    ('red_sand', (0.72, 0.38, 0.14), None), ('sandstone', (0.85, 0.80, 0.55), None), ('sand', (0.86, 0.82, 0.58), None),
    ('gravel', (0.55, 0.52, 0.50), None), ('terracotta', (0.60, 0.40, 0.30), None), ('clay', (0.62, 0.65, 0.72), None),
    ('coarse_dirt', (0.45, 0.32, 0.22), None), ('dirt', (0.52, 0.37, 0.25), None), ('mud', (0.25, 0.22, 0.25), None),
    ('deepslate', (0.28, 0.28, 0.30), None), ('bedrock', (0.15, 0.15, 0.15), None), ('stone', (0.52, 0.52, 0.52), None),
    ('andesite', (0.55, 0.55, 0.55), None), ('diorite', (0.75, 0.75, 0.75), None), ('granite', (0.60, 0.40, 0.33), None),
    ('tuff', (0.40, 0.40, 0.36), None), ('calcite', (0.88, 0.88, 0.85), None), ('basalt', (0.30, 0.30, 0.33), None),
    ('blackstone', (0.17, 0.15, 0.17), None), ('netherrack', (0.45, 0.17, 0.17), None), ('nylium', (0.55, 0.15, 0.20), None),
    ('soul_sand', (0.33, 0.26, 0.21), None), ('soul_soil', (0.30, 0.24, 0.20), None), ('magma', (0.60, 0.25, 0.08), None),
    ('end_stone', (0.86, 0.87, 0.62), None), ('obsidian', (0.08, 0.04, 0.15), None), ('glowstone', (0.95, 0.80, 0.40), None),
    ('log', (0.40, 0.30, 0.17), None), ('wood', (0.40, 0.30, 0.17), None), ('planks', (0.65, 0.52, 0.30), None),
    ('cobblestone', (0.45, 0.45, 0.45), None), ('bricks', (0.55, 0.45, 0.42), None), ('ore', (0.50, 0.48, 0.45), None),
    ('moss', (0.30, 0.45, 0.15), None), ('cactus', (0.20, 0.45, 0.20), None),
]


def _srgb_to_linear(c):
    c = np.asarray(c, np.float32)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def block_color(name):
    """(rgb, tint) для имени состояния блока: правила по подстроке, иначе стабильный цвет из хэша имени."""
    base = name.split('[', 1)[0].split(':', 1)[-1]
    if base in ('air', 'cave_air', 'void_air'):
        return (0.0, 0.0, 0.0), 'air'
    for key, rgb, tint in _COLOR_RULES:
        if key in base:
            return rgb, tint
    h = hashlib.md5(base.encode()).digest()
    return (0.35 + h[0] / 640.0, 0.35 + h[1] / 640.0, 0.35 + h[2] / 640.0), None


class PreviewSink(SceneSink):
    name = 'fallback-preview'

    def __init__(self):
        self._groups = []
        self._i = 0
        self._ctx = None
        self._stats = {'objects': 0, 'vertices': 0, 'faces': 0}
        self._coll = None
        self._mat = None

    # --- подготовка данных региона (numpy, без bpy) ---
    @staticmethod
    def grids(region, gen, view, world):
        """Собирает по всему региону: высота первой свободной клетки, id верхнего блока, id биома — сетки (nz*16, nx*16)."""
        from .lib import MC_HM_WORLD_SURFACE
        info = region.info
        W, D = info.nx * 16, info.nz * 16
        H = np.zeros((D, W), np.int32)
        ID = np.zeros((D, W), np.uint16)
        BIO = np.zeros((D, W), np.uint8)
        ymax = view.get('y_max', info.min_y + info.height - 1)
        ymin = view.get('y_min', info.min_y)
        zz, xx = np.mgrid[0:16, 0:16]
        for cz in range(info.cz0, info.cz0 + info.nz):
            for cx in range(info.cx0, info.cx0 + info.nx):
                hm = region.heightmap(cx, cz, MC_HM_WORLD_SURFACE).astype(np.int32)
                top = np.minimum(hm, ymax + 1)
                top = np.maximum(top, info.min_y + 1)
                blocks = region.blocks(cx, cz)
                idx = np.clip(top - 1 - info.min_y, 0, info.height - 1)
                ids = blocks[idx, zz, xx]
                bio = region.biomes(cx, cz)
                bq = bio[np.clip(idx // 4, 0, bio.shape[0] - 1), zz // 4, xx // 4]
                sz, sx = (cz - info.cz0) * 16, (cx - info.cx0) * 16
                H[sz:sz + 16, sx:sx + 16] = top
                ID[sz:sz + 16, sx:sx + 16] = ids
                BIO[sz:sz + 16, sx:sx + 16] = bq
        H = np.maximum(H, ymin)
        return H, ID, BIO

    @staticmethod
    def column_colors(ID, BIO, H, gen, world, view, pack_dir, assets_dir):
        """Цвета колонок (D, W, 3) sRGB: по имени верхнего блока, с оттенком биомов (если включено) и затемнением воды по глубине."""
        names = gen.block_names()
        uniq = np.unique(ID)
        lut = np.zeros((int(uniq.max()) + 1, 3), np.float32)
        tint = {}
        for u in uniq:
            rgb, t = block_color(names[u] if u < len(names) else 'unknown')
            lut[u] = rgb
            tint[int(u)] = t
        col = lut[ID]
        if view.get('tint_biomes', True) and pack_dir:
            bnames = gen.biome_names()
            grass = biomes_mod.palette(bnames, 'JSON', pack_dir, assets_dir)
            water = np.zeros_like(grass)
            for i, n in enumerate(bnames):
                c = biomes_mod.json_color(n, pack_dir, None) if ('ocean' in n or 'river' in n) else None
                water[i] = c if c else (0.25, 0.37, 0.85)
            gmask = np.isin(ID, [u for u, t in tint.items() if t == 'grass'])
            wmask = np.isin(ID, [u for u, t in tint.items() if t == 'water'])
            if gmask.any():
                col[gmask] = (grass[BIO] * 0.92 + lut[ID] * 0.08)[gmask]
            if wmask.any():
                col[wmask] = water[np.minimum(BIO, len(water) - 1)][wmask]
        wmask = np.isin(ID, [u for u, t in tint.items() if t == 'water'])
        if wmask.any():                       # вода темнеет с глубиной (нужен уровень моря)
            depth = np.clip(world.sea_level - H, 0, 40) / 40.0
            col[wmask] = col[wmask] * (1.0 - 0.55 * depth[wmask])[:, None]
        air = np.isin(ID, [u for u, t in tint.items() if t == 'air'])
        if air.any():
            col[air] = (0.1, 0.1, 0.12)
        return col

    @staticmethod
    def build_arrays(H, COL, x0, z0, x1, z1):
        """Вершины/квады среза колонок [z0:z1, x0:x1] (+ стенки к соседям по всей сетке). Возвращает (verts f32 (V,3) в осях Blender
        относительно (x0, z0), quads i32 (F,4), colors f32 (V,3) sRGB). Блок колонки (x, z) занимает [x, x+1] × [h-1, h] × [z, z+1]."""
        D, W = H.shape
        h = H[z0:z1, x0:x1].astype(np.float32)
        col = COL[z0:z1, x0:x1]
        gz, gx = np.mgrid[z0:z1, x0:x1]
        gx = gx.astype(np.float32) - x0
        gz = gz.astype(np.float32) - z0
        verts, quads, colors = [], [], []
        base = 0

        def add_quads(v4, c):
            nonlocal base
            n = v4.shape[0]
            verts.append(v4.reshape(-1, 3))
            colors.append(np.repeat(c, 4, axis=0))
            q = (np.arange(n, dtype=np.int32) * 4 + base)[:, None] + np.array([0, 1, 2, 3], np.int32)[None, :]
            quads.append(q)
            base += n * 4

        # верх: Blender (X, Y, Z) = (x, -z, y); обход против часовой при взгляде сверху (нормаль +Z)
        xs, zs, ys = gx.ravel(), gz.ravel(), h.ravel()
        v = np.stack([
            np.stack([xs, -zs - 1, ys], 1), np.stack([xs + 1, -zs - 1, ys], 1),
            np.stack([xs + 1, -zs, ys], 1), np.stack([xs, -zs, ys], 1)], 1)
        add_quads(v, col.reshape(-1, 3))

        # стенки: направление (dx, dz) в осях мира; у края всей сетки стенки нет
        def neighbor(dz, dx):
            zz = np.clip(np.arange(z0, z1) + dz, 0, D - 1)
            xx = np.clip(np.arange(x0, x1) + dx, 0, W - 1)
            return H[np.ix_(zz, xx)].astype(np.float32)

        for dz, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            hn = neighbor(dz, dx)
            m = (h > hn).ravel()
            if not m.any():
                continue
            x_, z_, hi, lo = xs[m], zs[m], ys[m], hn.ravel()[m]
            if dx == -1:     # западная стенка (x = const), нормаль -X
                p = [(x_, z_ + 1, lo), (x_, z_, lo), (x_, z_, hi), (x_, z_ + 1, hi)]
            elif dx == 1:
                p = [(x_ + 1, z_, lo), (x_ + 1, z_ + 1, lo), (x_ + 1, z_ + 1, hi), (x_ + 1, z_, hi)]
            elif dz == -1:   # северная (z = const, меньше z), нормаль на север (+Y Blender)
                p = [(x_, z_, lo), (x_ + 1, z_, lo), (x_ + 1, z_, hi), (x_, z_, hi)]
            else:
                p = [(x_ + 1, z_ + 1, lo), (x_, z_ + 1, lo), (x_, z_ + 1, hi), (x_ + 1, z_ + 1, hi)]
            v = np.stack([np.stack([a, -b, c], 1) for a, b, c in p], 1)
            add_quads(v, col.reshape(-1, 3)[m] * 0.78)
        return (np.concatenate(verts).astype(np.float32), np.concatenate(quads).astype(np.int32), np.concatenate(colors).astype(np.float32))

    # --- SceneSink ---
    def begin(self, ctx):
        import bpy
        self._ctx = ctx
        info = ctx.region.info
        view = ctx.view
        t0 = time.perf_counter()
        H, ID, BIO = self.grids(ctx.region, ctx.gen, view, ctx.world)
        COL = self.column_colors(ID, BIO, H, ctx.gen, ctx.world, view, ctx.pack_dir, ctx.assets_dir)
        self._H, self._COL = H, COL
        self._prep_time = time.perf_counter() - t0
        k = int(view.get('chunks_per_object', 1))
        self._coll = self._collection(bpy, ctx)
        self._mat = self._material(bpy)
        groups = []
        for gz in range(0, info.nz, k):
            for gx in range(0, info.nx, k):
                chunks = [(info.cx0 + i, info.cz0 + j) for j in range(gz, min(gz + k, info.nz)) for i in range(gx, min(gx + k, info.nx))]
                if ctx.changed is not None and not any(c in ctx.changed for c in chunks):
                    continue
                groups.append((gx, gz, min(gx + k, info.nx), min(gz + k, info.nz)))
        self._groups = groups
        self._i = 0
        self._stats = {'objects': 0, 'vertices': 0, 'faces': 0, 'rebuilt': 0}
        self._step = 1
        cols = sum((g[2] - g[0]) * (g[3] - g[1]) * 256 for g in groups)
        if cols > 4_000_000:                                    # очень большие области: предпросмотр с прореживанием
            self._step = 2 if cols <= 16_000_000 else 4

    def _collection(self, bpy, ctx):
        name = ctx.collection_name
        coll = bpy.data.collections.get(name)
        if coll is None:
            coll = bpy.data.collections.new(name)
        if coll.name not in ctx.scene.collection.children:
            ctx.scene.collection.children.link(coll)
        coll['mcgen_cx0'] = ctx.region.info.cx0
        coll['mcgen_cz0'] = ctx.region.info.cz0
        coll['mcgen_min_y'] = ctx.region.info.min_y
        coll['mcgen_sink'] = self.name
        return coll

    @staticmethod
    def _material(bpy):
        mat = bpy.data.materials.get('MCGen Preview')
        if mat is not None:
            return mat
        mat = bpy.data.materials.new('MCGen Preview')
        mat.use_nodes = True
        nt = mat.node_tree
        for n in list(nt.nodes):
            if n.type not in ('OUTPUT_MATERIAL', 'BSDF_PRINCIPLED'):
                nt.nodes.remove(n)
        bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None) or nt.nodes.new('ShaderNodeBsdfPrincipled')
        out = next((n for n in nt.nodes if n.type == 'OUTPUT_MATERIAL'), None) or nt.nodes.new('ShaderNodeOutputMaterial')
        attr = nt.nodes.new('ShaderNodeVertexColor')
        attr.layer_name = VCOL
        nt.links.new(attr.outputs['Color'], bsdf.inputs['Base Color'])
        if 'Roughness' in bsdf.inputs:
            bsdf.inputs['Roughness'].default_value = 1.0
        if not bsdf.outputs['BSDF'].links:
            nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
        mat['mcgen'] = True
        return mat

    @property
    def progress(self):
        return 1.0 if not self._groups else min(1.0, self._i / len(self._groups))

    def stats(self):
        return dict(self._stats)

    def step(self, budget_s):
        import bpy
        t_end = time.perf_counter() + budget_s
        ctx = self._ctx
        info = ctx.region.info
        while self._i < len(self._groups):
            gx0, gz0, gx1, gz1 = self._groups[self._i]
            self._i += 1
            x0, z0, x1, z1 = gx0 * 16, gz0 * 16, gx1 * 16, gz1 * 16
            st = self._step
            H, COL = self._H, self._COL
            if st > 1:
                Hs = H[::st, ::st]
                Cs = COL[::st, ::st]
                verts, quads, colors = self.build_arrays(Hs, Cs, x0 // st, z0 // st, x1 // st, z1 // st)
                verts[:, 0] *= st
                verts[:, 1] *= st
            else:
                verts, quads, colors = self.build_arrays(H, COL, x0, z0, x1, z1)
            cx, cz = info.cx0 + gx0, info.cz0 + gz0
            name = f'MC Chunk {cx},{cz}'
            self._make_object(bpy, name, verts, quads, colors, (x0, -z0, 0.0))
            self._stats['objects'] += 1
            self._stats['rebuilt'] += 1
            self._stats['vertices'] += len(verts)
            self._stats['faces'] += len(quads)
            if time.perf_counter() >= t_end:
                break
        return self._i >= len(self._groups)

    def _make_object(self, bpy, name, verts, quads, colors, loc):
        # заменяем прежний объект с этим именем (Update Layers пересобирает только изменившиеся группы)
        old = bpy.data.objects.get(name)
        if old is not None:
            old_mesh = old.data
            bpy.data.objects.remove(old, do_unlink=True)
            if old_mesh and old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
        mesh = bpy.data.meshes.new(name)
        nv, nf = len(verts), len(quads)
        mesh.vertices.add(nv)
        mesh.vertices.foreach_set('co', verts.ravel())
        mesh.loops.add(nf * 4)
        mesh.loops.foreach_set('vertex_index', quads.ravel())
        mesh.polygons.add(nf)
        mesh.polygons.foreach_set('loop_start', np.arange(0, nf * 4, 4, dtype=np.int32))
        mesh.update(calc_edges=True)
        ca = mesh.color_attributes.new(VCOL, 'FLOAT_COLOR', 'POINT')
        rgba = np.ones((nv, 4), np.float32)
        rgba[:, :3] = _srgb_to_linear(colors)
        ca.data.foreach_set('color', rgba.ravel())
        mesh.materials.append(self._mat)
        mesh[TAG] = True
        obj = bpy.data.objects.new(name, mesh)
        obj.location = loc
        obj[TAG] = True
        obj[STAMP] = next(_stamps)
        self._coll.objects.link(obj)
        return obj

    def clear(self, scene, collection_name):
        return clear_preview(scene, collection_name)


def clear_preview(scene, collection_name):
    """Удаляет объекты и меши предпросмотра (помеченные TAG) и пустую коллекцию. Возвращает число удалённых объектов."""
    import bpy
    n = 0
    for obj in [o for o in bpy.data.objects if o.get(TAG) or o.get('mcgen')]:
        mesh = obj.data if obj.type == 'MESH' else None
        bpy.data.objects.remove(obj, do_unlink=True)
        n += 1
        if mesh is not None and mesh.users == 0:
            bpy.data.meshes.remove(mesh)
    for me in [m for m in bpy.data.meshes if m.get(TAG) and m.users == 0]:
        bpy.data.meshes.remove(me)
    coll = bpy.data.collections.get(collection_name)
    if coll is not None and not coll.objects and not coll.children:
        bpy.data.collections.remove(coll)
    return n

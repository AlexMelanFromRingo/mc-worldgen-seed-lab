"""Построение сцены Blender из массивов блоков/биомов: один объект-меш на чанк (или на N×N чанков), 4 материала, обновление одного
чанка. Блоки — данные (u16 на чанк), а не объекты Blender; меш строит C-ядро (mesh/mesher.py), сюда попадают готовые массивы.

    sb = SceneBuilder(view_settings)
    sb.build(blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names=None, progress=None)
    sb.update_chunk(cx, cz)          # пересобрать меш чанка (и его группы)
    sb.update_chunks([(cx, cz), …])  # то же для набора (границы между чанками — вызывающая сторона добавляет соседей)
    sb.clear()

Опции вида (ViewSettings): `chunks_per_object` (1|2|4|8), `merge_flat` (жадное слияние граней: второй объект группы со «слитыми» гранями и
материалами с повтором тайла), `lod` + `lod_distance` + `lod_stride` (дальние группы — карта высот без текстур; центр — `set_lod_center`).
"""
import concurrent.futures
import os
import time

import bpy
import numpy as np

from ..assets import state_table
from ..mesh import lod as lod_mod
from ..mesh.mesher import MeshOptions, Mesher
from . import materials as materials_mod
from .settings import ViewSettings

__all__ = ['SceneBuilder', 'ViewSettings', 'face_info']

FACE_DIR_BITS = 3
FACE_BLOCK_BITS = 17
CHUNK_SHIFT = FACE_DIR_BITS + FACE_BLOCK_BITS


def face_info(code):
    """Распаковка атрибута `mc_face` грани -> (индекс чанка в группе, индекс блока ((y*16)+z)*16+x, направление 0..5)."""
    code = int(code)
    return code >> CHUNK_SHIFT, (code >> FACE_DIR_BITS) & ((1 << FACE_BLOCK_BITS) - 1), code & ((1 << FACE_DIR_BITS) - 1)


class _Part:
    """Один объект Blender группы: основной / со слитыми гранями / LOD."""
    __slots__ = ('obj', 'mesh', 'face_code', 'n_quads', 'kind')

    def __init__(self, kind):
        self.obj = None
        self.mesh = None
        self.face_code = None
        self.n_quads = 0
        self.kind = kind


class _Group:
    """Группа N×N чанков: основной объект, объект слитых граней (если включено) либо LOD-объект."""
    __slots__ = ('key', 'chunks', 'cache', 'parts', 'lod')

    def __init__(self, key):
        self.key = key
        self.chunks = []        # упорядоченный список ключей чанков (индекс = номер в атрибуте mc_face)
        self.cache = {}         # (cx, cz) -> (MeshData, MeshData|None) — только если группа из нескольких чанков
        self.parts = {}
        self.lod = False

    # совместимость с простым случаем (основной объект)
    @property
    def obj(self):
        p = self.parts.get('main') or self.parts.get('lod')
        return p.obj if p else None

    @property
    def mesh(self):
        p = self.parts.get('main') or self.parts.get('lod')
        return p.mesh if p else None

    @property
    def face_code(self):
        p = self.parts.get('main')
        return p.face_code if p else None

    @property
    def n_quads(self):
        return sum(p.n_quads for p in self.parts.values())


def _info_get(info, name, default=None):
    if info is None:
        return default
    if isinstance(info, dict):
        return info.get(name, default)
    return getattr(info, name, default)


class SceneBuilder:
    def __init__(self, view_settings=None):
        self.vs = ViewSettings.coerce(view_settings if view_settings is not None else ViewSettings())
        self.table = None
        self.mesher = None
        self.materials = None
        self.materials_tiled = None
        self.material_lod = None
        self.blocks = {}
        self.biomes = {}
        self.min_y = -64
        self.height = 384
        self.groups = {}
        self.chunk_group = {}
        self.collection = None
        self.stats = {}
        self._remap = None
        self._biome_names = None
        self._tint_flag = True
        self.biome_colors = None
        self._executor = None
        self._gen = None
        self.edit = None
        self.region = None            # (cx0, cz0, nx, nz) либо None
        self.lod_center = None        # (cx, cz)
        self._lod_heights = {}

    # ------------------------------------------------------------------------------------------------------------------
    #                                                  ресурсы
    # ------------------------------------------------------------------------------------------------------------------
    def mesh_options(self):
        vs = self.vs
        return MeshOptions(cutout_leaves=vs.cutout_leaves, bake_shade=vs.bake_shade, blender_axes=True, blender_uv=True,
                           blend_radius=int(vs.biome_blend), scale=float(vs.scale), merge=bool(vs.merge_flat),
                           no_water=(vs.water_style == 'HIDDEN'), y_min=vs.y_min, y_max=vs.y_max)

    def _flat_biome_colors(self, bc):
        """tint_biomes=False: все биомы получают цвета «равнин» (единый нейтральный оттенок трава/листва/вода), без шума болот."""
        import copy
        flat = copy.copy(bc)
        names = list(bc.names)
        row = names.index('minecraft:plains') if 'minecraft:plains' in names else 0
        flat.rgb = np.repeat(bc.rgb[row:row + 1], bc.rgb.shape[0], axis=0).copy()
        flat.mod = np.zeros_like(bc.mod)
        return flat

    def prepare(self, blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names=None, progress=None):
        """Публичная подготовка (без меширования): ресурсы (таблица состояний, атлас, материалы, цвета биомов), параметры области из
        `region_info` и массивы чанков (без копий, если нумерация состояний совпадает). После неё работают update_chunk(s)/build_iter."""
        self._ensure_resources(block_names, biome_names, progress)
        self.min_y = int(_info_get(region_info, 'min_y', -64))
        self.height = int(_info_get(region_info, 'height', 384))
        self.edit = None
        self._lod_heights = {}
        self.set_chunks(blocks_by_chunk, biomes_by_chunk)
        cx0, cz0, nx, nz = (_info_get(region_info, n) for n in ('cx0', 'cz0', 'nx', 'nz'))
        if None not in (cx0, cz0, nx, nz):
            self.region = (cx0, cz0, nx, nz)
            if self.lod_center is None:
                self.lod_center = (cx0 + nx // 2, cz0 + nz // 2)
        else:
            self.region = None
            ks = list(self.blocks)
            if ks and self.lod_center is None:
                self.lod_center = (sum(k[0] for k in ks) // len(ks), sum(k[1] for k in ks) // len(ks))
        return self

    def _ensure_resources(self, block_names, biome_names, progress=None):
        vs = self.vs
        if self.table is None:
            t0 = time.time()
            self.table = state_table.load(vs.assets_dir, vs.pack_dir, vs.cache_dir or os.path.join(vs.pack_dir, '..', 'mcgen-cache'),
                                          version=vs.version or None, progress=progress)
            self.stats['table_seconds'] = time.time() - t0
        t = self.table
        if biome_names is None:
            bdir = os.path.join(vs.pack_dir, 'data', 'minecraft', 'worldgen', 'biome')
            biome_names = sorted('minecraft:' + f[:-5] for f in os.listdir(bdir) if f.endswith('.json')) if os.path.isdir(bdir) else ['minecraft:plains']
        biome_names = list(biome_names)
        tint = bool(vs.tint_biomes)
        if biome_names != self._biome_names or self.mesher is None or tint != self._tint_flag:
            self._biome_names = biome_names
            self._tint_flag = tint
            t.set_biomes(biome_names, vs.assets_dir, vs.pack_dir)
            self.biome_colors = t.biome_colors if tint else self._flat_biome_colors(t.biome_colors)
            self.mesher = Mesher(t, self.biome_colors, self.mesh_options())
        else:
            self.mesher.opt = self.mesh_options()
        # перекодировка состояний, если нумерация вызывающей стороны отличается от blocks.json
        self._remap = None
        if block_names is not None and len(block_names) > 0:
            bn = list(block_names)
            same = len(bn) == len(t.names)
            if same:
                for i in range(0, len(bn), max(1, len(bn) // 997)):
                    if state_table.StateTable._canon(bn[i]) != state_table.StateTable._canon(t.names[i]):
                        same = False
                        break
            if not same:
                rm = np.zeros(len(bn), dtype=np.uint16)
                for i, n in enumerate(bn):
                    j = t.state_id(n)
                    rm[i] = j if j >= 0 else 0
                self._remap = rm
        cd = vs.cache_dir or '.'
        self.materials = materials_mod.ensure_materials(t, cd, vs.shading, vs.pixel_style, vs.water_style)
        self.materials_tiled = materials_mod.ensure_tiled_materials(t, cd, vs.shading, vs.pixel_style, vs.water_style) if vs.merge_flat else None
        self.material_lod = materials_mod.ensure_lod_material(vs.shading) if vs.lod else None

    def _get_collection(self):
        name = self.vs.collection or 'MC World'
        col = bpy.data.collections.get(name)
        if col is None:
            col = bpy.data.collections.new(name)
        if col.name not in bpy.context.scene.collection.children:
            try:
                bpy.context.scene.collection.children.link(col)
            except RuntimeError:
                pass
        self.collection = col
        return col

    # ------------------------------------------------------------------------------------------------------------------
    #                                                 построение
    # ------------------------------------------------------------------------------------------------------------------
    def _y_lo(self):
        y = self.vs.y_min
        return 0 if y is None else max(0, int(y) - self.min_y)

    def _y_hi(self):
        y = self.vs.y_max
        return None if y is None else int(y) - self.min_y

    def _n(self):
        return max(1, int(self.vs.chunks_per_object))

    def _group_is_lod(self, gk, chunks):
        vs = self.vs
        if not vs.lod or self.lod_center is None:
            return False
        cx, cz = self.lod_center
        d = min(max(abs(k[0] - cx), abs(k[1] - cz)) for k in chunks)
        return d > int(vs.lod_distance)

    def set_lod_center(self, cx, cz):
        """Задаёт центр, от которого считается расстояние до LOD (чанки). Возвращает число пересобранных групп."""
        self.lod_center = (int(cx), int(cz))
        if not self.groups:
            return 0
        n = 0
        col = self._get_collection()
        for gk, g in list(self.groups.items()):
            want = self._group_is_lod(gk, g.chunks)
            if want != g.lod:
                self._rebuild_group(gk, g.chunks, col)
                n += 1
        return n

    def build(self, blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names=None, progress=None):
        """Строит сцену целиком (блокирующий вызов; для неблокирующей формы — `build_iter`). Возвращает сводку."""
        for f in self.build_iter(blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names, progress):
            if progress:
                progress(f, 'меши')
        return self.stats['build']

    def abort(self):
        """Прерывает начатый build_iter (освобождает потоки)."""
        g, self._gen = self._gen, None
        if g is not None:
            g.close()

    def build_iter(self, blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names=None, progress=None):
        """Итеративная сборка сцены: генератор, отдающий долю готовности (float 0..1) после каждого чанка/группы. Один шаг — не более
        одного чанка меширования (в потоках) + создание одного объекта Blender, поэтому вызывающий может держать UI живым, вызывая
        `next()` в модальном таймере с бюджетом времени. Сводка — в `self.stats['build']` после исчерпания. Прервать — `abort()`."""
        t_start = time.time()
        self.clear()
        self.prepare(blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names, progress)
        keys = sorted(self.blocks.keys())
        if self.region is not None:
            cx0, cz0, nx, nz = self.region
            keys = [k for k in keys if cx0 <= k[0] < cx0 + nx and cz0 <= k[1] < cz0 + nz]
        N = self._n()
        by_group = {}
        for k in keys:
            by_group.setdefault((k[0] // N, k[1] // N), []).append(k)
        col = self._get_collection()
        t_mesh = t_fill = 0.0
        total = len(keys)
        done = 0
        nthreads = self.vs.threads_resolved()
        lod_groups = {gk for gk, v in by_group.items() if self._group_is_lod(gk, v)}
        work = []
        for gk in sorted(by_group):
            if gk in lod_groups:
                continue
            for ck in by_group[gk]:
                work.append((gk, ck))
        n_full = len(work)
        n_lod = len(lod_groups)
        steps_total = max(1, n_full + n_lod)
        self._executor = concurrent.futures.ThreadPoolExecutor(max_workers=nthreads) if nthreads > 1 else None
        self._gen = None
        try:
            pending = {}
            window = nthreads * 3

            def submit(i):
                gk, ck = work[i]
                pending[i] = self._executor.submit(self._mesh_one, ck) if self._executor is not None else None

            for i in range(min(window, len(work))):
                submit(i)
            nxt = min(window, len(work))
            gm = {}
            gk_remaining = {gk: len(v) for gk, v in by_group.items()}
            for i in range(len(work)):
                gk, ck = work[i]
                t0 = time.time()
                fut = pending.pop(i)
                md = fut.result() if fut is not None else self._mesh_one(ck)
                t_mesh += time.time() - t0
                if nxt < len(work):
                    submit(nxt)
                    nxt += 1
                gm.setdefault(gk, []).append((ck, md))
                gk_remaining[gk] -= 1
                done += 1
                if gk_remaining[gk] == 0:
                    t1 = time.time()
                    self._make_group(gk, gm.pop(gk), col)
                    t_fill += time.time() - t1
                yield 0.02 + 0.93 * done / steps_total
            t1 = time.time()
            for j, gk in enumerate(sorted(lod_groups)):
                self._make_group_lod(gk, by_group[gk], col)
                yield 0.02 + 0.93 * (n_full + j + 1) / steps_total
            t_lod = time.time() - t1
        finally:
            ex, self._executor = self._executor, None
            if ex is not None:
                ex.shutdown(wait=True, cancel_futures=True)
        self.stats['build'] = {'chunks': total, 'full_chunks': n_full, 'lod_groups': len(lod_groups), 'groups': len(self.groups),
                               'seconds': time.time() - t_start, 'mesh_wait_seconds': t_mesh, 'blender_seconds': t_fill, 'lod_seconds': t_lod,
                               'quads': int(sum(g.n_quads for g in self.groups.values())), 'threads': nthreads}
        yield 1.0

    def set_chunks(self, blocks_by_chunk, biomes_by_chunk):
        """Запоминает массивы чанков (без копий, либо с перекодировкой состояний)."""
        h = self.height
        self.blocks = {}
        self.biomes = {}
        for k, a in blocks_by_chunk.items():
            a = np.asarray(a)
            if a.size != h * 256:
                raise ValueError('чанк %s: размер блоков %d != %d' % (k, a.size, h * 256))
            if self._remap is not None:
                a = self._remap[a.reshape(-1)]
            elif a.dtype != np.uint16 or not a.flags['C_CONTIGUOUS']:
                a = np.ascontiguousarray(a, dtype=np.uint16)
            self.blocks[k] = a.reshape(-1)
        if biomes_by_chunk:
            for k, b in biomes_by_chunk.items():
                if b is not None:
                    self.biomes[k] = np.ascontiguousarray(b, dtype=np.uint8).reshape(-1)

    def set_chunk(self, cx, cz, blocks, biomes=None):
        """Добавляет/заменяет чанк (без пересборки меша)."""
        a = np.asarray(blocks)
        if self._remap is not None:
            a = self._remap[a.reshape(-1)]
        self.blocks[(cx, cz)] = a.reshape(-1) if a.dtype == np.uint16 else np.ascontiguousarray(a, dtype=np.uint16).reshape(-1)
        if biomes is not None:
            self.biomes[(cx, cz)] = np.ascontiguousarray(biomes, dtype=np.uint8).reshape(-1)
        self._lod_heights.pop((cx, cz), None)

    def _mesh_one(self, ck):
        md = self.mesher.mesh_chunk(ck[0], ck[1], self.blocks, self.biomes, self.min_y, self.height)
        if self.vs.merge_flat:
            return md.split_merged()
        return md, None

    # ------------------------------------------------------------------------------------------------------------------
    #                                                  Blender-меши
    # ------------------------------------------------------------------------------------------------------------------
    def _group_offset(self, gk, ck):
        N = self._n()
        s = float(self.vs.scale)
        return ((ck[0] - gk[0] * N) * 16 * s, -(ck[1] - gk[1] * N) * 16 * s)

    def _concat(self, gk, mds):
        """Объединяет меши чанков группы (список (ck, MeshData|None)) в общие массивы; позиции сдвигаются на смещение чанка в группе.
        -> (pos, uv, col, mat, rect, code)."""
        pos_l, uv_l, col_l, mat_l, code_l, rect_l = [], [], [], [], [], []
        multi = len(mds) > 1
        for ci, (ck, md) in enumerate(mds):
            if md is None or md.n_quads == 0:
                continue
            ox, oy = self._group_offset(gk, ck)
            p = md.pos
            if ox or oy:
                p = p.copy()
                p[:, :, 0] += ox
                p[:, :, 1] += oy
            pos_l.append(p)
            uv_l.append(md.uv)
            col_l.append(md.col)
            mat_l.append(md.mat)
            rect_l.append(md.rect)
            c = (md.block.astype(np.int32) << FACE_DIR_BITS) | md.dir.astype(np.int32)
            if multi:
                c = c | (ci << CHUNK_SHIFT)
            code_l.append(c)
        if not pos_l:
            z = np.zeros
            return (z((0, 4, 3), np.float32), z((0, 4, 2), np.float32), z((0, 4, 4), np.uint8), z(0, np.uint8), z((0, 4), np.float32),
                    z(0, np.int32))
        if len(pos_l) == 1:
            return pos_l[0], uv_l[0], col_l[0], mat_l[0], rect_l[0], code_l[0].astype(np.int32)
        return (np.concatenate(pos_l), np.concatenate(uv_l), np.concatenate(col_l), np.concatenate(mat_l), np.concatenate(rect_l),
                np.concatenate(code_l).astype(np.int32))

    _IDX = {'cap': 0}

    @classmethod
    def _index_arrays(cls, n):
        """Предвычисленные индексы для n четырёхугольников без общих вершин: (вершины/углы 0..4n-1, начала граней, рёбра (k, следующая в грани))."""
        d = cls._IDX
        if d['cap'] < n:
            cap = max(n, 8192, d['cap'] * 2)
            ar = np.arange(cap * 4, dtype=np.int32)
            q = ar.reshape(cap, 4)
            d.update(cap=cap, ar=ar, ls=np.arange(0, cap * 4, 4, dtype=np.int32),
                     ev=np.ascontiguousarray(np.stack([q, np.roll(q, -1, axis=1)], axis=2).reshape(-1)))
        return d['ar'][:4 * n], d['ls'][:n], d['ev'][:8 * n]

    @classmethod
    def _fill_mesh(cls, mesh, pos, uv, col, mat, code, rect=None):
        """Заполняет меш n четырёхугольников (4 своих вершины на грань). Быстрый путь — через атрибуты (position, .corner_vert, .edge_verts,
        .corner_edge, UVMap, material_index): на порядок быстрее RNA-доступа и mesh.update(calc_edges=True). Цвет граней — атрибут `Col`
        (BYTE_COLOR, область FACE: цвет у грани один, 4× меньше данных)."""
        n = int(mat.shape[0])
        mesh.clear_geometry()
        if n == 0:
            return
        ar, ls, ev = cls._index_arrays(n)
        mesh.vertices.add(n * 4)
        mesh.loops.add(n * 4)
        mesh.polygons.add(n)
        mesh.edges.add(n * 4)
        a = mesh.attributes
        try:
            a['position'].data.foreach_set('vector', np.ascontiguousarray(pos, dtype=np.float32).reshape(-1))
            a['.corner_vert'].data.foreach_set('value', ar)
            a['.edge_verts'].data.foreach_set('value', ev)
            a['.corner_edge'].data.foreach_set('value', ar)
            mesh.polygons.foreach_set('loop_start', ls)
            mesh.update()
            fast = True
        except (KeyError, RuntimeError, TypeError):
            fast = False
        if not fast:
            mesh.clear_geometry()
            mesh.vertices.add(n * 4)
            mesh.vertices.foreach_set('co', np.ascontiguousarray(pos, dtype=np.float32).reshape(-1))
            mesh.loops.add(n * 4)
            mesh.loops.foreach_set('vertex_index', ar)
            mesh.polygons.add(n)
            mesh.polygons.foreach_set('loop_start', ls)
            mesh.update(calc_edges=True)
        if uv is not None:
            if fast:
                ua = a.new('UVMap', 'FLOAT2', 'CORNER')
                ua.data.foreach_set('vector', np.ascontiguousarray(uv, dtype=np.float32).reshape(-1))
            else:
                uvl = mesh.uv_layers.new(name='UVMap')
                uvl.data.foreach_set('uv', np.ascontiguousarray(uv, dtype=np.float32).reshape(-1))
        ca = a.new('Col', 'BYTE_COLOR', 'FACE')
        ca.data.foreach_set('color_srgb', np.ascontiguousarray(col[:, 0, :]).reshape(-1).astype(np.float32) * np.float32(1.0 / 255.0))
        if mat is not None:
            if fast:
                ma = a.get('material_index') or a.new('material_index', 'INT', 'FACE')
                ma.data.foreach_set('value', mat.astype(np.int32))
            else:
                mesh.polygons.foreach_set('material_index', mat.astype(np.int32))
        if code is not None:
            fa = a.new('mc_face', 'INT', 'FACE')
            fa.data.foreach_set('value', code.astype(np.int32))
        if rect is not None:
            ra = a.new('Rect', 'FLOAT_COLOR', 'FACE')
            ra.data.foreach_set('color', np.ascontiguousarray(rect, dtype=np.float32).reshape(-1))

    def _link_part(self, g, kind, name, mesh, materials, loc, col):
        obj = bpy.data.objects.new(name, mesh)
        obj.location = loc
        obj['mc_group'] = list(g.key)
        obj['mc_part'] = kind
        col.objects.link(obj)
        p = _Part(kind)
        p.obj, p.mesh = obj, mesh
        for m in materials:
            mesh.materials.append(m)
        g.parts[kind] = p
        return p

    def _group_loc(self, gk):
        N = self._n()
        s = float(self.vs.scale)
        return (gk[0] * N * 16 * s, -gk[1] * N * 16 * s, self.min_y * s)

    def _make_group(self, gk, chunk_meshes, col):
        g = _Group(gk)
        g.chunks = [ck for ck, _ in chunk_meshes]
        loc = self._group_loc(gk)
        self._fill_group_parts(g, [(ck, m[0]) for ck, m in chunk_meshes], [(ck, m[1]) for ck, m in chunk_meshes], loc, col)
        if self._n() > 1:
            g.cache = {ck: m for ck, m in chunk_meshes}
        self.groups[gk] = g
        for ck in g.chunks:
            self.chunk_group[ck] = gk

    def _fill_group_parts(self, g, main_mds, merged_mds, loc, col):
        gk = g.key
        pos, uv, c, mat, rect, code = self._concat(gk, main_mds)
        name = 'mc_%d_%d' % gk
        p = g.parts.get('main')
        if p is None:
            mesh = bpy.data.meshes.new(name)
            p = self._link_part(g, 'main', name, mesh, self.materials, loc, col)
        self._fill_mesh(p.mesh, pos, uv, c, mat, code)
        p.face_code, p.n_quads = code, int(mat.shape[0])
        if self.vs.merge_flat:
            pos, uv, c, mat, rect, code = self._concat(gk, merged_mds)
            n = int(mat.shape[0])
            pm = g.parts.get('merged')
            if n == 0:
                if pm is not None:
                    self._remove_part(g, 'merged')
            else:
                if pm is None:
                    mesh = bpy.data.meshes.new(name + '_m')
                    pm = self._link_part(g, 'merged', name + '_m', mesh, self.materials_tiled, loc, col)
                self._fill_mesh(pm.mesh, pos, uv, c, mat, code, rect)
                pm.face_code, pm.n_quads = code, n

    def _remove_part(self, g, kind):
        p = g.parts.pop(kind, None)
        if p is None:
            return
        if p.obj is not None and p.obj.name in bpy.data.objects:
            bpy.data.objects.remove(p.obj, do_unlink=True)
        if p.mesh is not None and p.mesh.name in bpy.data.meshes:
            bpy.data.meshes.remove(p.mesh)

    def _remove_group(self, gk):
        g = self.groups.pop(gk, None)
        if g is None:
            return
        for kind in list(g.parts):
            self._remove_part(g, kind)
        for ck in g.chunks:
            if self.chunk_group.get(ck) == gk:
                del self.chunk_group[ck]

    def _make_group_lod(self, gk, chunks, col):
        g = _Group(gk)
        g.chunks = list(chunks)
        g.lod = True
        vs = self.vs
        pos, c = lod_mod.build_lod_group(self.table, self.biome_colors or self.table.biome_colors, self.blocks, self.biomes, chunks, gk, self._n(), self.min_y, self.height,
                                         stride=int(vs.lod_stride), scale=float(vs.scale), heights_cache=self._lod_heights,
                                         y_lo=self._y_lo(), y_hi=self._y_hi(), hide_water=(vs.water_style == 'HIDDEN'))
        name = 'mc_lod_%d_%d' % gk
        mesh = bpy.data.meshes.new(name)
        p = self._link_part(g, 'lod', name, mesh, [self.material_lod or materials_mod.ensure_lod_material(vs.shading)], self._group_loc(gk), col)
        n = pos.shape[0]
        self._fill_mesh(mesh, pos, None, c, np.zeros(n, dtype=np.uint8), None)
        p.n_quads = n
        self.groups[gk] = g
        for ck in g.chunks:
            self.chunk_group[ck] = gk

    def _rebuild_group(self, gk, chunks, col):
        """Полная пересборка группы (смена режима full ↔ LOD)."""
        self._remove_group(gk)
        if self._group_is_lod(gk, chunks):
            self._make_group_lod(gk, chunks, col)
        else:
            mds = [(ck, self._mesh_one(ck)) for ck in chunks]
            self._make_group(gk, mds, col)

    # ------------------------------------------------------------------------------------------------------------------
    #                                                  обновление
    # ------------------------------------------------------------------------------------------------------------------
    def update_chunk(self, cx, cz):
        """Пересобирает меш чанка (для групп N>1 — меш группы из кэша мешей остальных чанков). Возвращает затраченные секунды."""
        return self.update_chunks([(cx, cz)])

    def update_chunks(self, keys):
        """Пересобирает меши группы(групп) заданных чанков (блокирующий вызов). Возвращает секунды."""
        t0 = time.time()
        for _ in self.update_chunks_iter(keys):
            pass
        return time.time() - t0

    def update_chunks_iter(self, keys):
        """То же, что update_chunks, но генератор: после каждой группы отдаёт долю 0..1 (для неблокирующего UI)."""
        t0 = time.time()
        N = self._n()
        col = self._get_collection()
        keys = list(keys)
        touched = {}
        for ck in keys:
            if ck not in self.blocks:
                continue
            gk = self.chunk_group.get(ck, (ck[0] // N, ck[1] // N))
            touched.setdefault(gk, []).append(ck)
        for ck in keys:
            self._lod_heights.pop(ck, None)
        for gi, (gk, cks) in enumerate(touched.items()):
            yield gi / max(1, len(touched))
            g = self.groups.get(gk)
            if g is None:
                self._rebuild_group(gk, cks, col)
                continue
            if g.lod:
                self._remove_group(gk)
                self._make_group_lod(gk, g.chunks, col)
                continue
            loc = self._group_loc(gk)
            if N == 1:
                ck = cks[0]
                m = self._mesh_one(ck)
                self._fill_group_parts(g, [(ck, m[0])], [(ck, m[1])], loc, col)
            else:
                for ck in cks:
                    g.cache[ck] = self._mesh_one(ck)
                    if ck not in g.chunks:
                        g.chunks.append(ck)
                        self.chunk_group[ck] = gk
                self._fill_group_parts(g, [(ck, g.cache[ck][0]) for ck in g.chunks], [(ck, g.cache[ck][1]) for ck in g.chunks], loc, col)
        self.stats['last_update'] = {'chunks': len(keys), 'seconds': time.time() - t0}
        yield 1.0

    # ------------------------------------------------------------------------------------------------------------------
    #                                           выбор грани (используется picking.py)
    # ------------------------------------------------------------------------------------------------------------------
    def get_edit_session(self):
        """Сессия правок (mesh/edit.py), привязанная к массивам блоков этой сцены (правки идут прямо в них)."""
        if self.edit is None or self.edit.blocks is not self.blocks:
            from ..mesh.edit import EditSession
            self.edit = EditSession(self.table, self.blocks, self.min_y, self.height, self.vs.pack_dir)
        return self.edit

    def group_of_object(self, obj):
        gk = obj.get('mc_group') if obj is not None else None
        return self.groups.get(tuple(gk)) if gk is not None else None

    def part_of_object(self, obj):
        g = self.group_of_object(obj)
        if g is None:
            return None, None
        return g, g.parts.get(obj.get('mc_part', 'main'))

    def resolve_face(self, obj, poly_index):
        """Грань объекта -> (cx, cz, x, y, z, dir) в локальных координатах чанка (y от нижней границы мира) либо None.
        Для слитых граней блок — первая ячейка прямоугольника (точный блок даёт picking по точке попадания); для LOD — None."""
        g, p = self.part_of_object(obj)
        if g is None or p is None or p.kind == 'lod' or p.face_code is None or poly_index < 0 or poly_index >= p.n_quads:
            return None
        ci, blk, d = face_info(p.face_code[poly_index])
        ck = g.chunks[ci] if len(g.chunks) > 1 else g.chunks[0]
        return ck[0], ck[1], blk & 15, blk >> 8, (blk >> 4) & 15, d

    def all_objects(self):
        for g in self.groups.values():
            for p in g.parts.values():
                if p.obj is not None:
                    yield g, p

    # ------------------------------------------------------------------------------------------------------------------
    def clear(self, full=False):
        """Удаляет объекты и меши сцены; full=True — ещё материалы и изображение атласа."""
        from . import banner_overlay
        banner_overlay.remove_all()
        for gk in list(self.groups):
            self._remove_group(gk)
        self.groups.clear()
        self.chunk_group.clear()
        if self.collection is not None and self.collection.name in bpy.data.collections and len(self.collection.objects) == 0:
            bpy.data.collections.remove(self.collection)
        self.collection = None
        self._lod_heights = {}
        if full:
            materials_mod.remove_materials()
            self.materials = self.materials_tiled = self.material_lod = None
            self.blocks = {}
            self.biomes = {}

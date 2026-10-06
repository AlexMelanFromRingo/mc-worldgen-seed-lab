"""Обёртка C-ядра меширования (libmcgen/src/mesh) через ctypes + доступ к эталонной реализации (mesh/reference.py).

    from mcgen_addon.mesh.mesher import Mesher, MeshOptions
    m = Mesher(table, biome_colors, MeshOptions())
    data = m.mesh_chunk(cx, cz, blocks_by_chunk, biomes_by_chunk, min_y, height)   # -> MeshData (numpy)

Библиотека ищется так: переменная MCGEN_MESH_LIB; основная libmcgen (там же, если ядро собрано вместе с ней); lib/<платформа>/ аддона;
libmcgen/build/mesh_test/; если нигде нет — собирается из libmcgen/src/mesh/mesh.c локальным компилятором (cc/gcc/clang/zig cc) в кэш.
Без bpy.
"""
import ctypes
import os
import platform
import shutil
import subprocess
import sys
import tempfile

import numpy as np

__all__ = ['MeshOptions', 'MeshData', 'Mesher', 'load_library', 'build_core', 'MAT_SOLID', 'MAT_CUTOUT', 'MAT_TRANSLUCENT', 'MAT_WATER',
           'N_MAT', 'DIR_NAMES', 'ABI_VERSION', 'reference_mesh_chunk', 'weld_quads']

ABI_VERSION = 3
MAT_SOLID, MAT_CUTOUT, MAT_TRANSLUCENT, MAT_WATER, N_MAT = 0, 1, 2, 3, 4
DIR_NAMES = ('down', 'up', 'north', 'south', 'west', 'east')

OPT_CUTOUT_LEAVES = 1 << 0
OPT_BAKE_SHADE = 1 << 1
OPT_BLENDER_AXES = 1 << 2
OPT_BLENDER_UV = 1 << 3
OPT_NO_FLUIDS = 1 << 4
OPT_NO_MODELS = 1 << 5
OPT_AO = 1 << 6
OPT_MERGE = 1 << 7
OPT_NO_VARIANTS = 1 << 8
OPT_NO_WATER = 1 << 9
OPT_YRANGE = 1 << 10

_HERE = os.path.dirname(os.path.abspath(__file__))
_ADDON = os.path.dirname(_HERE)


def _repo_root():
    env = os.environ.get('MCGEN_ROOT')
    if env and os.path.isdir(os.path.join(env, 'libmcgen')):
        return env
    cand = os.path.dirname(os.path.dirname(_ADDON))
    return cand if os.path.isdir(os.path.join(cand, 'libmcgen', 'src', 'mesh')) else None


# ----------------------------------------------------------------------------------------------------------------------
#                                                   структуры ctypes
# ----------------------------------------------------------------------------------------------------------------------
P = ctypes.c_void_p


class _Tables(ctypes.Structure):
    _fields_ = [
        ('abi', ctypes.c_int32), ('n_states', ctypes.c_int32), ('n_biomes', ctypes.c_int32), ('n_masks', ctypes.c_int32),
        ('mangrove_roots_block', ctypes.c_int32),
        ('st_flags', P), ('st_block', P), ('st_occ', P), ('st_sturdy', P), ('st_grp_off', P), ('st_off_h', P), ('st_off_v', P),
        ('grp_list', P), ('grp_var_off', P), ('grp_total', P), ('var_baked', P), ('var_weight', P), ('baked_q_off', P),
        ('q_pos', P), ('q_uv', P), ('q_cull', P), ('q_dir', P), ('q_shade', P), ('q_tint', P), ('q_layer', P), ('q_merge', P), ('q_rgb', P),
        ('occ_masks', P), ('occ_cover', P),
        ('fluid_rect', (ctypes.c_float * 4) * 5), ('fluid_layer', ctypes.c_uint8 * 2),
        ('biome_rgb', P), ('biome_mod', P), ('swamp_perm', ctypes.c_uint8 * 256),
    ]


class _Options(ctypes.Structure):
    _fields_ = [
        ('flags', ctypes.c_uint32), ('blend_radius', ctypes.c_int32), ('cx', ctypes.c_int32), ('cz', ctypes.c_int32),
        ('min_y', ctypes.c_int32), ('n_sections', ctypes.c_int32), ('shade', ctypes.c_float * 6), ('scale', ctypes.c_float),
        ('y_offset', ctypes.c_int32), ('y_lo', ctypes.c_int32), ('y_hi', ctypes.c_int32),
    ]


class _Input(ctypes.Structure):
    _fields_ = [('blocks', P * 9), ('biomes', P * 9)]


class _Output(ctypes.Structure):
    _fields_ = [
        ('n_quads', ctypes.c_int32), ('capacity', ctypes.c_int32),
        ('pos', P), ('uv', P), ('col', P), ('mat', P), ('block', P), ('dir', P), ('merged', P), ('rect', P),
        ('n_blocks_visited', ctypes.c_int32), ('n_sections_skipped', ctypes.c_int32), ('n_merged_from', ctypes.c_int32),
        ('n_mat', ctypes.c_int32 * N_MAT),
    ]


# ----------------------------------------------------------------------------------------------------------------------
#                                                   загрузка / сборка библиотеки
# ----------------------------------------------------------------------------------------------------------------------

def _lib_names():
    if sys.platform.startswith('win'):
        return ('mcmesh.dll',)
    if sys.platform == 'darwin':
        return ('libmcmesh.dylib',)
    return ('libmcmesh.so',)


def _platform_tag():
    m = platform.machine().lower()
    arm = m in ('arm64', 'aarch64')
    if sys.platform.startswith('win'):
        return 'windows-arm64' if arm else 'windows-x64'
    if sys.platform == 'darwin':
        return 'macos-arm64' if arm else 'macos-x64'
    return 'linux-arm64' if arm else 'linux-x64'


def _candidates():
    out = []
    env = os.environ.get('MCGEN_MESH_LIB')
    if env:
        out.append(env)
    env = os.environ.get('MCGEN_LIB')
    if env:
        out.append(env)
    try:   # основная библиотека W5 (если ядро собрано в неё)
        from ..core import paths as _paths   # noqa
        p = _paths.find_library()
        if p:
            out.append(p)
    except Exception:
        pass
    for n in _lib_names():
        out.append(os.path.join(_ADDON, 'lib', _platform_tag(), n))
        out.append(os.path.join(_ADDON, 'lib', n))
    root = _repo_root()
    if root:
        for n in _lib_names():
            out.append(os.path.join(root, 'libmcgen', 'build', 'mesh_test', n))
        out.append(os.path.join(root, 'libmcgen', 'build', _platform_tag(), 'libmcgen.so'))
    out.append(os.path.join(tempfile.gettempdir(), 'mcgen-mesh-build', _lib_names()[0]))
    return out


def _find_compiler():
    for c in (os.environ.get('CC'), 'cc', 'gcc', 'clang'):
        if c and shutil.which(c):
            return [c]
    if shutil.which('zig'):
        return ['zig', 'cc']
    return None


def _source_mtime():
    """mtime исходников ядра в дереве репозитория (None, если запущено не из исходников)."""
    root = _repo_root()
    if not root:
        return None
    d = os.path.join(root, 'libmcgen', 'src', 'mesh')
    ts = [os.path.getmtime(os.path.join(d, f)) for f in ('mesh.c', 'mcgen_mesh.h') if os.path.isfile(os.path.join(d, f))]
    return max(ts) if ts else None


def build_core(out_dir=None, source=None):
    """Собирает mesh.c в разделяемую библиотеку локальным компилятором. Возвращает путь."""
    root = _repo_root()
    src = source or (os.path.join(root, 'libmcgen', 'src', 'mesh', 'mesh.c') if root else None)
    if not src or not os.path.isfile(src):
        raise RuntimeError('не найден libmcgen/src/mesh/mesh.c для сборки ядра меширования')
    cc = _find_compiler()
    if not cc:
        raise RuntimeError('нет компилятора C (cc/gcc/clang/zig) для сборки ядра меширования')
    if out_dir is None:
        out_dir = os.path.join(root, 'libmcgen', 'build', 'mesh_test') if root else os.path.join(tempfile.gettempdir(), 'mcgen-mesh-build')
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, _lib_names()[0])
    cmd = cc + ['-std=c11', '-O2', '-fPIC', '-shared', '-ffp-contract=off', '-I', os.path.dirname(src), '-o', out, src]
    if not sys.platform.startswith('win'):
        cmd.append('-lm')
    subprocess.run(cmd, check=True, capture_output=True)
    return out


_lib = None


def _bind(lib):
    lib.mcmesh_abi_version.restype = ctypes.c_int
    lib.mcmesh_chunk.argtypes = [ctypes.POINTER(_Tables), ctypes.POINTER(_Input), ctypes.POINTER(_Options), ctypes.POINTER(_Output)]
    lib.mcmesh_chunk.restype = ctypes.c_int
    lib.mcmesh_output_free.argtypes = [ctypes.POINTER(_Output)]
    lib.mcmesh_output_free.restype = None
    lib.mcmesh_mth_get_seed.argtypes = [ctypes.c_int32, ctypes.c_int32, ctypes.c_int32]
    lib.mcmesh_mth_get_seed.restype = ctypes.c_int64
    lib.mcmesh_pick_weighted.argtypes = [ctypes.c_int64, ctypes.c_int32]
    lib.mcmesh_pick_weighted.restype = ctypes.c_int
    lib.mcmesh_swamp_noise.argtypes = [P, ctypes.c_double, ctypes.c_double]
    lib.mcmesh_swamp_noise.restype = ctypes.c_float
    if hasattr(lib, 'mcmesh_weld'):          # необязательная функция (библиотеки старее v0.1.14 её не имеют — меш тогда строится без сварки)
        lib.mcmesh_weld.argtypes = [P, ctypes.c_int32, P, P, P, P, P]
        lib.mcmesh_weld.restype = ctypes.c_int
    return lib


def load_library(path=None, build=True):
    """Загружает (и при необходимости собирает) библиотеку с символами mcmesh_*; кэширует результат."""
    global _lib
    if _lib is not None and path is None:
        return _lib
    cands = [path] if path else _candidates()
    last = None
    src_t = _source_mtime() if path is None else None
    for p in cands:
        if p and os.path.isfile(p):
            if src_t is not None and os.path.getmtime(p) < src_t - 1.0:
                last = RuntimeError('библиотека %s старее исходников ядра — пропускаю (будет пересобрана)' % p)
                continue
            try:
                lib = ctypes.CDLL(p)
                if not hasattr(lib, 'mcmesh_chunk'):
                    continue
                _bind(lib)
                if lib.mcmesh_abi_version() != ABI_VERSION:
                    last = RuntimeError('ABI ядра меширования %d != %d (%s)' % (lib.mcmesh_abi_version(), ABI_VERSION, p))
                    continue
                if path is None:
                    _lib = lib
                lib._path = p
                return lib
            except OSError as e:
                last = e
    if build:
        p = build_core()
        lib = _bind(ctypes.CDLL(p))
        lib._path = p
        if path is None:
            _lib = lib
        return lib
    raise RuntimeError('библиотека ядра меширования не найдена: %s' % (last,))


def weld_quads(pos, lib=None):
    """Сварка вершин и рёбер четырёхугольников (ядро: mcmesh_weld). pos — float32 (n, 4, 3). Возвращает (corner_vert int32[4n], vert_pos float32[3·nv],
    edge_verts int32[2·ne], corner_edge int32[4n], nv, ne) или None, если библиотека без mcmesh_weld (или MCGEN_NO_WELD). Вызов освобождает GIL — безопасен
    из потоков."""
    if os.environ.get('MCGEN_NO_WELD'):
        return None
    try:
        lib = lib or load_library(build=False)
    except Exception:
        return None
    fn = getattr(lib, 'mcmesh_weld', None)
    if fn is None:
        return None
    pos = np.ascontiguousarray(pos, dtype=np.float32).reshape(-1, 4, 3)
    n = int(pos.shape[0])
    cv = np.empty(n * 4, np.int32)
    vp = np.empty(n * 12, np.float32)
    ev = np.empty(n * 8, np.int32)
    ce = np.empty(n * 4, np.int32)
    cnt = np.zeros(2, np.int32)
    rc = fn(pos.ctypes.data_as(P), n, cv.ctypes.data_as(P), vp.ctypes.data_as(P), ev.ctypes.data_as(P), ce.ctypes.data_as(P), cnt.ctypes.data_as(P))
    if rc != 0:
        return None
    nv, ne = int(cnt[0]), int(cnt[1])
    return cv, vp[:nv * 3], ev[:ne * 2], ce, nv, ne


# ----------------------------------------------------------------------------------------------------------------------
#                                                   высокоуровневый интерфейс
# ----------------------------------------------------------------------------------------------------------------------

class MeshOptions:
    """Опции меширования (см. McMeshOptions)."""

    def __init__(self, cutout_leaves=True, bake_shade=False, blender_axes=True, blender_uv=True, blend_radius=2,
                 shade=(0.5, 1.0, 0.8, 0.8, 0.6, 0.6), scale=1.0, no_fluids=False, no_models=False, y_offset=0, ao=False,
                 merge=False, no_variants=False, no_water=False, y_min=None, y_max=None):
        self.cutout_leaves, self.bake_shade, self.blender_axes, self.blender_uv = cutout_leaves, bake_shade, blender_axes, blender_uv
        self.blend_radius, self.shade, self.scale = blend_radius, tuple(shade), scale
        self.no_fluids, self.no_models, self.y_offset, self.ao = no_fluids, no_models, y_offset, ao
        self.merge, self.no_variants = merge, no_variants
        self.no_water, self.y_min, self.y_max = no_water, y_min, y_max      # y_min/y_max — мировые y (включительно) либо None

    def flags(self):
        f = 0
        if self.cutout_leaves:
            f |= OPT_CUTOUT_LEAVES
        if self.bake_shade:
            f |= OPT_BAKE_SHADE
        if self.blender_axes:
            f |= OPT_BLENDER_AXES
        if self.blender_uv:
            f |= OPT_BLENDER_UV
        if self.no_fluids:
            f |= OPT_NO_FLUIDS
        if self.no_models:
            f |= OPT_NO_MODELS
        if self.ao:
            f |= OPT_AO
        if self.merge:
            f |= OPT_MERGE
        if self.no_variants or self.merge:
            f |= OPT_NO_VARIANTS
        if self.no_water:
            f |= OPT_NO_WATER
        if self.y_min is not None or self.y_max is not None:
            f |= OPT_YRANGE
        return f

    def copy(self, **kw):
        o = MeshOptions(self.cutout_leaves, self.bake_shade, self.blender_axes, self.blender_uv, self.blend_radius, self.shade,
                        self.scale, self.no_fluids, self.no_models, self.y_offset, self.ao, self.merge, self.no_variants, self.no_water, self.y_min, self.y_max)
        for k, v in kw.items():
            setattr(o, k, v)
        return o


class MeshData:
    """Результат меширования одного чанка: четырёхугольники (4 вершины на грань, вершины не общие)."""
    __slots__ = ('pos', 'uv', 'col', 'mat', 'block', 'dir', 'stats', 'merged', 'rect')

    def __init__(self, pos, uv, col, mat, block, dir_, stats=None, merged=None, rect=None):
        self.pos, self.uv, self.col, self.mat, self.block, self.dir = pos, uv, col, mat, block, dir_
        self.stats = stats or {}
        self.merged = merged if merged is not None else np.zeros(mat.shape[0], dtype=np.uint8)   # 1 — слитая грань (тайл повторяется в шейдере)
        self.rect = rect if rect is not None else np.zeros((mat.shape[0], 4), dtype=np.float32)  # прямоугольник спрайта слитой грани

    def split_merged(self):
        """(обычные грани, слитые грани) как два MeshData."""
        m = self.merged.astype(bool)
        if not m.any():
            return self, None
        def sel(mask):
            return MeshData(self.pos[mask], self.uv[mask], self.col[mask], self.mat[mask], self.block[mask], self.dir[mask], self.stats,
                            self.merged[mask], self.rect[mask])
        return sel(~m), sel(m)

    @property
    def n_quads(self):
        return int(self.mat.shape[0])

    def block_xyz(self, i):
        """Локальные координаты блока граней i: (x, y, z) — y от нижней границы мира."""
        b = int(self.block[i])
        return b & 15, b >> 8, (b >> 4) & 15

    def canonical(self):
        """Канонизированный набор граней для сравнения (сортировка по блоку/материалу/позициям): список кортежей."""
        n = self.n_quads
        rows = []
        for i in range(n):
            rows.append((int(self.block[i]), int(self.mat[i]), int(self.dir[i]),
                         tuple(np.round(self.pos[i].reshape(-1), 4).tolist()), tuple(np.round(self.uv[i].reshape(-1), 5).tolist()),
                         tuple(self.col[i].reshape(-1).tolist())))
        rows.sort()
        return rows


def _ptr(a):
    return a.ctypes.data if a is not None else None


class Mesher:
    """Привязка таблицы состояний и цветов биомов к C-ядру."""

    def __init__(self, table, biome_colors=None, options=None, lib=None):
        self.table = table
        self.opt = options or MeshOptions()
        self.lib = lib or load_library()
        self.biome_colors = biome_colors if biome_colors is not None else table.biome_colors
        if self.biome_colors is None:
            from ..assets import tint as _tint
            self.biome_colors = _tint.BiomeColors(['minecraft:plains'], {}, {})
        t = table
        self._keep = []

        def A(name, dtype):
            a = np.ascontiguousarray(getattr(t, name), dtype=dtype)
            self._keep.append(a)
            return a
        roots = -1
        if 'minecraft:mangrove_roots' in t.block_names:
            roots = t.block_names.index('minecraft:mangrove_roots')
        T = _Tables()
        T.abi = ABI_VERSION
        T.n_states = int(t.st_flags.shape[0])
        bc = self.biome_colors
        self.n_biomes = int(bc.rgb.shape[0])
        T.n_biomes = self.n_biomes
        T.n_masks = int(t.occ_masks.shape[0])
        T.mangrove_roots_block = roots
        for f, n, dt in (('st_flags', 'st_flags', np.uint32), ('st_block', 'st_block', np.uint16), ('st_occ', 'st_occ', np.uint16),
                         ('st_sturdy', 'st_sturdy', np.uint8), ('st_grp_off', 'st_grp_off', np.int32), ('st_off_h', 'st_off_h', np.float32),
                         ('st_off_v', 'st_off_v', np.float32), ('grp_list', 'grp_list', np.int32), ('grp_var_off', 'grp_var_off', np.int32),
                         ('grp_total', 'grp_total', np.int32), ('var_baked', 'var_baked', np.int32), ('var_weight', 'var_weight', np.int32),
                         ('baked_q_off', 'baked_q_off', np.int32), ('q_pos', 'q_pos', np.float32), ('q_uv', 'q_uv', np.float32),
                         ('q_cull', 'q_cull', np.int8), ('q_dir', 'q_dir', np.uint8), ('q_shade', 'q_shade', np.uint8),
                         ('q_tint', 'q_tint', np.uint8), ('q_layer', 'q_layer', np.uint8), ('q_merge', 'q_merge', np.uint8), ('q_rgb', 'q_rgb', np.uint32),
                         ('occ_masks', 'occ_masks', np.uint8), ('occ_cover', 'occ_cover', np.uint8)):
            setattr(T, f, _ptr(A(n, dt)))
        rgb = np.ascontiguousarray(bc.rgb, dtype=np.uint32)
        mod = np.ascontiguousarray(bc.mod, dtype=np.uint8)
        self._keep += [rgb, mod]
        T.biome_rgb = _ptr(rgb)
        T.biome_mod = _ptr(mod)
        fr = np.asarray(t.fluid_rect, dtype=np.float32)
        for i in range(5):
            for j in range(4):
                T.fluid_rect[i][j] = float(fr[i, j])
        T.fluid_layer[0] = int(t.fluid_layer[0])
        T.fluid_layer[1] = int(t.fluid_layer[1])
        sp = np.asarray(t.swamp_perm, dtype=np.uint8)
        for i in range(256):
            T.swamp_perm[i] = int(sp[i])
        self._T = T

    # --- вызов ---
    def mesh_chunk(self, cx, cz, blocks_by_chunk, biomes_by_chunk, min_y, height, options=None):
        """blocks_by_chunk: {(cx, cz): u16 массив height*256}; biomes_by_chunk: {(cx, cz): u8 массив (height/4)*16} или None."""
        c = blocks_by_chunk.get((cx, cz))
        if c is None:
            raise KeyError('нет блоков чанка (%d, %d)' % (cx, cz))
        nb = []
        nbio = []
        for dz in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nb.append(blocks_by_chunk.get((cx + dx, cz + dz)))
                nbio.append(biomes_by_chunk.get((cx + dx, cz + dz)) if biomes_by_chunk else None)
        return self.mesh_arrays(cx, cz, nb, nbio, min_y, height, options)

    def mesh_arrays(self, cx, cz, nb, nbio, min_y, height, options=None):
        """nb, nbio — по 9 массивов (или None) окрестности 3×3 (индекс (dz+1)*3 + dx+1)."""
        o = options or self.opt
        I = _Input()
        keep = []
        for i in range(9):
            b = nb[i]
            if b is not None:
                b = np.ascontiguousarray(b, dtype=np.uint16).reshape(-1)
                if b.size != height * 256:
                    raise ValueError('размер массива блоков %d != %d' % (b.size, height * 256))
                keep.append(b)
                I.blocks[i] = b.ctypes.data
            else:
                I.blocks[i] = None
            bi = nbio[i]
            if bi is not None:
                bi = np.ascontiguousarray(bi, dtype=np.uint8).reshape(-1)
                if bi.size != (height // 4) * 16:
                    raise ValueError('размер массива биомов %d != %d' % (bi.size, (height // 4) * 16))
                keep.append(bi)
                I.biomes[i] = bi.ctypes.data
            else:
                I.biomes[i] = None
        O = _Options()
        O.flags = o.flags()
        O.blend_radius = int(o.blend_radius)
        O.cx, O.cz = cx, cz
        O.min_y = min_y
        O.n_sections = height // 16
        for i in range(6):
            O.shade[i] = float(o.shade[i])
        O.scale = float(o.scale)
        O.y_offset = int(o.y_offset)
        O.y_lo = 0 if o.y_min is None else max(0, int(o.y_min) - min_y)
        O.y_hi = height - 1 if o.y_max is None else min(height - 1, int(o.y_max) - min_y)
        out = _Output()
        rc = self.lib.mcmesh_chunk(ctypes.byref(self._T), ctypes.byref(I), ctypes.byref(O), ctypes.byref(out))
        if rc != 0:
            raise RuntimeError('mcmesh_chunk вернул %d' % rc)
        try:
            n = out.n_quads
            if n:
                pos = np.ctypeslib.as_array(ctypes.cast(out.pos, ctypes.POINTER(ctypes.c_float)), shape=(n, 4, 3)).copy()
                uv = np.ctypeslib.as_array(ctypes.cast(out.uv, ctypes.POINTER(ctypes.c_float)), shape=(n, 4, 2)).copy()
                col = np.ctypeslib.as_array(ctypes.cast(out.col, ctypes.POINTER(ctypes.c_uint8)), shape=(n, 4, 4)).copy()
                mat = np.ctypeslib.as_array(ctypes.cast(out.mat, ctypes.POINTER(ctypes.c_uint8)), shape=(n,)).copy()
                blk = np.ctypeslib.as_array(ctypes.cast(out.block, ctypes.POINTER(ctypes.c_uint32)), shape=(n,)).copy()
                dr = np.ctypeslib.as_array(ctypes.cast(out.dir, ctypes.POINTER(ctypes.c_uint8)), shape=(n,)).copy()
                mg = np.ctypeslib.as_array(ctypes.cast(out.merged, ctypes.POINTER(ctypes.c_uint8)), shape=(n,)).copy()
                rc = np.ctypeslib.as_array(ctypes.cast(out.rect, ctypes.POINTER(ctypes.c_float)), shape=(n, 4)).copy()
            else:
                pos = np.zeros((0, 4, 3), np.float32)
                uv = np.zeros((0, 4, 2), np.float32)
                col = np.zeros((0, 4, 4), np.uint8)
                mat = np.zeros(0, np.uint8)
                blk = np.zeros(0, np.uint32)
                dr = np.zeros(0, np.uint8)
                mg = np.zeros(0, np.uint8)
                rc = np.zeros((0, 4), np.float32)
            stats = {'blocks_visited': out.n_blocks_visited, 'sections_skipped': out.n_sections_skipped, 'by_material': list(out.n_mat),
                     'merged_from': out.n_merged_from}
        finally:
            self.lib.mcmesh_output_free(ctypes.byref(out))
        return MeshData(pos, uv, col, mat, blk, dr, stats, mg, rc)


def reference_mesh_chunk(*args, **kw):
    """Эталонная реализация (чистый Python/numpy), см. mesh/reference.py."""
    from . import reference
    return reference.reference_mesh_chunk(*args, **kw)

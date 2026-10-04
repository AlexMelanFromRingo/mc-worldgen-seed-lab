"""Мост ctypes к libmcgen по контракту libmcgen/include/mcgen.h (ABI 1).

Поверхность (общая с core/mock.py — переключение одной строкой в core/backend.py):

    McGen.open(pack_dir, version)                -> McGen
        .dimensions() .presets(dim) .block_names() .block_state_name(i) .block_state_from_name(s)
        .biome_names() .biome_name(i) .tweaks() -> [TweakInfo] .world(dim, preset, seeds, tweaks) -> McWorld
    McWorld: .min_y .height .sea_level .biome_at(x,y,z) .biome_grid(x0,z0,nx,nz,step,y) -> ndarray u8 (nz,nx)
             .generate_region(cx0,cz0,nx,nz,stages,threads,progress) -> McRegion
    McRegion: .info .blocks(cx,cz) -> u16 (height,16,16) [y][z][x]; .biomes(cx,cz) -> u8 (height/4,4,4) [qy][qz][qx];
              .heightmap(cx,cz,kind) -> i16 (16,16) [z][x]; .write_mcr(path); .close()

Массивы блоков/биомов/высот — numpy-представления на память региона БЕЗ копирования (изменяемые: правка массива = правка региона).
Представление держит ссылку на регион, поэтому память живёт, пока жив любой массив. Вызовы libmcgen идут через CDLL — GIL
на время вызова отпускается, потому генерацию можно вести из рабочих потоков Python.
"""
import ctypes as C
import os
import sys
import threading
from collections import namedtuple

import numpy as np

from . import paths

ABI_VERSION = 1
NAME = 'lib'

MC_STAGE_BIOMES, MC_STAGE_TERRAIN, MC_STAGE_SURFACE, MC_STAGE_CARVERS, MC_STAGE_FEATURES, MC_STAGE_STRUCTURES = 1, 2, 4, 8, 16, 32
MC_STAGE_ALL = 63
MC_HM_WORLD_SURFACE, MC_HM_OCEAN_FLOOR, MC_HM_MOTION_BLOCKING, MC_HM_MOTION_BLOCKING_NO_LEAVES = 0, 1, 2, 3

MCGEN_OK, MCGEN_E_ARG, MCGEN_E_IO, MCGEN_E_DATA, MCGEN_E_VERSION, MCGEN_E_NOMEM, MCGEN_E_CANCEL, MCGEN_E_UNSUPPORTED, MCGEN_E_INTERNAL = range(9)
ERROR_NAMES = {0: 'OK', 1: 'E_ARG', 2: 'E_IO', 3: 'E_DATA', 4: 'E_VERSION', 5: 'E_NOMEM', 6: 'E_CANCEL', 7: 'E_UNSUPPORTED', 8: 'E_INTERNAL'}

ERR_BUF = 1024

TweakInfo = namedtuple('TweakInfo', 'id label group description default min max soft_min soft_max is_int')
RegionInfo = namedtuple('RegionInfo', 'cx0 cz0 nx nz min_y height')
StructureStart = namedtuple('StructureStart', 'id chunk_x chunk_z bb piece_count')     # bb = (x0, y0, z0, x1, y1, z1) в блоках мира


class McError(RuntimeError):
    """Ошибка libmcgen: code — MCGEN_E_*, message — текст из err[]."""

    def __init__(self, code, message, where='', **kw):
        self.template = message                 # английский шаблон (для перевода интерфейсом), kw — его параметры
        self.kw = kw
        self.code = code
        self.message = message.format(**kw) if kw else message
        self.where = where
        super().__init__(f'{where}: {self.message} ({ERROR_NAMES.get(code, code)})' if where else f'{self.message} ({ERROR_NAMES.get(code, code)})')


class McCancelled(McError):
    """Генерация отменена (callback прогресса вернул != 0 → MCGEN_E_CANCEL)."""


# ---- структуры C ---------------------------------------------------------------------------------------------------------

class _McSeeds(C.Structure):
    _fields_ = [('climate', C.c_int64), ('terrain', C.c_int64), ('structures', C.c_int64), ('features', C.c_int64)]


class _McTweakInfo(C.Structure):
    _fields_ = [('id', C.c_char_p), ('label', C.c_char_p), ('group', C.c_char_p), ('description', C.c_char_p),
                ('dflt', C.c_double), ('min', C.c_double), ('max', C.c_double), ('soft_min', C.c_double), ('soft_max', C.c_double),
                ('is_int', C.c_int)]


class _McTweakValue(C.Structure):
    _fields_ = [('id', C.c_char_p), ('value', C.c_double)]


class _McRegionInfo(C.Structure):
    _fields_ = [('cx0', C.c_int), ('cz0', C.c_int), ('nx', C.c_int), ('nz', C.c_int), ('min_y', C.c_int), ('height', C.c_int)]


class _McStructureStart(C.Structure):
    _fields_ = [('id', C.c_char_p), ('chunk_x', C.c_int), ('chunk_z', C.c_int), ('bb', C.c_int * 6), ('piece_count', C.c_int)]


_PROGRESS_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_double, C.c_char_p)
_P = C.c_void_p


def _s(b):
    return b.decode('utf-8', 'replace') if b is not None else ''


def _b(s):
    return s.encode('utf-8') if isinstance(s, str) else s


# ---- загрузка библиотеки -------------------------------------------------------------------------------------------------

class Library:
    """Загруженная DLL/SO/dylib с прописанными прототипами. Один экземпляр на процесс (см. load())."""

    # имя -> (restype, argtypes, обязательна)
    _PROTOS = {
        'mcgen_version': (C.c_char_p, [], True),
        'mcgen_abi_version': (C.c_int, [], True),
        'mcgen_open': (C.c_int, [C.c_char_p, C.c_char_p, C.POINTER(_P), C.c_char_p, C.c_size_t], True),
        'mcgen_close': (None, [_P], True),
        'mcgen_dimension_count': (C.c_int, [_P], True),
        'mcgen_dimension_name': (C.c_char_p, [_P, C.c_int], True),
        'mcgen_preset_count': (C.c_int, [_P, C.c_char_p], True),
        'mcgen_preset_name': (C.c_char_p, [_P, C.c_char_p, C.c_int], True),
        'mcgen_block_state_count': (C.c_int, [_P], True),
        'mcgen_block_state_name': (C.c_char_p, [_P, C.c_int], True),
        'mcgen_block_state_from_name': (C.c_int, [_P, C.c_char_p], True),
        'mcgen_biome_count': (C.c_int, [_P], True),
        'mcgen_biome_name': (C.c_char_p, [_P, C.c_int], True),
        'mcgen_tweak_count': (C.c_int, [_P], True),
        'mcgen_tweak_info': (C.POINTER(_McTweakInfo), [_P, C.c_int], True),
        'mcgen_world_new': (C.c_int, [_P, C.c_char_p, C.c_char_p, C.POINTER(_McSeeds), C.POINTER(_McTweakValue), C.c_int,
                                       C.POINTER(_P), C.c_char_p, C.c_size_t], True),
        'mcgen_world_free': (None, [_P], True),
        'mcgen_world_set_schedule': (C.c_int, [_P, C.c_char_p, C.c_char_p, C.c_size_t], False),
        'mcgen_world_min_y': (C.c_int, [_P], True),
        'mcgen_world_height': (C.c_int, [_P], True),
        'mcgen_world_sea_level': (C.c_int, [_P], True),
        'mcgen_biome_at': (C.c_int, [_P, C.c_int, C.c_int, C.c_int], True),
        'mcgen_biome_grid': (C.c_int, [_P, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, _P], True),
        'mcgen_generate_region': (C.c_int, [_P, C.c_int, C.c_int, C.c_int, C.c_int, C.c_uint32, C.c_int, _PROGRESS_FN, _P,
                                            C.POINTER(_P), C.c_char_p, C.c_size_t], True),
        'mcgen_region_free': (None, [_P], True),
        'mcgen_region_info': (None, [_P, C.POINTER(_McRegionInfo)], True),
        'mcgen_region_blocks': (_P, [_P, C.c_int, C.c_int], True),
        'mcgen_region_biomes': (_P, [_P, C.c_int, C.c_int], True),
        'mcgen_region_heightmap': (_P, [_P, C.c_int, C.c_int, C.c_int], True),
        'mcgen_region_write_mcr': (C.c_int, [_P, _P, C.c_char_p, C.c_char_p, C.c_size_t], False),
        'mcgen_structure_starts': (C.c_int, [_P, C.c_int, C.c_int, C.c_int, C.c_int, C.POINTER(_McStructureStart), C.c_int], False),
        'mcgen_structure_piece_bb': (C.c_int, [_P, C.c_int, C.c_int, C.c_int, C.c_int, C.POINTER(C.c_int)], False),
    }

    def __init__(self, path):
        self.path = path
        if sys.platform.startswith('win'):
            try:
                os.add_dll_directory(os.path.dirname(os.path.abspath(path)))
            except (AttributeError, OSError):
                pass
            self.dll = C.CDLL(path, winmode=0)
        else:
            self.dll = C.CDLL(path)
        for name, (res, args, required) in self._PROTOS.items():
            try:
                fn = getattr(self.dll, name)
            except AttributeError:
                if required:
                    raise McError(MCGEN_E_VERSION, 'The library has no function {name} (ABI {abi} is required)', path, name=name, abi=ABI_VERSION)
                setattr(self, name, None)
                continue
            fn.restype = res
            fn.argtypes = args
            setattr(self, name, fn)
        self.abi = self.mcgen_abi_version()
        if self.abi != ABI_VERSION:
            raise McError(MCGEN_E_VERSION, 'The library has ABI {got}, the add-on expects {want}', path, got=self.abi, want=ABI_VERSION)
        self.version = _s(self.mcgen_version())

    def exported(self, name):
        try:
            getattr(self.dll, name)
            return True
        except AttributeError:
            return False


_lib_lock = threading.Lock()
_lib = None


def load(path=None):
    """Загружает библиотеку (один раз на процесс). path=None — искать по paths.lib_candidates(). McError, если не найдена/не подошла."""
    global _lib
    with _lib_lock:
        if _lib is not None and (path is None or os.path.abspath(path) == os.path.abspath(_lib.path)):
            return _lib
        p = path or paths.find_library()
        if not p:
            raise McError(MCGEN_E_IO, 'libmcgen was not found; searched: {places}', places='; '.join(paths.lib_candidates()))
        try:
            _lib = Library(p)
        except OSError as e:
            raise McError(MCGEN_E_IO, 'Could not load {path}: {err}', path=p, err=e)
        return _lib


def available():
    """(ok, сообщение) — можно ли использовать настоящую библиотеку."""
    try:
        lib = load()
        return True, f'{lib.version} ({lib.path})'
    except McError as e:
        return False, e.message


def _check(rc, err, where):
    if rc != MCGEN_OK:
        msg = _s(err.value) if err is not None else ''
        if rc == MCGEN_E_CANCEL:
            raise McCancelled(rc, msg or 'cancelled', where)
        raise McError(rc, msg or 'error', where)


def _numpy_view(owner, addr, ctype, count, shape):
    """Массив numpy без копирования поверх памяти по адресу addr; ссылка на owner держит память живой."""
    if not addr:
        raise IndexError('no data at this address (chunk outside the region?)')
    arr_t = ctype * count
    raw = arr_t.from_address(addr)
    raw._owner = owner                      # ctypes-массив держит владельца, numpy держит ctypes-массив через buffer
    return np.frombuffer(raw, dtype=_NP[ctype], count=count).reshape(shape)


_NP = {C.c_uint16: np.uint16, C.c_uint8: np.uint8, C.c_int16: np.int16}


# ---- McGen / McWorld / McRegion ------------------------------------------------------------------------------------------

class McGen:
    """Данные версии (датапак + реестры блоков/биомов). Неизменяем после открытия, безопасен для чтения из нескольких потоков."""

    def __init__(self, handle, lib, pack_dir, version):
        self._h = handle
        self._lib = lib
        self.pack_dir = pack_dir
        self.version = version
        self._block_names = None
        self._biome_names = None

    @classmethod
    def open(cls, pack_dir, version, lib=None):
        lib = lib or load()
        h = _P()
        err = C.create_string_buffer(ERR_BUF)
        rc = lib.mcgen_open(_b(os.fspath(pack_dir)), _b(version), C.byref(h), err, ERR_BUF)
        _check(rc, err, 'mcgen_open')
        return cls(h, lib, pack_dir, version)

    @property
    def backend(self):
        return NAME

    def close(self):
        if self._h:
            self._lib.mcgen_close(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def _need(self):
        if not self._h:
            raise McError(MCGEN_E_ARG, 'McGen is closed')
        return self._h

    def dimensions(self):
        h, L = self._need(), self._lib
        return [_s(L.mcgen_dimension_name(h, i)) for i in range(L.mcgen_dimension_count(h))]

    def presets(self, dimension):
        h, L = self._need(), self._lib
        d = _b(dimension)
        return [_s(L.mcgen_preset_name(h, d, i)) for i in range(max(0, L.mcgen_preset_count(h, d)))]

    @property
    def block_state_count(self):
        return self._lib.mcgen_block_state_count(self._need())

    def block_state_name(self, i):
        return _s(self._lib.mcgen_block_state_name(self._need(), i))

    def block_state_from_name(self, name):
        return self._lib.mcgen_block_state_from_name(self._need(), _b(name))

    def block_names(self):
        """Имена всех состояний блоков (индекс = id состояния); список строится один раз."""
        if self._block_names is None:
            h, L = self._need(), self._lib
            self._block_names = [_s(L.mcgen_block_state_name(h, i)) for i in range(L.mcgen_block_state_count(h))]
        return self._block_names

    @property
    def biome_count(self):
        return self._lib.mcgen_biome_count(self._need())

    def biome_name(self, i):
        return _s(self._lib.mcgen_biome_name(self._need(), i))

    def biome_names(self):
        if self._biome_names is None:
            h, L = self._need(), self._lib
            self._biome_names = [_s(L.mcgen_biome_name(h, i)) for i in range(L.mcgen_biome_count(h))]
        return self._biome_names

    def tweaks(self):
        h, L = self._need(), self._lib
        out = []
        for i in range(L.mcgen_tweak_count(h)):
            t = L.mcgen_tweak_info(h, i).contents
            out.append(TweakInfo(_s(t.id), _s(t.label), _s(t.group), _s(t.description), t.dflt, t.min, t.max, t.soft_min, t.soft_max, bool(t.is_int)))
        return out

    def world(self, dimension, preset, seeds, tweaks=None, schedule=''):
        """seeds: (climate, terrain, structures, features) int64 либо один int (единый); tweaks: {id: значение} или None;
        schedule: путь к .mcsched — расписание записанного прогона сервера (необязательно)."""
        w = McWorld.create(self, dimension, preset, seeds, tweaks)
        if schedule:
            w.set_schedule(schedule)
        return w


def _norm_seeds(seeds):
    if isinstance(seeds, int):
        seeds = (seeds,) * 4
    seeds = tuple(int(s) for s in seeds)
    if len(seeds) != 4:
        raise McError(MCGEN_E_ARG, 'Four seeds are required: climate, terrain, structures, features')
    for s in seeds:
        if not -(1 << 63) <= s < (1 << 63):
            raise McError(MCGEN_E_ARG, 'Seed {seed} is outside int64', seed=s)
    return seeds


class McWorld:
    """Измерение + пресет + сиды + тонкие настройки; предвычисленные шумы. Неизменяем, потокобезопасен для чтения."""

    def __init__(self, handle, gen, dimension, preset, seeds, tweaks):
        self._h = handle
        self.gen = gen
        self._lib = gen._lib
        self.dimension = dimension
        self.preset = preset
        self.seeds = seeds
        self.tweaks = dict(tweaks or {})
        L, h = self._lib, handle
        self.min_y = L.mcgen_world_min_y(h)
        self.height = L.mcgen_world_height(h)
        self.sea_level = L.mcgen_world_sea_level(h)

    @classmethod
    def create(cls, gen, dimension, preset, seeds, tweaks):
        seeds = _norm_seeds(seeds)
        cs = _McSeeds(*seeds)
        tw = dict(tweaks or {})
        arr = (_McTweakValue * max(1, len(tw)))()
        keep = []
        for i, (k, v) in enumerate(tw.items()):
            kb = _b(k)
            keep.append(kb)
            arr[i].id = kb
            arr[i].value = float(v)
        h = _P()
        err = C.create_string_buffer(ERR_BUF)
        rc = gen._lib.mcgen_world_new(gen._need(), _b(dimension), _b(preset), C.byref(cs), arr, len(tw), C.byref(h), err, ERR_BUF)
        _check(rc, err, 'mcgen_world_new')
        return cls(h, gen, dimension, preset, seeds, tw)

    def close(self):
        if self._h:
            self._lib.mcgen_world_free(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def _need(self):
        if not self._h:
            raise McError(MCGEN_E_ARG, 'McWorld is closed')
        return self._h

    def set_schedule(self, path):
        """Расписание записанного прогона настоящего сервера (.mcsched, tools/gt/jfr_order.py --sched): порядок шагов декораций и пост-обработки берётся из файла,
        мир повторяет тот прогон игры. '' — сбросить (модель планировщика). Вызывать до generate_region."""
        fn = self._lib.mcgen_world_set_schedule
        if fn is None:
            raise McError(MCGEN_E_UNSUPPORTED, 'The library has no mcgen_world_set_schedule')
        err = C.create_string_buffer(ERR_BUF)
        rc = fn(self._need(), _b(path or ''), err, ERR_BUF)
        _check(rc, err, 'mcgen_world_set_schedule')

    def biome_at(self, x, y, z):
        return self._lib.mcgen_biome_at(self._need(), int(x), int(y), int(z))

    def biome_grid(self, x0, z0, nx, nz, step=1, y=63):
        """Сетка биомов nx*nz (u8), шаг step блоков, уровень y; результат shape (nz, nx), индекс [iz, ix]."""
        out = np.empty((nz, nx), np.uint8)
        rc = self._lib.mcgen_biome_grid(self._need(), int(x0), int(z0), int(nx), int(nz), int(step), int(y), out.ctypes.data_as(_P))
        if rc != MCGEN_OK:
            raise McError(rc, 'mcgen_biome_grid failed', 'mcgen_biome_grid')
        return out

    def structure_starts(self, cx0, cz0, nx, nz):
        """Старты построек, чей чанк-источник лежит в [cx0, cx0+nx)×[cz0, cz0+nz): список StructureStart (стадия STRUCTURES не нужна —
        старты считаются лениво и потокобезопасно). Работает и до, и после generate_region."""
        fn = self._lib.mcgen_structure_starts
        if fn is None:
            raise McError(MCGEN_E_UNSUPPORTED, 'The library has no mcgen_structure_starts')
        cap = 512
        while True:
            buf = (_McStructureStart * cap)()
            total = fn(self._need(), int(cx0), int(cz0), int(nx), int(nz), buf, cap)
            if total < 0:
                raise McError(MCGEN_E_INTERNAL, 'mcgen_structure_starts failed', 'mcgen_structure_starts')
            if total <= cap:
                break
            cap = total + 16
        return [StructureStart(_s(b.id), b.chunk_x, b.chunk_z, tuple(b.bb), b.piece_count) for b in buf[:total]]

    def structure_piece_bb(self, chunk_x, chunk_z, index, piece):
        """bounding box (x0,y0,z0,x1,y1,z1) части piece index-го старта чанка-источника (порядок — как в structure_starts для одного чанка)."""
        fn = self._lib.mcgen_structure_piece_bb
        if fn is None:
            raise McError(MCGEN_E_UNSUPPORTED, 'The library has no mcgen_structure_piece_bb')
        bb = (C.c_int * 6)()
        rc = fn(self._need(), int(chunk_x), int(chunk_z), int(index), int(piece), bb)
        if rc != 0:
            raise IndexError('no such structure piece')
        return tuple(bb)

    def generate_region(self, cx0, cz0, nx, nz, stages=MC_STAGE_ALL, threads=0, progress=None):
        """Генерирует чанки [cx0, cx0+nx) × [cz0, cz0+nz). progress(fraction, what) -> truthy = отмена. McCancelled при отмене."""
        state = {'exc': None}

        def tramp(ud, frac, what):
            try:
                return 1 if progress(frac, _s(what)) else 0
            except BaseException as e:      # исключение нельзя пробрасывать через C: запоминаем и отменяем
                state['exc'] = e
                return 1

        cb = _PROGRESS_FN(tramp) if progress is not None else _PROGRESS_FN()      # пустой указатель на функцию = без прогресса
        out = _P()
        err = C.create_string_buffer(ERR_BUF)
        rc = self._lib.mcgen_generate_region(self._need(), cx0, cz0, nx, nz, stages & 0xFFFFFFFF, threads, cb, None, C.byref(out), err, ERR_BUF)
        if state['exc'] is not None:
            if out:
                self._lib.mcgen_region_free(out)
            raise state['exc']
        _check(rc, err, 'mcgen_generate_region')
        return McRegion(out, self, stages)


class McRegion:
    """Результат генерации. Массивы — представления на память региона (без копий); освобождается close()/сборщиком."""

    def __init__(self, handle, world, stages):
        self._h = handle
        self.world = world
        self._lib = world._lib
        self.stages = stages
        info = _McRegionInfo()
        self._lib.mcgen_region_info(handle, C.byref(info))
        self.info = RegionInfo(info.cx0, info.cz0, info.nx, info.nz, info.min_y, info.height)

    def close(self):
        if self._h:
            self._lib.mcgen_region_free(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def _need(self):
        if not self._h:
            raise McError(MCGEN_E_ARG, 'McRegion is closed')
        return self._h

    def has_chunk(self, cx, cz):
        i = self.info
        return i.cx0 <= cx < i.cx0 + i.nx and i.cz0 <= cz < i.cz0 + i.nz

    def chunks(self):
        """Координаты чанков региона в порядке cz-major."""
        i = self.info
        return [(cx, cz) for cz in range(i.cz0, i.cz0 + i.nz) for cx in range(i.cx0, i.cx0 + i.nx)]

    def blocks(self, cx, cz):
        a = self._lib.mcgen_region_blocks(self._need(), cx, cz)
        h = self.info.height
        return _numpy_view(self, a, C.c_uint16, h * 256, (h, 16, 16))

    def biomes(self, cx, cz):
        a = self._lib.mcgen_region_biomes(self._need(), cx, cz)
        h = self.info.height // 4
        return _numpy_view(self, a, C.c_uint8, h * 16, (h, 4, 4))

    def heightmap(self, cx, cz, kind=MC_HM_WORLD_SURFACE):
        a = self._lib.mcgen_region_heightmap(self._need(), cx, cz, int(kind))
        return _numpy_view(self, a, C.c_int16, 256, (16, 16))

    def memory_bytes(self):
        """Оценка занятой памяти: блоки u16 + биомы + 4 карты высот на чанк."""
        i = self.info
        per = i.height * 256 * 2 + (i.height // 4) * 16 + 4 * 256 * 2
        return per * i.nx * i.nz

    def write_mcr(self, path):
        fn = self._lib.mcgen_region_write_mcr
        if fn is None:
            raise McError(MCGEN_E_UNSUPPORTED, 'The library has no mcgen_region_write_mcr')
        err = C.create_string_buffer(ERR_BUF)
        rc = fn(self._need(), self.world.gen._need(), _b(os.fspath(path)), err, ERR_BUF)
        _check(rc, err, 'mcgen_region_write_mcr')


def version_string():
    return load().version

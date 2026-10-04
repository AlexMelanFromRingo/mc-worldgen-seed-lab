"""Ускорение на видеокарте (необязательно): мост к функциям mcgen_gpu_* библиотеки libmcgen и сведения для интерфейса. Без bpy.

Библиотека libmcgen сама ищет рядом с собой необязательную libmcgen_cuda (CUDA, только NVIDIA; поставляется отдельным файлом
в lib/<платформа>/). Нет файла / нет драйвера / нет устройства / провалена самопроверка «GPU = CPU» — всё считается на CPU с тем же
результатом побитно; причина попадает в status() и в панель Stats.

Режимы: AUTO (GPU там, где он окупается и проверен: карта биомов; рельеф — только по явному выбору), CPU, GPU (всё, что умеет GPU).
"""
import ctypes as C
import threading
import time

import numpy as np

from . import backend, gpu_build

MODES = ('AUTO', 'CPU', 'GPU')
_MODE_CODE = {'CPU': 0, 'GPU': 1, 'AUTO': 2}
_CODE_MODE = {v: k for k, v in _MODE_CODE.items()}

_lock = threading.RLock()
_bound = None                  # Library, для которой прописаны прототипы
_selftest = {}                 # версия -> (ok, текст)
_bench = None                  # последний результат benchmark()
_label_cache = [0.0, '']


def _library():
    """Загруженная настоящая libmcgen с прототипами mcgen_gpu_* или None (макет / библиотека без GPU-функций)."""
    global _bound
    try:
        m = backend.impl()
        if m.NAME != 'lib':
            return None
        L = m.load()
    except Exception:       # noqa: BLE001 - нет библиотеки: GPU недоступен
        return None
    with _lock:
        if _bound is not L:
            if not L.exported('mcgen_gpu_status'):
                return None
            d = L.dll
            P = C.c_void_p
            protos = {
                'mcgen_gpu_device_count': (C.c_int, []),
                'mcgen_gpu_device_info': (C.c_int, [C.c_int, C.c_char_p, C.c_size_t, C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_size_t)]),
                'mcgen_gpu_set_compute': (C.c_int, [C.c_int, C.c_int]),
                'mcgen_gpu_get_compute': (C.c_int, []),
                'mcgen_gpu_set_features': (C.c_int, [C.c_int]),
                'mcgen_gpu_get_features': (C.c_int, []),
                'mcgen_gpu_status': (C.c_int, [C.c_char_p, C.c_size_t]),
                'mcgen_gpu_selftest': (C.c_int, [P, C.c_int, C.c_char_p, C.c_size_t]),
                'mcgen_gpu_biome_grid': (C.c_int, [P, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, P]),
                'mcgen_biome_grid_cpu': (C.c_int, [P, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, P]),
            }
            for name, (res, args) in protos.items():
                fn = getattr(d, name)
                fn.restype, fn.argtypes = res, args
            gpu_build.activate(d, getattr(L, 'path', ''))        # библиотека, собранная из аддона (кнопка «Build GPU library»), если своей рядом с libmcgen нет
            _bound = L
        return _bound


def refresh():
    """После сборки библиотеки: заново подключить её к libmcgen и сбросить кэш подписи устройства."""
    L = _library()
    with _lock:
        _label_cache[0], _label_cache[1] = 0.0, ''
    if L is None:
        return None
    return gpu_build.activate(L.dll, getattr(L, 'path', ''))


def supported():
    """В библиотеке есть GPU-функции (но устройство может отсутствовать)."""
    return _library() is not None


def devices():
    """[(имя, 'sm_XY', память МБ)] устройств CUDA (пусто — нет libmcgen_cuda / драйвера / устройств)."""
    L = _library()
    if L is None:
        return []
    d = L.dll
    out = []
    for i in range(max(0, d.mcgen_gpu_device_count())):
        name = C.create_string_buffer(160)
        cc_a, cc_b, mem = C.c_int(), C.c_int(), C.c_size_t()
        if d.mcgen_gpu_device_info(i, name, 160, C.byref(cc_a), C.byref(cc_b), C.byref(mem)) == 0:
            out.append((name.value.decode('utf-8', 'replace'), f'sm_{cc_a.value}{cc_b.value}', int(mem.value)))
    return out


def device_label():
    """Имя первой карты (кэшируется на 30 секунд, чтобы не опрашивать драйвер при каждой перерисовке)."""
    now = time.time()
    with _lock:
        if now - _label_cache[0] < 30.0 and _label_cache[0]:
            return _label_cache[1]
    devs = devices()
    lab = devs[0][0] if devs else ''
    with _lock:
        _label_cache[0], _label_cache[1] = now, lab
    return lab


def available():
    return bool(device_label())


def get_mode():
    L = _library()
    return _CODE_MODE.get(L.dll.mcgen_gpu_get_compute(), 'AUTO') if L else 'AUTO'


def set_mode(mode, device=-1):
    """Применяет режим вычислений в библиотеке (AUTO | CPU | GPU). Без GPU-функций ничего не делает."""
    L = _library()
    if L is None:
        return False
    return L.dll.mcgen_gpu_set_compute(_MODE_CODE.get(mode, 2), int(device)) == 0


AUTO_TERRAIN_MIN_CHUNKS = 256        # с этой площади (16×16 чанков) рельеф на GPU в режиме Auto окупается: измерено — на 8×8 GPU медленнее процессора (накладные расходы), на 16×16 быстрее


def configure_for_area(chunks):
    """Режим Auto: рельеф считает видеокарта для областей от AUTO_TERRAIN_MIN_CHUNKS чанков (меньше — быстрее процессор); режим GPU всегда считает всё, что умеет."""
    return set_terrain_in_auto(chunks >= AUTO_TERRAIN_MIN_CHUNKS)


def set_terrain_in_auto(on):
    """Разрешить в режиме AUTO и рельеф на GPU (по умолчанию в AUTO на GPU считаются только биомы)."""
    L = _library()
    if L is None:
        return False
    return L.dll.mcgen_gpu_set_features(3 if on else 1) == 0


def status():
    """Словарь из текста mcgen_gpu_status: mode, state, device, library, selftest, last_fallback, reason, ready (bool)."""
    L = _library()
    if L is None:
        return {'ready': False, 'state': 'GPU acceleration is not installed (no libmcgen_cuda)', 'reason': ''}
    buf = C.create_string_buffer(2048)
    ready = L.dll.mcgen_gpu_status(buf, 2048)
    out = {}
    for ln in buf.value.decode('utf-8', 'replace').splitlines():
        if ': ' in ln:
            k, v = ln.split(': ', 1)
            out[k.strip()] = v.strip()
    out['ready'] = bool(ready)
    return out


def selftest(gen, npoints=20000, force=False):
    """Самопроверка GPU = CPU на тестовых мирах версии (кэшируется по версии). -> (ok|None, текст). None — GPU недоступен."""
    L = _library()
    if L is None or not available():
        return None, ''
    key = getattr(gen, 'version', None) or id(gen)
    with _lock:
        if not force and key in _selftest:
            return _selftest[key]
    buf = C.create_string_buffer(2048)
    rc = L.dll.mcgen_gpu_selftest(gen._need(), int(npoints), buf, 2048)
    res = (rc == 0, buf.value.decode('utf-8', 'replace'))
    with _lock:
        _selftest[key] = res
    return res


def ensure_selftest(gen):
    """При первом использовании GPU (режим не CPU) делает самопроверку версии. Вызывать из рабочего потока."""
    if get_mode() == 'CPU' or not available():
        return None
    return selftest(gen)


def last_selftest():
    with _lock:
        return dict(_selftest)


def biome_grid_cpu(world, x0, z0, nx, nz, step, y):
    L = _library()
    out = np.empty((nz, nx), np.uint8)
    L.dll.mcgen_biome_grid_cpu(world._need(), int(x0), int(z0), int(nx), int(nz), int(step), int(y), out.ctypes.data_as(C.c_void_p))
    return out


def biome_grid_gpu(world, x0, z0, nx, nz, step, y):
    L = _library()
    out = np.empty((nz, nx), np.uint8)
    rc = L.dll.mcgen_gpu_biome_grid(world._need(), int(x0), int(z0), int(nx), int(nz), int(step), int(y), out.ctypes.data_as(C.c_void_p))
    return out if rc == 0 else None


def benchmark(gen, dimension='minecraft:overworld', preset='normal', biome_n=2048, terrain_n=64, task=None):
    """Замеры CPU против GPU: карта биомов biome_n² (шаг 4) и стадии BIOMES+TERRAIN области terrain_n² чанков (без растекания жидкостей).
    Возвращает словарь с временами/ускорениями; результаты обоих режимов сравниваются (identical). Рабочий поток; task — Task для прогресса."""
    global _bench
    L = _library()
    if L is None or not available():
        raise RuntimeError('GPU is not available')
    saved = get_mode()
    res = {'device': device_label(), 'version': getattr(gen, 'version', '')}
    try:
        w = gen.world(dimension, preset, 12345, {'fluid_flow': 0.0})
        step = 4
        x0 = z0 = -biome_n * step // 2

        def rep(frac, msg):
            if task is not None:
                task.report(frac, msg)
                task.check()
        rep(0.02, 'GPU benchmark: biome map')
        t = []
        for _ in range(2):
            t0 = time.perf_counter()
            a = biome_grid_cpu(w, x0, z0, biome_n, biome_n, step, 64)
            t.append(time.perf_counter() - t0)
        res['biome_cpu'] = min(t)
        t = []
        b = None
        for _ in range(3):
            t0 = time.perf_counter()
            b = biome_grid_gpu(w, x0, z0, biome_n, biome_n, step, 64)
            t.append(time.perf_counter() - t0)
            if b is None:
                break
        if b is not None:
            res['biome_gpu'] = min(t[1:] or t)
            res['biome_identical'] = bool(np.array_equal(a, b))
        res['biome_n'] = biome_n
        rep(0.2, 'GPU benchmark: terrain on CPU')
        res['terrain_n'] = terrain_n
        out = {}
        for k, mode in enumerate(('CPU', 'GPU')):
            set_mode(mode)
            rep(0.2 + 0.4 * k, f'GPU benchmark: terrain {terrain_n}x{terrain_n} ({mode})')
            best = None
            for r in range(2 if mode == 'CPU' else 3):
                t0 = time.perf_counter()
                reg = w.generate_region(0, 0, terrain_n, terrain_n, 3, 0, None)
                dt = time.perf_counter() - t0
                if mode == 'GPU' and r == 0:
                    best_first = dt            # первый прогон GPU включает инициализацию CUDA/программы мира
                else:
                    best = dt if best is None else min(best, dt)
                if r == 0 or mode == 'CPU':
                    out[mode] = reg.blocks(terrain_n // 2, terrain_n // 3).copy()
                reg.close()
                rep(None, None)
            res['terrain_' + mode.lower()] = best
            if mode == 'GPU':
                res['terrain_gpu_first'] = best_first
        res['terrain_identical'] = bool(np.array_equal(out.get('CPU'), out.get('GPU')))
        for k in ('biome', 'terrain'):
            if res.get(k + '_cpu') and res.get(k + '_gpu'):
                res[k + '_speedup'] = res[k + '_cpu'] / res[k + '_gpu']
    finally:
        set_mode(saved)
    with _lock:
        _bench = res
    return res


def last_benchmark():
    with _lock:
        return dict(_bench) if _bench else None


# ---- рабочие функции для Task (ui/ops.py) -------------------------------------------------------------------------------------------

def selftest_worker(task, pack_dir, version):
    task.report(0.1, 'GPU self-test')
    gen = backend.open_gen(pack_dir, version)
    return selftest(gen, 20000, force=True)


def benchmark_worker(task, pack_dir, version):
    gen = backend.open_gen(pack_dir, version)
    ok = ensure_selftest(gen)
    return benchmark(gen, task=task)

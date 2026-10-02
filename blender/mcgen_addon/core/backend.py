"""Выбор реализации ядра генерации: настоящая libmcgen (core/lib.py) или макет на numpy (core/mock.py).

>>> ПЕРЕКЛЮЧЕНИЕ MOCK -> НАСТОЯЩАЯ БИБЛИОТЕКА — ОДНА СТРОКА: поставьте BACKEND = 'lib' (или оставьте 'auto'). <<<

  'auto'  — настоящая библиотека, если она найдена в lib/<платформа>/ (или libmcgen/build/) и её ABI подходит, иначе макет;
  'lib'   — только настоящая (ошибка, если не найдена);
  'mock'  — только макет (демонстрация/тесты интерфейса без библиотеки).
Переопределения (по убыванию приоритета): переменная окружения MCGEN_BACKEND, выбор в настройках аддона (set_mode), эта константа.
"""
import os
import threading

BACKEND = 'auto'            # <- единственная строка переключения

_forced = None              # из настроек аддона: 'auto' | 'lib' | 'mock' | None
_lock = threading.RLock()
_cache = {}                 # (имя бэкенда, pack_dir, версия) -> McGen
_note = ''


def set_mode(mode):
    global _forced
    _forced = mode if mode in ('auto', 'lib', 'mock') else None
    release_all()


def requested():
    return os.environ.get('MCGEN_BACKEND') or _forced or BACKEND


def impl():
    """Модуль-реализация с классами McGen/McWorld/McRegion и константами (lib или mock)."""
    global _note
    from . import lib, mock
    mode = requested()
    if mode == 'mock':
        _note = 'demo generator (numpy); libmcgen is not used'
        return mock
    ok, msg = lib.available()
    if ok:
        _note = msg
        return lib
    if mode == 'lib':
        raise lib.McError(lib.MCGEN_E_IO, '{msg}', msg=msg)
    _note = 'demo generator: libmcgen was not found'
    return mock


def describe_parts():
    """(шаблон, параметры) краткого описания бэкенда для интерфейса (шаблон переводится)."""
    m = impl()
    if m.NAME == 'mock':
        return ('Demo generator (numpy)' if requested() == 'mock' else 'Demo generator (libmcgen not found)'), {}
    return 'libmcgen {v}', {'v': m.version_string()}


def name():
    return impl().NAME


def describe():
    impl()
    return f'{name()}: {_note}'


def open_gen(pack_dir, version):
    """McGen для (pack_dir, версия); кэшируется, т. к. данные неизменяемы и потокобезопасны."""
    m = impl()
    key = (m.NAME, os.fspath(pack_dir) if pack_dir else '', version)
    with _lock:
        g = _cache.get(key)
        if g is None:
            g = _cache[key] = m.McGen.open(pack_dir, version)
        return g


def release_all():
    with _lock:
        for g in _cache.values():
            try:
                g.close()
            except Exception:
                pass
        _cache.clear()

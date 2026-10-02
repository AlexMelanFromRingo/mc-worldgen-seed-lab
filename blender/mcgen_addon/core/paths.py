"""Пути: платформа, поиск библиотеки libmcgen, кэш аддона. Работает и без bpy (тесты)."""
import os
import platform
import sys

_cache_override = None


def platform_tag():
    """Имя платформы как в blender_manifest.toml: windows-x64, linux-x64, macos-arm64, macos-x64 (+ arm64 для win/linux)."""
    m = platform.machine().lower()
    arm = m in ('arm64', 'aarch64')
    if sys.platform.startswith('win'):
        return 'windows-arm64' if arm else 'windows-x64'
    if sys.platform == 'darwin':
        return 'macos-arm64' if arm else 'macos-x64'
    return 'linux-arm64' if arm else 'linux-x64'


def lib_filename(tag=None):
    tag = tag or platform_tag()
    if tag.startswith('windows'):
        return 'mcgen.dll'
    if tag.startswith('macos'):
        return 'libmcgen.dylib'
    return 'libmcgen.so'


def addon_dir():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def repo_root():
    """Корень репозитория, если аддон запущен из исходников (blender/mcgen_addon внутри репозитория), иначе None."""
    env = os.environ.get('MCGEN_ROOT')
    if env and os.path.isdir(env):
        return env
    cand = os.path.dirname(os.path.dirname(addon_dir()))
    return cand if os.path.isdir(os.path.join(cand, 'libmcgen')) else None


def lib_candidates():
    """Где ищем библиотеку, по убыванию приоритета: lib/<платформа>/ аддона, libmcgen/build/ репозитория (запуск из исходников).

    Переменная MCGEN_LIB — явный путь к файлу: если она задана, ищется ТОЛЬКО он (никаких запасных мест).
    """
    env = os.environ.get('MCGEN_LIB')
    if env:
        return [env]
    out = []
    name = lib_filename()
    out.append(os.path.join(addon_dir(), 'lib', platform_tag(), name))
    root = repo_root()
    if root:
        out.append(os.path.join(root, 'libmcgen', 'build', platform_tag(), name))
        out.append(os.path.join(root, 'libmcgen', 'build', name))
    return out


def find_library():
    for p in lib_candidates():
        if os.path.isfile(p):
            return p
    return None


def set_cache_override(path):
    global _cache_override
    _cache_override = path or None


def _bpy_cache():
    try:
        import bpy
        pkg = (__package__ or '').rsplit('.', 1)[0]
        if pkg.startswith('bl_ext.'):
            return bpy.utils.extension_path_user(pkg, path='cache', create=True)
        return os.path.join(bpy.utils.user_resource('DATAFILES', create=True), 'mcgen_cache')
    except Exception:
        return None


def cache_dir(create=True):
    """Кэш аддона: packs/<версия>-<sha1>/ (датапак + reports), assets/<версия>-<sha1>/ (клиентские ресурсы), downloads/."""
    d = _cache_override or os.environ.get('MCGEN_CACHE') or _bpy_cache()
    if not d:
        if sys.platform.startswith('win'):
            d = os.path.join(os.environ.get('LOCALAPPDATA', os.path.expanduser('~')), 'mcgen')
        elif sys.platform == 'darwin':
            d = os.path.expanduser('~/Library/Caches/mcgen')
        else:
            d = os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'mcgen')
    if create:
        os.makedirs(d, exist_ok=True)
    return d


def tweaks_json_path():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tweaks.json')
    return p

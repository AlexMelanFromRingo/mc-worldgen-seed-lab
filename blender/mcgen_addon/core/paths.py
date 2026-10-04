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


# Windows: путь файла не длиннее 259 символов (MAX_PATH; Blender и Java/C без «длинных путей»). Самая длинная запись датапака — 139 символов, плюс каталог распаковки
# packs/<версия>-<хэш>.part/ (≈ 37) и разделители: корень кэша должен быть не длиннее ≈ 80 символов. Каталог расширения Blender (…\Blender Foundation\Blender\5.2\extensions\.user\
# user_default\mcgen\cache, у Microsoft Store ещё длиннее) этого лимита не укладывается — на Windows тогда берётся короткий %LOCALAPPDATA%\mcgen.
WIN_MAX_PATH = 259
WIN_PATH_BUDGET = 185


def win_cache_fits(root):
    """Укладывается ли корень кэша в лимит путей Windows с запасом под самые длинные файлы датапака."""
    return len(root) + WIN_PATH_BUDGET <= WIN_MAX_PATH


def is_packaged_app():
    """Windows: Blender запущен как упакованное приложение (Microsoft Store / MSIX). Такие приложения не пишут в AppData по-настоящему:
    записи перенаправляются в приватную папку пакета (…\\Packages\\<пакет>\\LocalCache\\…), и внешние программы (Java, nvcc, cmd) этих файлов не видят."""
    if not sys.platform.startswith('win'):
        return False
    try:
        import ctypes
        n = ctypes.c_uint32(0)
        return ctypes.windll.kernel32.GetCurrentPackageFullName(ctypes.byref(n), None) != 15700      # APPMODEL_ERROR_NO_PACKAGE
    except Exception:       # noqa: BLE001 - старая Windows без этой функции: определяем по пути exe
        return 'windowsapps' in (sys.executable or '').lower()


NO_WINDOW = 0x08000000 if sys.platform.startswith('win') else 0         # CREATE_NO_WINDOW: дочерние консольные программы без мигающего окна
_visible = {}


def children_see_files(d):
    """Видит ли ДОЧЕРНИЙ процесс (Java, cmd, nvcc) файлы, которые мы пишем в каталог d. Проверка прямая: пишем пробный файл и просим cmd его прочитать.
    Если не удалось даже запустить cmd — считаем, что видит (не блокируем без причины)."""
    if not sys.platform.startswith('win'):
        return True
    if d in _visible:
        return _visible[d]
    import subprocess
    ok = False
    probe = os.path.join(d, f'.probe-{os.getpid()}')
    try:
        os.makedirs(d, exist_ok=True)
        with open(probe, 'w') as f:
            f.write('1')
        try:
            r = subprocess.run([os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/c', 'type', probe], capture_output=True, timeout=30, creationflags=NO_WINDOW)
            ok = r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            ok = True
        finally:
            try:
                os.remove(probe)
            except OSError:
                pass
    except OSError:
        ok = False
    _visible[d] = ok
    return ok


def _default_cache():
    win = sys.platform.startswith('win')
    d = _bpy_cache()
    if win:
        if is_packaged_app():
            # Blender из Microsoft Store: ни AppData, ни путь расширения не подходят (внешние программы их не видят) — каталог пользователя вне AppData
            cands = [os.path.join(os.path.expanduser('~'), '.mcgen'), 'C:\\mcgen']
            for c in cands:
                if children_see_files(c):
                    return c
            return cands[0]
        if d and not win_cache_fits(d):
            d = None
    if d:
        return d
    if win:
        return os.path.join(os.environ.get('LOCALAPPDATA', os.path.expanduser('~')), 'mcgen')
    if sys.platform == 'darwin':
        return os.path.expanduser('~/Library/Caches/mcgen')
    return os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'mcgen')


def cache_dir(create=True):
    """Кэш аддона: packs/<версия>-<sha1>/ (датапак + reports), assets/<версия>-<sha1>/ (клиентские ресурсы), downloads/."""
    d = _cache_override or os.environ.get('MCGEN_CACHE') or _default_cache()
    if create:
        os.makedirs(d, exist_ok=True)
    return d


def tweaks_json_path():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tweaks.json')
    return p

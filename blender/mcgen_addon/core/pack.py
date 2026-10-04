"""Ресурсы пользователя: jar'ы Mojang -> кэш аддона (pack-каталог для libmcgen + клиентские ресурсы для мешей).

Материалы Mojang в аддон не входят: всё готовится на машине пользователя из его jar'ов и лежит в кэше (paths.cache_dir()).

    cache/packs/<версия>-<sha1 сервера[:10]>/      pack-каталог libmcgen (формат tools/make_pack.py):
        data/minecraft/**   датапак из внутреннего jar сервера (worldgen, structure, tags …)
        reports/blocks.json, registries.json, datapack.json   генератор данных игры (нужна Java ≥ версии игры)
        version.json, .complete
    cache/assets/<версия>-<sha1 клиента[:10]>/     assets/minecraft/{blockstates,models,textures,atlases,items,lang/en_us.json}
    cache/downloads/                               скачанные кнопкой «Download» jar'ы

Шаги prepare(): 1) определить версию и sha1 jar'ов; 2) распаковать датапак; 3) получить reports (папка пользователя | кэш | запуск
`java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports --output …`); 4) распаковать клиентские ресурсы.
Каждый шаг пропускается, если уже есть в кэше (ключ — sha1 jar). Скачивание требует явного принятия Minecraft EULA.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile
from collections import namedtuple

from . import paths
from .tasks import Cancelled

MANIFEST_URL = 'https://piston-meta.mojang.com/mc/game/version_manifest_v2.json'
SUPPORTED_VERSIONS = ('26.1', '26.2', '26.3', '26.4-snapshot-2')
ASSET_PREFIXES = ('assets/minecraft/blockstates/', 'assets/minecraft/models/', 'assets/minecraft/textures/', 'assets/minecraft/atlases/',
                  'assets/minecraft/items/', 'assets/minecraft/lang/en_us')
REPORT_FILES = ('blocks.json', 'registries.json', 'datapack.json', 'block_flags.json')
FLAGS_CLASS = 'mcgenflags.BlockFlags'        # наш класс (java/mcgenflags/BlockFlags.class в расширении), без кода Mojang
EULA_URL = 'https://aka.ms/MinecraftEULA'

JarInfo = namedtuple('JarInfo', 'path kind version world_version java_version size')   # kind: 'server' | 'client'


class PackError(RuntimeError):
    """Понятная пользователю ошибка подготовки ресурсов. Текст английский; template/kw позволяют интерфейсу перевести шаблон
    (bpy.app.translations) и подставить параметры: pgettext(e.template).format(**e.kw)."""

    def __init__(self, template, **kw):
        self.template = template
        self.kw = kw
        super().__init__(template.format(**kw) if kw else template)


class EulaNotAccepted(PackError):
    pass


class NeedJava(PackError):
    """Нет подходящей Java; сообщение объясняет запасной путь («папка reports»)."""


# ---- jar'ы ---------------------------------------------------------------------------------------------------------------

def sha1_file(path, task=None, block=1 << 20):
    h = hashlib.sha1()
    size = os.path.getsize(path) or 1
    done = 0
    with open(path, 'rb') as f:
        while True:
            b = f.read(block)
            if not b:
                break
            h.update(b)
            done += len(b)
            if task:
                task.check()
                task.report(message=f'sha1 {os.path.basename(path)} {100 * done // size}%')
    return h.hexdigest()


def inspect_jar(path):
    """JarInfo или None (не jar Mojang). kind: server — bundler (META-INF/versions.list), client — есть assets/minecraft/."""
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            nameset = set(names)
            if 'version.json' not in nameset:
                return None
            v = json.loads(z.read('version.json'))
            if 'META-INF/versions.list' in nameset or 'net/minecraft/bundler/Main.class' in nameset:
                kind = 'server'
            elif any(n.startswith('assets/minecraft/') for n in names[:200000]) and 'assets/minecraft/lang/en_us.json' in nameset:
                kind = 'client'
            elif 'net/minecraft/server/Main.class' in nameset:
                kind = 'server'
            else:
                return None
            return JarInfo(os.fspath(path), kind, v.get('id') or v.get('name'), v.get('world_version'), v.get('java_version'), os.path.getsize(path))
    except (OSError, zipfile.BadZipFile, ValueError, KeyError):
        return None


def minecraft_dirs():
    home = os.path.expanduser('~')
    c = []
    if sys.platform.startswith('win'):
        c.append(os.path.join(os.environ.get('APPDATA', home), '.minecraft'))
    elif sys.platform == 'darwin':
        c.append(os.path.join(home, 'Library', 'Application Support', 'minecraft'))
    else:
        c += [os.path.join(home, '.minecraft'), os.path.join(home, '.var', 'app', 'com.mojang.Minecraft', '.minecraft')]
    return [d for d in c if os.path.isdir(d)]


def launcher_roots():
    """Каталоги данных сторонних лаунчеров, где лежат клиентские jar'ы: MultiMC / Prism / PolyMC (libraries/com/mojang/minecraft/<v>/minecraft-<v>-client.jar),
    Modrinth App (meta/versions/<v>/<v>.jar). Официальный лаунчер — minecraft_dirs() (versions/<v>/<v>.jar)."""
    home = os.path.expanduser('~')
    names = ('PrismLauncher', 'PolyMC', 'MultiMC', 'ModrinthApp', 'com.modrinth.theseus', 'ATLauncher', 'GDLauncher')
    out = []
    if sys.platform.startswith('win'):
        for base in (os.environ.get('APPDATA', ''), os.environ.get('LOCALAPPDATA', '')):
            out += [os.path.join(base, n) for n in names if base]
    elif sys.platform == 'darwin':
        out += [os.path.join(home, 'Library', 'Application Support', n) for n in names]
    else:
        share = os.environ.get('XDG_DATA_HOME') or os.path.join(home, '.local', 'share')
        out += [os.path.join(share, n) for n in names] + [os.path.join(share, 'multimc')]
        out += [os.path.join(home, '.var', 'app', 'org.prismlauncher.PrismLauncher', 'data', 'PrismLauncher'),
                os.path.join(home, '.var', 'app', 'com.modrinth.ModrinthApp', 'data', 'ModrinthApp')]
    return [d for d in out if os.path.isdir(d)]


def _launcher_jars(root):
    """Клиентские jar'ы в каталоге данных лаунчера (MultiMC-подобного или Modrinth App)."""
    found = []
    libs = os.path.join(root, 'libraries', 'com', 'mojang', 'minecraft')
    if os.path.isdir(libs):
        for v in sorted(os.listdir(libs)):
            d = os.path.join(libs, v)
            if os.path.isdir(d):
                found += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.jar')]
    meta = os.path.join(root, 'meta', 'versions')
    if os.path.isdir(meta):
        for v in sorted(os.listdir(meta)):
            p = os.path.join(meta, v, v + '.jar')
            if os.path.isfile(p):
                found.append(p)
    return found


def _extra_search_dirs():
    home = os.path.expanduser('~')
    out = [os.path.join(home, d) for d in ('Downloads', 'Desktop', 'minecraft', 'Minecraft')]
    out.append(os.path.join(paths.cache_dir(False), 'downloads'))
    root = paths.repo_root()
    if root:
        out.append(os.path.join(root, 'jars'))
    return [d for d in out if os.path.isdir(d)]


def scan_jars(extra_dirs=(), kinds=('server', 'client')):
    """Ищет jar'ы Mojang: клиентские — .minecraft/versions/<v>/<v>.jar; серверные — server*.jar в .minecraft, Загрузках и т. п."""
    seen, out = set(), []

    def add(p):
        rp = os.path.realpath(p)
        if rp in seen:
            return
        seen.add(rp)
        info = inspect_jar(rp)
        if info and info.kind in kinds:
            out.append(info)

    for mc in minecraft_dirs():
        vdir = os.path.join(mc, 'versions')
        if os.path.isdir(vdir):
            for v in sorted(os.listdir(vdir)):
                p = os.path.join(vdir, v, v + '.jar')
                if os.path.isfile(p):
                    add(p)
    for root in launcher_roots() + [d for d in extra_dirs if d and os.path.isdir(d)]:
        try:
            for p in _launcher_jars(root):
                add(p)
        except OSError:
            continue
    dirs = list(minecraft_dirs()) + _extra_search_dirs() + [d for d in extra_dirs if d and os.path.isdir(d)]
    for d in dirs:
        try:
            for fn in sorted(os.listdir(d)):
                p = os.path.join(d, fn)
                low = fn.lower()
                if low.endswith('.jar') and os.path.isfile(p) and any(k in low for k in ('server', 'client', 'minecraft')):
                    add(p)
                elif os.path.isdir(p) and d.endswith(('Downloads', 'Desktop', 'minecraft', 'Minecraft')) and not fn.startswith('.'):
                    for fn2 in sorted(os.listdir(p))[:200]:
                        if fn2.lower().endswith('.jar') and any(k in fn2.lower() for k in ('server', 'client')):
                            add(os.path.join(p, fn2))
        except OSError:
            continue
    return out


def version_key(v):
    """Ключ сортировки версий '26.3' < '26.4-snapshot-2'."""
    return [int(x) if x.isdigit() else x for x in re.split(r'[.\-]', v or '')]


# ---- Java ----------------------------------------------------------------------------------------------------------------

_java_cache = {}


def java_major(path):
    """Мажорная версия Java по `java -version` (None, если запуск не удался)."""
    if path in _java_cache:
        return _java_cache[path]
    major = None
    try:
        r = subprocess.run([path, '-version'], capture_output=True, text=True, timeout=20)
        m = re.search(r'version "(\d+)(?:\.(\d+))?', r.stderr + r.stdout)
        if m:
            major = int(m.group(2)) if m.group(1) == '1' and m.group(2) else int(m.group(1))
    except (OSError, subprocess.SubprocessError):
        major = None
    _java_cache[path] = major
    return major


def java_candidates():
    exe = 'java.exe' if sys.platform.startswith('win') else 'java'
    out = []
    jh = os.environ.get('JAVA_HOME')
    if jh:
        out.append(os.path.join(jh, 'bin', exe))
    w = shutil.which('java')
    if w:
        out.append(w)
    home = os.path.expanduser('~')
    runtime_roots = [os.path.join(d, 'runtime') for d in minecraft_dirs()]
    if sys.platform.startswith('win'):
        la = os.environ.get('LOCALAPPDATA', '')
        runtime_roots.append(os.path.join(la, 'Packages', 'Microsoft.4297127D64EC6_8wekyb3d8bbwe', 'LocalCache', 'Local', 'runtime'))
        for pf in (os.environ.get('ProgramFiles', r'C:\Program Files'), os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')):
            for sub in ('Java', 'Eclipse Adoptium', 'Microsoft', 'Zulu', 'BellSoft', 'Amazon Corretto'):
                base = os.path.join(pf, sub)
                if os.path.isdir(base):
                    out += [os.path.join(base, x, 'bin', exe) for x in sorted(os.listdir(base))]
    elif sys.platform == 'darwin':
        for base in ('/Library/Java/JavaVirtualMachines', os.path.join(home, 'Library', 'Java', 'JavaVirtualMachines')):
            if os.path.isdir(base):
                out += [os.path.join(base, x, 'Contents', 'Home', 'bin', exe) for x in sorted(os.listdir(base))]
    else:
        for base in ('/usr/lib/jvm', '/usr/lib64/jvm', os.path.join(home, '.sdkman', 'candidates', 'java'), os.path.join(home, '.jdks')):
            if os.path.isdir(base):
                out += [os.path.join(base, x, 'bin', exe) for x in sorted(os.listdir(base))]
    for root in runtime_roots:                          # лаунчер: runtime/<компонент>/<платформа>/<компонент>/bin/java
        if not os.path.isdir(root):
            continue
        for comp in sorted(os.listdir(root)):
            for plat in sorted(os.listdir(os.path.join(root, comp))) if os.path.isdir(os.path.join(root, comp)) else []:
                base = os.path.join(root, comp, plat, comp)
                out.append(os.path.join(base, 'bin', exe))
                out.append(os.path.join(base, 'jre.bundle', 'Contents', 'Home', 'bin', exe))
    seen, res = set(), []
    for p in out:
        if p and p not in seen and os.path.isfile(p):
            seen.add(p)
            res.append(p)
    return res


def find_java(min_major, preferred=None):
    """(путь, major) первой Java с major >= min_major (preferred — путь из настроек), иначе None."""
    cands = ([preferred] if preferred else []) + java_candidates()
    for p in cands:
        if p and os.path.isfile(p):
            m = java_major(p)
            if m is not None and m >= min_major:
                return p, m
    return None


# ---- кэш -----------------------------------------------------------------------------------------------------------------

def packs_dir():
    return os.path.join(paths.cache_dir(), 'packs')


def assets_root():
    return os.path.join(paths.cache_dir(), 'assets')


def downloads_dir():
    return os.path.join(paths.cache_dir(), 'downloads')


def _stamp(d):
    try:
        with open(os.path.join(d, '.complete'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def list_packs():
    """Готовые pack-каталоги кэша: [{'version', 'dir', 'sha1', 'has_reports', 'complete'}], новые версии первыми."""
    out = []
    root = packs_dir()
    if os.path.isdir(root):
        for name in os.listdir(root):
            d = os.path.join(root, name)
            st = _stamp(d)
            if st:
                out.append({'version': st.get('version'), 'dir': d, 'sha1': st.get('sha1'),
                            'has_reports': os.path.isfile(os.path.join(d, 'reports', 'blocks.json')), 'complete': bool(st.get('complete'))})
    return sorted(out, key=lambda e: version_key(e['version'] or ''), reverse=True)


def list_assets():
    out = []
    root = assets_root()
    if os.path.isdir(root):
        for name in os.listdir(root):
            d = os.path.join(root, name)
            st = _stamp(d)
            if st and st.get('complete'):
                out.append({'version': st.get('version'), 'dir': d, 'sha1': st.get('sha1')})
    return sorted(out, key=lambda e: version_key(e['version'] or ''), reverse=True)


def read_version_json(directory):
    try:
        with open(os.path.join(directory, 'version.json'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def resolve(version, pack_override='', assets_override=''):
    """-> {'pack': путь|None, 'assets': путь|None, 'pack_ok': bool, 'assets_ok': bool} для версии.

    Приоритет: указанные пользователем папки (если версия в них совпадает или не указана), затем кэш аддона.
    pack_ok — есть датапак и reports/blocks.json (оба нужны libmcgen); assets_ok — есть blockstates/models/textures.
    """
    pack = assets = None
    if pack_override and os.path.isdir(pack_override):
        vj = read_version_json(pack_override)
        if not vj or vj.get('id') == version or vj.get('name') == version:
            pack = pack_override
    if assets_override and os.path.isdir(assets_override):
        assets = assets_override
    if pack is None:
        for e in list_packs():
            if e['version'] == version and e['has_reports'] and e['complete']:
                pack = e['dir']
                break
    if assets is None:
        for e in list_assets():
            if e['version'] == version:
                assets = e['dir']
                break
    return {
        'pack': pack,
        'assets': assets,
        'flags_ok': bool(pack and os.path.isfile(os.path.join(pack, 'reports', 'block_flags.json'))),
        'pack_ok': bool(pack and os.path.isdir(os.path.join(pack, 'data', 'minecraft')) and os.path.isfile(os.path.join(pack, 'reports', 'blocks.json'))),
        'assets_ok': bool(assets and os.path.isdir(os.path.join(assets, 'assets', 'minecraft', 'blockstates'))),
    }


def available_versions():
    """Версии для списка в интерфейсе: готовые в кэше + поддерживаемые (даже если ещё не подготовлены)."""
    vs = {e['version'] for e in list_packs() if e['version']} | set(SUPPORTED_VERSIONS)
    # стабильные версии от новой к старой, затем снимки (по умолчанию в интерфейсе — самая новая стабильная)
    stable = sorted((v for v in vs if '-' not in v), key=version_key, reverse=True)
    snaps = sorted((v for v in vs if '-' in v), key=version_key, reverse=True)
    return stable + snaps


# ---- шаги подготовки -----------------------------------------------------------------------------------------------------

def _inner_game_jar_name(z):
    """Имя внутреннего jar игры в bundler-jar (META-INF/versions.list: sha1\\tid\\tпуть)."""
    names = z.namelist()
    if 'META-INF/versions.list' in names:
        parts = z.read('META-INF/versions.list').decode().split()
        if len(parts) >= 3:
            cand = 'META-INF/versions/' + parts[-1]
            if cand in names:
                return cand
    for n in names:
        if n.startswith('META-INF/versions/') and n.endswith('.jar'):
            return n
    return None


def check_path_budget(root, names):
    """Windows: самый длинный файл распаковки не должен превышать 259 символов — иначе понятная ошибка вместо FileNotFoundError посреди распаковки."""
    if not sys.platform.startswith('win') or not names:
        return
    longest = max(len(os.path.abspath(root)) + 1 + len(n) for n in names)
    if longest > paths.WIN_MAX_PATH:
        raise PackError('The cache folder path is too long for Windows (a file would need {n} characters, the limit is 259). Set a shorter "Cache folder" in the add-on preferences '
                        '(for example C:\\mcgen) or enable long paths in Windows.', n=longest)


def _write_entry(zf, name, dst):
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(zf.read(name))
    except OSError as e:
        if sys.platform.startswith('win') and (len(dst) > paths.WIN_MAX_PATH or getattr(e, 'winerror', 0) in (3, 206)):
            raise PackError('The cache folder path is too long for Windows (a file would need {n} characters, the limit is 259). Set a shorter "Cache folder" in the add-on preferences '
                            '(for example C:\\mcgen) or enable long paths in Windows.', n=len(dst))
        raise PackError('Could not write "{path}": {err}', path=dst, err=e)


def _extract_data(zf, out_dir, task=None):
    n = 0
    names = [x for x in zf.namelist() if (x.startswith('data/') or x == 'version.json') and not x.endswith('/')]
    check_path_budget(out_dir, names)
    for i, name in enumerate(names):
        _write_entry(zf, name, os.path.join(out_dir, *name.split('/')))
        n += 1
        if task and i % 200 == 0:
            task.check()
            task.report(message=f'datapack {i}/{len(names)}')
    return n


def extract_datapack(server_jar, out_dir, task=None):
    """Датапак и version.json из серверного jar (bundler: из внутреннего jar игры; иначе из самого jar) -> out_dir."""
    with zipfile.ZipFile(server_jar) as z:
        inner = _inner_game_jar_name(z)
        if inner:
            import io
            with zipfile.ZipFile(io.BytesIO(z.read(inner))) as zz:
                return _extract_data(zz, out_dir, task)
        return _extract_data(z, out_dir, task)


def extract_assets(client_jar, out_dir, task=None):
    """Блокстейты, модели, текстуры, атласы, items, lang/en_us из клиентского jar (как tools/make_pack.py)."""
    n = 0
    with zipfile.ZipFile(client_jar) as z:
        names = [x for x in z.namelist() if x.startswith(ASSET_PREFIXES) and not x.endswith('/')]
        total = len(names)
        check_path_budget(out_dir, names)
        for i, name in enumerate(names):
            _write_entry(z, name, os.path.join(out_dir, *name.split('/')))
            n += 1
            if task and i % 300 == 0:
                task.check()
                task.report(message=f'assets {i}/{total}')
    return n


def reports_from_folder(folder):
    """Находит blocks.json в папке пользователя (сама папка, folder/reports или folder/generated/reports) -> каталог с файлами или None."""
    for sub in ('', 'reports', os.path.join('generated', 'reports'), os.path.join('out', 'reports')):
        d = os.path.normpath(os.path.join(folder, sub))
        if os.path.isfile(os.path.join(d, 'blocks.json')):
            return d
    return None


def run_data_generator(server_jar, java, out_dir, task=None, timeout=1800):
    """Запускает генератор данных игры; результат — out_dir/reports/*.json. Возвращает каталог reports."""
    work = tempfile.mkdtemp(prefix='mcgen-gen-', dir=paths.cache_dir())
    try:
        out = os.path.join(work, 'out')
        cmd = [java, '-DbundlerMainClass=net.minecraft.data.Main', '-jar', os.fspath(server_jar), '--reports', '--output', out]
        proc = subprocess.Popen(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        t0 = time.time()
        providers = 0
        tail = []
        import threading
        lines = []

        def reader():
            for ln in proc.stdout:
                lines.append(ln.rstrip())

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        seen = 0
        while proc.poll() is None:
            if task and task.should_cancel():
                proc.kill()
                proc.wait()
                raise Cancelled()
            if time.time() - t0 > timeout:
                proc.kill()
                raise PackError('The game data generator did not finish in {timeout} s', timeout=timeout)
            while seen < len(lines):
                ln = lines[seen]
                seen += 1
                tail.append(ln)
                if 'Starting provider' in ln:
                    providers += 1
                    if task:
                        task.report(message='reports: ' + ln.split('Starting provider:')[-1].strip())
            time.sleep(0.1)
        th.join(5)
        tail += lines[seen:]
        if proc.returncode != 0:
            raise PackError('The game data generator exited with code {code}:\n{log}', code=proc.returncode, log='\n'.join(tail[-12:]))
        rep = os.path.join(out, 'reports')
        if not os.path.isfile(os.path.join(rep, 'blocks.json')):
            raise PackError('The game data generator did not create reports/blocks.json:\n{log}', log='\n'.join(tail[-8:]))
        dest = os.path.join(out_dir, 'reports')
        os.makedirs(dest, exist_ok=True)
        for f in REPORT_FILES:
            if os.path.isfile(os.path.join(rep, f)):
                shutil.copy2(os.path.join(rep, f), os.path.join(dest, f))
        return dest
    finally:
        shutil.rmtree(work, ignore_errors=True)


_NEED_JAVA = ('No Java {major}+ was found for the game data generator (reports/blocks.json). Install JDK {major}+ (for example Temurin) or set the '
              'path to java in the settings. Fallback: run the generator by hand\n'
              '  java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports --output out\n'
              'and select the folder out/reports (or out) in the "Reports folder" field.')


def flags_class_dir():
    """Каталог с классом mcgenflags/BlockFlags.class нашего расширения (собирается tools/build_extension.py) или None."""
    d = os.path.join(paths.addon_dir(), 'java')
    return d if os.path.isfile(os.path.join(d, 'mcgenflags', 'BlockFlags.class')) else None


def flags_source():
    """Исходник BlockFlags.java в репозитории (запуск из исходников) или None."""
    root = paths.repo_root()
    p = os.path.join(root, 'libmcgen', 'tests', 'g5_blockflags', 'BlockFlags.java') if root else None
    return p if p and os.path.isfile(p) else None


def find_javac(java):
    """javac рядом с найденной java (тот же JDK) либо в PATH."""
    exe = 'javac.exe' if sys.platform.startswith('win') else 'javac'
    cand = os.path.join(os.path.dirname(os.path.realpath(java)), exe)
    return cand if os.path.isfile(cand) else shutil.which('javac')


def bundle_classpath(server_jar, java, work, task=None):
    """Распаковывает библиотеки и внутренний jar игры bundler'ом в work (`--help` генератора данных: ~2 с, ничего не генерирует) и
    возвращает classpath игры из META-INF/classpath-joined (пути относительно work)."""
    try:
        with zipfile.ZipFile(server_jar) as z:
            joined = z.read('META-INF/classpath-joined').decode().strip()
    except (KeyError, OSError, zipfile.BadZipFile):
        raise PackError('Could not unpack the server bundle: {log}', log='no META-INF/classpath-joined')
    r = subprocess.run([java, '-DbundlerMainClass=net.minecraft.data.Main', '-jar', os.fspath(server_jar), '--help'], cwd=work,
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        raise PackError('Could not unpack the server bundle: {log}', log=(r.stderr or r.stdout)[-400:])
    parts = [os.path.join(work, *p.split('/')) for p in joined.split(';') if p]
    return os.pathsep.join(parts)


def run_block_flags(server_jar, java, out_path, task=None, timeout=900):
    """reports/block_flags.json (свойства состояний блоков из Java-кода игры: solid, replaceable, жидкости … — нужны стадии FEATURES).

    Запускает НАШ класс mcgenflags.BlockFlags на classpath игры пользователя (нужна только JRE той же версии, что для --reports). Класс лежит
    в расширении (java/mcgenflags/BlockFlags.class, собран tools/build_extension.py); при запуске из исходников и наличии javac он
    компилируется на лету из libmcgen/tests/g5_blockflags/BlockFlags.java в кэш."""
    work = tempfile.mkdtemp(prefix='mcgen-flags-', dir=paths.cache_dir())
    try:
        cp = bundle_classpath(server_jar, java, work, task)
        cls_dir = flags_class_dir()
        if cls_dir is None:
            src, javac = flags_source(), find_javac(java)
            if not src or not javac:
                raise PackError('The BlockFlags helper class is not part of this build and no JDK (javac) is available to compile it')
            cls_dir = os.path.join(work, 'cls')
            os.makedirs(cls_dir)
            r = subprocess.run([javac, '-nowarn', '-Xlint:none', '-encoding', 'UTF-8', '-d', cls_dir, '-cp', cp, src], capture_output=True, text=True)
            if r.returncode != 0:
                raise PackError('javac failed on BlockFlags.java:\n{log}', log=r.stderr[-800:])
        tmp = out_path + '.tmp'
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        cmd = [java, '-Xss8m', '--sun-misc-unsafe-memory-access=allow', '-cp', cls_dir + os.pathsep + cp, FLAGS_CLASS, tmp]
        proc = subprocess.Popen(cmd, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors='replace')
        t0 = time.time()
        while proc.poll() is None:
            if task and task.should_cancel():
                proc.kill()
                proc.wait()
                raise Cancelled()
            if time.time() - t0 > timeout:
                proc.kill()
                raise PackError('The game data generator did not finish in {timeout} s', timeout=timeout)
            time.sleep(0.1)
        if proc.returncode != 0 or not os.path.isfile(tmp):
            raise PackError('BlockFlags exited with code {code}:\n{log}', code=proc.returncode, log=(proc.stderr.read() or '')[-600:])
        with open(tmp, encoding='utf-8') as f:
            d = json.load(f)
        if not d.get('nstates'):
            raise PackError('block_flags.json is damaged: {err}', err='no states')
        os.replace(tmp, out_path)
        return out_path
    finally:
        shutil.rmtree(work, ignore_errors=True)


PrepareResult = namedtuple('PrepareResult', 'version pack_dir assets_dir pack_ok assets_ok steps flags_ok')


def prepare(server_jar='', client_jar='', reports_folder='', java='', task=None, want_assets=True):
    """Готовит pack-каталог и ресурсы клиента в кэше. Возвращает PrepareResult; исключения: PackError/NeedJava/Cancelled.

    server_jar нужен для датапака и reports; client_jar — для ресурсов (и датапака, если сервера нет — но reports тогда только из
    reports_folder). Все шаги идемпотентны: готовое (по sha1) не пересоздаётся.
    """
    def rep(f, m):
        if task:
            task.check()
            task.report(f, m)

    steps = []
    for jp in (server_jar, client_jar):
        if jp and not os.path.isfile(jp):
            raise PackError('The jar file "{path}" does not exist (maybe the cache was cleared). Pick the file again or press Download.', path=jp)
    s_info = inspect_jar(server_jar) if server_jar else None
    c_info = inspect_jar(client_jar) if client_jar else None
    if server_jar and not s_info:
        raise PackError('"{path}" does not look like a Mojang server jar (no version.json / bundler)', path=server_jar)
    if client_jar and not c_info:
        raise PackError('"{path}" does not look like a Mojang client jar (no version.json / assets)', path=client_jar)
    if not s_info and not c_info:
        raise PackError('Select a Minecraft server and/or client jar (the Auto-detect button looks for them in .minecraft).')
    if s_info and c_info and s_info.version != c_info.version:
        raise PackError('The jar versions differ: server {server}, client {client}', server=s_info.version, client=c_info.version)
    version = (s_info or c_info).version
    world_version = (s_info or c_info).world_version
    java_needed = (s_info or c_info).java_version or 25

    # --- pack ---
    pack_dir = None
    pack_ok = False
    src = s_info or c_info
    rep(0.02, f'sha1 {os.path.basename(src.path)}')
    sha = sha1_file(src.path, task)
    pack_dir = os.path.join(packs_dir(), f'{version}-{sha[:10]}')
    st = _stamp(pack_dir) or {}
    os.makedirs(pack_dir, exist_ok=True)
    if not (st.get('data') and os.path.isdir(os.path.join(pack_dir, 'data', 'minecraft'))):
        rep(0.10, 'Extracting datapack')
        tmp = pack_dir + '.part'
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        try:
            n = extract_datapack(src.path, tmp, task)
        except BaseException:
            shutil.rmtree(tmp, ignore_errors=True)    # недораспакованный .part не оставляем
            raise
        for name in os.listdir(tmp):               # переносим data/ и version.json в pack_dir
            dst = os.path.join(pack_dir, name)
            if os.path.isdir(dst):
                shutil.rmtree(dst)
            elif os.path.exists(dst):
                os.remove(dst)
            shutil.move(os.path.join(tmp, name), dst)
        shutil.rmtree(tmp, ignore_errors=True)
        st.update(data=n)
        steps.append(f'datapack: {n} files')
    else:
        steps.append('datapack: cached')
    st.update(version=version, sha1=sha, world_version=world_version, source=os.path.basename(src.path))
    _write_stamp(pack_dir, st)

    # --- reports ---
    have_reports = os.path.isfile(os.path.join(pack_dir, 'reports', 'blocks.json')) and os.path.isfile(os.path.join(pack_dir, 'reports', 'registries.json'))
    if not have_reports:
        folder_reports = reports_from_folder(reports_folder) if reports_folder else None
        if reports_folder and not folder_reports:
            raise PackError('The folder "{path}" has no blocks.json (expected the reports folder or its parent)', path=reports_folder)
        if folder_reports:
            rep(0.35, 'Copying reports')
            dest = os.path.join(pack_dir, 'reports')
            os.makedirs(dest, exist_ok=True)
            for f in REPORT_FILES:
                if os.path.isfile(os.path.join(folder_reports, f)):
                    shutil.copy2(os.path.join(folder_reports, f), os.path.join(dest, f))
            steps.append('reports: copied from folder')
        else:
            if not s_info:
                raise NeedJava('reports/blocks.json needs the server jar (game data generator) or a ready reports folder.\n' + _NEED_JAVA, major=java_needed)
            found = find_java(java_needed, java or None)
            if not found:
                raise NeedJava(_NEED_JAVA, major=java_needed)
            rep(0.35, f'Running the game data generator (Java {found[1]}) …')
            run_data_generator(s_info.path, found[0], pack_dir, task)
            steps.append(f'reports: generated by Java {found[1]}')
    else:
        steps.append('reports: cached')
    try:
        with open(os.path.join(pack_dir, 'reports', 'blocks.json'), encoding='utf-8') as f:
            blocks = json.load(f)
        nstates = sum(len(b['states']) for b in blocks.values())
    except (OSError, ValueError, KeyError) as e:
        raise PackError('reports/blocks.json is damaged: {err}', err=e)
    st.update(blocks=len(blocks), states=nstates, complete=True, created=time.strftime('%Y-%m-%d %H:%M:%S'))
    _write_stamp(pack_dir, st)
    pack_ok = True

    # --- block_flags.json: свойства состояний из Java-кода игры (без него стадия FEATURES использует эвристики по именам) ---
    flags_path = os.path.join(pack_dir, 'reports', 'block_flags.json')
    flags_ok = os.path.isfile(flags_path)
    if flags_ok:
        steps.append('block_flags: cached')
    elif s_info:
        found = find_java(java_needed, java or None)
        if found:
            rep(0.45, 'Extracting block flags')
            try:
                run_block_flags(s_info.path, found[0], flags_path, task)
                flags_ok = True
                steps.append('block_flags: generated')
            except PackError as e:
                steps.append('block_flags: skipped (' + str(e).split('\n')[0][:120] + ')')
        else:
            steps.append('block_flags: skipped (no Java)')
    else:
        steps.append('block_flags: skipped (no server jar)')

    # --- assets ---
    assets_dir = None
    assets_ok = False
    if want_assets and c_info:
        rep(0.55, f'sha1 {os.path.basename(c_info.path)}')
        csha = sha1_file(c_info.path, task)
        assets_dir = os.path.join(assets_root(), f'{version}-{csha[:10]}')
        ast = _stamp(assets_dir) or {}
        if not (ast.get('complete') and os.path.isdir(os.path.join(assets_dir, 'assets', 'minecraft', 'blockstates'))):
            rep(0.62, 'Extracting client assets')
            tmp = assets_dir + '.part'
            shutil.rmtree(tmp, ignore_errors=True)
            os.makedirs(tmp)
            n = extract_assets(c_info.path, tmp, task)
            shutil.rmtree(assets_dir, ignore_errors=True)
            os.makedirs(os.path.dirname(assets_dir), exist_ok=True)
            shutil.move(tmp, assets_dir)
            _write_stamp(assets_dir, {'version': version, 'sha1': csha, 'files': n, 'complete': True, 'created': time.strftime('%Y-%m-%d %H:%M:%S')})
            steps.append(f'assets: {n} files')
        else:
            steps.append('assets: cached')
        assets_ok = True
    rep(1.0, 'Ready')
    return PrepareResult(version, pack_dir, assets_dir, pack_ok, assets_ok, steps, flags_ok)


def _write_stamp(d, st):
    tmp = os.path.join(d, '.complete.tmp')
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, os.path.join(d, '.complete'))


def clear_cache(what='all'):
    """Удаляет кэш аддона ('all' | 'packs' | 'assets' | 'downloads')."""
    n = 0
    for sub in (('packs', 'assets', 'downloads') if what == 'all' else (what,)):
        d = os.path.join(paths.cache_dir(False), sub)
        if os.path.isdir(d):
            shutil.rmtree(d, ignore_errors=True)
            n += 1
    return n


def cache_size(path=None):
    root = path or paths.cache_dir(False)
    total = 0
    for dp, _dn, fns in os.walk(root):
        for f in fns:
            try:
                total += os.path.getsize(os.path.join(dp, f))
            except OSError:
                pass
    return total


# ---- скачивание официальных jar'ов ---------------------------------------------------------------------------------------

def manifest_url():
    return os.environ.get('MCGEN_MANIFEST_URL') or MANIFEST_URL


def _urlopen(url, timeout=60):
    p = urllib.parse.urlparse(url)
    if p.scheme not in ('https', 'http'):
        raise PackError('Unsupported URL scheme: {scheme}', scheme=p.scheme)
    req = urllib.request.Request(url, headers={'User-Agent': 'mcgen-blender-addon'})
    return urllib.request.urlopen(req, timeout=timeout)


def fetch_manifest(url=None):
    try:
        with _urlopen(url or manifest_url()) as r:
            return json.loads(r.read())
    except (OSError, ValueError) as e:
        raise PackError('Could not fetch the Mojang version list: {err}', err=e)


def manifest_versions(manifest, prefix='26.'):
    return [v['id'] for v in manifest.get('versions', []) if v['id'].startswith(prefix)]


def download_file(url, dest, sha1=None, size=None, task=None, label=''):
    tmp = dest + '.part'
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    h = hashlib.sha1()
    done = 0
    with _urlopen(url, timeout=120) as r, open(tmp, 'wb') as f:
        total = size or int(r.headers.get('Content-Length') or 0)
        while True:
            b = r.read(1 << 18)
            if not b:
                break
            f.write(b)
            h.update(b)
            done += len(b)
            if task:
                task.check()
                task.report(message=f'{label} {done / 1e6:.0f}/{total / 1e6:.0f} MB' if total else f'{label} {done / 1e6:.0f} MB')
    if sha1 and h.hexdigest() != sha1:
        os.remove(tmp)
        raise PackError('sha1 mismatch for {name}: got {got}, expected {want}', name=os.path.basename(dest), got=h.hexdigest(), want=sha1)
    os.replace(tmp, dest)
    return dest


def download_jars(version, accept_eula, which=('server', 'client'), task=None, manifest=None):
    """Скачивает официальные jar'ы Mojang версии `version` в cache/downloads. БЕЗ принятия EULA не качает (EulaNotAccepted).

    Возвращает {'server': путь, 'client': путь}. sha1 проверяется по метаданным Mojang."""
    if not accept_eula:
        raise EulaNotAccepted('Downloading means accepting the Minecraft EULA ({url}). Tick "I accept the Minecraft EULA" if you agree.', url=EULA_URL)
    if task:
        task.report(0.0, 'Fetching version manifest')
    m = manifest or fetch_manifest()
    entry = next((v for v in m.get('versions', []) if v['id'] == version), None)
    if entry is None:
        raise PackError('Version {version} was not found in the Mojang manifest', version=version)
    with _urlopen(entry['url']) as r:
        meta = json.loads(r.read())
    out = {}
    for i, kind in enumerate(which):
        d = meta.get('downloads', {}).get(kind)
        if not d:
            raise PackError('Version {version} has no "{kind}" download', version=version, kind=kind)
        dest = os.path.join(downloads_dir(), f'{kind}-{version}.jar')
        if os.path.isfile(dest) and os.path.getsize(dest) == d.get('size') and sha1_file(dest) == d.get('sha1'):
            out[kind] = dest
            continue
        if task:
            task.report(i / len(which), f'Downloading {kind}.jar')
        out[kind] = download_file(d['url'], dest, d.get('sha1'), d.get('size'), task, label=f'{kind}.jar')
    if task:
        task.report(1.0, 'Downloaded')
    return out

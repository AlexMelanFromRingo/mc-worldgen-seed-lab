#!/usr/bin/env python3
"""Сборка расширений Blender «MC Worldgen» (zip по платформам), проверка manifest и установка в чистый профиль.

    python3 tools/build_extension.py                      # все платформы -> blender/dist/mcgen-<версия>-<платформа>.zip (+ validate)
    python3 tools/build_extension.py --platforms linux-x64 --test 4.5,5.2
                                                          # + установка в чистый профиль Blender 4.5 и 5.2 и прогон тестов внутри установленного расширения
    python3 tools/build_extension.py --build-libs         # сначала собрать библиотеки (libmcgen/build.py) под нужные платформы
    python3 tools/build_extension.py --allow-missing-libs # допустить сборку без libmcgen (аддон тогда работает на демо-макете)

Каждая платформа собирается из отдельного «стейджинга»: исходники аддона + lib/<платформа>/ ТОЛЬКО этой платформы, а в
blender_manifest.toml поле platforms = ["<платформа>"]. Дальше `blender --command extension build` -> zip, `extension validate` -> проверка.
Материалы Mojang в zip не попадают (их аддон готовит на лету из jar пользователя). Тесты, __pycache__ и dev-каталоги исключены.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ADDON = os.path.join(ROOT, 'blender', 'mcgen_addon')
DEFAULT_OUT = os.path.join(ROOT, 'blender', 'dist')
PLATFORMS = ('windows-x64', 'linux-x64', 'macos-arm64', 'macos-x64')
LIBNAME = {'windows': 'mcgen.dll', 'linux': 'libmcgen.so', 'macos': 'libmcgen.dylib'}
BLENDERS = {
    '4.5': os.environ.get('BLENDER_45') or os.path.expanduser('~/tools/blender-4.5.14-linux-x64/blender'),
    '5.2': os.environ.get('BLENDER_52') or os.path.expanduser('~/tools/blender-5.2.2-linux-x64/blender'),
}
EXCLUDE_DIRS = {'__pycache__', 'dev', 'tests', '.git', '.idea'}
EXCLUDE_SUFFIX = ('.pyc', '.pyo', '.zip', '.orig', '.rej', '.swp')


def read_manifest():
    with open(os.path.join(ADDON, 'blender_manifest.toml'), encoding='utf-8') as f:
        return f.read()


def manifest_value(text, key):
    m = re.search(rf'^{key}\s*=\s*"([^"]*)"', text, re.M)
    return m.group(1) if m else None


def stage(platform, dest, allow_missing_libs):
    """Копирует аддон в dest: исходники + lib/<платформа>/; manifest с platforms = [платформа]. Возвращает (число файлов, есть ли библиотека)."""
    n = 0
    for dp, dns, fns in os.walk(ADDON):
        rel = os.path.relpath(dp, ADDON)
        dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
        parts = rel.split(os.sep)
        if parts[0] == 'lib':
            # из lib/ берём только каталог своей платформы
            if len(parts) == 1:
                dns[:] = [d for d in dns if d == platform]
                continue
        for fn in fns:
            if fn.endswith(EXCLUDE_SUFFIX) or fn == '.gitignore':
                continue
            if rel == '.' and fn == 'blender_manifest.toml':
                continue
            if parts[0] == 'lib' and len(parts) == 1:
                continue
            src = os.path.join(dp, fn)
            dst = os.path.join(dest, rel, fn) if rel != '.' else os.path.join(dest, fn)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            n += 1
    text = read_manifest()
    text, k = re.subn(r'^platforms\s*=\s*\[[^\]]*\]', f'platforms = ["{platform}"]', text, flags=re.M)
    if k != 1:
        raise SystemExit('в blender_manifest.toml нет строки platforms = [...]')
    with open(os.path.join(dest, 'blender_manifest.toml'), 'w', encoding='utf-8') as f:
        f.write(text)
    libfile = os.path.join(dest, 'lib', platform, LIBNAME[platform.split('-')[0]])
    have = os.path.isfile(libfile)
    if not have and not allow_missing_libs:
        raise SystemExit(f'нет библиотеки для {platform}: {os.path.relpath(libfile, dest)} — соберите libmcgen/build.py или добавьте --allow-missing-libs')
    return n, have


FLAGS_SRC = os.path.join(ROOT, 'libmcgen', 'tests', 'g5_blockflags', 'BlockFlags.java')
FLAGS_OUT = os.path.join(ADDON, 'java')


def _find_tool(name):
    jh = os.environ.get('JAVA_HOME')
    for c in ([os.path.join(jh, 'bin', name)] if jh else []) + [shutil.which(name) or '']:
        if c and os.path.isfile(c):
            return c
    return None


def build_flags_class(game_jar=None, force=False, required=False):
    """Компилирует НАШ класс mcgenflags.BlockFlags (libmcgen/tests/g5_blockflags/BlockFlags.java) в blender/mcgen_addon/java/.

    Класс обращается к классам игры по имени и не содержит кода Mojang; на машине пользователя он запускается на classpath его jar
    (`java -cp java:<classpath игры> mcgenflags.BlockFlags out.json`) — нужна только JRE 25, как для --reports. Компиляция идёт против
    серверного jar из jars/ (класс совместим по байткоду со всеми версиями 26.x: проверено побайтным совпадением block_flags.json).
    Возвращает путь к .class или None (если нет javac/jar и не required)."""
    out = os.path.join(FLAGS_OUT, 'mcgenflags', 'BlockFlags.class')
    if os.path.isfile(out) and not force and os.path.getmtime(out) >= os.path.getmtime(FLAGS_SRC):
        return out
    javac, java = _find_tool('javac'), _find_tool('java')
    jar = game_jar or next((os.path.join(ROOT, 'jars', n) for n in ('server-26.3.jar', 'server-26.2.jar', 'server-26.1.jar')
                            if os.path.isfile(os.path.join(ROOT, 'jars', n))), None)
    if not (javac and java and jar and os.path.isfile(FLAGS_SRC)):
        msg = 'BlockFlags не собран: нужны javac (JDK 25), серверный jar игры (jars/server-26.3.jar) и исходник ' + os.path.relpath(FLAGS_SRC, ROOT)
        if required:
            sys.exit(msg)
        print('ПРЕДУПРЕЖДЕНИЕ:', msg)
        return None
    sys.path.insert(0, os.path.join(ADDON, '..'))
    import types
    pkg = types.ModuleType('mcgen_addon_build')
    pkg.__path__ = [ADDON]
    sys.modules['mcgen_addon_build'] = pkg
    from importlib import import_module
    pack = import_module('mcgen_addon_build.core.pack')
    work = tempfile.mkdtemp(prefix='mcgen-javac-')
    try:
        cp = pack.bundle_classpath(jar, java, work)
        shutil.rmtree(FLAGS_OUT, ignore_errors=True)
        os.makedirs(FLAGS_OUT)
        with open(os.path.join(FLAGS_OUT, '.gitignore'), 'w') as f:
            f.write('# класс BlockFlags собирает tools/build_extension.py; в git не попадает\n*\n!.gitignore\n')
        r = subprocess.run([javac, '--release', '25', '-nowarn', '-Xlint:none', '-encoding', 'UTF-8', '-d', FLAGS_OUT, '-cp', cp, FLAGS_SRC],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('javac не смог собрать BlockFlags.java:\n' + r.stderr[-1500:])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return out


def run_blender(exe, args, env=None, timeout=600):
    r = subprocess.run([exe] + args, capture_output=True, text=True, env=env, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def zip_summary(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        info = z.infolist()
    return {'files': len(names), 'bytes': sum(i.file_size for i in info), 'zip_bytes': os.path.getsize(path),
            'has_manifest': 'blender_manifest.toml' in names,
            'libs': sorted(n for n in names if n.startswith('lib/') and not n.endswith('/')),
            'java': sorted(n for n in names if n.startswith('java/') and not n.endswith('/')),
            'mojang_files': [n for n in names if re.search(r'\.jar$|assets/minecraft|data/minecraft', n) or (n.endswith('.class') and not n.startswith('java/mcgenflags/'))]}


def build_one(platform, out_dir, exe, allow_missing_libs, validate_with):
    manifest = read_manifest()
    ext_id, ver = manifest_value(manifest, 'id'), manifest_value(manifest, 'version')
    tmp = tempfile.mkdtemp(prefix=f'mcgen-ext-{platform}-')
    try:
        src = os.path.join(tmp, 'src')
        n, have = stage(platform, src, allow_missing_libs)
        os.makedirs(out_dir, exist_ok=True)
        zip_path = os.path.join(out_dir, f'{ext_id}-{ver}-{platform}.zip')
        if os.path.exists(zip_path):
            os.remove(zip_path)
        code, log = run_blender(exe, ['--command', 'extension', 'build', '--source-dir', src, '--output-filepath', zip_path])
        if code != 0 or not os.path.exists(zip_path):
            raise SystemExit(f'[{platform}] extension build не удался:\n{log[-3000:]}')
        res = {'platform': platform, 'zip': zip_path, 'staged_files': n, 'has_lib': have, 'build_log': [ln for ln in log.splitlines() if ln.strip()][-4:]}
        res.update(zip_summary(zip_path))
        code, log = run_blender(validate_with, ['--command', 'extension', 'validate', zip_path])
        res['validate'] = {'blender': validate_with, 'ok': code == 0 and 'error' not in log.lower(), 'log': [ln for ln in log.splitlines() if ln.strip()][-3:]}
        return res
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def install_and_test(ver, zip_path, platform_ok, backend, scratch):
    """Чистый профиль -> extension install-file --enable -> blender_tests.py внутри установленного расширения (bl_ext.user_default.mcgen)."""
    exe = BLENDERS[ver]
    prof = os.path.join(scratch, f'ext-profile-{ver}')
    shutil.rmtree(prof, ignore_errors=True)
    os.makedirs(prof)
    env = dict(os.environ, BLENDER_USER_RESOURCES=prof, PYTHONDONTWRITEBYTECODE='1')
    t0 = time.time()
    code, log = run_blender(exe, ['--command', 'extension', 'install-file', '-r', 'user_default', '-e', zip_path], env=env)
    res = {'blender': ver, 'install_ok': code == 0, 'install_log': [ln for ln in log.splitlines() if ln.strip()][-3:]}
    ext_dir = os.path.join(prof, 'extensions', 'user_default', 'mcgen')
    res['installed'] = os.path.isfile(os.path.join(ext_dir, 'blender_manifest.toml'))
    res['installed_libs'] = sorted(os.listdir(os.path.join(ext_dir, 'lib'))) if os.path.isdir(os.path.join(ext_dir, 'lib')) else []
    if not res['installed']:
        return res
    sys.path.insert(0, os.path.join(ROOT, 'blender', 'tests', 'addon'))
    out_json = os.path.join(scratch, f'ext-tests-{ver}.json')
    env.update(MCGEN_BACKEND=backend, MCGEN_EXT_MODULE='bl_ext.user_default.mcgen', MCGEN_SCRATCH=scratch,
               MCGEN_CACHE=os.path.join(scratch, 'real-cache'), MCGEN_STUB_LIB=env.get('MCGEN_LIB', ''))
    if backend == 'lib':
        env['MCGEN_REAL'] = '1'                      # внутри расширения — настоящая libmcgen платформы
    code, log = run_blender(exe, ['-b', '--factory-startup', '--python', os.path.join(ROOT, 'blender', 'tests', 'addon', 'blender_tests.py'), '--', '--json', out_json],
                            env=env, timeout=900)
    res['tests_ok'] = code == 0
    if os.path.exists(out_json):
        res['tests'] = json.load(open(out_json))
    else:
        res['tests_log'] = log[-3000:]
    res['seconds'] = round(time.time() - t0, 1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--platforms', default='all')
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--build-libs', action='store_true', help='собрать libmcgen/build.py под выбранные платформы перед упаковкой')
    ap.add_argument('--allow-missing-libs', action='store_true')
    ap.add_argument('--test', default='', help='версии Blender для установки и прогона тестов: 4.5,5.2')
    ap.add_argument('--backend', default='mock', help='mock | lib | auto — какой бэкенд проверять при тесте установленного расширения')
    ap.add_argument('--validate-with', default='4.5', help='версия Blender для extension build/validate (4.5 | 5.2)')
    ap.add_argument('--game-jar', help='серверный jar Mojang для компиляции BlockFlags (по умолчанию jars/server-26.3.jar)')
    ap.add_argument('--rebuild-flags', action='store_true', help='пересобрать java/mcgenflags/BlockFlags.class')
    ap.add_argument('--require-flags', action='store_true', help='ошибка, если BlockFlags.class нельзя собрать')
    ap.add_argument('--json', help='записать отчёт в файл')
    a = ap.parse_args()
    plats = list(PLATFORMS) if a.platforms == 'all' else a.platforms.split(',')
    for p in plats:
        if p not in PLATFORMS:
            sys.exit(f'неизвестная платформа {p}; доступны {", ".join(PLATFORMS)}')
    subprocess.run([sys.executable, os.path.join(ROOT, 'libmcgen', 'gen_tweaks.py'), '--check'], check=True)
    flags_cls = build_flags_class(a.game_jar, a.rebuild_flags, a.require_flags)
    print('BlockFlags.class:', os.path.relpath(flags_cls, ROOT) if flags_cls else 'нет (аддон будет полагаться на javac пользователя или эвристики)')
    skipped = []
    if a.build_libs:
        rc = subprocess.run([sys.executable, os.path.join(ROOT, 'libmcgen', 'build.py'), '--targets', ','.join(plats)]).returncode
        if rc != 0:
            print('libmcgen/build.py вернул ошибку: платформы без собранной библиотеки будут пропущены (если не задан --allow-missing-libs)')
    if not a.allow_missing_libs:
        for p in list(plats):
            if not os.path.isfile(os.path.join(ADDON, 'lib', p, LIBNAME[p.split('-')[0]])):
                print(f'[{p}] нет библиотеки в blender/mcgen_addon/lib/{p}/ — платформа пропущена')
                skipped.append(p)
                plats.remove(p)
    exe = BLENDERS[a.validate_with]
    report = {'built': [], 'tests': []}
    for p in list(plats):
        r = build_one(p, a.out, exe, a.allow_missing_libs, exe)
        report['built'].append(r)
        print(f'[{p}] {os.path.basename(r["zip"])}: {r["zip_bytes"] / 1024:.0f} KB, файлов {r["files"]}, библиотека: {"да" if r["has_lib"] else "НЕТ"}, '
              f'validate: {"OK" if r["validate"]["ok"] else "ОШИБКА"}')
        if r['mojang_files']:
            sys.exit(f'в zip попали файлы Mojang: {r["mojang_files"][:5]}')
        if not r['validate']['ok']:
            print('\n'.join(r['validate']['log']))
    scratch = os.environ.get('MCGEN_SCRATCH') or os.path.join(os.environ.get('TMPDIR', '/tmp'), 'mcgen-tests')
    os.makedirs(scratch, exist_ok=True)
    if a.test:
        host = 'linux-x64'
        z = next((b['zip'] for b in report['built'] if b['platform'] == host), None)
        if z is None:
            sys.exit('для теста установки нужна платформа linux-x64 (хост)')
        for ver in a.test.split(','):
            t = install_and_test(ver, z, True, a.backend, scratch)
            report['tests'].append(t)
            tt = t.get('tests', {})
            print(f'[установка в Blender {ver}] install: {"OK" if t["install_ok"] else "ОШИБКА"}, распаковано: {t["installed"]}, '
                  f'тесты: {"OK" if t.get("tests_ok") else "ОШИБКА"} ({tt.get("run", "?")} тестов, пропущено {tt.get("skipped", "?")})')
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1, ensure_ascii=False)
    bad = [b for b in report['built'] if not b['validate']['ok']] or [t for t in report['tests'] if not t.get('tests_ok') or not t['install_ok']]
    report['skipped_platforms'] = skipped
    sys.exit(1 if (bad or skipped) else 0)


if __name__ == '__main__':
    main()

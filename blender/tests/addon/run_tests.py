#!/usr/bin/env python3
"""Прогон всех тестов аддона: ядро (python3), затем Blender 4.5 LTS и 5.x headless на макете и на заглушке библиотеки.

    python3 blender/tests/addon/run_tests.py [--blender 4.5,5.2] [--backend mock,lib] [--skip-core] [--json результат.json]

Пути Blender: $BLENDER_45 / $BLENDER_52 или ~/tools/blender-<версия>-linux-x64/blender. Результаты печатаются таблицей и пишутся в JSON.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402

BLENDERS = {
    '4.5': os.environ.get('BLENDER_45') or os.path.expanduser('~/tools/blender-4.5.14-linux-x64/blender'),
    '5.2': os.environ.get('BLENDER_52') or os.path.expanduser('~/tools/blender-5.2.2-linux-x64/blender'),
}


def run_core(extra):
    t0 = time.time()
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_core.py')] + extra, capture_output=True, text=True)
    out = r.stderr + r.stdout
    m = re.search(r'Ran (\d+) tests? in ([\d.]+)s', out)
    skipped = re.search(r'skipped=(\d+)', out)
    res = {'what': 'core (python3)', 'run': int(m.group(1)) if m else 0, 'skipped': int(skipped.group(1)) if skipped else 0, 'ok': r.returncode == 0,
           'seconds': round(time.time() - t0, 1), 'python': sys.version.split()[0]}
    if r.returncode != 0:
        res['log'] = out[-6000:]
    return res


def run_blender(ver, backend, stub_lib, cache, scratch, real_lib=None):
    exe = BLENDERS[ver]
    label = backend
    if backend == 'real':                     # настоящая libmcgen потока W1 (бэкенд lib, но не заглушка)
        backend, stub_lib = 'lib', real_lib
    res_dir = os.path.join(scratch, f'blender-profile-{ver}-{backend}')
    shutil.rmtree(res_dir, ignore_errors=True)
    os.makedirs(os.path.join(res_dir, 'extensions', 'user_default'))
    os.symlink(_boot.ADDON_DIR, os.path.join(res_dir, 'extensions', 'user_default', 'mcgen'))
    env = dict(os.environ, BLENDER_USER_RESOURCES=res_dir, MCGEN_BACKEND=backend, MCGEN_CACHE=cache, MCGEN_SCRATCH=scratch, PYTHONDONTWRITEBYTECODE='1')
    if backend == 'lib':
        env['MCGEN_LIB'] = stub_lib
        env['MCGEN_STUB_LIB'] = stub_lib
    else:
        env.pop('MCGEN_LIB', None)
    out_json = os.path.join(scratch, f'blender-{ver}-{backend}.json')
    t0 = time.time()
    r = subprocess.run([exe, '-b', '--factory-startup', '--python', os.path.join(HERE, 'blender_tests.py'), '--', '--json', out_json],
                       capture_output=True, text=True, env=env)
    log = r.stdout + r.stderr
    summ = None
    if os.path.exists(out_json):
        summ = json.load(open(out_json))
        os.remove(out_json)
    res = {'what': f'Blender {ver} / {label}', 'ok': r.returncode == 0 and summ is not None, 'seconds': round(time.time() - t0, 1)}
    if summ:
        res.update(blender=summ['blender'], python=summ['python'], run=summ['run'], skipped=summ['skipped'], failed=summ['failed'])
    if not res['ok']:
        res['log'] = log[-8000:]
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--blender', default='4.5,5.2')
    ap.add_argument('--backend', default='mock,lib,real', help='mock — макет; lib — заглушка библиотеки; real — настоящая libmcgen (если собрана)')
    ap.add_argument('--skip-core', action='store_true')
    ap.add_argument('--json')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    scratch = _boot.SCRATCH
    os.makedirs(scratch, exist_ok=True)
    results = []
    if not a.skip_core:
        results.append(run_core(['-v'] if a.v else []))
        print(results[-1]['what'], 'OK' if results[-1]['ok'] else 'FAIL', results[-1]['run'], 'тестов', results[-1]['seconds'], 'с')
        if not results[-1]['ok']:
            print(results[-1].get('log', ''))
    cache = os.path.join(scratch, 'real-cache')
    stub_dir = os.path.join(scratch, 'stub-build')
    if not os.path.exists(os.path.join(stub_dir, 'linux-x64', 'libmcgen.so')):
        subprocess.run([sys.executable, os.path.join(_boot.REPO, 'libmcgen', 'build.py'), '--stub', '--targets', 'linux-x64', '--out', stub_dir, '--no-install'], check=True)
    stub_lib = os.path.join(stub_dir, 'linux-x64', 'libmcgen.so')
    import test_core
    real_lib = test_core.find_real_lib()
    for ver in a.blender.split(','):
        for be in a.backend.split(','):
            if be == 'real' and not real_lib:
                print(f'Blender {ver} / real: пропуск — нет собранной libmcgen')
                continue
            r = run_blender(ver, be, stub_lib, cache, scratch, real_lib)
            results.append(r)
            print(r['what'], 'OK' if r['ok'] else 'FAIL', r.get('run'), 'тестов', 'пропущено', r.get('skipped'), r['seconds'], 'с', r.get('failed') or '')
            if not r['ok']:
                print(r.get('log', ''))
    if a.json:
        json.dump(results, open(a.json, 'w'), indent=1, ensure_ascii=False)
    sys.exit(0 if all(r['ok'] for r in results) else 1)


if __name__ == '__main__':
    main()

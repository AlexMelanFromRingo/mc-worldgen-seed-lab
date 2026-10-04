#!/usr/bin/env python3
"""Сквозная проверка РАСШИРЕНИЯ: zip -> чистый профиль Blender (extension install-file) -> e2e_inside.py (сценарий пользователя на настоящих jar).

    python3 blender/tests/addon/e2e_extension.py [--zip blender/dist/mcgen-0.1.0-linux-x64.zip] [--blender 4.5,5.2] [--n 32] [--json результат.json]

Нужны: собранное расширение (tools/build_extension.py), jar игры 26.3 (jars/server-26.3.jar, jars/client-26.3.jar или --server/--client), Java 25.
Профиль, кэш и .blend — во временном каталоге (tempfile); после прогона каталог удаляется.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
BLENDERS = {
    '4.5': os.environ.get('BLENDER_45') or os.path.expanduser('~/tools/blender-4.5.14-linux-x64/blender'),
    '5.2': os.environ.get('BLENDER_52') or os.path.expanduser('~/tools/blender-5.2.2-linux-x64/blender'),
}


def run(ver, zip_path, a):
    tmp = tempfile.mkdtemp(prefix='mcgen-e2e-')
    prof = os.path.join(tmp, 'profile')
    env = dict(os.environ, BLENDER_USER_RESOURCES=prof, MCGEN_CACHE=os.path.join(tmp, 'cache'), PYTHONDONTWRITEBYTECODE='1', TMPDIR=tmp,
               MCGEN_E2E_SERVER_JAR=os.path.abspath(a.server), MCGEN_E2E_CLIENT_JAR=os.path.abspath(a.client), MCGEN_E2E_N=str(a.n),
               MCGEN_E2E_BLEND=os.path.join(tmp, 'e2e.blend'))
    for k in ('MCGEN_LIB', 'MCGEN_BACKEND', 'MCGEN_REAL_LIB'):
        env.pop(k, None)               # проверяем библиотеку из самого расширения
    try:
        t0 = time.time()
        if a.dev:                      # отладка сценария: исходники аддона по ссылке (+ библиотека из MCGEN_DEV_LIB), без zip
            os.makedirs(os.path.join(prof, 'extensions', 'user_default'))
            os.symlink(os.path.join(REPO, 'blender', 'mcgen_addon'), os.path.join(prof, 'extensions', 'user_default', 'mcgen'))
            if os.environ.get('MCGEN_DEV_LIB'):
                env['MCGEN_LIB'] = os.environ['MCGEN_DEV_LIB']
        else:
            r = subprocess.run([BLENDERS[ver], '-b', '--factory-startup', '--command', 'extension', 'install-file', '-r', 'user_default', '-e', zip_path],
                               capture_output=True, text=True, env=env)
            if r.returncode != 0:
                return {'blender': ver, 'ok': False, 'stage': 'install', 'log': (r.stdout + r.stderr)[-1500:]}
        r = subprocess.run([BLENDERS[ver], '-b', '--factory-startup', '--python', os.path.join(HERE, 'e2e_inside.py')], capture_output=True, text=True, env=env)
        log = r.stdout + r.stderr
        line = next((ln for ln in log.splitlines() if ln.startswith('E2E ')), None)
        res = json.loads(line[4:]) if line else {}
        res.update(blender_series=ver, ok=r.returncode == 0 and bool(line), seconds=round(time.time() - t0, 1))
        if not res['ok']:
            res['log'] = '\n'.join(ln for ln in log.splitlines() if 'E2E-FAIL' in ln or 'Traceback' in ln or 'Error' in ln)[-2000:] or log[-2000:]
        return res
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip', default=os.path.join(REPO, 'blender', 'dist', 'mcgen-0.1.0-linux-x64.zip'))
    ap.add_argument('--blender', default='4.5,5.2')
    ap.add_argument('--n', type=int, default=32)
    ap.add_argument('--server', default=os.path.join(REPO, 'jars', 'server-26.3.jar'))
    ap.add_argument('--client', default=os.path.join(REPO, 'jars', 'client-26.3.jar'))
    ap.add_argument('--json')
    ap.add_argument('--dev', action='store_true', help='отладка: аддон из исходников по ссылке (библиотека — $MCGEN_DEV_LIB), без установки zip')
    a = ap.parse_args()
    results = []
    for ver in a.blender.split(','):
        r = run(ver, a.zip, a)
        results.append(r)
        print(ver, 'OK' if r['ok'] else 'FAIL', {k: r.get(k) for k in ('prepare_s', 'generate_total_s', 'generate', 'edit_ms', 'load_voxels_s', 'update_layers_s', 'peak_rss_mb', 'failed')}
              if r['ok'] else r.get('log', r))
    if a.json:
        json.dump(results, open(a.json, 'w'), indent=1, ensure_ascii=False)
    sys.exit(0 if all(r['ok'] for r in results) else 1)


if __name__ == '__main__':
    main()

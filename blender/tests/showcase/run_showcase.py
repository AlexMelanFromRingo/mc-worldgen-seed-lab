#!/usr/bin/env python3
"""Съёмка показательных кадров: чистый профиль Blender с аддоном из исходников + shoot.py на каждую сцену.

    python3 blender/tests/showcase/run_showcase.py [forest village ...] [--preview] [--blender 4.5] [--override '{"cam":{"azimuth":30}}']

Без имён сцен снимает все записи scenes.json. Кадры — docs/blender/img/render-<сцена>.jpg (при --preview: preview-<сцена>.jpg в --outdir).
Библиотека: $MCGEN_LIB, иначе lib/<платформа>/ аддона или libmcgen/build (см. core/paths.py). Ресурсы Mojang — run/pack-<V>, run/assets-<V>
(tools/make_pack.py); в репозиторий кадры попадают как рендеры, файлы игры — нет. Временные файлы — в каталоге tempfile.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
BLENDERS = {
    '4.5': os.environ.get('BLENDER_45') or os.path.expanduser('~/tools/blender-4.5.14-linux-x64/blender'),
    '5.2': os.environ.get('BLENDER_52') or os.path.expanduser('~/tools/blender-5.2.2-linux-x64/blender'),
}


def make_profile(tmp):
    res = os.path.join(tmp, 'profile')
    os.makedirs(os.path.join(res, 'extensions', 'user_default'), exist_ok=True)
    link = os.path.join(res, 'extensions', 'user_default', 'mcgen')
    if not os.path.lexists(link):
        os.symlink(os.path.join(REPO, 'blender', 'mcgen_addon'), link)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenes', nargs='*')
    ap.add_argument('--blender', default='4.5')
    ap.add_argument('--preview', action='store_true')
    ap.add_argument('--outdir', default=os.path.join(REPO, 'docs', 'blender', 'img'))
    ap.add_argument('--override', default='{}')
    ap.add_argument('--engine', default='eevee')
    ap.add_argument('--save-blend', action='store_true', help='сохранить .blend сцены рядом с кадром (в tempfile-каталог запуска)')
    a = ap.parse_args()
    spec = json.load(open(os.path.join(HERE, 'scenes.json')))
    names = a.scenes or [k for k in spec if not k.startswith('_')]
    tmp = tempfile.mkdtemp(prefix='mcgen-showcase-')
    prof = make_profile(tmp)
    env = dict(os.environ, BLENDER_USER_RESOURCES=prof, MCGEN_CACHE=os.path.join(tmp, 'cache'), PYTHONDONTWRITEBYTECODE='1')
    os.makedirs(a.outdir, exist_ok=True)
    rc_all = 0
    try:
        for n in names:
            cmd = [BLENDERS[a.blender], '-b', '--factory-startup', '--python', os.path.join(HERE, 'shoot.py'), '--', '--scene', n,
                   '--outdir', a.outdir, '--override', a.override, '--engine', a.engine]
            if a.preview:
                cmd.append('--preview')
            if a.save_blend:
                cmd += ['--save-blend', os.path.join(tmp, n + '.blend')]
            r = subprocess.run(cmd, capture_output=True, text=True, env=env)
            line = next((ln for ln in (r.stdout + r.stderr).splitlines() if ln.startswith('RESULT ')), None)
            print(n, 'OK' if r.returncode == 0 and line else 'FAIL', line[7:] if line else (r.stdout + r.stderr)[-1500:], flush=True)
            rc_all |= r.returncode
    finally:
        if os.environ.get('MCGEN_KEEP_TMP'):
            print('временный каталог:', tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(rc_all)


if __name__ == '__main__':
    main()

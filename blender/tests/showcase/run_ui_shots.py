#!/usr/bin/env python3
"""Драйвер снимков интерфейса: Xvfb + настоящий GUI Blender + ui_shots.py, затем нарезка/сборка кадров в docs/blender/img/ui-*.

    python3 blender/tests/showcase/run_ui_shots.py [--blender 4.5] [--modes panels,viewport,progress,biomemap] [--langs en,ru] [--raw]

Сырые кадры (до нарезки) — во временном каталоге tempfile; с --raw они остаются там и путь печатается. Нужны xvfb-run и Pillow.
Нарезка панелей (crop_panels) — по координатам блоков, найденным по заголовкам панелей (светлая полоса), см. split_panels().
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
BLENDERS = {
    '4.5': os.environ.get('BLENDER_45') or os.path.expanduser('~/tools/blender-4.5.14-linux-x64/blender'),
    '5.2': os.environ.get('BLENDER_52') or os.path.expanduser('~/tools/blender-5.2.2-linux-x64/blender'),
}
GEOM = {'prepare': (1280, 800), 'panels': (720, 4800), 'viewport': (1920, 1200), 'progress': (1920, 1200), 'biomemap': (1920, 1200)}


def run_one(blender, mode, lang, out, prof, cache, variant='default'):
    w, h = GEOM[mode]
    env = dict(os.environ, BLENDER_USER_RESOURCES=prof, MCGEN_CACHE=cache, PYTHONDONTWRITEBYTECODE='1', LIBGL_ALWAYS_SOFTWARE='1')
    env.pop('DISPLAY', None)
    cmd = ['xvfb-run', '-a', '-s', f'-screen 0 {w}x{h}x24', blender, '--factory-startup', '--window-geometry', '0', '0', str(w), str(h),
           '--python', os.path.join(HERE, 'ui_shots.py'), '--', '--mode', mode, '--lang', lang, '--out', out, '--variant', variant]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=1800)
    log = [ln for ln in (r.stdout + r.stderr).splitlines() if ln.startswith('UI-SHOTS') or 'Error' in ln or 'Traceback' in ln]
    print(mode, lang, variant, 'rc=%d' % r.returncode, *log[-6:], sep='\n  ', flush=True)
    return r.returncode


def header_runs(img):
    """Заголовки подпанелей в высоком кадре Properties: полосы цвета 61 (светлее фона 48/54) высотой ~25 px на x=250 (пустое место заголовка)."""
    v = np.asarray(img.convert('RGB'))[:, 250, 0].astype(int)
    runs, start = [], 0
    for y in range(1, len(v) + 1):
        if y == len(v) or v[y] != v[start]:
            runs.append((start, y - 1, int(v[start])))
            start = y
    first_btn = next(r for r in runs if r[2] == 84)             # кнопка Generate (цвет 84) — под заголовком «MC World»
    main_top = runs[runs.index(first_btn) - 1][0]
    heads = [a for a, b, c in runs if c == 61 and 22 <= b - a + 1 <= 28 and a > main_top]
    return main_top, heads


PANEL_ORDER = ['seeds', 'area', 'layers', 'tweaks', 'view', 'biomes', 'resources', 'stats', 'end']


def split_panels(path):
    """-> {'main','area','tweaks','view','resources','stats' (+ 'seeds' отдельно)}: PIL-изображения. Проверяет, что найдены все 8 подпанелей + «Animation»."""
    img = Image.open(path).convert('RGB')
    top, heads = header_runs(img)
    # 8 подпанелей MC World идут первыми; за ними — штатные панели Scene «Animation» и «Custom Properties» (два заголовка подряд, зазор ~3 px): конец
    # Stats — начало «Animation». Кнопки внутри панелей (Structure Markers) тоже могут давать полосу того же цвета, поэтому «конец» берём с хвоста списка.
    if len(heads) < 10 or heads[-1] - heads[-2] > 32:
        raise SystemExit('не найдены заголовки подпанелей в %s: %s' % (path, heads))
    pos = dict(zip(PANEL_ORDER, heads[:8] + [heads[-2]]))
    w = img.width

    def box(a, b):
        return img.crop((0, a, w, b))
    return {'main': box(top, pos['area']), 'seeds': box(pos['seeds'], pos['area']), 'area': box(pos['area'], pos['tweaks']),
            'tweaks': box(pos['tweaks'], pos['view']), 'view': box(pos['view'], pos['resources']), 'resources': box(pos['resources'], pos['stats']),
            'stats': box(pos['stats'], pos['end'])}


def pair(a, b, gap=24, bg=(48, 48, 48)):
    h = max(a.height, b.height)
    out = Image.new('RGB', (a.width + gap + b.width, h), bg)
    out.paste(a, (0, 0))
    out.paste(b, (a.width + gap, 0))
    return out


def save_png(im, path):
    im.save(path, optimize=True)
    print('  ', os.path.relpath(path, REPO), os.path.getsize(path) // 1024, 'КБ', im.size)


def compose(raw, outdir, variants=('default',)):
    """Собирает docs/blender/img/ui-*.png (EN | RU рядом) и ui-<вид>-<язык>.jpg."""
    for var in variants:
        en = split_panels(os.path.join(raw, f'panels-{var}-en.png'))
        ru = split_panels(os.path.join(raw, f'panels-{var}-ru.png'))
        if var == 'default':
            for k in ('main', 'area', 'tweaks', 'view', 'resources', 'stats'):
                save_png(pair(en[k], ru[k]), os.path.join(outdir, f'ui-{k}.png'))
        else:
            save_png(pair(en['seeds'], ru['seeds']), os.path.join(outdir, f'ui-seeds-{var}.png'))
            save_png(pair(en['tweaks'], ru['tweaks']), os.path.join(outdir, f'ui-tweaks-{var}.png'))
    for k in ('viewport', 'progress', 'biomemap'):
        for lang in ('en', 'ru'):
            src = os.path.join(raw, f'{k}-{lang}.png')
            if os.path.exists(src):
                dst = os.path.join(outdir, f'ui-{k}-{lang}.jpg')
                Image.open(src).convert('RGB').save(dst, quality=88, optimize=True)
                print('  ', os.path.relpath(dst, REPO), os.path.getsize(dst) // 1024, 'КБ')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--blender', default='4.5')
    ap.add_argument('--modes', default='panels,viewport,progress,biomemap')
    ap.add_argument('--langs', default='en,ru')
    ap.add_argument('--raw', default=None, help='каталог сырых кадров (по умолчанию — временный); с --compose-only только нарезка')
    ap.add_argument('--compose-only', action='store_true')
    ap.add_argument('--outdir', default=os.path.join(REPO, 'docs', 'blender', 'img'))
    a = ap.parse_args()
    # нейтральный каталог кэша (путь виден на кадрах панели Resources): <tmp>/mcgen-ui; подготовленные ресурсы переживают запуски
    root = os.path.join(tempfile.gettempdir(), 'mcgen-ui')
    prof = os.path.join(root, 'profile')
    cache = os.path.join(root, 'cache')
    raw = a.raw or os.path.join(root, 'raw')
    os.makedirs(os.path.join(prof, 'extensions', 'user_default'), exist_ok=True)
    link = os.path.join(prof, 'extensions', 'user_default', 'mcgen')
    if not os.path.lexists(link):
        os.symlink(os.path.join(REPO, 'blender', 'mcgen_addon'), link)
    os.makedirs(raw, exist_ok=True)
    rc = 0
    if not a.compose_only:
        modes = a.modes.split(',')
        if not os.path.isdir(os.path.join(cache, 'packs')):
            rc |= run_one(BLENDERS[a.blender], 'prepare', 'en', raw, prof, cache)
        for mode in modes:
            for lang in a.langs.split(','):
                rc |= run_one(BLENDERS[a.blender], mode, lang, raw, prof, cache)
                if mode == 'panels':
                    rc |= run_one(BLENDERS[a.blender], mode, lang, raw, prof, cache, variant='split')
    compose(raw, a.outdir, ('default', 'split'))
    sys.exit(rc)


if __name__ == '__main__':
    main()

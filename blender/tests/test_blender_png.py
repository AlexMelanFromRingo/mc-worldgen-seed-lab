"""Сверка собственного PNG-декодера (assets/pngio.py) с декодером Blender (libpng/OIIO) на всех текстурах блоков и colormaps.
Запуск: blender -b --factory-startup --python blender/tests/test_blender_png.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402

from mcgen_addon.assets import pngio  # noqa: E402


def main():
    base = os.path.join(_boot.ASSETS_DIR, 'assets', 'minecraft', 'textures')
    files = []
    for sub in ('block', 'colormap'):
        for dp, _, fn in os.walk(os.path.join(base, sub)):
            files += [os.path.join(dp, f) for f in sorted(fn) if f.endswith('.png')]
    bad = []
    n = 0
    maxdiff = 0
    for p in files:
        mine = pngio.read_png(p)
        img = bpy.data.images.load(p)
        img.colorspace_settings.name = 'Non-Color'
        img.alpha_mode = 'STRAIGHT'
        w, h = img.size
        px = np.empty(w * h * 4, dtype=np.float32)
        img.pixels.foreach_get(px)
        bl = np.round(px.reshape(h, w, 4)[::-1] * 255.0).astype(np.int32)
        bpy.data.images.remove(img)
        if bl.shape != mine.shape:
            bad.append((p, 'shape', bl.shape, mine.shape))
            continue
        m = mine.astype(np.int32)
        # у полностью прозрачных пикселей RGB может отличаться (premultiplied): сравниваем RGB только там, где alpha > 0
        mask = m[:, :, 3] > 0
        d = np.abs(bl - m)
        diff = max(int(d[mask].max()) if mask.any() else 0, int(d[:, :, 3].max()))
        maxdiff = max(maxdiff, diff)
        if diff > 1:
            bad.append((p, 'diff', diff))
        n += 1
    print('сверено файлов: %d, максимальное расхождение: %d, провалов: %d' % (n, maxdiff, len(bad)))
    for b in bad[:10]:
        print(' ', b)
    print('ИТОГО: %s' % ('OK' if not bad else 'ПРОВАЛ'))
    if bad:
        sys.exit(1)


main()

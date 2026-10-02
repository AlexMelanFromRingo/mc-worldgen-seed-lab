"""Headless-рендер сцены из настоящих чанков .mca (проверка текстур, поворотов, оттенков, воды, листвы, швов между чанками).

    blender -b --python blender/tests/render_mca.py -- --cx0 0 --cz0 0 --nx 4 --nz 4 --out overview [опции]

Рисует PNG в docs/blender/img/<out>.png (или --outdir) и печатает JSON со временем сборки. Нужны run/assets-26.3, run/pack-26.3,
run/server-26.3 (мир w12345). Работает в Blender 4.5 и 5.x (выбор движка EEVEE по версии).
"""
import argparse
import json
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402  (подменяет пакет mcgen_addon без выполнения его __init__)
import anvil_util  # noqa: E402
import bpy  # noqa: E402
import mathutils  # noqa: E402
import numpy as np  # noqa: E402

from mcgen_addon.assets import state_table  # noqa: E402
from mcgen_addon.render import scene as scene_mod  # noqa: E402
from render_common import setup_render, setup_camera, clean_default_scene  # noqa: E402


def parse():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--cx0', type=int, default=0)
    ap.add_argument('--cz0', type=int, default=0)
    ap.add_argument('--nx', type=int, default=4)
    ap.add_argument('--nz', type=int, default=4)
    ap.add_argument('--dim', default='overworld')
    ap.add_argument('--out', default='render')
    ap.add_argument('--outdir', default=os.path.join(_boot.REPO, 'docs', 'blender', 'img'))
    ap.add_argument('--engine', default='eevee', choices=['eevee', 'workbench', 'cycles'])
    ap.add_argument('--res', default='1280x720')
    ap.add_argument('--samples', type=int, default=24)
    ap.add_argument('--cam', default='iso', choices=['iso', 'top', 'persp', 'ortho'])
    ap.add_argument('--yaw', type=float, default=35.0, help='азимут камеры от севера, градусы (по часовой)')
    ap.add_argument('--pitch', type=float, default=38.0, help='угол места камеры, градусы')
    ap.add_argument('--zoom', type=float, default=1.0)
    ap.add_argument('--target-y', type=float, default=None, help='высота точки взгляда (блоки); по умолчанию — поверхность в центре')
    ap.add_argument('--dist', type=float, default=None)
    ap.add_argument('--ymax', type=int, default=None, help='обрезать блоки выше этой y (разрез)')
    ap.add_argument('--ymin', type=int, default=None, help='обрезать блоки ниже этой y')
    ap.add_argument('--per-object', type=int, default=1)
    ap.add_argument('--shading', default='lit')
    ap.add_argument('--blend', type=int, default=2)
    ap.add_argument('--no-cutout', action='store_true')
    ap.add_argument('--merge', action='store_true')
    ap.add_argument('--lod', action='store_true')
    ap.add_argument('--lod-distance', type=int, default=2)
    ap.add_argument('--lod-stride', type=int, default=2)
    ap.add_argument('--lod-center', default=None, help='cx,cz центра LOD (по умолчанию — центр региона)')
    ap.add_argument('--smooth', action='store_true', help='Linear вместо Closest')
    ap.add_argument('--bake-shade', action='store_true')
    ap.add_argument('--sun-az', type=float, default=140.0)
    ap.add_argument('--sun-el', type=float, default=48.0)
    ap.add_argument('--sun-strength', type=float, default=4.0)
    ap.add_argument('--save-blend', default=None)
    ap.add_argument('--cache', default=os.path.join(_boot.SCRATCH, 'cache'))
    ap.add_argument('--version', default=_boot.VERSION)
    ap.add_argument('--ring', type=int, default=1, help='ширина кольца соседних чанков-контекста')
    ap.add_argument('--mcr', default=None, help='дамп региона MCR1 libmcgen (tools/gt/mcr.py) вместо .mca сервера')
    return ap.parse_args(argv)


DIMS = {'overworld': (-64, 384), 'the_nether': (0, 256), 'the_end': (0, 256)}


def load_world(a, table, biome_names, rd):
    bidx = {n: i for i, n in enumerate(biome_names)}
    blocks, bio = {}, {}
    ring = a.ring
    for cz in range(a.cz0 - ring, a.cz0 + a.nz + ring):
        for cx in range(a.cx0 - ring, a.cx0 + a.nx + ring):
            r = anvil_util.load_chunk(rd, cx, cz, table.state_from_props, lambda n: bidx.get(n, 0), min_y=DIMS[a.dim][0], height=DIMS[a.dim][1])
            if r is None:
                continue
            blocks[(cx, cz)], bio[(cx, cz)] = r
    return blocks, bio


def main():
    a = parse()
    t_all = time.time()
    rd = os.path.join(_boot.SERVER_DIR, 'w12345', 'dimensions', 'minecraft', a.dim, 'region')
    os.makedirs(a.cache, exist_ok=True)
    t0 = time.time()
    table = state_table.load(_boot.ASSETS_DIR, _boot.PACK_DIR, a.cache, version=a.version)
    t_table = time.time() - t0
    biome_names = sorted('minecraft:' + f[:-5] for f in os.listdir(os.path.join(_boot.PACK_DIR, 'data', 'minecraft', 'worldgen', 'biome')) if f.endswith('.json'))
    t0 = time.time()
    block_names = None
    MCR_DIMS = None
    if a.mcr:
        sys.path.insert(0, os.path.join(_boot.REPO, 'tools', 'gt'))
        import mcr as mcrmod
        mm = mcrmod.Mcr(a.mcr)
        biome_names = list(mm.biome_names)
        block_names = list(mm.state_names)
        DIMS['mcr'] = (mm.min_y, mm.height)
        a.dim = 'mcr'
        blocks, bio = {}, {}
        for cz in range(a.cz0 - a.ring, a.cz0 + a.nz + a.ring):
            for cx in range(a.cx0 - a.ring, a.cx0 + a.nx + a.ring):
                if mm.has(cx, cz):
                    blocks[(cx, cz)] = np.array(mm.blocks(cx, cz)).reshape(-1)
                    bio[(cx, cz)] = np.array(mm.biomes(cx, cz)).reshape(-1)
    else:
        blocks, bio = load_world(a, table, biome_names, rd)
    t_load = time.time() - t0
    MINY, HGT = DIMS[a.dim]
    if a.ymax is not None or a.ymin is not None:
        air = table.state_id('minecraft:air')
        for k, arr in blocks.items():
            v = arr.reshape(HGT, 256)
            if a.ymax is not None:
                v[a.ymax - MINY + 1:] = air
            if a.ymin is not None:
                v[:a.ymin - MINY] = air
    vs = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=a.cache, version=a.version,
                                chunks_per_object=a.per_object, biome_blend=a.blend, cutout_leaves=not a.no_cutout,
                                bake_shade=a.bake_shade, shading=a.shading, merge_flat=a.merge, lod=a.lod, lod_distance=a.lod_distance,
                                lod_stride=a.lod_stride, pixel_style=not a.smooth)
    clean_default_scene()
    sb = scene_mod.SceneBuilder(vs)
    if a.lod_center:
        sb.lod_center = tuple(int(v) for v in a.lod_center.split(','))
    info = {'cx0': a.cx0, 'cz0': a.cz0, 'nx': a.nx, 'nz': a.nz, 'min_y': MINY, 'height': HGT}
    stats = sb.build(blocks, bio, info, block_names, biome_names)
    # границы по высоте поверхности (по существующим блокам)
    ys = []
    for k, arr in blocks.items():
        if a.cx0 <= k[0] < a.cx0 + a.nx and a.cz0 <= k[1] < a.cz0 + a.nz:
            drawable = (table.st_flags[arr.reshape(HGT, 256)] & (state_table.F.GEOM | state_table.F.WATER | state_table.F.LAVA)) != 0
            rows = np.nonzero(drawable.any(axis=1))[0]
            if len(rows):
                ys.append(int(rows[-1]))
    ytop = (max(ys) if ys else 128) + MINY
    cx0b, cx1b = a.cx0 * 16, (a.cx0 + a.nx) * 16
    cy0b, cy1b = -(a.cz0 + a.nz) * 16, -a.cz0 * 16
    ysurf = float(np.median(ys)) + MINY if ys else 64
    setup_render(a)
    setup_camera(a, ((cx0b, cy0b, MINY if a.ymin is not None else ysurf - 16), (cx1b, cy1b, ytop)))
    os.makedirs(a.outdir, exist_ok=True)
    out = os.path.join(a.outdir, a.out + '.png')
    bpy.context.scene.render.filepath = out
    t0 = time.time()
    bpy.ops.render.render(write_still=True)
    t_render = time.time() - t0
    if a.save_blend:
        bpy.ops.wm.save_as_mainfile(filepath=a.save_blend)
    res = {'out': out, 'blender': bpy.app.version_string, 'table_seconds': round(t_table, 2), 'load_seconds': round(t_load, 2),
           'build': {k: (round(v, 3) if isinstance(v, float) else v) for k, v in stats.items()}, 'render_seconds': round(t_render, 2),
           'total_seconds': round(time.time() - t_all, 2)}
    print('RESULT ' + json.dumps(res))


main()

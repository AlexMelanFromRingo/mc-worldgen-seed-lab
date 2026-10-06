"""Витрина материалов для проверки PBR (headless): синтетический чанк, два ряда блоков разных классов (камень, кирпич, дерево, металлы, руды, стекло, лёд,
светящиеся…) на тёмном полу; низкое солнце, чтобы рельеф нормалей был виден. Рисует PNG без PBR и с PBR.

    blender -b --factory-startup --python blender/tests/render_pbr_showcase.py -- --out /tmp/pbr [--engine cycles|eevee] [--res 1600x700] [--samples 64]
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402
import bpy  # noqa: E402
import mathutils  # noqa: E402
import numpy as np  # noqa: E402

import common  # noqa: E402
from mcgen_addon.render import scene as scene_mod  # noqa: E402
from render_common import clean_default_scene, setup_render  # noqa: E402

ROW_A = ['stone', 'cobblestone', 'stone_bricks', 'polished_andesite', 'deepslate_bricks', 'oak_planks', 'spruce_log', 'bricks', 'diamond_ore', 'iron_ore', 'gold_ore',
         'redstone_ore', 'copper_ore', 'coal_ore']
ROW_B = ['iron_block', 'gold_block', 'copper_block', 'weathered_cut_copper', 'diamond_block', 'glass', 'ice', 'obsidian', 'glowstone', 'sea_lantern', 'magma_block',
         'white_wool', 'light_blue_glazed_terracotta', 'netherite_block']


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(_boot.SCRATCH, 'pbr_showcase'))
    ap.add_argument('--engine', default='cycles', choices=['cycles', 'eevee', 'workbench'])
    ap.add_argument('--res', default='1600x700')
    ap.add_argument('--samples', type=int, default=64)
    ap.add_argument('--only', default=None, choices=[None, 'off', 'on'])
    a = ap.parse_args(argv)
    a.sun_az, a.sun_el, a.sun_strength = 250.0, 24.0, 5.0
    t = common.table()
    air = t.state_id('minecraft:air')
    blocks = {}
    H = 32
    for cx in (-1, 0, 1):
        for cz in (-1, 0, 1):
            blocks[(cx, cz)] = np.full(H * 256, air, np.uint16)
    def sid_of(n):
        v = t.state_from_props('minecraft:' + n, {})
        if v < 0:
            raise KeyError(n)
        return v

    floor = sid_of('deepslate')
    for (cx, cz), arr in blocks.items():
        arr.reshape(H, 16, 16)[0] = floor
    ch = blocks[(0, 0)].reshape(H, 16, 16)
    for row, names, z in ((0, ROW_A, 5), (1, ROW_B, 9)):
        for i, n in enumerate(names):
            sid = sid_of(n)
            ch[1, z, i + 1] = sid
            ch[2, z, i + 1] = sid if n in ('stone',) else air
    bio = {k: np.zeros(H // 4 * 16, np.uint8) for k in blocks}
    os.makedirs(a.out, exist_ok=True)
    for mode in ('off', 'on'):
        if a.only and a.only != mode:
            continue
        clean_default_scene()
        for m in list(bpy.data.materials):
            bpy.data.materials.remove(m)
        vs = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=os.path.join(_boot.SCRATCH, 'cache'), merge_flat=True,
                                    pbr=(mode == 'on'))
        sb = scene_mod.SceneBuilder(vs)
        sb.build(blocks, bio, {'min_y': 0, 'height': H, 'cx0': 0, 'cz0': 0, 'nx': 1, 'nz': 1}, None, common.biome_names())
        setup_render(a)
        scn = bpy.context.scene
        # камера: чуть сверху, смотрит на два ряда
        cam = bpy.data.cameras.new('cam')
        co = bpy.data.objects.new('cam', cam)
        scn.collection.objects.link(co)
        scn.camera = co
        cam.type = 'PERSP'
        cam.lens = 42
        target = mathutils.Vector((8.0, -7.6, 1.0))
        pos = target + mathutils.Vector((0.0, -12.5, 8.5))
        co.location = pos
        co.rotation_euler = (target - pos).normalized().to_track_quat('-Z', 'Y').to_euler()
        scn.render.filepath = os.path.join(a.out, 'showcase_%s.png' % mode)
        bpy.ops.render.render(write_still=True)
        print('SAVED', scn.render.filepath)


main()

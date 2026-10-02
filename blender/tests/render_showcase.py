"""Headless-рендер «витрины» блоков (синтетический мир): повороты и формы блоков, ступени/плиты/двери/заборы/панели, растения, жидкости.

    blender -b --python blender/tests/render_showcase.py -- --out showcase_a [--rows 0,1,2] [--cam persp] ...
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402

from mcgen_addon.assets import state_table  # noqa: E402
from mcgen_addon.render import scene as scene_mod  # noqa: E402
from render_common import setup_render, setup_camera, clean_default_scene  # noqa: E402


def parse():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='showcase')
    ap.add_argument('--outdir', default=os.path.join(_boot.REPO, 'docs', 'blender', 'img'))
    ap.add_argument('--sheet', default='a')
    ap.add_argument('--engine', default='eevee')
    ap.add_argument('--res', default='1600x900')
    ap.add_argument('--samples', type=int, default=24)
    ap.add_argument('--cam', default='persp')
    ap.add_argument('--yaw', type=float, default=-20.0)
    ap.add_argument('--pitch', type=float, default=42.0)
    ap.add_argument('--zoom', type=float, default=1.0)
    ap.add_argument('--target-y', type=float, default=None)
    ap.add_argument('--dist', type=float, default=None)
    ap.add_argument('--shading', default='lit')
    ap.add_argument('--sun-az', type=float, default=150.0)
    ap.add_argument('--sun-el', type=float, default=50.0)
    ap.add_argument('--sun-strength', type=float, default=4.0)
    ap.add_argument('--cache', default=os.path.join(_boot.SCRATCH, 'cache'))
    ap.add_argument('--biome', default='minecraft:plains')
    ap.add_argument('--save-blend', default=None)
    return ap.parse_args(argv)


# ---------------------------------------------------------------------------------------------------------------------
# листы витрины: список рядов; ряд — список (имя блока, {свойства}); блоки ставятся вдоль +X с шагом 2, ряды — вдоль +Z с шагом 3
# ---------------------------------------------------------------------------------------------------------------------
def S(name, **props):
    return ('minecraft:' + name, props)


SHEETS = {
    'e': [   # блоки с моделями сущностей (заглушки): сундуки, баннеры, шулкеры, головы, колокол, портал Края
        [S('chest', facing='north', type='single'), S('chest', facing='east', type='single'), S('chest', facing='south', type='left'),
         S('chest', facing='south', type='right'), S('trapped_chest', facing='west', type='single'), S('ender_chest', facing='north'),
         S('copper_chest', facing='north', type='single'), S('oxidized_copper_chest', facing='east', type='single'),
         S('bell', attachment='floor', facing='north', powered='false'), S('enchanting_table'), S('end_portal')],
        [S('red_banner', rotation='0'), S('blue_banner', rotation='4'), S('white_banner', rotation='8'), S('lime_banner', rotation='12'),
         S('yellow_wall_banner', facing='north'), S('black_wall_banner', facing='east'), S('orange_wall_banner', facing='south'),
         S('shulker_box', facing='up'), S('purple_shulker_box', facing='up'), S('cyan_shulker_box', facing='north'), S('lime_shulker_box', facing='east')],
        [S('skeleton_skull', rotation='0', powered='false'), S('zombie_head', rotation='4', powered='false'), S('creeper_head', rotation='8', powered='false'),
         S('wither_skeleton_skull', rotation='12', powered='false'), S('player_head', rotation='2', powered='false'),
         S('skeleton_wall_skull', facing='north', powered='false'), S('zombie_wall_head', facing='east', powered='false'),
         S('creeper_wall_head', facing='south', powered='false'), S('piglin_head', rotation='0', powered='false'), S('white_bed', facing='east', part='foot'),
         S('white_bed', facing='east', part='head')],
    ],
    'a': [   # кубы, оси, поворачиваемые кубы, листва, стекло
        [S('grass_block'), S('dirt'), S('stone'), S('cobblestone'), S('oak_planks'), S('sand'), S('glass'), S('oak_leaves'), S('birch_leaves'),
         S('spruce_leaves'), S('jungle_leaves'), S('ice'), S('sea_lantern'), S('bookshelf'), S('crafting_table'), S('tnt')],
        [S('oak_log', axis='x'), S('oak_log', axis='y'), S('oak_log', axis='z'), S('birch_log', axis='y'), S('stripped_oak_log', axis='x'),
         S('hay_block', axis='x'), S('quartz_pillar', axis='z'), S('bone_block', axis='x'), S('basalt', axis='y'), S('deepslate', axis='z'),
         S('furnace', facing='north', lit='false'), S('furnace', facing='east', lit='true'), S('dispenser', facing='up'), S('observer', facing='south'),
         S('piston', facing='east', extended='false'), S('sticky_piston', facing='up', extended='false')],
        [S('carved_pumpkin', facing='north'), S('carved_pumpkin', facing='east'), S('jack_o_lantern', facing='south'), S('jack_o_lantern', facing='west'),
         S('glazed_terracotta' if False else 'white_glazed_terracotta', facing='north'), S('white_glazed_terracotta', facing='east'),
         S('white_glazed_terracotta', facing='south'), S('white_glazed_terracotta', facing='west'), S('magma_block'), S('obsidian'),
         S('white_wool'), S('red_wool'), S('white_stained_glass'), S('red_stained_glass'), S('slime_block'), S('honey_block')],
    ],
    'b': [   # ступени всех ориентаций и форм
        [S('oak_stairs', facing=f, half='bottom', shape='straight') for f in ('north', 'east', 'south', 'west')] +
        [S('oak_stairs', facing=f, half='top', shape='straight') for f in ('north', 'east', 'south', 'west')] +
        [S('stone_stairs', facing='north', half='bottom', shape=sh) for sh in ('inner_left', 'inner_right', 'outer_left', 'outer_right')] +
        [S('brick_stairs', facing='east', half='top', shape=sh) for sh in ('inner_left', 'outer_right')],
        [S('oak_slab', type='bottom'), S('oak_slab', type='top'), S('oak_slab', type='double'), S('stone_slab', type='bottom'),
         S('stone_slab', type='top'), S('smooth_stone_slab', type='double'), S('oak_trapdoor', facing='north', half='bottom', open='false'),
         S('oak_trapdoor', facing='north', half='top', open='false'), S('oak_trapdoor', facing='north', half='bottom', open='true'),
         S('oak_trapdoor', facing='east', half='bottom', open='true'), S('iron_trapdoor', facing='south', half='top', open='true'),
         S('oak_button', face='floor', facing='north'), S('oak_button', face='wall', facing='east'), S('oak_button', face='ceiling', facing='south'),
         S('stone_pressure_plate'), S('lever', face='wall', facing='west')],
        [S('oak_door', facing='north', half='lower', hinge='left', open='false'), S('oak_door', facing='north', half='upper', hinge='left', open='false'),
         S('oak_door', facing='east', half='lower', hinge='right', open='false'), S('oak_door', facing='east', half='upper', hinge='right', open='false'),
         S('oak_door', facing='north', half='lower', hinge='left', open='true'), S('oak_door', facing='north', half='upper', hinge='left', open='true'),
         S('iron_door', facing='south', half='lower', hinge='left', open='false'), S('iron_door', facing='south', half='upper', hinge='left', open='false'),
         S('oak_fence_gate', facing='north', open='false', in_wall='false'), S('oak_fence_gate', facing='east', open='true', in_wall='false'),
         S('oak_sign', rotation='0'), S('oak_sign', rotation='4'), S('oak_wall_sign', facing='north'), S('oak_wall_sign', facing='east'),
         S('white_bed', facing='north', part='foot'), S('white_bed', facing='north', part='head')],
    ],
    'c': [   # заборы, стены, панели (со связями), плиты-детали
        [S('oak_fence', north='false', east='false', south='false', west='false'), S('oak_fence', north='true', east='false', south='false', west='false'),
         S('oak_fence', north='true', east='true', south='false', west='false'), S('oak_fence', north='true', east='true', south='true', west='false'),
         S('oak_fence', north='true', east='true', south='true', west='true'), S('nether_brick_fence', north='false', east='true', south='false', west='true'),
         S('cobblestone_wall', north='none', east='none', south='none', west='none', up='true'), S('cobblestone_wall', north='low', east='none', south='low', west='none', up='false'),
         S('cobblestone_wall', north='tall', east='tall', south='none', west='none', up='true'), S('cobblestone_wall', north='low', east='low', south='low', west='low', up='true'),
         S('glass_pane', north='false', east='false', south='false', west='false'), S('glass_pane', north='true', east='false', south='true', west='false'),
         S('glass_pane', north='true', east='true', south='false', west='false'), S('iron_bars', north='true', east='true', south='true', west='true'),
         S('red_stained_glass_pane', north='false', east='true', south='false', west='true'), S('iron_chain', axis='y')],
        [S('anvil', facing='north'), S('anvil', facing='east'), S('cauldron'), S('water_cauldron', level='3'), S('hopper', facing='down', enabled='true'),
         S('brewing_stand', has_bottle_0='true', has_bottle_1='false', has_bottle_2='true'), S('flower_pot'), S('potted_poppy'),
         S('lectern', facing='north', has_book='true', powered='false'), S('enchanting_table'), S('end_portal_frame', eye='true', facing='north'),
         S('beacon'), S('campfire', facing='north', lit='true', signal_fire='false', waterlogged='false'), S('lantern', hanging='false'),
         S('lantern', hanging='true'), S('conduit')],
        [S('snow', layers=str(i)) for i in range(1, 9)] + [S('white_carpet'), S('farmland', moisture='7'), S('dirt_path'), S('soul_sand'),
                                                            S('cactus', age='0'), S('sugar_cane', age='0'), S('bamboo', age='0', leaves='large', stage='0'), S('scaffolding', bottom='false', distance='0', waterlogged='false')],
    ],
    'd': [   # растения, свет, рельсы, лозы
        [S('short_grass'), S('tall_grass', half='lower'), S('tall_grass', half='upper'), S('fern'), S('large_fern', half='lower'), S('large_fern', half='upper'),
         S('poppy'), S('dandelion'), S('blue_orchid'), S('allium'), S('oak_sapling', stage='0'), S('dead_bush'), S('red_mushroom'), S('brown_mushroom'),
         S('torch'), S('wall_torch', facing='north')],
        [S('rail', shape='north_south'), S('rail', shape='east_west'), S('rail', shape='ascending_east'), S('rail', shape='north_east'), S('powered_rail', shape='north_south', powered='true'),
         S('ladder', facing='north'), S('ladder', facing='east'), S('vine', north='true', east='false', south='false', west='false', up='false'),
         S('vine', north='true', east='true', south='false', west='false', up='true'), S('glow_lichen', north='true', east='false', south='false', west='false', up='false', down='false', west_='x') if False else S('glow_lichen', north='true', up='true'),
         S('lily_pad'), S('wheat', age='7'), S('carrots', age='3'), S('melon_stem', age='7'), S('pumpkin_stem', age='3'), S('redstone_wire', north='side', east='side', south='side', west='side', power='12')],
        [S('pink_petals', facing='north', flower_amount='1'), S('pink_petals', facing='north', flower_amount='4'), S('sweet_berry_bush', age='3'),
         S('cobweb'), S('end_rod', facing='up'), S('lightning_rod', facing='up', powered='false', waterlogged='false'), S('big_dripleaf', facing='north', tilt='none', waterlogged='false'),
         S('small_dripleaf', facing='north', half='lower', waterlogged='false'), S('sea_pickle', pickles='4', waterlogged='false'), S('turtle_egg', eggs='3', hatch='0'),
         S('cocoa', age='2', facing='north'), S('tripwire_hook', facing='north', attached='false', powered='false'), S('cake', bites='0'),
         S('azalea', ), S('flowering_azalea'), S('spore_blossom')],
    ],
}


def build_world(table, sheet, biome_id):
    H = 384
    rows = SHEETS[sheet]
    nx = (max(len(r) for r in rows) * 2 + 3 + 15) // 16
    nz = (len(rows) * 3 + 3 + 15) // 16
    nx, nz = max(nx, 1), max(nz, 1)
    air = table.state_id('minecraft:air')
    stone = table.state_id('minecraft:grass_block')
    floor = table.state_id('minecraft:smooth_stone') if table.state_id('minecraft:smooth_stone') >= 0 else stone
    blocks = {}
    bio = {}
    for cz in range(nz):
        for cx in range(nx):
            a = np.full((H, 16, 16), air, dtype=np.uint16)
            a[63] = floor
            blocks[(cx, cz)] = a
            bio[(cx, cz)] = np.full((H // 4) * 16, biome_id, dtype=np.uint8)
    missing = []
    for ri, row in enumerate(rows):
        for ci, (name, props) in enumerate(row):
            sid = table.state_from_props(name, {k: v for k, v in props.items()})
            if sid < 0:
                missing.append(name)
                continue
            x, z = 1 + ci * 2, 1 + ri * 3
            blocks[(x // 16, z // 16)][64, z % 16, x % 16] = sid
            # двойные блоки: верхняя половина двери/травы — отдельная строка листа; ничего автоматом не добавляем
    return blocks, bio, nx, nz, missing


def main():
    a = parse()
    t0 = time.time()
    table = state_table.load(_boot.ASSETS_DIR, _boot.PACK_DIR, a.cache, version=_boot.VERSION)
    biome_names = sorted('minecraft:' + f[:-5] for f in os.listdir(os.path.join(_boot.PACK_DIR, 'data', 'minecraft', 'worldgen', 'biome')) if f.endswith('.json'))
    bi = biome_names.index(a.biome)
    sid_probe = table.state_from_props('minecraft:oak_door', {'half': 'upper'})
    blocks, bio, nx, nz, missing = build_world(table, a.sheet, bi)
    # верхние половины двойных блоков кладём над нижними
    rows = SHEETS[a.sheet]
    vs = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=a.cache, version=_boot.VERSION, shading=a.shading)
    clean_default_scene()
    sb = scene_mod.SceneBuilder(vs)
    stats = sb.build(blocks, bio, {'min_y': -64, 'height': 384}, None, biome_names)
    w, d = nx * 16, nz * 16
    class A:
        pass
    ra = A()
    ra.engine, ra.res, ra.samples = a.engine, a.res, a.samples
    ra.sun_strength, ra.sun_az, ra.sun_el = a.sun_strength, a.sun_az, a.sun_el
    ra.cam, ra.yaw, ra.pitch, ra.zoom, ra.target_y, ra.dist = a.cam, a.yaw, a.pitch, a.zoom, a.target_y, a.dist
    setup_render(ra)
    nrow = len(rows)
    ncol = max(len(r) for r in rows)
    x1 = 1 + ncol * 2 + 1
    z1 = 1 + nrow * 3
    bbox = ((0, -z1, 0), (x1, 0, 4))
    setup_camera(ra, bbox)
    os.makedirs(a.outdir, exist_ok=True)
    out = os.path.join(a.outdir, a.out + '.png')
    bpy.context.scene.render.filepath = out
    bpy.ops.render.render(write_still=True)
    if a.save_blend:
        bpy.ops.wm.save_as_mainfile(filepath=a.save_blend)
    print('RESULT ' + json.dumps({'out': out, 'missing': missing, 'quads': stats.get('quads'), 'seconds': round(time.time() - t0, 1)}))


main()

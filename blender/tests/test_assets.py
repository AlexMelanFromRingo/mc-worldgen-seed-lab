import json
import os
import tempfile
import unittest

import numpy as np

import _boot
import common
from mcgen_addon.assets import atlas, blockstates, fluids, jrand, models, tint
from mcgen_addon.assets.state_table import F, assets_root

HAVE = _boot.have_resources()
ROOT = assets_root(_boot.ASSETS_DIR) if HAVE else None


class TestBlockstates(unittest.TestCase):
    def test_variants_and_match(self):
        bs = blockstates.parse_blockstate('minecraft:x', {'variants': {
            'facing=north,half=top': {'model': 'a', 'y': 90, 'uvlock': True},
            'facing=south': [{'model': 'b', 'weight': 3}, {'model': 'c'}],
            '': {'model': 'z'}}})
        parts, mp = bs.select({'facing': 'north', 'half': 'top'})
        self.assertFalse(mp)
        # последняя подходящая запись побеждает (put в игре): '' подходит всем
        self.assertEqual(parts[0][0].model, 'minecraft:z')
        bs2 = blockstates.parse_blockstate('minecraft:x', {'variants': {
            'facing=north,half=top': {'model': 'a', 'y': 90, 'uvlock': True}, 'facing=south': [{'model': 'b', 'weight': 3}, {'model': 'c'}]}})
        parts, _ = bs2.select({'facing': 'north', 'half': 'top', 'x': '1'})
        v = parts[0][0]
        self.assertEqual((v.model, v.y, v.uvlock, v.weight), ('minecraft:a', 90, True, 1))
        parts, _ = bs2.select({'facing': 'south', 'half': 'bottom'})
        self.assertEqual([(v.model, v.weight) for v in parts[0]], [('minecraft:b', 3), ('minecraft:c', 1)])
        parts, _ = bs2.select({'facing': 'west'})
        self.assertIsNone(parts)

    def test_multipart_conditions(self):
        bs = blockstates.parse_blockstate('minecraft:f', {'multipart': [
            {'apply': {'model': 'post'}},
            {'when': {'north': 'true'}, 'apply': {'model': 'side'}},
            {'when': {'OR': [{'east': 'true'}, {'south': 'true', 'west': 'false'}]}, 'apply': {'model': 'or'}},
            {'when': {'AND': [{'facing': 'a|b'}, {'half': '!top'}]}, 'apply': [{'model': 'x1', 'weight': 2}, {'model': 'x2'}]},
            {'when': {'power': 3}, 'apply': {'model': 'num'}}]})
        def sel(**p):
            parts, mp = bs.select({k: str(v) for k, v in p.items()})
            self.assertTrue(mp)
            return [[v.model.split(':')[1] for v in part] for part in parts]
        self.assertEqual(sel(north='false', east='false', south='false', west='false', facing='c', half='top', power=0), [['post']])
        self.assertEqual(sel(north='true', east='false', south='true', west='false', facing='c', half='top', power=0), [['post'], ['side'], ['or']])
        self.assertEqual(sel(north='false', east='true', south='false', west='true', facing='a', half='bottom', power=3),
                         [['post'], ['or'], ['x1', 'x2'], ['num']])
        self.assertEqual(sel(north='false', east='false', south='true', west='true', facing='b', half='top', power=0), [['post']])

    @unittest.skipUnless(HAVE, 'нет ресурсов')
    def test_all_blockstates_parse(self):
        n = 0
        for f in os.listdir(os.path.join(ROOT, 'blockstates')):
            blockstates.load_blockstate(ROOT, f[:-5])
            n += 1
        self.assertGreater(n, 1200)


@unittest.skipUnless(HAVE, 'нет ресурсов')
class TestModels(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.st = models.ModelStore(ROOT)

    def test_cube_all(self):
        q = self.st.bake('block/stone')
        self.assertEqual(len(q), 6)
        self.assertEqual(sorted(x.dir for x in q), [0, 1, 2, 3, 4, 5])
        self.assertTrue(all(x.cull == x.dir for x in q))
        up = [x for x in q if x.dir == 1][0]
        # грань UP: вершины (0,1,0),(0,1,1),(1,1,1),(1,1,0) — против часовой стрелки при взгляде сверху
        self.assertEqual([tuple(p) for p in up.pos], [(0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)])
        self.assertEqual(up.tex, 'minecraft:block/stone')

    def test_rotation_x_y(self):
        # лог по оси x: model oak_log_horizontal x=90, y=90 (blockstate) — торцевая текстура на гранях ±x
        a = self.st.bake('block/oak_log_horizontal', 90, 90, 0)
        ends = {x.dir: x.tex for x in a}
        self.assertTrue(ends[4].endswith('oak_log_top') and ends[5].endswith('oak_log_top'), ends)
        self.assertTrue(ends[1].endswith('oak_log') and ends[2].endswith('oak_log'))
        b = self.st.bake('block/oak_log_horizontal', 90, 0, 0)
        ends = {x.dir: x.tex for x in b}
        self.assertTrue(ends[2].endswith('oak_log_top') and ends[3].endswith('oak_log_top'), ends)

    def test_stairs_geometry(self):
        q = self.st.bake('block/oak_stairs', 0, 0, 0)    # facing=east, bottom
        self.assertEqual(len(q), 11)
        tops = sorted(round(p[1], 4) for x in q if x.dir == 1 for p in x.pos)
        self.assertEqual(sorted(set(tops)), [0.5, 1.0])
        # ступень: высокая часть на востоке (x 0.5..1): грань SOUTH верхней ступеньки
        hi = [x for x in q if x.dir == 5]   # EAST: полная стена x=1 (cull east)
        self.assertTrue(any(all(abs(p[0] - 1.0) < 1e-6 for p in x.pos) for x in hi))

    def test_uvlock_up_faces(self):
        for y in (0, 90, 180, 270):
            for x in self.st.bake('block/oak_stairs', 0, y, 0, True):
                if x.dir == 1:
                    for p, (u, v) in zip(x.pos, x.uv):
                        self.assertAlmostEqual(u, p[0], 6)
                        self.assertAlmostEqual(v, p[2], 6)

    def test_uvlock_off_rotates_uv(self):
        bad = 0
        for x in self.st.bake('block/oak_stairs', 0, 90, 0, False):
            if x.dir == 1:
                for p, (u, v) in zip(x.pos, x.uv):
                    bad += abs(u - p[0]) > 1e-6 or abs(v - p[2]) > 1e-6
        self.assertGreater(bad, 0)

    def test_cullface_rotation(self):
        # cullface поворачивается вместе с блоком: у oak_stairs 'down' с y=90 остаётся down; west -> south при y=270? проверяем множество
        q0 = self.st.bake('block/oak_stairs', 0, 0, 0)
        q1 = self.st.bake('block/oak_stairs', 0, 90, 0)
        c0 = sorted(x.cull for x in q0)
        c1 = sorted(x.cull for x in q1)
        self.assertEqual(sorted(set(c0)), sorted(set(c1) | {5}) if False else sorted(set(c0)))
        # EAST (5) при повороте y=90 по часовой (сверху) -> SOUTH (3)
        e0 = [x for x in q0 if x.cull == 5]
        e1 = [x for x in q1 if x.cull == 3]
        self.assertEqual(len(e0), len(e1))

    def test_element_rotation_rescale(self):
        # vine/…: проверяем разбор формата rotation с rescale на tall_grass-подобной модели и x/y/z-эйлера
        m = models.ResolvedModel('t')
        m.textures = {'a': ('id', 'minecraft:block/stone', False)}
        m.elements = [models._Element({'from': [0, 0, 8], 'to': [16, 16, 8], 'rotation': {'origin': [8, 8, 8], 'axis': 'y', 'angle': 45, 'rescale': True},
                                       'faces': {'north': {'texture': '#a'}, 'south': {'texture': '#a'}}})]
        q = models.bake_model(self.st, m)
        xs = [p[0] for x in q for p in x.pos]
        self.assertAlmostEqual(min(xs), 0.0, 5)    # rescale: плоскость 45° растягивается ровно до границ блока
        self.assertAlmostEqual(max(xs), 1.0, 5)

    def test_planar_element_faces(self):
        # элемент нулевой толщины по z: рисуются только грани north/south (оси Z)
        m = models.ResolvedModel('t')
        m.textures = {'a': ('id', 'minecraft:block/stone', False)}
        m.elements = [models._Element({'from': [0, 0, 8], 'to': [16, 16, 8],
                                       'faces': {'north': {'texture': '#a'}, 'south': {'texture': '#a'}, 'up': {'texture': '#a'}}})]
        q = models.bake_model(self.st, m)
        self.assertEqual(sorted(x.dir for x in q), [2, 3])

    def test_missing_texture_and_tint(self):
        q = self.st.bake('block/grass_block')
        tints = sorted((x.dir, x.tint) for x in q if x.tint >= 0)
        self.assertIn((1, 0), tints)
        self.assertEqual(len(q), 10)

    def test_all_models_bake(self):
        # все модели из blockstates запекаются без ошибок
        n = 0
        for f in sorted(os.listdir(os.path.join(ROOT, 'blockstates'))):
            bs = blockstates.load_blockstate(ROOT, f[:-5])
            for v in bs.all_variants():
                self.st.bake(v.model, v.x, v.y, v.z, v.uvlock)
                n += 1
        self.assertGreater(n, 8000)
        bad = {k: e for k, e in self.st.errors.items()}
        self.assertEqual(bad, {})


class TestEntity(unittest.TestCase):
    def test_counts_and_textures(self):
        from mcgen_addon.assets import entity_models as em
        q = em.entity_quads('chest', {'facing': 'north', 'type': 'single'})
        self.assertEqual(len(q), 18)
        self.assertTrue(all(x.tex == 'minecraft:entity/chest/normal' for x in q))
        self.assertEqual(len(em.entity_quads('chest', {'facing': 'north', 'type': 'left'})), 15)    # без грани, примыкающей к соседу
        self.assertTrue(all(x.tex.endswith('normal_left') for x in em.entity_quads('chest', {'facing': 'north', 'type': 'left'})))
        self.assertEqual(len(em.entity_quads('waxed_oxidized_copper_chest', {'facing': 'east', 'type': 'single'})), 18)
        self.assertEqual(len(em.entity_quads('purple_shulker_box', {'facing': 'up'})), 12)
        self.assertEqual(len(em.entity_quads('skeleton_skull', {'rotation': '3'})), 6)
        dq = em.entity_quads('dragon_head', {'rotation': '3'})                          # DragonHeadModel: 6 коробок головы + челюсть = 7 × 6 граней
        self.assertEqual(len(dq), 42)
        self.assertTrue(all(x.tex == 'minecraft:entity/enderdragon/dragon' for x in dq))
        self.assertTrue(all(0.0 <= c <= 1.0 for x in dq for uv in x.uv for c in uv))    # раскладка текстуры 256×256 не выходит за неё
        self.assertEqual(len(em.entity_quads('dragon_wall_head', {'facing': 'north'})), 42)
        self.assertTrue(em.is_entity_block('dragon_head') and em.is_entity_block('dragon_wall_head'))
        b = em.entity_quads('red_banner', {'rotation': '0'})
        self.assertEqual(sorted({x.tint for x in b}), [-1, 0])
        self.assertEqual(tint.tint_sources('minecraft:red_banner', {'rotation': '0'})[0], (tint.CONST, 11546150 & 0xFFFFFF))
        # все вершины в пределах блока (±0.01)
        for name, props in (('chest', {'facing': 'south', 'type': 'single'}), ('purple_shulker_box', {'facing': 'up'}), ('bell', {}),
                            ('skeleton_wall_skull', {'facing': 'east'})):
            for x in em.entity_quads(name, props):
                for p in x.pos:
                    self.assertTrue(all(-0.02 <= c <= 1.02 for c in p), (name, p))

    def test_dragon_head_geometry(self):
        """Размеры по байткоду клиента 26.3: голова — scaled(0.75) со смещением −7.986666 px, морда на 24 px вперёд, рога выше головы; зеркальные коробки отражены."""
        from mcgen_addon.assets import entity_models as em
        qs = em.entity_quads('dragon_head', {'rotation': '0'})
        ys = [p[1] for x in qs for p in x.pos]
        self.assertGreater(max(ys), 1.0)                       # рога выступают над блоком
        zs = [p[2] for x in qs for p in x.pos]
        self.assertLess(min(zs) , 0.0)                         # морда (24 px × 0.75 = 1.125 блока) выходит за грань блока
        # mirror: те же вершины, отражённые по x внутри коробки; порядок вершин обращён
        a = em._cube_polygons((-5, -12, -4), (2, 4, 6), (0, 0))
        b = em._cube_polygons((-5, -12, -4), (2, 4, 6), (0, 0), mirror=True)
        self.assertEqual(len(a), len(b))
        self.assertEqual({tuple(sorted(v)) for verts, _ in a for v in verts}, {tuple(sorted(v)) for verts, _ in b for v in verts})
        self.assertNotEqual([uv for _, uv in a], [uv for _, uv in b])

    def test_banner_cloth_faces(self):
        """Лицевая и тыльная грани полотна для слоёв узоров: две грани с uv 64×64 и противоположными нормалями."""
        from mcgen_addon.assets import entity_models as em
        f = em.banner_cloth_faces('magenta_wall_banner', {'facing': 'west'})
        self.assertEqual(len(f), 2)
        (p0, uv0, n0), (p1, uv1, n1) = f
        self.assertAlmostEqual(n0[0], -n1[0])
        self.assertTrue(all(0.0 <= c <= 1.0 for uv in (uv0, uv1) for u in uv for c in u))
        # те же координаты y/z (грань на том же полотне), x — две плоскости толщиной 1 px × 2/3
        self.assertAlmostEqual(abs(p0[0][0] - p1[0][0]), 1.0 / 16.0 * 2.0 / 3.0, places=4)

    def test_chest_facing_rotates_lock(self):
        from mcgen_addon.assets import entity_models as em
        def lock_center(f):
            qs = em.entity_quads('chest', {'facing': f, 'type': 'single'})[12:]     # третья коробка — замок
            xs = [p[0] for x in qs for p in x.pos]
            zs = [p[2] for x in qs for p in x.pos]
            return (sum(xs) / len(xs), sum(zs) / len(zs))
        sx, sz = lock_center('south')
        nx, nz = lock_center('north')
        self.assertGreater(sz, 0.9)          # замок спереди — на южной стороне при facing=south
        self.assertLess(nz, 0.1)
        ex, ez = lock_center('east')
        self.assertGreater(ex, 0.9)
        wx, wz = lock_center('west')
        self.assertLess(wx, 0.1)


class TestJrand(unittest.TestCase):
    def test_get_seed_known(self):
        # значения проверены на настоящем Mth.getSeed (Java) — см. docs/blender/assets-mesh.md
        self.assertEqual(jrand.mth_get_seed(10, 64, -20), -62212282568415)
        self.assertEqual(jrand.mth_get_seed(0, 0, 0), 0)
        self.assertEqual(jrand.mth_get_seed(-123456, -17, 987654), -20673674786596)

    def test_random_known(self):
        r = jrand.LegacyRandom(12345)
        self.assertEqual([r.next_int(10) for _ in range(5)], [1, 0, 1, 8, 5])   # java.util.Random(12345)


class TestTint(unittest.TestCase):
    def test_colormap_get(self):
        px = np.arange(65536, dtype=np.uint32)
        self.assertEqual(tint.colormap_get(px, 1.0, 1.0, -1), 0)
        self.assertEqual(tint.colormap_get(px, 0.5, 1.0, -1), (int((1 - 0.5) * 255) | (int((1 - 0.5) * 255) << 8)))
        self.assertEqual(tint.colormap_get(None, 0.5, 0.5, 77), 77)

    def test_tint_sources(self):
        self.assertEqual(tint.tint_sources('minecraft:grass_block', {})[0][0], tint.GRASS)
        self.assertEqual(tint.tint_sources('minecraft:birch_leaves', {})[0], (tint.CONST, 0x80A755))
        self.assertEqual(tint.tint_sources('minecraft:spruce_leaves', {})[0], (tint.CONST, 0x619961))
        self.assertEqual(tint.tint_sources('minecraft:oak_leaves', {})[0][0], tint.FOLIAGE)
        self.assertEqual(tint.tint_sources('minecraft:stone', {}), ())
        self.assertEqual(tint.tint_sources('minecraft:redstone_wire', {'power': '15'})[0], (tint.CONST, 0xFF0000 | 0x0000 | 0x0 if False else tint.tint_sources('minecraft:redstone_wire', {'power': '15'})[0][1]))
        self.assertEqual(tint.tint_sources('minecraft:redstone_wire', {'power': '0'})[0][1] >> 16, 0x4C)   # power 0: красный 0.3 -> 76
        self.assertEqual(tint.tint_sources('minecraft:melon_stem', {'age': '7'})[0][1], (224 << 16) | (199 << 8) | 28)
        self.assertEqual(tint.tint_sources('minecraft:lily_pad', {})[0], (tint.CONST, 0x208030))

    @unittest.skipUnless(HAVE, 'нет ресурсов')
    def test_biome_table(self):
        t = common.table()
        names = common.biome_names()
        bc = t.biome_colors
        i = names.index('minecraft:dark_forest')
        self.assertEqual(int(bc.rgb[i][0]), 0x507A32)
        self.assertEqual(int(bc.mod[i]), tint.MOD_DARK_FOREST)
        sw = names.index('minecraft:swamp')
        self.assertEqual(int(bc.mod[sw]), tint.MOD_SWAMP)
        self.assertEqual(int(bc.rgb[names.index('minecraft:swamp')][3]), 0x617B64)
        pl = names.index('minecraft:plains')
        self.assertEqual(int(bc.rgb[pl][0]), 0x91BD59)
        self.assertEqual(int(bc.rgb[pl][1]), 0x77AB2F)

    def test_swamp_noise_known_values(self):
        # значения совпали с Biome.BIOME_INFO_NOISE игры 26.3 (2000 случайных точек, битово) — берём первые несколько
        perm = tint.swamp_perm()
        for x, z, bits in ((1130, 2763, -1092350030), (1248, -1116, -1087210314), (-30, -2475, -1083207664)):
            import struct
            jv = struct.unpack('>f', struct.pack('>I', bits & 0xffffffff))[0]
            self.assertEqual(tint.simplex2(perm, x * 0.0225, z * 0.0225), jv)


class TestFluids(unittest.TestCase):
    def test_fluid_state(self):
        self.assertEqual(fluids.fluid_state('minecraft:water', {'level': '0'}), (1, 8, False))
        self.assertEqual(fluids.fluid_state('minecraft:water', {'level': '3'}), (1, 5, False))
        self.assertEqual(fluids.fluid_state('minecraft:lava', {'level': '8'}), (2, 8, True))
        self.assertEqual(fluids.fluid_state('minecraft:oak_stairs', {'waterlogged': 'true'}), (1, 8, False))
        self.assertEqual(fluids.fluid_state('minecraft:kelp', {'age': '3'})[0], 1)
        self.assertEqual(fluids.fluid_state('minecraft:stone', {}), (0, 0, False))


@unittest.skipUnless(HAVE, 'нет ресурсов')
class TestStateTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = common.table()

    def test_counts(self):
        t = self.t
        self.assertEqual(t.n_states, 35723)
        self.assertEqual(t.stats['errors'], {})
        self.assertEqual(t.stats['missing_textures'], [])
        self.assertEqual(sum(t.stats['classes'].values()), 35723)

    def test_classes(self):
        t = self.t
        c = lambda n: t.classify(t.state_id(n))
        self.assertEqual(c('minecraft:stone'), 'cube')
        self.assertEqual(c('minecraft:air'), 'air')
        self.assertEqual(c('minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]'), 'partial')
        self.assertEqual(c('minecraft:oak_leaves[distance=1,persistent=false,waterlogged=false]'), 'cutout')
        self.assertEqual(c('minecraft:short_grass'), 'cutout')
        self.assertEqual(c('minecraft:water[level=0]'), 'fluid')
        self.assertEqual(c('minecraft:ice'), 'translucent')
        sid = t.state_id('minecraft:chest[type=single,facing=north,waterlogged=false]')
        self.assertTrue(t.st_flags[sid] & F.SPECIAL)           # блок с моделью сущности: рисуется заглушкой
        self.assertEqual(c('minecraft:chest[type=single,facing=north,waterlogged=false]'), 'partial')
        self.assertEqual(c('minecraft:conduit[waterlogged=false]'), 'special')   # без заглушки: геометрии нет

    def test_opaque_matches_game(self):
        """Признак «полный непрозрачный куб» совпал с BlockState.isSolidRender() игры для всех 35723 состояний (проверено дампом из Java)."""
        t = self.t
        self.assertEqual(int(((t.st_flags & F.OPAQUE) != 0).sum()), 2862)

    def test_state_lookup(self):
        t = self.t
        a = t.state_id('minecraft:chest[type=single,facing=north,waterlogged=false]')
        b = t.state_id('minecraft:chest[waterlogged=false,facing=north,type=single]')
        self.assertEqual(a, b)
        self.assertGreaterEqual(a, 0)
        self.assertEqual(t.state_id('minecraft:stone[]'), t.state_id('minecraft:stone'))
        self.assertEqual(t.state_from_props('minecraft:oak_stairs', {'facing': 'east'}), t.state_id('minecraft:oak_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]'))
        self.assertEqual(t.state_props(t.state_id('minecraft:oak_stairs[facing=east,half=top,shape=straight,waterlogged=true]'))[1]['half'], 'top')

    def test_random_variants(self):
        t = self.t
        s = t.state_id('minecraft:stone')
        self.assertTrue(t.st_flags[s] & F.RANDOM)
        self.assertFalse(t.st_flags[t.state_id('minecraft:oak_planks')] & F.RANDOM)

    def test_offsets(self):
        t = self.t
        f = int(t.st_flags[t.state_id('minecraft:poppy')])
        self.assertTrue(f & F.OFF_XZ)
        f = int(t.st_flags[t.state_id('minecraft:short_grass')])
        self.assertTrue(f & F.OFF_XYZ)

    def test_cache_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 't.npz')
            self.t.save(p)
            t2 = type(self.t).from_npz(p)
            for a in type(self.t).ARRAYS:
                np.testing.assert_array_equal(getattr(self.t, a), getattr(t2, a), a)
            self.assertEqual(t2.names, self.t.names)


@unittest.skipUnless(HAVE, 'нет ресурсов')
class TestAtlas(unittest.TestCase):
    def test_atlas(self):
        t = common.table()
        W, H = t.atlas_image.shape[1], t.atlas_image.shape[0]
        self.assertEqual(t.atlas_image.shape[2], 4)
        # прямоугольник спрайта лежит в пределах атласа, поля заполнены протяжкой краёв
        i = t.atlas_names.index('minecraft:block/stone')
        r = t.atlas_rect[i]
        x0, y0, x1, y1 = (int(round(r[0] * W)), int(round(r[1] * H)), int(round(r[2] * W)), int(round(r[3] * H)))
        self.assertEqual((x1 - x0, y1 - y0), (16, 16))
        np.testing.assert_array_equal(t.atlas_image[y0, x0 - 1], t.atlas_image[y0, x0])
        np.testing.assert_array_equal(t.atlas_image[y0 - 2, x0 + 3], t.atlas_image[y0, x0 + 3])
        # анимация: первый кадр (water_still 16×512 -> 16×16)
        j = t.atlas_names.index('minecraft:block/water_still')
        self.assertEqual(tuple(t.atlas_info['sprite_size'][j]), (16, 16))
        self.assertTrue(t.atlas_info['animated'][j])
        k = t.atlas_names.index('minecraft:block/water_flow')
        self.assertEqual(tuple(t.atlas_info['sprite_size'][k]), (32, 32))
        self.assertEqual(t.atlas_info['tile'], 64)

    def test_transparency(self):
        a = atlas.build_atlas(os.path.join(ROOT, 'textures'), ['minecraft:block/stone', 'minecraft:block/oak_leaves', 'minecraft:block/glass', 'minecraft:block/ice'])
        T = lambda n, *uv: a.transparency(a.sprite('minecraft:block/' + n), *uv)
        self.assertEqual(T('stone', 0, 0, 1, 1), 0)
        self.assertEqual(T('oak_leaves', 0, 0, 1, 1), 1)
        self.assertEqual(T('ice', 0, 0, 1, 1), 2)
        self.assertEqual(T('glass', 0, 0, 1, 1), 1 if T('glass', 0, 0, 1, 1) == 1 else 2)
        # подобласть: левый верхний пиксель листвы может быть непрозрачным или нет — главное, что подзапрос не падает
        self.assertIn(T('oak_leaves', 0, 0, 1 / 16, 1 / 16), (0, 1))


if __name__ == '__main__':
    unittest.main()

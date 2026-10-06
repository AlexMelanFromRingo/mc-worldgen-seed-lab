"""Тесты assets/pbr.py (PBR-карты из атласа): классы по именам, свойства карт, инварианты на настоящем атласе. Запуск: python3 blender/tests/test_pbr.py"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402
import numpy as np  # noqa: E402

from mcgen_addon.assets import pbr  # noqa: E402


def sprite(color, size=16, noise=0):
    rng = np.random.default_rng(1)
    a = np.zeros((size, size, 4), np.uint8)
    a[:, :, :3] = np.array(color, np.uint8)
    a[:, :, 3] = 255
    if noise:
        a[:, :, :3] = np.clip(a[:, :, :3].astype(int) + rng.integers(-noise, noise + 1, (size, size, 1)), 0, 255).astype(np.uint8)
    return a


class TestClassify(unittest.TestCase):
    def test_names(self):
        c = pbr.classify
        self.assertEqual(c('minecraft:block/iron_block')['metal'], 1.0)
        self.assertEqual(c('minecraft:block/copper_block')['metal'], 1.0)
        self.assertLess(c('minecraft:block/oxidized_copper')['metal'], c('minecraft:block/weathered_copper')['metal'])
        self.assertLess(c('minecraft:block/weathered_copper')['metal'], c('minecraft:block/exposed_copper')['metal'])
        self.assertLess(c('block/glass')['rough'], 0.1)
        self.assertEqual(c('block/glass')['nrm'], 0.0)
        self.assertEqual(c('block/glowstone')['emit'], 'full')
        self.assertEqual(c('block/torch')['emit'], 'lum')
        self.assertEqual(c('block/magma')['emit'], 'lum')
        self.assertEqual(c('block/crying_obsidian')['emit'], 'sat')
        self.assertEqual(c('block/sea_lantern')['emit'], 'full')

    def test_ores_are_not_metal_blocks(self):
        for n in ('copper_ore', 'deepslate_copper_ore', 'iron_ore', 'gold_ore', 'diamond_ore', 'nether_quartz_ore'):
            p = pbr.classify('block/' + n)
            self.assertTrue(p['ore'], n)
            self.assertEqual(p['metal'], 0.0, n)

    def test_wood_stone_defaults(self):
        self.assertGreater(pbr.classify('block/oak_planks')['rough'], 0.5)
        self.assertGreater(pbr.classify('block/stone')['nrm'], pbr.classify('block/white_wool')['nrm'])
        self.assertEqual(pbr.classify('block/definitely_unknown_block')['rule'], -1)
        self.assertEqual(pbr.classify('minecraft:entity/chest/normal')['rule'], -1)           # сущности — по умолчанию (кроме медных)


class TestMaps(unittest.TestCase):
    def test_flat_sprite_has_flat_normals(self):
        n, o = pbr._sprite_maps(sprite((120, 120, 120)), pbr.classify('block/stone'))
        self.assertTrue((n == np.array([128, 128, 255], np.uint8)).all() or (np.abs(n.astype(int) - np.array([128, 128, 255])) <= 1).all())

    def test_bump_normals_tilt_outward(self):
        a = sprite((60, 60, 60))
        a[4:12, 4:12, :3] = 230
        p = dict(pbr.classify('block/stone'))
        p['nrm'] = 1.5
        n, _ = pbr._sprite_maps(a, p)
        self.assertLess(int(n[8, 3, 0]), 100)          # левый склон: нормаль влево (R < 128)
        self.assertGreater(int(n[8, 12, 0]), 156)      # правый: вправо
        self.assertGreater(int(n[3, 8, 1]), 156)       # верхний край (строка 3 — выше выпуклости): нормаль вверх по v (G > 128)
        self.assertLess(int(n[12, 8, 1]), 100)         # нижний
        self.assertTrue((n[:, :, 2] >= 128).all())     # нормали смотрят наружу

    def test_metal_and_emission(self):
        _, o = pbr._sprite_maps(sprite((200, 200, 200), noise=10), pbr.classify('block/iron_block'))
        self.assertGreater(float(o[:, :, 1].mean()), 250)                  # металличность
        self.assertLess(float(o[:, :, 0].mean()), 120)                     # гладкий
        _, o = pbr._sprite_maps(sprite((250, 220, 120)), pbr.classify('block/glowstone'))
        self.assertTrue((o[:, :, 2] == 255).all())                         # светится целиком
        _, o = pbr._sprite_maps(sprite((90, 90, 90)), pbr.classify('block/stone'))
        self.assertTrue((o[:, :, 2] == 0).all())

    def test_ore_gem_mask(self):
        a = sprite((125, 125, 125), noise=6)
        a[3:5, 3:5, :3] = (60, 230, 240)                                  # «алмаз»
        _, o = pbr._sprite_maps(a, pbr.classify('block/diamond_ore'))
        self.assertLess(int(o[3, 3, 0]), 100)                              # вкрапление блестит (шероховатость ниже)
        self.assertGreater(int(o[10, 10, 0]), 200)                         # камень вокруг шероховатый
        _, o = pbr._sprite_maps(a, pbr.classify('block/iron_ore'))
        self.assertGreater(int(o[3, 3, 1]), -1)

    def test_transparent_pixels_neutral(self):
        a = sprite((90, 90, 90))
        a[:, :8, 3] = 0
        n, o = pbr._sprite_maps(a, pbr.classify('block/oak_leaves'))
        self.assertTrue((o[:, :8, 0] == 255).all() and (o[:, :8, 1:] == 0).all())


@unittest.skipUnless(_boot.have_resources(), 'нет ресурсов клиента')
class TestAtlas(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import common
        cls.t = common.table()
        cls.n, cls.o, cls.stats = pbr.build_pbr(cls.t.atlas_image, cls.t.atlas_rect, cls.t.atlas_names, pad=int(cls.t.atlas_info.get('pad', 2)))

    def test_shapes_and_range(self):
        H, W = self.t.atlas_image.shape[:2]
        self.assertEqual(self.n.shape, (H, W, 3))
        self.assertEqual(self.o.shape, (H, W, 3))
        self.assertEqual(self.n.dtype, np.uint8)
        self.assertTrue((self.n[:, :, 2] >= 127).all())                    # z нормали ≥ 0 везде

    def test_metal_blocks_are_metallic(self):
        names = [x.replace('minecraft:block/', '') for x in self.t.atlas_names]
        H, W = self.t.atlas_image.shape[:2]
        for nm, want in (('iron_block', 250), ('gold_block', 250), ('glowstone', 0)):
            i = names.index(nm)
            u0, v0, u1, v1 = self.t.atlas_rect[i]
            sl = self.o[int(round(v0 * H)):int(round(v1 * H)), int(round(u0 * W)):int(round(u1 * W)), :]
            if want:
                self.assertGreater(float(sl[:, :, 1].mean()), want, nm)
            else:
                self.assertLess(float(sl[:, :, 1].mean()), 5, nm)
        i = names.index('glowstone')
        u0, v0, u1, v1 = self.t.atlas_rect[i]
        self.assertEqual(int(self.o[int(round(v0 * H)):int(round(v1 * H)), int(round(u0 * W)):int(round(u1 * W)), 2].min()), 255)

    def test_most_sprites_matched_by_rules(self):
        names = [x for x in self.t.atlas_names if not x.startswith('minecraft:entity/')]
        default = sum(1 for x in names if pbr.classify(x)['rule'] == -1)
        self.assertLess(default / len(names), 0.12, 'слишком много спрайтов в классе по умолчанию: %d из %d' % (default, len(names)))

    def test_speed(self):
        import time
        t0 = time.time()
        pbr.build_pbr(self.t.atlas_image, self.t.atlas_rect, self.t.atlas_names, pad=2)
        self.assertLess(time.time() - t0, 5.0)


if __name__ == '__main__':
    unittest.main()

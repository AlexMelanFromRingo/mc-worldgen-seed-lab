import os
import time
import unittest

import numpy as np

import _boot
import _cmp
import common
from mcgen_addon.assets import state_table
from mcgen_addon.assets.state_table import F
from mcgen_addon.mesh import mesher, reference


class TestMesher(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not _boot.have_resources():
            raise unittest.SkipTest('нет ресурсов клиента')
        cls.t = common.table()
        cls.m = mesher.Mesher(cls.t)

    def _cmp_random(self, seed, opt, **kw):
        rng = np.random.default_rng(seed)
        nb, nbio = common.random_world(self.t, rng, **kw)
        c = self.m.mesh_arrays(3, -2, nb, nbio, -64, 32, opt)
        r = reference.reference_mesh_chunk(self.t, self.t.biome_colors, opt, 3, -2, -64, 32, nb, nbio)
        self.assertTrue(_cmp.compare(c, r), 'seed %d: ядро и эталон расходятся' % seed)
        return c

    def test_random_chunks_default(self):
        tot = 0
        for seed in range(6):
            tot += self._cmp_random(seed, self.m.opt).n_quads
        self.assertGreater(tot, 1000)

    def test_random_chunks_options(self):
        opts = [self.m.opt.copy(cutout_leaves=False), self.m.opt.copy(bake_shade=True), self.m.opt.copy(blender_axes=False, blender_uv=False),
                self.m.opt.copy(blend_radius=0), self.m.opt.copy(blend_radius=4, scale=0.5, y_offset=16)]
        for i, o in enumerate(opts):
            self._cmp_random(100 + i, o)

    def test_random_sparse(self):
        # разреженные миры: больше открытых граней/жидкостей/границ
        for seed in range(3):
            self._cmp_random(200 + seed, self.m.opt, p_air=0.85, p_cube=0.05)

    def test_missing_neighbors(self):
        rng = np.random.default_rng(7)
        nb, nbio = common.random_world(self.t, rng)
        for i in (0, 1, 2, 3, 5, 6, 7, 8):
            nb[i] = None
            nbio[i] = None
        c = self.m.mesh_arrays(0, 0, nb, nbio, -64, 32)
        r = reference.reference_mesh_chunk(self.t, self.t.biome_colors, self.m.opt, 0, 0, -64, 32, nb, nbio)
        self.assertTrue(_cmp.compare(c, r))
        nbio[4] = None
        c = self.m.mesh_arrays(0, 0, nb, nbio, -64, 32)
        r = reference.reference_mesh_chunk(self.t, self.t.biome_colors, self.m.opt, 0, 0, -64, 32, nb, nbio)
        self.assertTrue(_cmp.compare(c, r))

    def test_full_cube_hidden(self):
        """Куб из камня в окружении камня не даёт ни одной грани; одиночный блок камня — 6 граней."""
        t = self.t
        stone = t.state_id('minecraft:stone')
        air = t.state_id('minecraft:air')
        a = np.full(32 * 256, air, dtype=np.uint16)
        a.reshape(32, 16, 16)[5, 8, 8] = stone
        d = self.m.mesh_arrays(0, 0, [None] * 4 + [a] + [None] * 4, [None] * 9, -64, 32)
        self.assertEqual(d.n_quads, 6)
        self.assertEqual(sorted(d.dir.tolist()), [0, 1, 2, 3, 4, 5])
        b = np.full(32 * 256, stone, dtype=np.uint16)
        d = self.m.mesh_arrays(0, 0, [b] * 9, [None] * 9, -64, 32)
        # боковые грани скрыты соседями; остаются только верх и низ мира (за границей высоты — «воздух»)
        self.assertEqual(d.n_quads, 2 * 256)
        self.assertEqual(sorted(set(d.dir.tolist())), [0, 1])

    def test_stairs_slab_cull(self):
        t = self.t
        air = t.state_id('minecraft:air')
        stone = t.state_id('minecraft:stone')
        slab = t.state_id('minecraft:stone_slab[type=bottom,waterlogged=false]')
        a = np.full(32 * 256, air, dtype=np.uint16)
        v = a.reshape(32, 16, 16)
        v[3, 8, 8] = stone
        v[4, 8, 8] = slab      # нижняя плита над камнем: низ плиты и верх камня скрыты
        d = self.m.mesh_arrays(0, 0, [None] * 4 + [a] + [None] * 4, [None] * 9, -64, 32)
        r = reference.reference_mesh_chunk(t, t.biome_colors, self.m.opt, 0, 0, -64, 32, [None] * 4 + [a] + [None] * 4, [None] * 9)
        self.assertTrue(_cmp.compare(d, r))
        # у камня 5 граней (верх скрыт плитой), у плиты 5 (низ скрыт)
        by_block = {}
        for i in range(d.n_quads):
            by_block.setdefault(int(d.block[i]), []).append(int(d.dir[i]))
        self.assertEqual(len(by_block[(3 * 16 + 8) * 16 + 8]), 5)
        self.assertEqual(len(by_block[(4 * 16 + 8) * 16 + 8]), 5)

    def test_variant_selection_matches_java(self):
        """Выбор варианта по позиции: stone имеет 4 варианта (y-повороты); проверяем, что ядро выбирает тот же, что и эталон на Python."""
        t = self.t
        stone = t.state_id('minecraft:stone')
        air = t.state_id('minecraft:air')
        a = np.full(32 * 256, air, dtype=np.uint16)
        a.reshape(32, 16, 16)[2, 1:15, 1:15] = stone
        d = self.m.mesh_arrays(7, -5, [None] * 4 + [a] + [None] * 4, [None] * 9, -64, 32)
        r = reference.reference_mesh_chunk(t, t.biome_colors, self.m.opt, 7, -5, -64, 32, [None] * 4 + [a] + [None] * 4, [None] * 9)
        self.assertTrue(_cmp.compare(d, r))
        # разные варианты действительно встречаются
        ups = d.uv[d.dir == 1][:, 0, :].round(4)
        self.assertGreater(len({tuple(u) for u in ups}), 1)

    def _merge_vs_unmerged(self, nb, nbio, opt=None):
        opt = opt or self.m.opt
        base = self.m.mesh_arrays(1, 1, nb, nbio, -64, 32, opt.copy(no_variants=True))
        mg = self.m.mesh_arrays(1, 1, nb, nbio, -64, 32, opt.copy(merge=True))
        ex = _cmp.expand_merged(mg)
        self.assertEqual(_cmp.key_set_nob(base), _cmp.key_set_nob(ex))
        return base, mg

    def test_merge_equivalence_random(self):
        for seed in range(4):
            rng = np.random.default_rng(300 + seed)
            nb, nbio = common.random_world(self.t, rng, p_air=0.35, p_cube=0.5)
            self._merge_vs_unmerged(nb, nbio)

    def test_merge_structured(self):
        """Плоские поля одного блока с дырами и перепадами: слияние сильно уменьшает число граней, но набор единичных граней тот же."""
        t = self.t
        air = t.state_id('minecraft:air')
        grass = t.state_id('minecraft:grass_block[snowy=false]')
        stone = t.state_id('minecraft:stone')
        water = t.state_id('minecraft:water[level=0]')
        rng = np.random.default_rng(11)
        nb = []
        for k in range(9):
            a = np.full((32, 16, 16), air, dtype=np.uint16)
            a[:10] = stone
            a[10] = grass
            holes = rng.random((16, 16)) < 0.08
            a[10][holes] = air
            a[9][holes] = water
            a[11, 3:9, 4:12] = stone
            nb.append(a.reshape(-1))
        nbio = [np.zeros(8 * 16, np.uint8)] * 9
        base, mg = self._merge_vs_unmerged(nb, nbio)
        self.assertLess(mg.n_quads, base.n_quads * 0.6)
        self.assertGreater(int(mg.merged.sum()), 0)
        # сплошная вода: поверхность одного чанка сливается в один квад
        a = np.full((32, 16, 16), air, dtype=np.uint16)
        a[:10] = stone
        a[10] = water
        nbw = [a.reshape(-1)] * 9
        base, mg = self._merge_vs_unmerged(nbw, nbio)
        self.assertLessEqual(mg.n_quads, 8)

    def test_performance_smoke(self):
        rng = np.random.default_rng(5)
        nb, nbio = common.random_world(self.t, rng, height=384, p_air=0.6, p_cube=0.3)
        t0 = time.time()
        d = self.m.mesh_arrays(0, 0, nb, nbio, -64, 384)
        dt = time.time() - t0
        self.assertGreater(d.n_quads, 0)
        self.assertLess(dt, 2.0)


if __name__ == '__main__':
    unittest.main()

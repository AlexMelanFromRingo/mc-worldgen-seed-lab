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

    def test_y_range_and_no_water(self):
        # диапазон высот и скрытая вода: ядро == эталон; блоки вне диапазона не видны и не закрывают соседей
        for seed, opt in ((400, self.m.opt.copy(y_min=-50, y_max=-40)), (401, self.m.opt.copy(y_max=-45)), (402, self.m.opt.copy(y_min=-52)),
                          (403, self.m.opt.copy(no_water=True)), (404, self.m.opt.copy(no_water=True, y_min=-60, y_max=-36, merge=True))):
            c = self._cmp_random(seed, opt, p_air=0.5, p_cube=0.2) if not opt.merge else None
            if opt.merge:
                rng = np.random.default_rng(seed)
                nb, nbio = common.random_world(self.t, rng, p_air=0.5, p_cube=0.2)
                d = self.m.mesh_arrays(3, -2, nb, nbio, -64, 32, opt)
                base = self.m.mesh_arrays(3, -2, nb, nbio, -64, 32, opt.copy(merge=False, no_variants=True))
                self.assertEqual(_cmp.key_set_nob(_cmp.expand_merged(d)), _cmp.key_set_nob(base))
                c = d
            if opt.y_min is not None or opt.y_max is not None:
                # все блоки граней в пределах диапазона
                lo = 0 if opt.y_min is None else opt.y_min + 64
                hi = 31 if opt.y_max is None else opt.y_max + 64
                ys = c.block[c.merged == 0] >> 8
                self.assertTrue(len(ys) == 0 or (ys.min() >= lo and ys.max() <= hi), (ys.min(), ys.max(), lo, hi))
            if opt.no_water:
                water = self.t.state_id('minecraft:water[level=0]')
                # у воды-блоков нет граней: материал WATER (3) отсутствует
                self.assertEqual(int((c.mat == 3).sum()), 0)

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


class TestWeld(unittest.TestCase):
    """mcmesh_weld: сварка вершин и рёбер четырёхугольников (общие углы — одна вершина, одинаковые рёбра — одно ребро)."""

    @classmethod
    def setUpClass(cls):
        try:
            cls.lib = mesher.load_library(build=False)
        except Exception as e:  # noqa: BLE001
            raise unittest.SkipTest('нет библиотеки ядра: %s' % e)
        if not hasattr(cls.lib, 'mcmesh_weld'):
            raise unittest.SkipTest('библиотека без mcmesh_weld')

    @staticmethod
    def _grid_quads(rng, n):
        """n случайных граней сетки 1/16 (углы часто совпадают), порядок углов как у ядра."""
        q = np.zeros((n, 4, 3), np.float32)
        for i in range(n):
            x, y, z = (int(v) for v in rng.integers(0, 6, 3))
            ax = int(rng.integers(0, 3))
            u, v = [a for a in range(3) if a != ax]
            for k, (du, dv) in enumerate(((0, 0), (0, 1), (1, 1), (1, 0))):
                p = [x, y, z]
                p[u] += du
                p[v] += dv
                q[i, k] = np.array(p, np.float32) / np.float32(16.0) * np.float32(16.0)
        return q

    def _check(self, quads):
        n = quads.shape[0]
        w = mesher.weld_quads(quads, self.lib)
        self.assertIsNotNone(w)
        cv, vp, ev, ce, nv, ne = w
        self.assertEqual((cv.shape[0], ce.shape[0], vp.shape[0], ev.shape[0]), (4 * n, 4 * n, 3 * nv, 2 * ne))
        pos = vp.reshape(-1, 3)[cv].reshape(n, 4, 3)
        self.assertTrue(np.array_equal(pos + np.float32(0.0), quads + np.float32(0.0)), 'позиции углов не совпали с исходными')   # +0.0: -0.0 == 0.0
        evv = ev.reshape(-1, 2)
        cvq = cv.reshape(n, 4)
        for k in range(4):
            a, b = cvq[:, k], cvq[:, (k + 1) & 3]
            e = evv[ce.reshape(n, 4)[:, k]]
            self.assertTrue(((e[:, 0] == a) & (e[:, 1] == b) | (e[:, 0] == b) & (e[:, 1] == a)).all(), 'corner_edge указывает не на своё ребро')
        self.assertTrue((evv[:, 0] != evv[:, 1]).all(), 'ребро из одной вершины')
        self.assertEqual(len({tuple(sorted(r)) for r in evv.tolist()}), ne, 'повторяющиеся рёбра')
        return nv, ne, cvq

    def test_shared_grid(self):
        rng = np.random.default_rng(1)
        q = self._grid_quads(rng, 3000)
        nv, ne, _ = self._check(q)
        uniq = {tuple(p) for p in q.reshape(-1, 3).tolist()}
        self.assertEqual(nv, len(uniq))                    # без вырожденных граней число вершин = число различных позиций
        self.assertLess(nv, 4 * 3000 // 2)

    def test_negative_zero_and_degenerate(self):
        q = np.array([[[0, 0, 0], [0, 1, 0], [1, 1, 0], [1, 0, 0]],
                      [[-0.0, 0, 0], [-0.0, 1, 0], [1, 1, -0.0], [1, 0, 0]],            # те же углы с -0.0
                      [[2, 0, 0], [2, 0, 0], [3, 1, 0], [3, 0, 0]],                      # вырожденная: совпали углы 0 и 1
                      [[5, 5, 5], [5, 5, 5], [5, 5, 5], [5, 5, 5]]], np.float32)         # все четыре угла в одной точке
        nv, ne, cvq = self._check(q)
        self.assertTrue((cvq[0] == cvq[1]).all())          # -0.0 сварен с +0.0
        for r in cvq[2:]:
            self.assertEqual(len(set(r.tolist())), 4, 'у вырожденной грани должны быть 4 разные вершины')

    def test_empty(self):
        w = mesher.weld_quads(np.zeros((0, 4, 3), np.float32), self.lib)
        self.assertEqual((w[4], w[5]), (0, 0))


if __name__ == '__main__':
    unittest.main()

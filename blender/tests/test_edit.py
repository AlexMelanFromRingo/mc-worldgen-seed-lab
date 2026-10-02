import json
import unittest

import numpy as np

import _boot
import common
from mcgen_addon.mesh.edit import EditSession, PlaceContext, DIR_NAME

HAVE = _boot.have_resources()


@unittest.skipUnless(HAVE, 'нет ресурсов клиента')
class TestEdit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = common.table()

    def make(self, sy=64):
        t = self.t
        air = t.state_id('minecraft:air')
        stone = t.state_id('minecraft:stone')
        blocks = {}
        for cz in (-1, 0, 1):
            for cx in (-1, 0, 1):
                a = np.full(384 * 256, air, np.uint16)
                a.reshape(384, 16, 16)[:sy] = stone      # y < 0 — камень
                blocks[(cx, cz)] = a
        return EditSession(self.t, blocks, -64, 384), blocks

    def name(self, ed, x, y, z):
        return self.t.names[ed.get(x, y, z)]

    def test_fence_connects_and_undo(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:oak_fence')
        ed.place(1, 0, 0, 'minecraft:oak_fence')
        self.assertIn('east=true', self.name(ed, 0, 0, 0))
        self.assertIn('west=true', self.name(ed, 1, 0, 0))
        # на камне рядом (полная грань) — забор тоже соединяется
        ed.set_state(0, 0, 1, self.t.state_id('minecraft:stone'))
        self.assertIn('south=true', self.name(ed, 0, 0, 0))
        ed.break_block(1, 0, 0)
        self.assertIn('east=false', self.name(ed, 0, 0, 0))
        # undo: ломаем -> возвращаем забор -> связи восстановлены
        ed.undo()
        self.assertIn('east=true', self.name(ed, 0, 0, 0))
        self.assertIn('fence', self.name(ed, 1, 0, 0))
        ed.undo(); ed.undo()
        self.assertEqual(self.name(ed, 0, 0, 1), 'minecraft:air')
        self.assertEqual(self.name(ed, 1, 0, 0), 'minecraft:air')
        ed.redo(); ed.redo()
        self.assertEqual(self.name(ed, 0, 0, 1), 'minecraft:stone')
        self.assertIn('fence', self.name(ed, 1, 0, 0))

    def test_fence_not_connect_to_other_wood_class(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:oak_fence')
        ed.place(1, 0, 0, 'minecraft:nether_brick_fence')
        self.assertIn('east=false', self.name(ed, 0, 0, 0))
        self.assertIn('west=false', self.name(ed, 1, 0, 0))

    def test_fence_not_connect_leaves_but_connects_gate(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:oak_fence')
        ed.place(1, 0, 0, 'minecraft:oak_leaves')
        self.assertIn('east=false', self.name(ed, 0, 0, 0))
        # ворота, смотрящие вдоль x, соединяются с забором по оси z... проверяем правило осей
        ed.place(-1, 0, 0, 'minecraft:oak_fence_gate', PlaceContext(face=1, look=2))     # facing=north: ось z, gate соединяется с x-соседями
        self.assertIn('west=true', self.name(ed, 0, 0, 0))

    def test_panes(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:glass_pane')
        ed.place(1, 0, 0, 'minecraft:glass_pane')
        ed.place(0, 0, -1, 'minecraft:oak_log')
        s = self.name(ed, 0, 0, 0)
        self.assertIn('east=true', s)
        self.assertIn('north=true', s)
        self.assertIn('west=false', s)

    def test_stairs_corner_shape(self):
        ed, _ = self.make()
        # A: facing=north bottom; за ним (на севере) ступень B facing=east -> A: outer_right
        ed.set_state(0, 0, -1, self.t.state_from_props('minecraft:oak_stairs', {'facing': 'east'}))
        ed.place(0, 0, 0, 'minecraft:oak_stairs', PlaceContext(face=1, hit=(.5, 0, .5), look=2))
        self.assertIn('facing=north', self.name(ed, 0, 0, 0))
        self.assertIn('shape=outer_right', self.name(ed, 0, 0, 0))
        # B, facing=east, впереди (на востоке) никого — straight; перед A (на юге) — ступень facing=west?: inner
        ed2, _ = self.make()
        ed2.set_state(0, 0, 1, self.t.state_from_props('minecraft:oak_stairs', {'facing': 'west'}))
        ed2.place(0, 0, 0, 'minecraft:oak_stairs', PlaceContext(face=1, hit=(.5, 0, .5), look=2))
        # A=north: фронт (юг) — ступень facing=west; west == ccw(north) -> inner_left
        self.assertIn('shape=inner_left', self.name(ed2, 0, 0, 0))
        # цепочка одинаковых — прямая
        ed3, _ = self.make()
        for x in range(3):
            ed3.place(x, 0, 0, 'minecraft:oak_stairs', PlaceContext(face=1, hit=(.5, 0, .5), look=5))
        self.assertTrue(all('shape=straight' in self.name(ed3, x, 0, 0) for x in range(3)))

    def test_stairs_half_from_click(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:oak_stairs', PlaceContext(face=2, hit=(.5, .8, 1.0), look=3))
        self.assertIn('half=top', self.name(ed, 0, 0, 0))
        ed.place(2, 0, 0, 'minecraft:oak_stairs', PlaceContext(face=2, hit=(.5, .2, 1.0), look=3))
        self.assertIn('half=bottom', self.name(ed, 2, 0, 0))
        ed.place(4, 0, 0, 'minecraft:oak_stairs', PlaceContext(face=0, hit=(.5, 1.0, .5), look=3))
        self.assertIn('half=top', self.name(ed, 4, 0, 0))

    def test_snowy(self):
        ed, _ = self.make()
        g = self.t.state_from_props('minecraft:grass_block', {'snowy': 'false'})
        ed.set_state(0, 0, 0, g)
        ed.place(0, 1, 0, 'minecraft:snow')
        self.assertIn('snowy=true', self.name(ed, 0, 0, 0))
        ed.break_block(0, 1, 0)
        self.assertIn('snowy=false', self.name(ed, 0, 0, 0))

    def test_door_and_double_plant(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:oak_door', PlaceContext(face=1, hit=(.5, 0, .5), look=3))
        self.assertIn('half=lower', self.name(ed, 0, 0, 0))
        self.assertIn('half=upper', self.name(ed, 0, 1, 0))
        ed.break_block(0, 1, 0)       # ломаем верхнюю — исчезает и нижняя
        self.assertEqual(self.name(ed, 0, 0, 0), 'minecraft:air')
        ed.undo()
        self.assertIn('half=lower', self.name(ed, 0, 0, 0))
        ed.place(3, 0, 0, 'minecraft:tall_grass')
        self.assertIn('half=upper', self.name(ed, 3, 1, 0))
        ed.break_block(3, 0, 0)
        self.assertEqual(self.name(ed, 3, 1, 0), 'minecraft:air')

    def test_door_hinge(self):
        ed, _ = self.make()
        # справа (при взгляде на юг право = запад) твёрдый блок -> петля справа
        ed.set_state(-1, 0, 0, self.t.state_id('minecraft:stone'))
        ed.place(0, 0, 0, 'minecraft:oak_door', PlaceContext(face=1, hit=(.5, 0, .5), look=3))
        self.assertIn('hinge=right', self.name(ed, 0, 0, 0))

    def test_slab(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:stone_slab', PlaceContext(face=1, hit=(.5, 0, .5), look=3))
        self.assertIn('type=bottom', self.name(ed, 0, 0, 0))
        ed.place(0, 0, 0, 'minecraft:stone_slab', PlaceContext(face=1, hit=(.5, .5, .5), look=3))
        self.assertIn('type=double', self.name(ed, 0, 0, 0))
        ed.place(2, 0, 0, 'minecraft:stone_slab', PlaceContext(face=0, hit=(.5, 1, .5), look=3))
        self.assertIn('type=top', self.name(ed, 2, 0, 0))

    def test_bed(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:red_bed', PlaceContext(face=1, hit=(.5, 0, .5), look=3))     # facing=south: голова на юг
        self.assertIn('part=foot', self.name(ed, 0, 0, 0))
        self.assertIn('part=head', self.name(ed, 0, 0, 1))
        ed.break_block(0, 0, 1)
        self.assertEqual(self.name(ed, 0, 0, 0), 'minecraft:air')

    def test_waterlogged_and_water_break(self):
        ed, _ = self.make()
        ed.set_state(0, 0, 0, self.t.state_id('minecraft:water[level=0]'))
        ed.place(0, 0, 0, 'minecraft:oak_stairs', PlaceContext(face=1, hit=(.5, 0, .5), look=3))
        self.assertIn('waterlogged=true', self.name(ed, 0, 0, 0))
        ed.break_block(0, 0, 0)
        self.assertEqual(self.name(ed, 0, 0, 0), 'minecraft:water[level=0]')

    def test_wall(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:cobblestone_wall')
        ed.place(1, 0, 0, 'minecraft:cobblestone_wall')
        s = self.name(ed, 0, 0, 0)
        self.assertIn('east=low', s)
        self.assertIn('north=none', s)
        # блок над стеной: сторона становится tall
        ed.set_state(0, 1, 0, self.t.state_id('minecraft:stone'))
        ed.set_state(1, 1, 0, self.t.state_id('minecraft:stone'))
        self.assertIn('east=tall', self.name(ed, 0, 0, 0))

    def test_axis_and_signs(self):
        ed, _ = self.make()
        ed.place(0, 0, 0, 'minecraft:oak_log', PlaceContext(face=4))
        self.assertIn('axis=x', self.name(ed, 0, 0, 0))
        ed.place(2, 0, 0, 'minecraft:oak_log', PlaceContext(face=3))
        self.assertIn('axis=z', self.name(ed, 2, 0, 0))
        ed.place(4, 0, 0, 'minecraft:oak_sign', PlaceContext(face=2, hit=(.5, .5, 1), look=3))
        self.assertIn('wall_sign', self.name(ed, 4, 0, 0))
        self.assertIn('facing=north', self.name(ed, 4, 0, 0))
        ed.place(6, 0, 0, 'minecraft:oak_sign', PlaceContext(face=1, yaw=180))
        self.assertIn('rotation=0', self.name(ed, 6, 0, 0))

    def test_boundary_affected(self):
        ed, _ = self.make()
        ed.set_state(15, 0, 15, self.t.state_id('minecraft:stone'))
        self.assertEqual(ed.take_affected(), {(0, 0), (1, 0), (0, 1), (1, 1)})
        ed.set_state(0, 0, 8, self.t.state_id('minecraft:stone'))
        self.assertEqual(ed.take_affected(), {(0, 0), (-1, 0)})
        ed.set_state(5, 0, 5, self.t.state_id('minecraft:stone'))
        self.assertEqual(ed.take_affected(), {(0, 0)})

    def test_transactions_and_layer(self):
        ed, blocks = self.make()
        ed.begin('много блоков')
        for x in range(5):
            ed.place(x, 0, 0, 'minecraft:oak_planks')
        ed.commit()
        self.assertEqual(len(ed.undo_stack), 1)
        self.assertEqual(sum(len(d) for d in ed.layer.values()), 5)
        ed.undo()
        self.assertEqual(ed.layer, {})
        ed.redo()
        d = ed.to_dict()
        self.assertEqual(len(d['chunks']['0,0']), 5)
        # слой «накладывается» на свежий мир
        ed2, blocks2 = self.make()
        applied, conflicts, aff = ed2.load_dict(json.loads(ed.to_json()))
        self.assertEqual((applied, conflicts), (5, 0))
        self.assertEqual(self.name(ed2, 3, 0, 0), 'minecraft:oak_planks')
        # конфликт: исходный блок в мире отличается от записанного
        ed3, blocks3 = self.make()
        blocks3[(0, 0)].reshape(384, 16, 16)[64, 0, 2] = self.t.state_id('minecraft:dirt')
        applied, conflicts, aff = ed3.load_dict(json.loads(ed.to_json()))
        self.assertEqual(conflicts, 1)

    def test_undo_limit(self):
        ed, _ = self.make()
        ed.max_undo = 3
        for i in range(6):
            ed.set_state(i, 0, 0, self.t.state_id('minecraft:stone'))
        self.assertEqual(len(ed.undo_stack), 3)

    def test_edit_vs_mesh_consistency(self):
        """После правки (граница чанков) меш затронутых чанков идентичен мешу, построенному с нуля (нет «швов»)."""
        from mcgen_addon.mesh import mesher
        ed, blocks = self.make()
        m = mesher.Mesher(self.t, self.t.biome_colors)
        mesh = lambda ck: m.mesh_chunk(ck[0], ck[1], blocks, {}, -64, 384)
        ed.set_state(16, 5, 7, self.t.state_id('minecraft:stone'))        # блок в соседнем чанке (1, 0)
        ed.take_affected()
        before = {ck: mesh(ck) for ck in [(0, 0), (1, 0)]}
        ed.set_state(15, 5, 7, self.t.state_id('minecraft:oak_planks'))   # на границе чанков (0, 0)/(1, 0)
        aff = ed.take_affected()
        self.assertEqual(aff, {(0, 0), (1, 0)})
        after = {ck: mesh(ck) for ck in aff}
        self.assertEqual(after[(1, 0)].n_quads, before[(1, 0)].n_quads - 1)     # западная грань блока соседа скрыта
        self.assertEqual(after[(0, 0)].n_quads, before[(0, 0)].n_quads + 5)
        ed.undo()
        for ck in [(0, 0), (1, 0)]:
            self.assertEqual(mesh(ck).n_quads, before[ck].n_quads)


if __name__ == '__main__':
    unittest.main()

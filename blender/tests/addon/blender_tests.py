"""Тесты аддона ВНУТРИ Blender (headless):  blender -b --factory-startup --python blender_tests.py [-- --json out.json]

Окружение (задаёт run_tests.py):
  BLENDER_USER_RESOURCES  чистый профиль; в нём extensions/user_default/mcgen -> каталог аддона (как «разработка из исходников»)
  MCGEN_EXT_MODULE        имя пакета расширения (по умолчанию bl_ext.user_default.mcgen)
  MCGEN_BACKEND, MCGEN_LIB  какой бэкенд проверять: mock | lib (заглушка, собранная build.py)
  MCGEN_CACHE             кэш с готовыми ресурсами 26.3 (их подготовил test_core.py), чтобы pack.resolve находил pack
"""
import json
import os
import sys
import tempfile
import time
import traceback
import unittest

import bpy
import addon_utils

HERE = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()
sys.path.insert(0, HERE)
EXT = os.environ.get('MCGEN_EXT_MODULE', 'bl_ext.user_default.mcgen')
BACKEND = os.environ.get('MCGEN_BACKEND', 'mock')

addon_utils.enable(EXT, default_set=True, persistent=False, handle_error=lambda e: (_ for _ in ()).throw(e))
import importlib  # noqa: E402

ADDON = importlib.import_module(EXT)
props = importlib.import_module(EXT + '.ui.props')
panels = importlib.import_module(EXT + '.ui.panels')
ops = importlib.import_module(EXT + '.ui.ops')
jobs = importlib.import_module(EXT + '.core.jobs')
pack = importlib.import_module(EXT + '.core.pack')
backend = importlib.import_module(EXT + '.core.backend')
seeds_mod = importlib.import_module(EXT + '.core.seeds')
params_mod = importlib.import_module(EXT + '.core.params')
biomes_mod = importlib.import_module(EXT + '.core.biomes')
catalog = importlib.import_module(EXT + '.core.catalog')
mock = importlib.import_module(EXT + '.core.mock')
translations = importlib.import_module(EXT + '.ui.translations')
scene_iface = importlib.import_module(EXT + '.core.scene_iface')
paths_mod = importlib.import_module(EXT + '.core.paths')
import numpy as np  # noqa: E402

import extract_strings  # noqa: E402

TWEAKS = params_mod.tweaks_doc()['tweaks']
REAL = os.environ.get('MCGEN_REAL') == '1'          # настоящая libmcgen (а не заглушка)
mock_only = unittest.skipIf(BACKEND != 'mock', 'содержимое мира проверяется только на макете (заглушка библиотеки возвращает плоский мир)')


def S():
    return bpy.context.scene.mcgen


def reset_scene():
    ops_ = bpy.ops.mcgen
    try:
        ops_.clear()
    except RuntimeError:
        pass
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    s = S()
    for name in props.SETTINGS_PRESET_FIELDS:
        s.property_unset(name)
    s.property_unset('collection_name')
    s.property_unset('auto_update')
    for t in TWEAKS:
        s.tweaks.property_unset(t['id'])
    s.stats.has_data = False
    s.stats.error = ''
    jobs.drop_sessions()
    prefs = props.get_prefs()
    if prefs:
        prefs.sink = 'FALLBACK'


class RecordingLayout:
    """Подмена UILayout для проверки функций draw_*: запоминает свойства, операторы, тексты; sub-layout'ы возвращают себя."""
    ICONS = set(bpy.types.UILayout.bl_rna.functions['label'].parameters['icon'].enum_items.keys())

    def __init__(self, log=None):
        self.log = log if log is not None else {'props': [], 'ops': [], 'labels': [], 'icons': set(), 'progress': 0}

    def __getattr__(self, name):
        if name in ('use_property_split', 'use_property_decorate', 'active', 'enabled', 'alert', 'scale_y', 'scale_x', 'emboss', 'alignment', 'operator_context'):
            return None
        raise AttributeError(name)

    def __setattr__(self, name, value):
        if name == 'log':
            object.__setattr__(self, name, value)

    def _icon(self, kw):
        ic = kw.get('icon')
        if ic:
            self.log['icons'].add(ic)

    def prop(self, data, name, **kw):
        assert name in data.bl_rna.properties, f'{data.bl_rna.identifier}.{name} не существует'
        self.log['props'].append((data.bl_rna.identifier, name))
        self._icon(kw)
        if kw.get('text'):
            self.log['labels'].append(kw['text'])

    def label(self, text='', **kw):
        self.log['labels'].append(text)
        self._icon(kw)

    def operator(self, idname, text='', **kw):
        mod, fn = idname.split('.')
        assert hasattr(getattr(bpy.ops, mod), fn), f'нет оператора {idname}'
        getattr(getattr(bpy.ops, mod), fn).get_rna_type()
        self.log['ops'].append(idname)
        self._icon(kw)
        if text:
            self.log['labels'].append(text)

        class Op:
            def __setattr__(s, k, v):
                pass
        return Op()

    def progress(self, **kw):
        self.log['progress'] += 1

    def popover(self, *a, **kw):
        pass

    def menu(self, *a, **kw):
        pass

    def separator(self, *a, **kw):
        pass

    def row(self, *a, **kw):
        return self

    def column(self, *a, **kw):
        return self

    def box(self, *a, **kw):
        return self

    def split(self, *a, **kw):
        return self

    def grid_flow(self, *a, **kw):
        return self


class FakePanel:
    def __init__(self, layout):
        self.layout = layout


class T01_Registration(unittest.TestCase):
    def test_extension_enabled(self):
        self.assertIn(EXT, bpy.context.preferences.addons)
        self.assertTrue(hasattr(bpy.types.Scene, 'mcgen'))
        self.assertEqual(props.ROOT, EXT)

    def test_classes_registered(self):
        ids = [c for c in dir(bpy.types) if c.startswith('MCGEN_')]
        self.assertGreaterEqual(len(ids), 45, ids)
        for op in ('generate', 'update_layers', 'biome_map', 'prepare_resources', 'clear', 'cancel', 'download_jars', 'detect_jars', 'preset_add', 'random_seed', 'reset_tweaks'):
            self.assertTrue(hasattr(bpy.ops.mcgen, op), op)

    def test_panel_locations(self):
        main = [c for c in (getattr(bpy.types, n) for n in dir(bpy.types) if n.startswith('MCGEN_PT_')) if c.bl_label == 'MC World']
        spaces = {(c.bl_space_type, getattr(c, 'bl_category', None) or getattr(c, 'bl_context', None)) for c in main}
        self.assertEqual(spaces, {('PROPERTIES', 'scene'), ('VIEW_3D', 'MC World')})
        sub = {c.bl_label for c in (getattr(bpy.types, n) for n in dir(bpy.types) if n.startswith('MCGEN_PT_') and not n.startswith('MCGEN_PT_n_'))}
        for label in ('Version & World', 'Seeds', 'Area', 'Layers', 'World Tweaks', 'View', 'Biome Preview', 'Resources', 'Stats'):
            self.assertIn(label, sub)
        groups = {g['label'] for g in params_mod.tweaks_doc()['groups']}
        self.assertTrue(groups <= sub, groups - sub)

    def test_preferences(self):
        p = props.get_prefs()
        self.assertIsNotNone(p)
        self.assertFalse(p.accept_eula)                       # EULA по умолчанию НЕ принята
        self.assertEqual(p.backend, 'auto')


class T02_Properties(unittest.TestCase):
    def setUp(self):
        reset_scene()

    def test_defaults(self):
        s = S()
        self.assertEqual((s.version, s.dimension, s.preset), ('26.3', 'minecraft:overworld', 'normal'))
        self.assertEqual((s.seed_mode, s.seed, s.unit, s.size_x, s.size_z), ('UNIFIED', '12345', 'BLOCKS', 8, 8))
        self.assertEqual((s.use_terrain, s.use_surface, s.use_caves, s.use_features, s.use_structures, s.use_tweaks), (True, True, True, False, False, False))

    def test_slider_limits(self):
        r = S().bl_rna.properties
        for n in ('size_x', 'size_z'):
            self.assertEqual((r[n].soft_max, r[n].hard_max, r[n].hard_min), (64, 512, 1), n)
        self.assertGreaterEqual(r['y_max'].hard_max, 320)

    def test_tweaks_dynamic(self):
        tw = S().tweaks
        rna = tw.bl_rna.properties
        self.assertEqual(len(TWEAKS), len([p for p in rna if p.identifier not in ('rna_type', 'name')]))
        for t in TWEAKS:
            p = rna[t['id']]
            self.assertEqual(p.name, t['label'])
            if t['type'] == 'float':
                self.assertEqual(p.type, 'FLOAT')
                self.assertAlmostEqual(p.hard_min, t['min'], places=5)
                self.assertAlmostEqual(p.hard_max, t['max'], places=5)
                self.assertAlmostEqual(p.default, t['default'])
                self.assertAlmostEqual(p.soft_max, t['soft_max'], places=5)
            elif t['type'] == 'int':
                self.assertEqual((p.type, p.hard_min, p.hard_max, p.default), ('INT', t['min'], t['max'], t['default']))
            else:
                self.assertEqual((p.type, bool(p.default)), ('BOOLEAN', bool(t['default'])))
        self.assertEqual(S().tweaks_changed(), ())
        S().tweaks.cave_density = 1.5
        S().tweaks.sea_level_offset = -3
        self.assertEqual(dict(S().tweaks_changed()), {'cave_density': 1.5, 'sea_level_offset': -3})

    def test_enum_lists(self):
        s = S()
        # элементы динамических списков вычисляются обратными вызовами (bl_rna.enum_items для них пуст)
        vers = [i[0] for i in props._version_items(s, None)]
        self.assertEqual(vers[0], '26.3')
        self.assertIn('26.4-snapshot-2', vers)
        self.assertEqual(vers[-1], '26.4-snapshot-2')                        # снимки — после стабильных версий
        dims = [i[0] for i in props._dimension_items(s, None)]
        self.assertEqual(dims, ['minecraft:overworld', 'minecraft:the_nether', 'minecraft:the_end'])
        s.dimension = 'minecraft:the_end'
        self.assertEqual([i[0] for i in props._preset_items(s, None)], ['normal'])
        s.dimension = 'minecraft:overworld'
        self.assertIn('amplified', [i[0] for i in props._preset_items(s, None)])
        with self.assertRaises(TypeError):
            s.dimension = 'minecraft:nowhere'
        with self.assertRaises(TypeError):
            s.version = '1.2.3'
        s.preset = 'amplified'
        s.dimension = 'minecraft:the_nether'
        self.assertEqual(s.preset, 'normal')                   # пресет, которого нет у измерения, сбрасывается

    def test_seed_modes(self):
        s = S()
        s.seed = 'hello'
        s.seed_mode = 'SPLIT'
        self.assertEqual((s.seed_climate, s.seed_terrain, s.seed_structures, s.seed_features), ('hello',) * 4)
        s.seed_features = '5'
        s.seed_mode = 'UNIFIED'
        self.assertEqual(s.seed, 'hello')
        bpy.ops.mcgen.random_seed(target='seed')
        self.assertNotEqual(s.seed, 'hello')
        self.assertEqual(seeds_mod.parse_seed(s.seed)[1], 'number')

    def test_unit_conversion(self):
        s = S()
        s.origin_x, s.origin_z = 160, -33
        s.unit = 'CHUNKS'
        self.assertEqual((s.origin_x, s.origin_z), (10, -3))
        self.assertEqual(s.origin_chunks(), (10, -3))
        s.unit = 'BLOCKS'
        self.assertEqual((s.origin_x, s.origin_z), (160, -48))
        self.assertEqual(s.origin_chunks(), (10, -3))

    def test_collect_params(self):
        s = S()
        s.seed_mode = 'SPLIT'
        s.seed_climate, s.seed_terrain, s.seed_structures, s.seed_features = 'hello', '12345', '-7', 'world'
        s.origin_x, s.origin_z, s.size_x, s.size_z = -17, 31, 5, 3
        s.use_features = True
        s.use_tweaks = True
        s.tweaks.ore_density = 2.0
        p = ops.collect_params(bpy.context.scene, props.get_prefs())
        self.assertEqual(p.seeds, (99162322, 12345, -7, 113318802))
        self.assertEqual((p.cx0, p.cz0, p.nx, p.nz), (-2, 1, 5, 3))
        self.assertEqual(p.stages, 1 | 2 | 4 | 8 | 16)
        self.assertEqual(p.tweaks, (('ore_density', 2.0),))
        s.use_tweaks = False
        self.assertEqual(ops.collect_params(bpy.context.scene, None).tweaks, ())
        s.seed_mode = 'UNIFIED'
        s.seed = ''
        p = ops.collect_params(bpy.context.scene, None)
        self.assertNotEqual(s.seed, '')                          # пустой сид заменён случайным и показан пользователю
        self.assertEqual(len(set(p.seeds)), 1)

    def test_reset_tweaks(self):
        S().tweaks.terrain_amplitude = 3.0
        bpy.ops.mcgen.reset_tweaks()
        self.assertEqual(S().tweaks.terrain_amplitude, 1.0)


class T03_Panels(unittest.TestCase):
    def setUp(self):
        reset_scene()

    def test_draw_all_panels(self):
        n = 0
        allicons = set()
        for name in sorted(dir(bpy.types)):
            if not name.startswith('MCGEN_PT_') or name in ('MCGEN_PT_presets', 'MCGEN_PT_presets_view3d'):
                continue
            cls = getattr(bpy.types, name)
            fn = cls.draw if hasattr(cls, 'draw') else None
            self.assertIsNotNone(fn, name)
            log = {'props': [], 'ops': [], 'labels': [], 'icons': set(), 'progress': 0}
            lay = RecordingLayout(log)
            # методы класса Panel, заданные нами через type(): берём функции из __dict__ исходного Python-класса
            pyfn = fn
            pyfn(FakePanel(lay), bpy.context)
            allicons |= log['icons']
            if cls.bl_label in ('Seeds', 'Area', 'Layers', 'View', 'Version & World', 'Biome Preview'):
                self.assertTrue(log['props'], name)
            n += 1
        self.assertGreaterEqual(n, 30)
        bad = allicons - RecordingLayout.ICONS
        self.assertFalse(bad, f'несуществующие значки: {bad}')

    def test_draw_with_stats_and_error(self):
        s = S()
        s.stats.fill({'backend': 'mock', 'sink': 'x', 'chunks': 4, 'objects': 4, 'faces': 12345, 't_generate': 1.0, 't_build': 0.5, 't_total': 1.6,
                      'memory': 5e6, 'stage_times': {'terrain': 0.7, 'surface': 0.2}, 'peak_rss': 8e8, 'mode': 'update', 'rebuilt': 3,
                      'structures_total': 3, 'structure_types': {'minecraft:village_plains': 2, 'minecraft:mineshaft': 1},
                      'structure_starts': [('minecraft:village_plains', 1, 2, (16, 60, 32, 60, 80, 70), 12)]})
        s.stats.error = 'line1\nline2'
        for name in ('MCGEN_PT_stats', 'MCGEN_PT_main', 'MCGEN_PT_resources', 'MCGEN_PT_n_stats'):
            lay = RecordingLayout()
            getattr(bpy.types, name).draw(FakePanel(lay), bpy.context)
        lay = RecordingLayout()
        getattr(bpy.types, 'MCGEN_PT_stats').draw(FakePanel(lay), bpy.context)
        self.assertTrue(any('Faces' in x for x in lay.log['labels']))
        self.assertTrue(any('Structures: 3' in x for x in lay.log['labels']), lay.log['labels'])
        self.assertTrue(any(x.startswith('village_plains: 2') for x in lay.log['labels']))
        self.assertIn('mcgen.structure_markers', lay.log['ops'])
        self.assertTrue(lay.log['icons'] <= RecordingLayout.ICONS)

    def test_tweak_groups_cover_all_tweaks(self):
        shown = set()
        for gid, _label, items in props.tweak_groups():
            lay = RecordingLayout()
            panels.make_tweak_group_draw(gid)(FakePanel(lay), bpy.context)
            shown |= {n for _i, n in lay.log['props']}
        self.assertEqual(shown, {t['id'] for t in TWEAKS})

    def test_prefs_draw(self):
        lay = RecordingLayout()
        panels.draw_resources(lay, bpy.context, props.get_prefs(), in_prefs=True)
        names = {n for _i, n in lay.log['props']}
        for p in ('server_jar', 'client_jar', 'java_path', 'reports_folder', 'accept_eula', 'backend', 'threads', 'cache_dir'):
            self.assertIn(p, names)
        self.assertIn('mcgen.download_jars', lay.log['ops'])


def run_job(op_name, **kw):
    return getattr(bpy.ops.mcgen, op_name)('EXEC_DEFAULT', **kw)


class T04_Operators(unittest.TestCase):
    def setUp(self):
        reset_scene()
        s = S()
        s.size_x = s.size_z = 3
        s.seed = '7'
        s.origin_x, s.origin_z = -240, 240                      # на макете здесь холмистая суша (плоский океан ничего бы не проверял)
        os.environ['MCGEN_BACKEND'] = BACKEND
        backend.release_all()

    def objs(self):
        return {o.name: o for o in bpy.data.objects if o.get('mcgen_preview')}

    def test_backend_in_use(self):
        self.assertEqual(backend.name(), 'mock' if BACKEND == 'mock' else 'lib')

    def test_generate_builds_scene(self):
        self.assertEqual(run_job('generate'), {'FINISHED'})
        o = self.objs()
        self.assertEqual(len(o), 9)
        s = S()
        st = s.stats
        self.assertTrue(st.has_data)
        self.assertEqual((st.chunks, st.objects, st.backend), (9, 9, backend.name()))
        # верхние грани колонок (+ боковые стенки там, где есть перепады; у плоской заглушки/океана их нет)
        (self.assertGreater if BACKEND == 'mock' else self.assertGreaterEqual)(st.faces, 9 * 256)
        self.assertGreater(st.t_generate, 0)
        self.assertGreater(st.memory_mb, 1.0)
        self.assertGreaterEqual(len(st.stage_times), 1)
        coll = bpy.data.collections['MC World']
        self.assertEqual((coll['mcgen_cx0'], coll['mcgen_cz0']), (-15, 15))
        self.assertEqual(coll['mcgen_min_y'], -64)
        # геометрия: колонка (x,z) чанка 0,0: Blender (x, -z, y); верх первого чанка лежит в плоскости y в пределах мира
        ob = o['MC Chunk -15,15']
        vs = np.empty(len(ob.data.vertices) * 3, np.float32)
        ob.data.vertices.foreach_get('co', vs)
        vs = vs.reshape(-1, 3)
        self.assertTrue((vs[:, 0] >= 0).all() and (vs[:, 0] <= 16).all())
        self.assertTrue((vs[:, 1] <= 0).all() and (vs[:, 1] >= -16).all())          # север = +Y, юг = -Y
        self.assertIn('Col', ob.data.color_attributes)

    def test_chunks_per_object(self):
        S().chunks_per_object = '2'
        run_job('generate')
        self.assertEqual(len(self.objs()), 4)                  # 3×3 чанка по 2×2 -> 4 группы

    def test_origin_offsets_objects(self):
        S().origin_x, S().origin_z = 32, -16
        run_job('generate')
        o = self.objs()
        self.assertIn('MC Chunk 2,-1', o)
        self.assertEqual(tuple(o['MC Chunk 2,-1'].location), (0, 0, 0))
        self.assertEqual(tuple(o['MC Chunk 3,-1'].location), (16, 0, 0))
        self.assertEqual(tuple(o['MC Chunk 2,0'].location), (0, -16, 0))

    def test_generate_matches_library_data(self):
        """Меш и данные согласованы: высота верхней грани колонки в предпросмотре = карта высот региона."""
        run_job('generate')
        sess = jobs.session(bpy.context.scene.name)
        reg = sess.region
        i = reg.info
        c = (i.cx0 + 1, i.cz0 + 1)
        hm = reg.heightmap(*c, 0)
        ob = self.objs()[f'MC Chunk {c[0]},{c[1]}']
        vs = np.empty(len(ob.data.vertices) * 3, np.float32)
        ob.data.vertices.foreach_get('co', vs)
        vs = vs.reshape(-1, 3)
        top = vs[vs[:, 2] == float(hm[0, 0])]
        self.assertGreater(len(top), 0)

    def test_update_noop_and_view_only(self):
        run_job('generate')
        before = {k: v['mcgen_stamp'] for k, v in self.objs().items()}
        t_gen = S().stats.t_generate
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        self.assertEqual({k: v['mcgen_stamp'] for k, v in self.objs().items()}, before)            # ничего не менялось — ничего не пересобрано
        S().tint_biomes = False
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        st = S().stats
        self.assertEqual(st.mode, 'update')
        self.assertEqual(st.objects, 9)                                                         # перекраска: все меши пересобраны
        self.assertEqual(jobs.session(bpy.context.scene.name).stats['reasons'], ['view'])
        self.assertNotEqual({k: v['mcgen_stamp'] for k, v in self.objs().items()}, before)

    @mock_only
    def test_update_rebuilds_only_changed_chunks(self):
        s = S()
        s.size_x = s.size_z = 12
        s.use_features = True
        s.use_structures = True
        s.seed = '7'
        run_job('generate')
        sess = jobs.session(bpy.context.scene.name)
        import hashlib
        snap = {c: hashlib.md5(sess.region.blocks(*c).tobytes()).hexdigest() for c in sess.region.chunks()}
        before = {k: v['mcgen_stamp'] for k, v in self.objs().items()}
        n_all = len(before)
        s.use_tweaks = True
        s.tweaks.structure_frequency = 0.0                     # убирает «постройки» только там, где они были
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        new_region = sess.region
        expected = {f'MC Chunk {c[0]},{c[1]}' for c in new_region.chunks() if hashlib.md5(new_region.blocks(*c).tobytes()).hexdigest() != snap[c]}
        st = S().stats
        after = {k: v['mcgen_stamp'] for k, v in self.objs().items()}
        rebuilt = {k for k in after if after[k] != before.get(k)}
        self.assertEqual(len(after), n_all)
        self.assertGreater(len(expected), 0, 'структуры должны были исчезнуть хотя бы в одном чанке')
        self.assertLess(st.objects, n_all)                                                      # пересобрана часть, не всё
        self.assertTrue(expected <= rebuilt)                                                    # все изменившиеся чанки пересобраны
        grown = set()                                                                           # + их соседи (границы чанков без швов)
        for name in expected:
            cx, cz = map(int, name.split(' ')[2].split(','))
            for dz in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    grown.add(f'MC Chunk {cx + dx},{cz + dz}')
        self.assertEqual(rebuilt, grown & set(after))
        self.assertEqual(len(rebuilt), st.objects)
        self.assertEqual(sess.stats['reasons'], ['tweak structure_frequency'])

    @mock_only
    def test_update_layers_toggle(self):
        run_job('generate')
        before = {k: v['mcgen_stamp'] for k, v in self.objs().items()}
        S().use_features = True
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        st = S().stats
        self.assertIn('layers', jobs.session(bpy.context.scene.name).stats['reasons'])
        self.assertGreater(st.objects, 0)
        # слои выключили обратно -> воксели совпадают с исходными, но объекты обновлены
        S().use_features = False
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        self.assertEqual(len(self.objs()), 9)

    def test_area_change_rebuilds_new_chunks(self):
        run_job('generate')
        S().size_x = 4
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        self.assertEqual(len(self.objs()), 12)

    def test_clear(self):
        run_job('generate')
        bpy.ops.mcgen.clear()
        self.assertEqual(len(self.objs()), 0)
        self.assertIsNone(jobs.session(bpy.context.scene.name).region)
        self.assertNotIn('MC Chunk 0,0', bpy.data.objects)
        self.assertEqual(len([m for m in bpy.data.meshes if m.get('mcgen_preview')]), 0)
        self.assertNotIn('MC World', bpy.data.collections)

    @mock_only
    def test_seed_changes_world(self):
        run_job('generate')
        a = np.array(jobs.session(bpy.context.scene.name).region.heightmap(-15, 15, 0))
        S().seed = '99999'
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        b = np.array(jobs.session(bpy.context.scene.name).region.heightmap(-15, 15, 0))
        self.assertFalse(np.array_equal(a, b))
        self.assertEqual(jobs.session(bpy.context.scene.name).stats['reasons'][0], 'seed climate')

    def test_dimensions(self):
        for dim, h in (('minecraft:the_nether', 256), ('minecraft:the_end', 256)):
            S().dimension = dim
            S().y_min, S().y_max = 0, 255
            run_job('generate')
            self.assertEqual(jobs.session(bpy.context.scene.name).region.info.height, h)
            self.assertEqual(len(self.objs()), 9)
            bpy.ops.mcgen.clear()

    def test_error_reporting(self):
        S().seed_mode = 'UNIFIED'
        os.environ['MCGEN_BACKEND'] = 'lib'
        os.environ['MCGEN_LIB'] = '/nonexistent/libmcgen.so'
        try:
            import importlib as il
            il.import_module(EXT + '.core.lib')._lib = None
            backend.release_all()
            with self.assertRaises(RuntimeError) as c:
                run_job('generate')
            self.assertIn('libmcgen', str(c.exception))
        finally:
            os.environ.pop('MCGEN_LIB', None)
            os.environ['MCGEN_BACKEND'] = BACKEND
            if BACKEND == 'lib' and os.environ.get('MCGEN_STUB_LIB'):
                os.environ['MCGEN_LIB'] = os.environ['MCGEN_STUB_LIB']
            backend.release_all()

    def test_biome_map(self):
        s = S()
        s.size_x = s.size_z = 4
        s.bm_step = '4'
        s.bm_palette = 'MAP'
        run_job('biome_map')
        img = bpy.data.images['MCGen Biomes overworld']
        self.assertEqual(tuple(img.size), (16, 16))
        self.assertIsNotNone(img.packed_file)                       # карта сохраняется в .blend
        plane = [o for o in bpy.data.objects if o.get('mcgen_biome_plane')]
        self.assertEqual(len(plane), 1)
        # северный край (z0) — верхняя строка изображения: сверяем пиксели с сеткой биомов напрямую
        sess_params = ops.collect_params(bpy.context.scene, None)
        gen = backend.open_gen(pack.resolve('26.3')['pack'] if pack.resolve('26.3')['pack_ok'] else None, '26.3')
        w = gen.world(sess_params.dimension, sess_params.preset, sess_params.seeds, {})
        grid = w.biome_grid(-240, 240, 16, 16, 4, 64)
        pal = biomes_mod.palette(gen.biome_names(), 'MAP')
        px = np.array(img.pixels[:]).reshape(16, 16, 4)[::-1]                      # строки сверху вниз
        np.testing.assert_allclose(px[..., :3], pal[grid], atol=1.5 / 255.0)
        # 4 палитры
        for pal_mode in ('HASH', 'JSON'):
            s.bm_palette = pal_mode
            run_job('biome_map')
        s.bm_plane = False
        s.bm_step = '1'
        run_job('biome_map')
        self.assertEqual(tuple(bpy.data.images['MCGen Biomes overworld'].size), (64, 64))

    def _layers(self, **kw):
        s = S()
        for k in ('terrain', 'surface', 'caves', 'features', 'structures'):
            setattr(s, 'use_' + k, kw.get(k, False))

    def test_layers_are_real_stage_masks(self):
        """Terrain, Surface, Caves, Features, Structures -> реальные маски стадий библиотеки (BIOMES всегда)."""
        bits = {'terrain': 2, 'surface': 4, 'caves': 8, 'features': 16, 'structures': 32}
        sess = jobs.session(bpy.context.scene.name)
        S().size_x = S().size_z = 2
        for combo, mask in (({'terrain': True}, 3), ({'terrain': True, 'surface': True, 'caves': True}, 15), ({'terrain': True, 'features': True}, 19),
                            ({'terrain': True, 'surface': True, 'caves': True, 'features': True, 'structures': True}, 63), ({'surface': True}, 1)):
            self._layers(**combo)
            self.assertEqual(ops.collect_params(bpy.context.scene, None).stages, mask, combo)
            run_job('generate')
            self.assertEqual(sess.region.stages, mask)

    def test_full_pipeline_stats_and_structures(self):
        s = S()
        s.size_x = s.size_z = 16
        self._layers(terrain=True, surface=True, caves=True, features=True, structures=True)
        run_job('generate')
        sess = jobs.session(bpy.context.scene.name)
        st = s.stats
        i = sess.region.info
        starts = sess.world.structure_starts(i.cx0, i.cz0, i.nx, i.nz)
        self.assertGreater(len(starts), 0, 'в области должны быть постройки')
        self.assertEqual(st.structures_total, len(starts))
        self.assertEqual({e.name: e.count for e in st.structure_types}, {k: sum(1 for x in starts if x.id == k) for k in {x.id for x in starts}})
        self.assertEqual(len(st.structure_starts), min(len(starts), props.MAX_LISTED_STARTS))
        e0, x0 = st.structure_starts[0], starts[0]
        self.assertEqual((e0.name, e0.chunk_x, e0.chunk_z, tuple(e0.bb)), (x0.id, x0.chunk_x, x0.chunk_z, tuple(x0.bb)))
        self.assertIn('structure starts', {e.name for e in st.stage_times})
        if sys.platform.startswith('linux'):
            self.assertGreater(st.peak_rss_mb, 50)
        print('\n   [стадии]', {e.name: round(e.seconds, 3) for e in st.stage_times}, '[построек]', st.structures_total, dict(list({e.name: e.count for e in st.structure_types}.items())[:3]))
        # слой Structures выключен -> списка нет
        s.use_structures = False
        run_job('generate')
        self.assertEqual(S().stats.structures_total, 0)

    def test_structure_markers_operator(self):
        s = S()
        s.size_x = s.size_z = 16
        self._layers(terrain=True, structures=True)
        run_job('generate')
        n = len(s.stats.structure_starts)
        self.assertGreater(n, 0)
        self.assertEqual(bpy.ops.mcgen.structure_markers('EXEC_DEFAULT'), {'FINISHED'})
        marks = [o for o in bpy.data.objects if o.get('mcgen_structure')]
        self.assertEqual(len(marks), n)
        e = s.stats.structure_starts[0]
        m = next(o for o in marks if o.name == f'{e.name.split(":", 1)[-1]} {e.chunk_x},{e.chunk_z}' or o['mcgen_structure'] == e.name)
        x0, y0, z0, x1, y1, z1 = e.bb
        i = jobs.session(bpy.context.scene.name).region.info
        self.assertAlmostEqual(m.location.x, (x0 + x1 + 1) / 2.0 - i.cx0 * 16, places=3)
        self.assertAlmostEqual(m.location.y, -((z0 + z1 + 1) / 2.0 - i.cz0 * 16), places=3)
        self.assertAlmostEqual(m.location.z, (y0 + y1 + 1) / 2.0, places=3)
        bpy.ops.mcgen.clear()
        self.assertEqual(len([o for o in bpy.data.objects if o.get('mcgen_structure')]), 0)

    def test_cancel_midway_keeps_previous_scene(self):
        s = S()
        s.size_x = s.size_z = 2
        run_job('generate')
        sess = jobs.session(bpy.context.scene.name)
        before = sess.region
        stamps = {k: v['mcgen_stamp'] for k, v in self.objs().items()}
        s.size_x = s.size_z = {'mock': 14}.get(BACKEND, 14) if not REAL else 24
        s.seed = '424242'
        if BACKEND == 'lib' and not REAL:
            os.environ['MCGEN_STUB_DELAY_MS'] = '15'
        try:
            self._layers(terrain=True, surface=True, caves=True, features=REAL, structures=REAL)
            params = ops.collect_params(bpy.context.scene, None)
            job = jobs.GenerateJob(bpy.context.scene, params, mode='update', skey=bpy.context.scene.name, sink_pref='FALLBACK').start()
            t0 = time.time()
            while not job.finished and time.time() - t0 < 120:
                job.poll(0.01)
                if job.fraction > 0.12 and job.phase == 'generate':
                    job.cancel()
                time.sleep(0.005)
        finally:
            os.environ.pop('MCGEN_STUB_DELAY_MS', None)
        self.assertEqual(job.state, 'cancelled')
        self.assertIs(sess.region, before)                                   # прежний регион и сцена не тронуты
        self.assertEqual({k: v['mcgen_stamp'] for k, v in self.objs().items()}, stamps)
        run_job('generate')                                                  # после отмены всё работает
        self.assertEqual(len(self.objs()), 14 * 14 if not REAL else 24 * 24)

    @unittest.skipUnless(REAL, 'нужна настоящая библиотека: слои меняют содержимое чанков')
    def test_update_after_enabling_features_and_structures(self):
        s = S()
        s.size_x = s.size_z = 6
        s.seed = '12345'
        self._layers(terrain=True, surface=True, caves=True)
        run_job('generate')
        stamps = {k: v['mcgen_stamp'] for k, v in self.objs().items()}
        s.use_features = True
        s.use_structures = True
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        sess = jobs.session(bpy.context.scene.name)
        self.assertIn('layers', sess.stats['reasons'])
        self.assertEqual(sess.region.stages, 63)
        self.assertGreater(S().stats.objects, 0)
        new = {k: v['mcgen_stamp'] for k, v in self.objs().items()}
        self.assertTrue(any(new[k] != stamps[k] for k in stamps), 'Update Layers не пересобрал ни одного чанка')
        self.assertEqual(S().stats.mode, 'update')

    def test_refuses_area_that_does_not_fit_in_ram(self):
        s = S()
        s.size_x = s.size_z = 512
        with self.assertRaises(RuntimeError) as c:
            run_job('generate')
        self.assertIn('GB', str(c.exception))
        self.assertEqual(len(self.objs()), 0)

    def test_biome_map_big_area_auto_step(self):
        s = S()
        s.size_x = s.size_z = 512
        s.bm_step = '1'
        p = ops.collect_params(bpy.context.scene, None)
        self.assertEqual(jobs.auto_step(p, 1), 2)                  # 8192² пикселей -> шаг 2 (4096²)

    def test_download_requires_eula_and_online(self):
        prefs = props.get_prefs()
        prefs.accept_eula = False
        with self.assertRaises(RuntimeError) as c:
            bpy.ops.mcgen.download_jars('EXEC_DEFAULT')
        self.assertIn('EULA', str(c.exception))
        prefs.accept_eula = True
        if not getattr(bpy.app, 'online_access', True):
            with self.assertRaises(RuntimeError) as c:
                bpy.ops.mcgen.download_jars('EXEC_DEFAULT')
            self.assertIn('Online access', str(c.exception))
        prefs.accept_eula = False

    def test_prepare_resources_operator_with_reports_folder(self):
        from test_core import fake_server_jar, fake_client_jar, fake_reports  # noqa: E402
        root = tempfile.mkdtemp(prefix='mcgen-ops-')
        old_cache = paths_mod._cache_override
        paths_mod.set_cache_override(os.path.join(root, 'cache'))
        prefs = props.get_prefs()
        keep = (prefs.server_jar, prefs.client_jar, prefs.reports_folder, prefs.java_path)
        try:
            prefs.server_jar = fake_server_jar(os.path.join(root, 's.jar'), '99.2')
            prefs.client_jar = fake_client_jar(os.path.join(root, 'c.jar'), '99.2')
            prefs.reports_folder = fake_reports(os.path.join(root, 'rep'))
            self.assertEqual(bpy.ops.mcgen.prepare_resources('EXEC_DEFAULT'), {'FINISHED'})
            self.assertTrue(pack.resolve('99.2')['pack_ok'])
            self.assertIn('99.2', ops.resource_state['message'])
            # без reports и без Java -> понятная ошибка
            prefs.reports_folder = ''
            prefs.server_jar = fake_server_jar(os.path.join(root, 's3.jar'), '99.3')
            prefs.client_jar = fake_client_jar(os.path.join(root, 'c3.jar'), '99.3')
            prefs.java_path = '/nonexistent/java'
            orig = pack.find_java
            pack.find_java = lambda *a, **k: None
            try:
                with self.assertRaises(RuntimeError) as c:
                    bpy.ops.mcgen.prepare_resources('EXEC_DEFAULT')
            finally:
                pack.find_java = orig
            self.assertIn('Java', str(c.exception))
            self.assertIn('Java 25', ops.resource_state['error'])
        finally:
            prefs.server_jar, prefs.client_jar, prefs.reports_folder, prefs.java_path = keep
            paths_mod.set_cache_override(old_cache)
            catalog.clear()

    def test_detect_jars_operator(self):
        from test_core import fake_server_jar, fake_client_jar  # noqa: E402
        root = tempfile.mkdtemp(prefix='mcgen-det-')
        home = os.path.join(root, 'home')
        os.makedirs(os.path.join(home, '.minecraft', 'versions', '99.4'))
        os.makedirs(os.path.join(home, 'Downloads'))
        fake_client_jar(os.path.join(home, '.minecraft', 'versions', '99.4', '99.4.jar'), '99.4')
        fake_server_jar(os.path.join(home, 'Downloads', 'server.jar'), '99.4')
        old = os.environ.get('HOME')
        os.environ['HOME'] = home
        prefs = props.get_prefs()
        keep = (prefs.server_jar, prefs.client_jar)
        try:
            bpy.ops.mcgen.detect_jars('EXEC_DEFAULT')
            self.assertTrue(prefs.server_jar.endswith('server.jar') and prefs.client_jar.endswith('99.4.jar'), (prefs.server_jar, prefs.client_jar))
        finally:
            os.environ['HOME'] = old
            prefs.server_jar, prefs.client_jar = keep


class T05_W4Adapter(unittest.TestCase):
    """Адаптер SceneBuilder (W4) на фиктивном модуле render.scene: проверка контракта вызовов."""

    def setUp(self):
        reset_scene()
        S().size_x = S().size_z = 2
        os.environ['MCGEN_BACKEND'] = BACKEND
        backend.release_all()

    def _install_fake(self, with_iter):
        import types
        calls = {}
        mod = types.ModuleType(EXT + '.render.scene')

        class ViewSettings:                    # как render.settings.ViewSettings потока W4: ViewSettings(dict), неизвестные ключи пропускаются
            _D = dict(assets_dir='', pack_dir='', cache_dir='', version='', chunks_per_object=1, pixel_style=True, collection='x', merge_flat=False, lod=False)

            def __init__(self, src=None, **kw):
                self.__dict__.update(self._D)
                for k in self._D:
                    if src is not None and k in src:
                        setattr(self, k, src[k])

        class SceneBuilder:
            def __init__(self, vs):
                calls['vs'] = vs
                self.vs = vs

            def build(self, blocks, biomes, info, block_names, biome_names=None, progress=None):
                calls['build'] = (len(blocks), next(iter(blocks.values())).shape, info.height, len(block_names), len(biome_names))
                if progress:
                    progress(1.0, 'done')

            def clear(self):
                calls['clear'] = True
            if with_iter:
                def build_iter(self, blocks, biomes, info, block_names, biome_names=None):
                    n = 0
                    for c in blocks:
                        n += 1
                        yield n / len(blocks)
                    calls['iter_done'] = n
        mod.ViewSettings, mod.SceneBuilder = ViewSettings, SceneBuilder
        sys_mod = sys.modules
        sys_mod[EXT + '.render.scene'] = mod
        render_pkg = sys_mod.get(EXT + '.render')
        self._saved = (render_pkg,)
        return calls

    def tearDown(self):
        sys.modules.pop(EXT + '.render.scene', None)

    def test_blocking_contract(self):
        calls = self._install_fake(False)
        self.assertTrue(scene_iface.w4_available())
        prefs = props.get_prefs()
        prefs.sink = 'AUTO'
        # ресурсы клиента должны быть «готовы»: подставляем готовый каталог из кэша, если есть, иначе пропускаем
        res = pack.resolve('26.3')
        if not res['assets_ok']:
            self.skipTest('нет подготовленных ресурсов 26.3 в кэше')
        run_job('generate')
        self.assertIn('build', calls)
        n, shape, height, nb, nbi = calls['build']
        self.assertEqual((n, shape, height), (4, (384 * 256,), 384))
        self.assertGreater(nb, 5)
        vs = calls['vs']
        self.assertEqual((vs.version, vs.chunks_per_object, vs.collection), ('26.3', 1, 'MC World'))
        self.assertEqual(S().stats.sink, 'render.scene')

    def test_incremental_contract(self):
        calls = self._install_fake(True)
        res = pack.resolve('26.3')
        if not res['assets_ok']:
            self.skipTest('нет подготовленных ресурсов 26.3 в кэше')
        props.get_prefs().sink = 'AUTO'
        run_job('generate')
        self.assertEqual(calls.get('iter_done'), 4)
        self.assertNotIn('build', calls)


class T06_Presets(unittest.TestCase):
    def setUp(self):
        reset_scene()

    def test_shipped_presets_apply(self):
        d = os.path.join(paths_mod.addon_dir(), 'presets', 'mcgen')
        files = sorted(f for f in os.listdir(d) if f.endswith('.py'))
        self.assertGreaterEqual(len(files), 6)
        for f in files:
            reset_scene()
            bpy.ops.script.execute_preset(filepath=os.path.join(d, f), menu_idname='MCGEN_PT_presets')
        reset_scene()
        bpy.ops.script.execute_preset(filepath=os.path.join(d, 'Tall Mountains (tweaked).py'), menu_idname='MCGEN_PT_presets')
        s = S()
        self.assertEqual((s.use_tweaks, s.size_x), (True, 12))
        self.assertAlmostEqual(s.tweaks.terrain_amplitude, 1.8, places=5)
        reset_scene()
        bpy.ops.script.execute_preset(filepath=os.path.join(d, 'Split Seeds Demo.py'), menu_idname='MCGEN_PT_presets')
        self.assertEqual((s.seed_mode, s.seed_climate, s.seed_features), ('SPLIT', 'hello', 'world'))
        reset_scene()
        bpy.ops.script.execute_preset(filepath=os.path.join(d, 'The End 12x12.py'), menu_idname='MCGEN_PT_presets')
        self.assertEqual((s.dimension, s.size_x, s.y_max), ('minecraft:the_end', 12, 255))

    def test_save_load_delete_user_preset(self):
        s = S()
        s.seed = 'my seed'
        s.size_x = 21
        s.use_tweaks = True
        s.tweaks.cave_size = 1.75
        bpy.ops.mcgen.preset_add(name='unit test preset')
        user_dir = bpy.utils.user_resource('SCRIPTS', path='presets/mcgen')
        path = os.path.join(user_dir, 'unit_test_preset.py')
        self.assertTrue(os.path.isfile(path), os.listdir(user_dir) if os.path.isdir(user_dir) else user_dir)
        text = open(path).read()
        self.assertIn("s.seed = 'my seed'", text)
        self.assertIn('s.tweaks.cave_size = 1.75', text)
        self.assertNotIn('server_jar', text)                           # пути к jar в пресеты не попадают
        s.seed, s.size_x, s.tweaks.cave_size = '1', 3, 1.0
        bpy.ops.script.execute_preset(filepath=path, menu_idname='MCGEN_PT_presets')
        self.assertEqual((s.seed, s.size_x), ('my seed', 21))
        self.assertAlmostEqual(s.tweaks.cave_size, 1.75, places=5)
        bpy.ops.mcgen.preset_add(name='unit test preset', remove_name=True)
        self.assertFalse(os.path.isfile(path))

    def test_preset_fields_exist(self):
        for v in props.preset_values():
            obj = S()
            for part in v.split('.')[1:-1]:
                obj = getattr(obj, part)
            self.assertTrue(hasattr(obj, v.split('.')[-1]), v)


class T07_Translations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        v = bpy.context.preferences.view
        cls.keep = (v.language, v.use_translate_interface, v.use_translate_tooltips, v.use_translate_new_dataname)
        v.language = 'ru_RU'
        v.use_translate_interface = True
        v.use_translate_tooltips = True

    @classmethod
    def tearDownClass(cls):
        v = bpy.context.preferences.view
        v.language, v.use_translate_interface, v.use_translate_tooltips, v.use_translate_new_dataname = cls.keep

    def test_samples(self):
        tr = bpy.app.translations.pgettext_iface
        self.assertEqual(tr('Seeds'), 'Сиды')
        self.assertEqual(tr('Generate', 'Operator'), 'Сгенерировать')
        self.assertEqual(tr('Update Layers', 'Operator'), 'Обновить слои')
        self.assertEqual(tr('Chunks: {n}').format(n=3), 'Чанков: 3')

    def test_all_ui_strings_translated(self):
        strings = extract_strings.all_strings()
        missing = [s for s in strings if ('*', s) not in translations.build()['ru_RU'] and s not in ('X', 'Z', 'Java', 'libmcgen', 'UVMap')]
        self.assertFalse(missing, 'нет перевода: ' + '; '.join(missing[:20]))
        # перевод реально действует в интерфейсе
        tr = bpy.app.translations.pgettext_iface
        same_in_ru = {k for k, v in translations.RU.items() if k == v}                      # одинаково звучащие термины (X, Z, Java …)
        not_applied = [s for s in strings if tr(s) == s and s not in same_in_ru and s not in ('X', 'Z', 'Java', 'libmcgen', 'UVMap', '1 × 1', '2 × 2', '4 × 4', '8 × 8')]
        self.assertFalse(not_applied, 'перевод не применяется: ' + '; '.join(not_applied[:20]))

    def test_exception_messages_are_translated(self):
        i18n = importlib.import_module(EXT + '.ui.i18n')
        e = pack.PackError('The jar versions differ: server {server}, client {client}', server='1', client='2')
        self.assertEqual(str(e), 'The jar versions differ: server 1, client 2')
        self.assertEqual(i18n.exc_text(e), 'Версии jar не совпадают: сервер 1, клиент 2')
        e = pack.NeedJava(pack._NEED_JAVA, major=25)
        self.assertIn('Не найдена Java 25+', i18n.exc_text(e))
        lib = importlib.import_module(EXT + '.core.lib')
        e = lib.McError(lib.MCGEN_E_IO, 'libmcgen was not found; searched: {places}', places='a; b')
        self.assertEqual(i18n.exc_text(e), 'libmcgen не найдена; искали: a; b (E_IO)')
        self.assertEqual(i18n.progress_text('Building scene 42%'), 'Построение сцены 42%')
        self.assertEqual(i18n.rpt('Removed {n} objects', n=3), 'Удалено объектов: 3')

    def test_tweak_texts_translated(self):
        tr = bpy.app.translations.pgettext_iface
        for t in TWEAKS:
            self.assertEqual(tr(t['label']), t['label_ru'])
            self.assertEqual(tr(t['description']), t['description_ru'])
        for g in params_mod.tweaks_doc()['groups']:
            self.assertEqual(tr(g['label']), g['label_ru'])

    def test_dynamic_enum_labels(self):
        tr = bpy.app.translations.pgettext_iface
        for d in catalog.DEFAULT_DIMENSIONS:
            self.assertNotEqual(tr(catalog.pretty_dim(d)), catalog.pretty_dim(d), d)
        for p in ('normal', 'large_biomes', 'amplified'):
            self.assertNotEqual(tr(catalog.pretty_preset(p)), catalog.pretty_preset(p), p)

    def test_no_cyrillic_in_english_source(self):
        for s in extract_strings.all_strings():
            self.assertFalse(any('а' <= c.lower() <= 'я' for c in s), s)


class T07b_BlendFile(unittest.TestCase):
    """Настройки и построенная сцена сохраняются в .blend; воксельные данные — нет (воспроизводятся по настройкам)."""

    def test_roundtrip_and_update_after_reload(self):
        reset_scene()
        s = S()
        s.seed_mode = 'SPLIT'
        s.seed_climate, s.seed_terrain, s.seed_structures, s.seed_features = 'blend-test', '5', '6', '7'
        s.size_x = s.size_z = 3
        s.origin_x, s.origin_z = -240, 240
        s.use_tweaks = True
        s.tweaks.cave_size = 1.5
        s.tweaks.sea_level_offset = 4
        s.bm_step = '8'
        os.environ['MCGEN_BACKEND'] = BACKEND
        backend.release_all()
        bpy.ops.mcgen.generate('EXEC_DEFAULT')
        bpy.ops.mcgen.biome_map('EXEC_DEFAULT')
        n_obj = len([o for o in bpy.data.objects if o.get('mcgen_preview')])
        self.assertEqual(n_obj, 9)
        path = os.path.join(tempfile.mkdtemp(prefix='mcgen-blend-'), 'roundtrip.blend')
        bpy.ops.wm.save_as_mainfile(filepath=path)
        self.assertTrue(jobs.session(bpy.context.scene.name).region is not None)
        bpy.ops.wm.open_mainfile(filepath=path)
        s = S()
        self.assertEqual((s.seed_mode, s.seed_climate, s.seed_terrain, s.seed_structures, s.seed_features), ('SPLIT', 'blend-test', '5', '6', '7'))
        self.assertEqual((s.size_x, s.origin_x, s.origin_z, s.use_tweaks, s.bm_step), (3, -240, 240, True, '8'))
        self.assertAlmostEqual(s.tweaks.cave_size, 1.5, places=5)
        self.assertEqual(s.tweaks.sea_level_offset, 4)
        self.assertTrue(s.stats.has_data)                                     # статистика сохранена
        self.assertEqual(len([o for o in bpy.data.objects if o.get('mcgen_preview')]), 9)    # меши сохранены
        img = bpy.data.images['MCGen Biomes overworld']
        self.assertIsNotNone(img.packed_file)
        self.assertEqual(tuple(img.size), (6, 6))
        # воксельных данных после загрузки нет -> Update Layers пересоздаёт всё
        self.assertIsNone(jobs.session(bpy.context.scene.name).region)
        bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
        self.assertIsNotNone(jobs.session(bpy.context.scene.name).region)
        self.assertEqual(S().stats.mode, 'update')
        self.assertEqual(S().stats.objects, 9)


class T08_Unregister(unittest.TestCase):
    def test_unregister_clean_and_reregister(self):
        addon_utils.disable(EXT, default_set=False)
        left = [c for c in dir(bpy.types) if c.startswith('MCGEN_')]
        self.assertEqual(left, [])
        self.assertFalse(hasattr(bpy.types.Scene, 'mcgen'))
        self.assertEqual([o for o in dir(bpy.ops.mcgen)] if hasattr(bpy.ops, 'mcgen') else [], [])
        self.assertEqual(ops._active, [])
        # повторное включение и ещё один цикл
        for _ in range(2):
            addon_utils.enable(EXT, default_set=True, persistent=False)
            self.assertTrue(hasattr(bpy.types.Scene, 'mcgen'))
            self.assertEqual(S().version, '26.3')
            addon_utils.disable(EXT, default_set=False)
            self.assertEqual([c for c in dir(bpy.types) if c.startswith('MCGEN_')], [])
        addon_utils.enable(EXT, default_set=True, persistent=False)


def main():
    out = None
    if '--' in sys.argv:
        a = sys.argv[sys.argv.index('--') + 1:]
        if '--json' in a:
            out = a[a.index('--json') + 1]
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for cls in (T01_Registration, T02_Properties, T03_Panels, T04_Operators, T05_W4Adapter, T06_Presets, T07_Translations, T07b_BlendFile, T08_Unregister):
        suite.addTests(loader.loadTestsFromTestCase(cls))
    t0 = time.time()
    res = unittest.TextTestRunner(verbosity=2).run(suite)
    summary = {'blender': bpy.app.version_string, 'python': sys.version.split()[0], 'backend': BACKEND, 'ext': EXT, 'run': res.testsRun,
               'failures': len(res.failures), 'errors': len(res.errors), 'skipped': len(res.skipped), 'seconds': round(time.time() - t0, 1),
               'failed': [str(t) for t, _ in res.failures + res.errors]}
    print('SUMMARY ' + json.dumps(summary, ensure_ascii=False))
    if out:
        json.dump(summary, open(out, 'w'), indent=1, ensure_ascii=False)
    sys.stdout.flush()
    os._exit(0 if res.wasSuccessful() else 1)


main()

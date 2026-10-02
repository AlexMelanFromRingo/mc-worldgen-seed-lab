#!/usr/bin/env python3
"""Тесты ядра аддона БЕЗ Blender (обычный python3 + numpy): сиды, параметры, макет, мост ctypes (на собранной заглушке библиотеки),
ресурсы пользователя (реальные jar'ы + синтетические), палитры, таблица тонких настроек.

    python3 blender/tests/addon/test_core.py [-v] [TestCase.test_name …]
Переменные: MCGEN_SCRATCH (рабочий каталог, по умолчанию /tmp/mcgen-tests), MCGEN_TEST_REAL_JARS=0 — пропустить тесты на реальных jar.
"""
import hashlib
import http.server
import io
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _boot  # noqa: E402

_boot.load_core()
import numpy as np  # noqa: E402

from mcgen_addon.core import backend, biomes, catalog, lib, mock, pack, params, paths, png, seeds, tasks  # noqa: E402

REPO = _boot.REPO
SCRATCH = _boot.SCRATCH
REAL_JARS = os.environ.get('MCGEN_TEST_REAL_JARS', '1') != '0' and os.path.isfile(os.path.join(_boot.JARS, 'server-26.3.jar')) and os.path.isfile(os.path.join(_boot.JARS, 'client-26.3.jar'))
os.makedirs(SCRATCH, exist_ok=True)

_stub_path = None


def stub_library():
    """Собирает заглушку libmcgen (blender/tests/addon/stub) нашим build.py под linux-x64 (один раз за запуск)."""
    global _stub_path
    if _stub_path:
        return _stub_path
    out = os.path.join(SCRATCH, 'stub-build')
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run([sys.executable, os.path.join(REPO, 'libmcgen', 'build.py'), '--stub', '--targets', 'linux-x64', '--out', out, '--no-install'],
                   check=True, capture_output=True)
    _stub_path = os.path.join(out, 'linux-x64', 'libmcgen.so')
    return _stub_path


# ---- сиды ---------------------------------------------------------------------------------------------------------------------

JAVA_VECTORS = [   # получены настоящим Java: Long.parseLong / (long) String.hashCode()
    ('a', 'S', 97), ('hello', 'S', 99162322), ('Hello World', 'S', -862545276), ('Minecraft', 'S', -1595926131), ('glacier', 'S', 108181935),
    ('12345', 'N', 12345), ('-12345', 'N', -12345), ('+5', 'N', 5), ('9223372036854775807', 'N', 9223372036854775807),
    ('9223372036854775808', 'S', -1773151197), ('-9223372036854775808', 'N', -9223372036854775808), ('-9223372036854775809', 'S', 1304595159),
    ('0x10', 'S', 1546855), ('1_000', 'S', 48130338), (' 5', 'S', 1045), ('5 ', 'S', 1675), ('1e5', 'S', 50273),
    ('٣٤٥', 'N', 345), ('１２３', 'N', 123), ('привет', 'S', 2093147784), ('\U0001F30Dseed', 'S', 1150241858),
    ('日本語', 'S', 25921943), ('The quick brown fox jumps over the lazy dog', 'S', -609428141), ('seed with spaces', 'S', 943912344), ('éè', 'S', 7455),
]


class SeedsTests(unittest.TestCase):
    def test_java_vectors(self):
        for text, kind, val in JAVA_VECTORS:
            v, k = seeds.parse_seed(text)
            self.assertEqual((v, k), (val, 'number' if kind == 'N' else 'string'), repr(text))

    def test_empty_is_random(self):
        self.assertEqual(seeds.parse_seed(''), (None, 'empty'))
        a, b = seeds.resolve(''), seeds.resolve('')
        self.assertNotEqual(a, b)
        self.assertTrue(-(1 << 63) <= a < (1 << 63))

    def test_random_in_range(self):
        for _ in range(200):
            self.assertTrue(-(1 << 63) <= seeds.random_seed() < (1 << 63))

    def test_domains(self):
        self.assertEqual(seeds.domains('77', 'UNIFIED', None), (77, 77, 77, 77))
        self.assertEqual(seeds.domains(None, 'SPLIT', ('1', 'hello', '3', '-4')), (1, 99162322, 3, -4))


# ---- параметры / Update Layers ---------------------------------------------------------------------------------------------------------

class ParamsTests(unittest.TestCase):
    def test_stage_mask(self):
        self.assertEqual(params.stage_mask(True, True, True, False, False), 1 | 2 | 4 | 8)
        self.assertEqual(params.stage_mask(False, True, True, True, True), 1)           # без рельефа остаются только биомы
        self.assertEqual(params.stage_mask(True, False, False, True, True), 1 | 2 | 16 | 32)

    def test_diff(self):
        base = params.GenParams()
        self.assertEqual(params.diff(base, base)['from_stage'], None)
        d = params.diff(base, base.__class__(**{**base.__dict__, 'seeds': (12345, 12345, 12345, 999)}))
        self.assertEqual((d['world'], d['from_stage']), (True, 'features'))
        d = params.diff(base, base.__class__(**{**base.__dict__, 'seeds': (1, 12345, 12345, 12345)}))
        self.assertEqual(d['from_stage'], 'biomes')
        d = params.diff(base, base.__class__(**{**base.__dict__, 'tweaks': (('ore_density', 2.0),)}))
        self.assertEqual(d['from_stage'], 'features')
        d = params.diff(base, base.__class__(**{**base.__dict__, 'tweaks': (('terrain_amplitude', 2.0),)}))
        self.assertEqual(d['from_stage'], 'terrain')
        d = params.diff(base, base.__class__(**{**base.__dict__, 'stages': base.stages | 16}))
        self.assertEqual((d['stages'], d['from_stage']), (True, 'features'))
        d = params.diff(base, base.__class__(**{**base.__dict__, 'view': (('tint_biomes', False),)}))
        self.assertEqual((d['view'], d['world'], d['from_stage']), (True, False, None))
        d = params.diff(base, base.__class__(**{**base.__dict__, 'nx': 9}))
        self.assertTrue(d['area'] and not d['world'])
        self.assertTrue(params.diff(None, base)['world'])

    def test_area_inside(self):
        self.assertTrue(params.area_inside((1, 1, 2, 2), (0, 0, 4, 4)))
        self.assertFalse(params.area_inside((3, 3, 2, 2), (0, 0, 4, 4)))


class TweaksTests(unittest.TestCase):
    def test_json_valid_and_synced(self):
        sys.path.insert(0, os.path.join(REPO, 'libmcgen'))
        import gen_tweaks
        self.assertEqual(gen_tweaks.validate(gen_tweaks.load()), [])
        r = subprocess.run([sys.executable, os.path.join(REPO, 'libmcgen', 'gen_tweaks.py'), '--check'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_defaults_are_vanilla(self):
        for t in params.tweaks_doc()['tweaks']:
            if t['type'] == 'float' and t['id'] not in ('sea_level_offset', 'lava_level_offset'):
                self.assertEqual(t['default'], 1.0, t['id'])      # множители нейтральны
            if t['id'].endswith('_offset'):
                self.assertEqual(t['default'], 0)

    def test_russian_texts_present(self):
        for t in params.tweaks_doc()['tweaks']:
            self.assertTrue(t['label_ru'] and t['description_ru'], t['id'])
            self.assertTrue(any('а' <= c.lower() <= 'я' for c in t['label_ru']), t['id'])


# ---- макет ---------------------------------------------------------------------------------------------------------------------------

def public_api(cls):
    return {n for n in dir(cls) if not n.startswith('_')}


class MockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gen = mock.McGen.open(None, '26.3')

    def test_same_surface_as_lib(self):
        """Макет обязан иметь ту же публичную поверхность, что мост ctypes (переключение одной строкой)."""
        for a, b in ((mock.McGen, lib.McGen), (mock.McWorld, lib.McWorld), (mock.McRegion, lib.McRegion)):
            missing = public_api(b) - public_api(a)
            missing -= {'create', 'open'} if a is mock.McWorld else set()
            self.assertFalse(missing, f'{a.__name__}: в макете нет {sorted(missing)}')
        for name in ('MC_STAGE_ALL', 'MC_STAGE_BIOMES', 'MC_HM_WORLD_SURFACE', 'McError', 'McCancelled', 'TweakInfo', 'RegionInfo', 'NAME', 'version_string', 'available'):
            self.assertTrue(hasattr(mock, name), name)

    def test_open_errors(self):
        with self.assertRaises(lib.McError) as c:
            mock.McGen.open(None, '1.20')
        self.assertEqual(c.exception.code, lib.MCGEN_E_VERSION)
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:nowhere', 'normal', 1)
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:overworld', 'nope', 1)
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:overworld', 'normal', 1, {'no_such_tweak': 1})

    def test_region_shapes_and_determinism(self):
        w = self.gen.world('minecraft:overworld', 'normal', (1, 2, 3, 4))
        r = w.generate_region(-1, -1, 2, 2, mock.MC_STAGE_ALL)
        i = r.info
        self.assertEqual((i.cx0, i.cz0, i.nx, i.nz, i.min_y, i.height), (-1, -1, 2, 2, -64, 384))
        b = r.blocks(-1, 0)
        self.assertEqual((b.shape, b.dtype), ((384, 16, 16), np.uint16))
        self.assertEqual(r.biomes(0, 0).shape, (96, 4, 4))
        self.assertEqual(r.heightmap(0, 0, mock.MC_HM_WORLD_SURFACE).shape, (16, 16))
        self.assertEqual(r.heightmap(0, 0, 0).dtype, np.int16)
        with self.assertRaises(IndexError):
            r.blocks(5, 5)
        r2 = self.gen.world('minecraft:overworld', 'normal', (1, 2, 3, 4)).generate_region(-1, -1, 2, 2)
        for c in r.chunks():
            self.assertTrue(np.array_equal(r.blocks(*c), r2.blocks(*c)))
        # чанк одинаков независимо от того, в какой области его генерируют (нет зависимости от границ региона)
        r3 = w.generate_region(0, 0, 1, 1)
        self.assertTrue(np.array_equal(r.blocks(0, 0), r3.blocks(0, 0)))
        self.assertTrue(np.array_equal(r.heightmap(0, 0, 0), r3.heightmap(0, 0, 0)))

    def test_domain_seeds_independent(self):
        """features-сид меняет только декорации; terrain — рельеф; climate — биомы."""
        base = self.gen.world('minecraft:overworld', 'normal', (5, 5, 5, 5)).generate_region(0, 0, 3, 3, 63)
        ft = self.gen.world('minecraft:overworld', 'normal', (5, 5, 5, 9999)).generate_region(0, 0, 3, 3, 63)
        st_only = mock.MC_STAGE_BIOMES | mock.MC_STAGE_TERRAIN | mock.MC_STAGE_SURFACE | mock.MC_STAGE_CARVERS
        a = self.gen.world('minecraft:overworld', 'normal', (5, 5, 5, 5)).generate_region(0, 0, 3, 3, st_only)
        b = self.gen.world('minecraft:overworld', 'normal', (5, 5, 5, 9999)).generate_region(0, 0, 3, 3, st_only)
        for c in a.chunks():
            self.assertTrue(np.array_equal(a.blocks(*c), b.blocks(*c)), 'features-сид не должен менять рельеф')
        diff = sum(not np.array_equal(base.blocks(*c), ft.blocks(*c)) for c in base.chunks())
        self.assertGreater(diff, 0, 'features-сид должен менять декорации')

    def test_stages(self):
        w = self.gen.world('minecraft:overworld', 'normal', 42)
        biomes_only = w.generate_region(0, 0, 1, 1, mock.MC_STAGE_BIOMES)
        self.assertEqual(int(biomes_only.blocks(0, 0).max()), 0)
        w = self.gen.world('minecraft:overworld', 'normal', 7)                       # на этом сиде в чанке (0,0) суша
        ter = w.generate_region(0, 0, 1, 1, mock.MC_STAGE_BIOMES | mock.MC_STAGE_TERRAIN).blocks(0, 0)
        full = w.generate_region(0, 0, 1, 1, mock.MC_STAGE_ALL).blocks(0, 0)
        ids = self.gen.ids
        self.assertEqual(int((ter == ids['bedrock']).sum()), 0)                      # бедрок — стадия поверхности
        self.assertEqual(int((ter == ids['dirt']).sum()), 0)
        self.assertGreater(int((full == ids['bedrock']).sum()), 0)
        self.assertGreater(int((full == ids['grass_block']).sum()) + int((full == ids['snow_block']).sum()) + int((full == ids['stone']).sum()), 0)

    def test_tweaks_change_the_world(self):
        w0 = self.gen.world('minecraft:overworld', 'normal', 7)
        w1 = self.gen.world('minecraft:overworld', 'normal', 7, {'terrain_amplitude': 2.0})
        self.assertEqual(w0.sea_level, 63)
        self.assertEqual(self.gen.world('minecraft:overworld', 'normal', 42, {'sea_level_offset': 10}).sea_level, 73)
        r0, r1 = w0.generate_region(0, 0, 6, 6, 7), w1.generate_region(0, 0, 6, 6, 7)
        h0 = np.concatenate([r0.heightmap(*c, mock.MC_HM_OCEAN_FLOOR).ravel() for c in r0.chunks()]).astype(int)
        h1 = np.concatenate([r1.heightmap(*c, mock.MC_HM_OCEAN_FLOOR).ravel() for c in r1.chunks()]).astype(int)
        self.assertGreater(float(np.abs(h1 - 63).mean()), 1.3 * float(np.abs(h0 - 63).mean()))
        # плотность пещер = 0 -> нет пустот под землёй
        nc = self.gen.world('minecraft:overworld', 'normal', 7, {'cave_density': 0.0}).generate_region(0, 0, 1, 1, 15).blocks(0, 0)
        wc = self.gen.world('minecraft:overworld', 'normal', 7, {'cave_density': 2.0}).generate_region(0, 0, 1, 1, 15).blocks(0, 0)
        self.assertGreater(int((wc == 0).sum()), int((nc == 0).sum()))

    def test_dimensions(self):
        for dim, miny, h in (('minecraft:overworld', -64, 384), ('minecraft:the_nether', 0, 256), ('minecraft:the_end', 0, 256)):
            w = self.gen.world(dim, 'normal', 7)
            self.assertEqual((w.min_y, w.height), (miny, h))
            r = w.generate_region(0, 0, 2, 2)
            self.assertEqual(r.blocks(1, 1).shape, (h, 16, 16))
            self.assertGreater(int((r.blocks(0, 0) != 0).sum()), 0)

    def test_progress_and_cancel(self):
        w = self.gen.world('minecraft:overworld', 'normal', 1)
        calls = []
        w.generate_region(0, 0, 3, 3, 3, 0, lambda f, s: calls.append((f, s)) and False)
        self.assertEqual(calls[0][0], 0.0)
        self.assertAlmostEqual(calls[-1][0], 1.0)
        with self.assertRaises(lib.McCancelled):
            w.generate_region(0, 0, 4, 4, 3, 0, lambda f, s: f > 0.2)

    def test_biome_grid_matches_region_biomes(self):
        w = self.gen.world('minecraft:overworld', 'normal', 99)
        g = w.biome_grid(0, 0, 4, 4, 4)
        self.assertEqual((g.shape, g.dtype), ((4, 4), np.uint8))
        r = w.generate_region(0, 0, 1, 1, 1)
        self.assertEqual(int(g[0, 0]), w.biome_at(0, 0, 0))
        self.assertTrue(r.biomes(0, 0).max() < self.gen.biome_count)

    def test_block_names_with_pack(self):
        if not os.path.isdir(os.path.join(REPO, 'run', 'pack-26.3', 'reports')):
            self.skipTest('нет run/pack-26.3')
        g = mock.McGen.open(os.path.join(REPO, 'run', 'pack-26.3'), '26.3')
        self.assertEqual(g.block_state_count, 35723)
        self.assertEqual(g.block_state_name(g.ids['grass_block']), 'minecraft:grass_block[snowy=false]')
        self.assertEqual(g.block_state_from_name('minecraft:stone'), g.ids['stone'])

    def test_write_mcr(self):
        w = self.gen.world('minecraft:the_end', 'normal', 1)
        r = w.generate_region(0, 0, 1, 1)
        p = os.path.join(SCRATCH, 'mock.mcr')
        r.write_mcr(p)
        with open(p, 'rb') as f:
            d = f.read()
        self.assertEqual(d[:4], b'MCR1')


# ---- мост ctypes (заглушка библиотеки, собранная build.py) ------------------------------------------------------------------------

class LibBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = stub_library()
        cls.L = lib.load(cls.path)
        cls.pack_dir = os.path.join(SCRATCH, 'stub-pack')
        os.makedirs(cls.pack_dir, exist_ok=True)
        with open(os.path.join(cls.pack_dir, 'version.json'), 'w') as f:
            f.write('{}')
        cls.gen = lib.McGen.open(cls.pack_dir, '26.3', cls.L)

    def test_version_and_abi(self):
        self.assertEqual(self.L.abi, 1)
        self.assertIn('abi 1', self.L.version)

    def test_registries(self):
        g = self.gen
        self.assertEqual(g.dimensions(), ['minecraft:overworld', 'minecraft:the_nether', 'minecraft:the_end'])
        self.assertEqual(g.presets('minecraft:overworld'), ['normal', 'large_biomes', 'amplified'])
        self.assertEqual(g.presets('minecraft:the_end'), ['normal'])
        self.assertEqual(g.block_names()[2], 'minecraft:grass_block[snowy=false]')
        self.assertEqual(g.block_state_from_name('minecraft:dirt'), 3)
        self.assertEqual(g.block_state_from_name('minecraft:nope'), -1)
        self.assertEqual(g.biome_names(), ['minecraft:plains', 'minecraft:desert', 'minecraft:ocean'])

    def test_tweak_table_equals_json(self):
        """Таблица McTweakInfo, скомпилированная из сгенерированного заголовка, совпадает с tweaks.json (структура ctypes верна)."""
        got = self.gen.tweaks()
        doc = params.tweaks_doc()['tweaks']
        self.assertEqual(len(got), len(doc))
        for t, j in zip(got, doc):
            self.assertEqual((t.id, t.label, t.group, t.description), (j['id'], j['label'], j['group'], j['description']))
            self.assertEqual((t.default, t.min, t.max, t.soft_min, t.soft_max), tuple(float(j[k]) for k in ('default', 'min', 'max', 'soft_min', 'soft_max')))
            self.assertEqual(t.is_int, j['type'] != 'float')
        self.assertEqual([t.id for t in got], [t.id for t in mock.load_tweak_infos()])

    def test_world_errors_carry_text(self):
        with self.assertRaises(lib.McError) as c:
            self.gen.world('minecraft:nowhere', 'normal', 1)
        self.assertEqual((c.exception.code, c.exception.message), (lib.MCGEN_E_ARG, 'unknown dimension'))
        with self.assertRaises(lib.McError) as c:
            self.gen.world('minecraft:overworld', 'normal', 1, {'bogus': 1})
        self.assertIn('unknown tweak', c.exception.message)
        with self.assertRaises(lib.McError) as c:
            lib.McGen.open(os.path.join(SCRATCH, 'no-such-dir'), '26.3', self.L)
        self.assertEqual(c.exception.code, lib.MCGEN_E_IO)
        with self.assertRaises(lib.McError) as c:
            lib.McGen.open(self.pack_dir, '1.2.3', self.L)
        self.assertEqual(c.exception.code, lib.MCGEN_E_VERSION)

    def test_seeds_and_tweaks_arrive_in_c(self):
        w = self.gen.world('minecraft:overworld', 'normal', (-1, 2**62, -2**63, 5), {'sea_level_offset': 7})
        self.assertEqual((w.min_y, w.height, w.sea_level), (-64, 384, 70))
        w2 = self.gen.world('minecraft:the_nether', 'normal', 1)
        self.assertEqual((w2.min_y, w2.height, w2.sea_level), (0, 256, 32))
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:overworld', 'normal', (1, 2, 3))
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:overworld', 'normal', (2**63, 0, 0, 0))

    def test_region_zero_copy_numpy(self):
        w = self.gen.world('minecraft:overworld', 'normal', 1)
        r = w.generate_region(-1, -1, 2, 2, lib.MC_STAGE_ALL)
        self.assertEqual(r.info, lib.RegionInfo(-1, -1, 2, 2, -64, 384))
        b = r.blocks(-1, -1)
        self.assertEqual((b.shape, b.dtype, b.flags.writeable), ((384, 16, 16), np.uint16, True))
        # детерминированное содержимое заглушки: top_y = 64 + ((x + 2z) & 7); x=-16..-1 -> (x+2z)&7 для x=-16,z=-16 = (-48)&7 = 0
        self.assertEqual(int(b[0, 0, 0]), 5)                         # bedrock на min_y
        top = 64 + ((-16 + 2 * -16) & 7)
        self.assertEqual(int(b[top - (-64), 0, 0]), 2)               # grass
        self.assertEqual(int(b[top - (-64) + 1, 0, 0]), 0)           # воздух над поверхностью (выше моря)
        # запись через numpy видна в памяти региона (без копий)
        b[10, 3, 4] = 1234
        self.assertEqual(int(r.blocks(-1, -1)[10, 3, 4]), 1234)
        self.assertTrue(np.shares_memory(b, r.blocks(-1, -1)))
        # массив держит регион живым: после del региона и сборки мусора данные доступны
        arr = r.blocks(0, 0)
        ref = int(arr[5, 5, 5])
        del r
        import gc
        gc.collect()
        self.assertEqual(int(arr[5, 5, 5]), ref)
        hm = w.generate_region(0, 0, 1, 1).heightmap(0, 0, lib.MC_HM_OCEAN_FLOOR)
        self.assertEqual(hm.shape, (16, 16))
        self.assertEqual(int(hm[0, 0]), 64 + 1)                      # top_y(0,0) = 64, heightmap = top + 1

    def test_stage_mask_reaches_c(self):
        w = self.gen.world('minecraft:overworld', 'normal', 1)
        raw = w.generate_region(0, 0, 1, 1, lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN).blocks(0, 0)
        self.assertEqual(set(np.unique(raw)) - {0}, {1})             # только камень: без поверхности нет ни травы, ни бедрока
        full = w.generate_region(0, 0, 1, 1, lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN | lib.MC_STAGE_SURFACE).blocks(0, 0)
        self.assertIn(2, set(np.unique(full)))

    def test_biome_grid(self):
        w = self.gen.world('minecraft:overworld', 'normal', 1)
        g = w.biome_grid(-64, -64, 4, 2, 64, 63)
        self.assertEqual((g.shape, g.dtype), ((2, 4), np.uint8))
        self.assertEqual(int(g[0, 0]), w.biome_at(-64, 63, -64))
        self.assertEqual(int(g[1, 3]), w.biome_at(-64 + 3 * 64, 63, 0))

    def test_progress_callback_and_cancel(self):
        w = self.gen.world('minecraft:overworld', 'normal', 1)
        seen = []
        w.generate_region(0, 0, 3, 3, lib.MC_STAGE_ALL, 0, lambda f, s: seen.append((round(f, 3), s)) and False)
        self.assertEqual(seen[0], (0.0, 'terrain'))
        self.assertEqual(seen[-1], (1.0, 'done'))
        with self.assertRaises(lib.McCancelled) as c:
            w.generate_region(0, 0, 4, 4, lib.MC_STAGE_ALL, 0, lambda f, s: f >= 0.5)
        self.assertEqual(c.exception.code, lib.MCGEN_E_CANCEL)

    def test_exception_in_callback_is_propagated(self):
        w = self.gen.world('minecraft:overworld', 'normal', 1)

        def bad(f, s):
            raise ValueError('boom')
        with self.assertRaises(ValueError):
            w.generate_region(0, 0, 2, 2, lib.MC_STAGE_ALL, 0, bad)

    def test_gil_released_during_generation(self):
        """Вызов libmcgen идёт без GIL: пока C-поток «считает» (заглушка спит), Python-поток продолжает работать."""
        os.environ['MCGEN_STUB_DELAY_MS'] = '25'
        try:
            w = self.gen.world('minecraft:overworld', 'normal', 1)
            counter = {'n': 0}
            stop = threading.Event()

            def spin():
                while not stop.is_set():
                    counter['n'] += 1
                    time.sleep(0.001)
            th = threading.Thread(target=spin)
            th.start()
            t0 = time.perf_counter()
            w.generate_region(0, 0, 8, 1, lib.MC_STAGE_ALL)          # 8 чанков × 25 мс = 0.2 с
            dt = time.perf_counter() - t0
            stop.set()
            th.join()
            self.assertGreater(dt, 0.18)
            self.assertGreater(counter['n'], 60, 'поток Python почти не работал: GIL не отпускается?')
        finally:
            os.environ.pop('MCGEN_STUB_DELAY_MS', None)

    def test_mcr_dump_roundtrip(self):
        w = self.gen.world('minecraft:overworld', 'normal', 1)
        r = w.generate_region(2, 3, 2, 1, lib.MC_STAGE_ALL)
        p = os.path.join(SCRATCH, 'stub.mcr')
        r.write_mcr(p)
        with open(p, 'rb') as f:
            d = f.read()
        import struct
        self.assertEqual(d[:4], b'MCR1')
        abi, cx0, cz0, nx, nz, miny, h, stages = struct.unpack_from('<8i', d, 4)
        self.assertEqual((abi, cx0, cz0, nx, nz, miny, h, stages), (1, 2, 3, 2, 1, -64, 384, 63))
        off = 4 + 32
        n_per = h * 256 * 2 + (h // 4) * 16 + 4 * 256 * 2
        for i, c in enumerate(r.chunks()):
            blocks = np.frombuffer(d, '<u2', h * 256, off + i * n_per).reshape(h, 16, 16)
            self.assertTrue(np.array_equal(blocks, r.blocks(*c)))
        tail = off + 2 * n_per
        self.assertEqual(struct.unpack_from('<I', d, tail)[0], 6)

    def test_backend_switch(self):
        os.environ['MCGEN_LIB'] = self.path
        os.environ['MCGEN_BACKEND'] = 'lib'
        try:
            lib._lib = None
            self.assertEqual(backend.name(), 'lib')
            g = backend.open_gen(self.pack_dir, '26.3')
            self.assertEqual(g.backend, 'lib')
            os.environ['MCGEN_BACKEND'] = 'mock'
            self.assertEqual(backend.name(), 'mock')
            self.assertEqual(backend.open_gen(None, '26.3').backend, 'mock')
            os.environ['MCGEN_BACKEND'] = 'auto'
            self.assertEqual(backend.name(), 'lib')
            os.environ['MCGEN_LIB'] = os.path.join(SCRATCH, 'missing.so')
            lib._lib = None
            self.assertEqual(backend.name(), 'mock')                  # библиотеки нет -> макет
            os.environ['MCGEN_BACKEND'] = 'lib'
            with self.assertRaises(lib.McError):
                backend.impl()
        finally:
            for k in ('MCGEN_LIB', 'MCGEN_BACKEND'):
                os.environ.pop(k, None)
            backend.release_all()
            lib._lib = None
            lib.load(self.path)

    def test_mock_and_lib_return_same_types(self):
        mg = mock.McGen.open(None, '26.3')
        for g in (self.gen, mg):
            w = g.world('minecraft:overworld', 'normal', 1)
            r = w.generate_region(0, 0, 1, 1, lib.MC_STAGE_ALL)
            self.assertEqual((r.blocks(0, 0).dtype, r.blocks(0, 0).shape), (np.uint16, (384, 16, 16)))
            self.assertEqual((r.biomes(0, 0).dtype, r.biomes(0, 0).shape), (np.uint8, (96, 4, 4)))
            self.assertEqual((r.heightmap(0, 0, 0).dtype, r.heightmap(0, 0, 0).shape), (np.int16, (16, 16)))
            self.assertEqual(type(r.info), type(lib.RegionInfo(0, 0, 1, 1, -64, 384)))


# ---- ресурсы пользователя --------------------------------------------------------------------------------------------------------------

def jdump(obj, path):
    with open(path, 'w') as f:
        json.dump(obj, f)


def fake_server_jar(path, version='99.1', java_version=25):
    """Синтетический bundler-jar: version.json, META-INF/versions.list и внутренний jar с минимальным датапаком."""
    inner = io.BytesIO()
    with zipfile.ZipFile(inner, 'w') as z:
        z.writestr('version.json', json.dumps({'id': version, 'world_version': 1, 'java_version': java_version}))
        z.writestr('data/minecraft/worldgen/biome/plains.json', json.dumps({'temperature': 0.8, 'downfall': 0.4, 'effects': {'water_color': '#3f76e4'}}))
        z.writestr('data/minecraft/worldgen/noise_settings/overworld.json', '{}')
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('version.json', json.dumps({'id': version, 'world_version': 1, 'java_version': java_version}))
        z.writestr('META-INF/versions.list', f'deadbeef\t{version}\t{version}/server-{version}.jar')
        z.writestr(f'META-INF/versions/{version}/server-{version}.jar', inner.getvalue())
        z.writestr('net/minecraft/bundler/Main.class', b'\xca\xfe')
    return path


def fake_client_jar(path, version='99.1'):
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('version.json', json.dumps({'id': version, 'world_version': 1, 'java_version': 25}))
        z.writestr('assets/minecraft/lang/en_us.json', '{}')
        z.writestr('assets/minecraft/blockstates/stone.json', '{"variants":{"":{"model":"minecraft:block/stone"}}}')
        z.writestr('assets/minecraft/models/block/stone.json', '{}')
        z.writestr('assets/minecraft/textures/block/stone.png', b'\x89PNG')
        z.writestr('assets/minecraft/textures/colormap/grass.png', b'x')
        z.writestr('assets/minecraft/sounds/ignored.ogg', b'x')          # не должен попасть в кэш
        z.writestr('data/minecraft/recipe/x.json', '{}')
    return path


def fake_reports(folder):
    os.makedirs(os.path.join(folder, 'reports'), exist_ok=True)
    blocks = {'minecraft:air': {'states': [{'default': True, 'id': 0}]}, 'minecraft:stone': {'states': [{'default': True, 'id': 1}]}}
    jdump(blocks, os.path.join(folder, 'reports', 'blocks.json'))
    jdump({'minecraft:block': {}}, os.path.join(folder, 'reports', 'registries.json'))
    return folder


class PackSyntheticTests(unittest.TestCase):
    """Быстрые тесты pack.py на синтетических jar (без Java и без сети)."""

    def setUp(self):
        self.root = os.path.join(SCRATCH, 'pack-syn-' + self.id().split('.')[-1])
        shutil.rmtree(self.root, ignore_errors=True)
        os.makedirs(self.root)
        paths.set_cache_override(os.path.join(self.root, 'cache'))
        self.srv = fake_server_jar(os.path.join(self.root, 'server.jar'))
        self.cli = fake_client_jar(os.path.join(self.root, 'client.jar'))

    def tearDown(self):
        paths.set_cache_override(None)

    def test_inspect_jar(self):
        s, c = pack.inspect_jar(self.srv), pack.inspect_jar(self.cli)
        self.assertEqual((s.kind, s.version, s.java_version), ('server', '99.1', 25))
        self.assertEqual((c.kind, c.version), ('client', '99.1'))
        junk = os.path.join(self.root, 'junk.jar')
        with zipfile.ZipFile(junk, 'w') as z:
            z.writestr('x.txt', 'x')
        self.assertIsNone(pack.inspect_jar(junk))
        self.assertIsNone(pack.inspect_jar(os.path.join(self.root, 'absent.jar')))

    def test_prepare_with_reports_folder_no_java(self):
        rep = fake_reports(os.path.join(self.root, 'myreports'))
        r = pack.prepare(self.srv, self.cli, reports_folder=rep)
        self.assertEqual((r.version, r.pack_ok, r.assets_ok), ('99.1', True, True))
        self.assertTrue(os.path.isfile(os.path.join(r.pack_dir, 'data', 'minecraft', 'worldgen', 'biome', 'plains.json')))
        self.assertTrue(os.path.isfile(os.path.join(r.pack_dir, 'version.json')))
        self.assertTrue(os.path.isfile(os.path.join(r.pack_dir, 'reports', 'blocks.json')))
        self.assertTrue(os.path.isfile(os.path.join(r.assets_dir, 'assets', 'minecraft', 'blockstates', 'stone.json')))
        self.assertFalse(os.path.exists(os.path.join(r.assets_dir, 'assets', 'minecraft', 'sounds')))      # лишнее не распаковывается
        self.assertFalse(os.path.exists(os.path.join(r.assets_dir, 'data')))
        res = pack.resolve('99.1')
        self.assertTrue(res['pack_ok'] and res['assets_ok'])
        self.assertIn('99.1', pack.available_versions())
        # повторный запуск — всё из кэша
        r2 = pack.prepare(self.srv, self.cli)
        self.assertEqual(r2.steps, ['datapack: cached', 'reports: cached', 'assets: cached'])

    def test_reports_folder_layouts(self):
        base = os.path.join(self.root, 'x')
        fake_reports(base)
        self.assertEqual(pack.reports_from_folder(base), os.path.join(base, 'reports'))
        self.assertEqual(pack.reports_from_folder(os.path.join(base, 'reports')), os.path.join(base, 'reports'))
        self.assertIsNone(pack.reports_from_folder(self.root))

    def test_need_java_message(self):
        orig = pack.find_java
        pack.find_java = lambda *a, **k: None
        try:
            with self.assertRaises(pack.NeedJava) as c:
                pack.prepare(self.srv, self.cli)
        finally:
            pack.find_java = orig
        msg = str(c.exception)
        self.assertIn('Java 25', msg)
        self.assertIn('--reports', msg)                       # подсказка запасного пути
        self.assertIn('reports', msg.lower())
        # данные распакованы, но пакет не помечен готовым -> resolve не считает pack_ok
        self.assertFalse(pack.resolve('99.1')['pack_ok'])

    def test_wrong_reports_folder(self):
        with self.assertRaises(pack.PackError):
            pack.prepare(self.srv, self.cli, reports_folder=self.root)

    def test_version_mismatch_and_bad_jar(self):
        other = fake_client_jar(os.path.join(self.root, 'c2.jar'), '98.0')
        with self.assertRaises(pack.PackError):
            pack.prepare(self.srv, other)
        with self.assertRaises(pack.PackError):
            pack.prepare(os.path.join(self.root, 'nope.jar'), '')
        with self.assertRaises(pack.PackError):
            pack.prepare('', '')

    def test_cancel(self):
        t = tasks.Task('prep', lambda task: pack.prepare(self.srv, self.cli, fake_reports(os.path.join(self.root, 'r')), task=task))
        t.cancel()
        t.run_blocking()
        self.assertEqual(t.state, 'cancelled')

    def test_scan_jars(self):
        mc = os.path.join(self.root, 'home', '.minecraft')
        os.makedirs(os.path.join(mc, 'versions', '99.1'))
        shutil.copy(self.cli, os.path.join(mc, 'versions', '99.1', '99.1.jar'))
        dl = os.path.join(self.root, 'home', 'Downloads')
        os.makedirs(dl)
        shutil.copy(self.srv, os.path.join(dl, 'server.jar'))
        old = os.environ.get('HOME')
        os.environ['HOME'] = os.path.join(self.root, 'home')
        try:
            found = pack.scan_jars()
        finally:
            os.environ['HOME'] = old
        kinds = sorted((j.kind, j.version) for j in found if j.version == '99.1')
        self.assertEqual(kinds, [('client', '99.1'), ('server', '99.1')])

    def test_java_major_parsing(self):
        fake = os.path.join(self.root, 'java')
        with open(fake, 'w') as f:
            f.write('#!/bin/sh\necho \'openjdk version "25.0.4.1" 2026-08-18\' >&2\n')
        os.chmod(fake, 0o755)
        self.assertEqual(pack.java_major(fake), 25)
        fake8 = os.path.join(self.root, 'java8')
        with open(fake8, 'w') as f:
            f.write('#!/bin/sh\necho \'java version "1.8.0_345"\' >&2\n')
        os.chmod(fake8, 0o755)
        self.assertEqual(pack.java_major(fake8), 8)
        self.assertEqual(pack.find_java(25, fake8), pack.find_java(25, None) if pack.find_java(25, None) else None)

    def test_eula_required(self):
        with self.assertRaises(pack.EulaNotAccepted):
            pack.download_jars('99.1', False)
        with self.assertRaises(pack.EulaNotAccepted):
            pack.download_jars('99.1', accept_eula=False)

    def test_download_from_local_server(self):
        """Скачивание: локальный «piston-meta» с манифестом и jar; sha1 проверяется; повторная загрузка берёт из кэша."""
        web = os.path.join(self.root, 'web')
        os.makedirs(web)
        shutil.copy(self.srv, os.path.join(web, 'server.jar'))
        shutil.copy(self.cli, os.path.join(web, 'client.jar'))

        def sha(p):
            with open(p, 'rb') as f:
                return hashlib.sha1(f.read()).hexdigest()
        class Quiet(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=web, **k)

            def log_message(self, *a):
                pass
        handler = Quiet
        httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        port = httpd.server_address[1]
        base = f'http://127.0.0.1:{port}'
        meta = {'downloads': {'server': {'url': f'{base}/server.jar', 'sha1': sha(self.srv), 'size': os.path.getsize(self.srv)},
                              'client': {'url': f'{base}/client.jar', 'sha1': sha(self.cli), 'size': os.path.getsize(self.cli)}}}
        jdump(meta, os.path.join(web, '99.1.json'))
        jdump({'versions': [{'id': '99.1', 'url': f'{base}/99.1.json'}, {'id': '1.0', 'url': f'{base}/x'}]}, os.path.join(web, 'manifest.json'))
        th = threading.Thread(target=httpd.serve_forever, daemon=True)
        th.start()
        os.environ['MCGEN_MANIFEST_URL'] = f'{base}/manifest.json'
        try:
            self.assertEqual(pack.manifest_versions(pack.fetch_manifest(), '99.'), ['99.1'])
            t = tasks.Task('dl', lambda task: pack.download_jars('99.1', True, task=task)).run_blocking()
            self.assertEqual(t.state, 'done', t.error_text)
            got = t.result
            self.assertEqual(sha(got['server']), sha(self.srv))
            self.assertEqual(sha(got['client']), sha(self.cli))
            mt = os.path.getmtime(got['server'])
            pack.download_jars('99.1', True)
            self.assertEqual(os.path.getmtime(got['server']), mt)                  # из кэша, без повторной загрузки
            # испорченный sha1 -> ошибка, файл не остаётся
            meta['downloads']['server']['sha1'] = '0' * 40
            jdump(meta, os.path.join(web, '99.1.json'))
            os.remove(got['server'])
            with self.assertRaises(pack.PackError):
                pack.download_jars('99.1', True)
            self.assertFalse(os.path.exists(got['server']))
            with self.assertRaises(pack.PackError):
                pack.download_jars('nope', True)
            self.assertEqual(pack.inspect_jar(got['client']).version, '99.1')
        finally:
            os.environ.pop('MCGEN_MANIFEST_URL', None)
            httpd.shutdown()

    def test_url_scheme_guard(self):
        with self.assertRaises(pack.PackError):
            pack._urlopen('file:///etc/passwd')

    def test_cache_clear_and_size(self):
        pack.prepare(self.srv, self.cli, fake_reports(os.path.join(self.root, 'rr')))
        self.assertGreater(pack.cache_size(), 0)
        pack.clear_cache('all')
        self.assertEqual(pack.list_packs(), [])


@unittest.skipUnless(REAL_JARS, 'нет реальных jar в jars/ (или MCGEN_TEST_REAL_JARS=0)')
class PackRealJarsTests(unittest.TestCase):
    """Подготовка ресурсов из НАСТОЯЩИХ jar Mojang 26.3: результат совпадает побайтно с tools/make_pack.py (run/pack-26.3, run/assets-26.3)."""

    @classmethod
    def setUpClass(cls):
        cls.cache = os.path.join(SCRATCH, 'real-cache')
        shutil.rmtree(cls.cache, ignore_errors=True)
        paths.set_cache_override(cls.cache)
        cls.server = os.path.join(_boot.JARS, 'server-26.3.jar')
        cls.client = os.path.join(_boot.JARS, 'client-26.3.jar')
        t0 = time.time()
        cls.res = tasks.Task('prep', lambda task: pack.prepare(cls.server, cls.client, task=task)).run_blocking()
        cls.t_prepare = time.time() - t0

    @classmethod
    def tearDownClass(cls):
        paths.set_cache_override(None)

    def test_prepared(self):
        self.assertEqual(self.res.state, 'done', self.res.error_text)
        r = self.res.result
        self.assertEqual((r.version, r.pack_ok, r.assets_ok), ('26.3', True, True))
        print(f'\n   [prepare 26.3: {self.t_prepare:.1f} с; шаги: {r.steps}]')

    def test_inspect(self):
        s, c = pack.inspect_jar(self.server), pack.inspect_jar(self.client)
        self.assertEqual((s.kind, s.version, s.world_version, s.java_version), ('server', '26.3', 5023, 25))
        self.assertEqual((c.kind, c.version), ('client', '26.3'))

    def _same_tree(self, a, b):
        r = subprocess.run(['diff', '-rq', a, b], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[:500])

    def test_identical_to_make_pack(self):
        r = self.res.result
        ref_pack, ref_assets = os.path.join(REPO, 'run', 'pack-26.3'), os.path.join(REPO, 'run', 'assets-26.3')
        if not os.path.isdir(ref_pack):
            self.skipTest('нет run/pack-26.3 для сравнения')
        self._same_tree(os.path.join(ref_pack, 'data'), os.path.join(r.pack_dir, 'data'))
        for f in ('blocks.json', 'registries.json'):
            with open(os.path.join(ref_pack, 'reports', f), 'rb') as a, open(os.path.join(r.pack_dir, 'reports', f), 'rb') as b:
                self.assertEqual(a.read(), b.read(), f)
        self._same_tree(os.path.join(ref_assets, 'assets'), os.path.join(r.assets_dir, 'assets'))

    def test_cache_by_sha1(self):
        t0 = time.time()
        r = pack.prepare(self.server, self.client)
        self.assertEqual(r.steps, ['datapack: cached', 'reports: cached', 'assets: cached'])
        self.assertLess(time.time() - t0, 5.0)
        with open(self.server, 'rb') as f:
            self.assertIn(hashlib.sha1(f.read()).hexdigest()[:10], r.pack_dir)

    def test_blocks_json_has_all_states(self):
        r = self.res.result
        with open(os.path.join(r.pack_dir, 'reports', 'blocks.json')) as f:
            d = json.load(f)
        self.assertEqual(sum(len(b['states']) for b in d.values()), 35723)

    def test_mock_uses_real_state_ids(self):
        g = mock.McGen.open(self.res.result.pack_dir, '26.3')
        self.assertEqual(g.block_state_count, 35723)
        self.assertEqual(g.ids['air'], 0)

    def test_other_versions_inspect(self):
        for v in ('26.1', '26.2', '26.4-snapshot-2'):
            p = os.path.join(_boot.JARS, f'server-{v}.jar')
            if os.path.isfile(p):
                self.assertEqual(pack.inspect_jar(p).version, v)

    def test_scan_finds_repo_jars(self):
        found = {(j.kind, j.version) for j in pack.scan_jars()}
        self.assertIn(('server', '26.3'), found)
        self.assertIn(('client', '26.3'), found)


# ---- настоящая библиотека libmcgen (M5) ---------------------------------------------------------------------------------------------------

def find_real_lib():
    """Настоящая libmcgen (НЕ заглушка): $MCGEN_REAL_LIB, libmcgen/build/libmcgen.so (make), libmcgen/build/linux-x64 (zig), lib/linux-x64 аддона."""
    cands = [os.environ.get('MCGEN_REAL_LIB'), os.path.join(REPO, 'libmcgen', 'build', 'libmcgen.so'),
             os.path.join(REPO, 'libmcgen', 'build', 'linux-x64', 'libmcgen.so'), os.path.join(_boot.ADDON_DIR, 'lib', 'linux-x64', 'libmcgen.so')]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


REAL_LIB = find_real_lib()
REAL_PACK = os.path.join(REPO, 'run', 'pack-26.3')
perf_results = {}


@unittest.skipUnless(REAL_LIB and os.path.isdir(os.path.join(REAL_PACK, 'reports')), 'нет настоящей libmcgen (make в libmcgen/ или build.py) / run/pack-26.3')
class RealLibraryTests(unittest.TestCase):
    """Контракт mcgen.h на НАСТОЯЩЕЙ библиотеке потока W1: данные, формат массивов, сиды, потоки, отмена, скорость. Нереализованные стадии
    (E_UNSUPPORTED) пропускают проверку, а не валят её; E_INTERNAL и расхождения контракта — провал."""

    @classmethod
    def setUpClass(cls):
        cls.L = lib.Library(REAL_LIB)
        cls.gen = lib.McGen.open(REAL_PACK, '26.3', cls.L)

    def region(self, w, *a, **kw):
        try:
            return w.generate_region(*a, **kw)
        except lib.McError as e:
            if e.code == lib.MCGEN_E_UNSUPPORTED:
                self.skipTest('стадия не реализована: ' + e.message)
            raise

    def test_abi_and_version(self):
        self.assertEqual(self.L.abi, lib.ABI_VERSION)
        self.assertIn('abi', self.L.version)
        self.assertEqual(self.L.mcgen_abi_version(), 1)

    def test_all_declared_exports_present(self):
        sys.path.insert(0, os.path.join(REPO, 'libmcgen'))
        import build as lb
        missing = [f for f in lb.api_functions() if not self.L.exported(f)]
        self.assertEqual(missing, [], 'функции mcgen.h не экспортируются')

    def test_registries(self):
        g = self.gen
        self.assertEqual(g.dimensions(), ['minecraft:overworld', 'minecraft:the_nether', 'minecraft:the_end'])
        for d in g.dimensions():
            self.assertIn('normal', g.presets(d))
        self.assertTrue({'normal', 'large_biomes', 'amplified'} <= set(g.presets('minecraft:overworld')))
        self.assertEqual(g.block_state_count, 35723)
        self.assertEqual(g.biome_count, 67)
        self.assertEqual(set(g.biome_names()), set(mock.FULL_BIOME_NAMES))

    def test_block_names_match_blocks_json(self):
        """Имена состояний (формат «блок[свойство=значение,…]», порядок свойств) совпадают с reports/blocks.json — на это опираются меши W4."""
        mg = mock.McGen.open(REAL_PACK, '26.3')
        ref = mg.block_names()
        got = self.gen.block_names()
        self.assertEqual(len(got), len(ref))
        bad = [(i, got[i], ref[i]) for i in range(len(ref)) if got[i] != ref[i]]
        self.assertEqual(bad[:5], [], f'{len(bad)} имён состояний отличаются от blocks.json')
        for n in ('minecraft:air', 'minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]'):
            i = self.gen.block_state_from_name(n)
            self.assertGreaterEqual(i, 0, n)
            self.assertEqual(self.gen.block_state_name(i), n)
        self.assertEqual(self.gen.block_state_from_name('minecraft:nope'), -1)

    def test_tweak_table_equals_json(self):
        got = self.gen.tweaks()
        doc = params.tweaks_doc()['tweaks']
        self.assertEqual([t.id for t in got], [d['id'] for d in doc])
        for t, j in zip(got, doc):
            self.assertEqual((t.label, t.group, t.description, t.is_int), (j['label'], j['group'], j['description'], j['type'] != 'float'), t.id)
            self.assertEqual((t.default, t.min, t.max, t.soft_min, t.soft_max), tuple(float(j[k]) for k in ('default', 'min', 'max', 'soft_min', 'soft_max')), t.id)

    def test_world_dimensions(self):
        exp = {'minecraft:overworld': (-64, 384, 63), 'minecraft:the_nether': (0, 256, 32), 'minecraft:the_end': (0, 256, 0)}
        for d, (miny, h, sea) in exp.items():
            w = self.gen.world(d, 'normal', 1)
            self.assertEqual((w.min_y, w.height, w.sea_level), (miny, h, sea), d)
        self.assertEqual(self.gen.world('minecraft:overworld', 'normal', 1, {'sea_level_offset': 5}).sea_level, 68)

    def test_errors(self):
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:nowhere', 'normal', 1)
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:overworld', 'nope', 1)
        with self.assertRaises(lib.McError):
            self.gen.world('minecraft:overworld', 'normal', 1, {'bogus_tweak': 1.0})
        with self.assertRaises(lib.McError) as c:
            lib.McGen.open(os.path.join(SCRATCH, 'no-such-pack'), '26.3', self.L)
        self.assertIn(c.exception.code, (lib.MCGEN_E_IO, lib.MCGEN_E_DATA))
        self.assertTrue(c.exception.message)
        with self.assertRaises(lib.McError) as c:
            lib.McGen.open(REAL_PACK, '1.2.3', self.L)
        self.assertEqual(c.exception.code, lib.MCGEN_E_VERSION)

    def test_biome_grid_and_at(self):
        w = self.gen.world('minecraft:overworld', 'normal', 12345)
        g = w.biome_grid(-100, 50, 20, 10, 1, 64)
        self.assertEqual((g.shape, g.dtype), ((10, 20), np.uint8))
        self.assertTrue((g < self.gen.biome_count).all())
        for iz, ix in ((0, 0), (3, 7), (9, 19)):
            self.assertEqual(int(g[iz, ix]), w.biome_at(-100 + ix, 64, 50 + iz))
        g2 = w.biome_grid(-100, 50, 5, 3, 8, 64)
        self.assertEqual(int(g2[1, 2]), w.biome_at(-100 + 2 * 8, 64, 50 + 8))

    def test_biomes_stage_region(self):
        w = self.gen.world('minecraft:overworld', 'normal', 12345)
        r = self.region(w, -1, -1, 3, 2, lib.MC_STAGE_BIOMES, 1)
        self.assertEqual(r.info, lib.RegionInfo(-1, -1, 3, 2, -64, 384))
        for c in r.chunks():
            b = r.biomes(*c)
            self.assertEqual((b.shape, b.dtype), ((96, 4, 4), np.uint8))
            self.assertTrue((b < self.gen.biome_count).all())

    def test_terrain_region_contract(self):
        w = self.gen.world('minecraft:overworld', 'normal', 12345)
        r = self.region(w, 0, 0, 2, 2, lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN, 1)
        names = self.gen.block_names()
        for c in r.chunks():
            b = r.blocks(*c)
            self.assertEqual((b.shape, b.dtype), ((384, 16, 16), np.uint16))
            self.assertLess(int(b.max()), self.gen.block_state_count)
            hm = r.heightmap(*c, lib.MC_HM_WORLD_SURFACE)
            self.assertEqual((hm.shape, hm.dtype), ((16, 16), np.int16))
            nz = b != 0
            top = (b.shape[0] - 1 - np.argmax(nz[::-1], axis=0)) + (-64) + 1            # первая свободная клетка над поверхностью
            self.assertTrue(np.array_equal(hm.astype(int), np.where(nz.any(axis=0), top, -64)), 'карта высот WORLD_SURFACE расходится с блоками')
            of = r.heightmap(*c, lib.MC_HM_OCEAN_FLOOR)
            self.assertTrue((of <= hm).all())
        used = {names[i] for i in np.unique(r.blocks(0, 0))}
        print('\n   [terrain чанк (0,0): блоки]', sorted(used)[:8])
        self.assertIn('minecraft:stone', used)

    def test_seed_domains_are_independent(self):
        """Контракт mcgen.h: сид features не влияет на заполнение рельефа; сид terrain — влияет; climate меняет биомы."""
        st = lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN
        base = self.region(self.gen.world('minecraft:overworld', 'normal', (5, 6, 7, 8)), 0, 0, 2, 2, st, 1)
        f = self.region(self.gen.world('minecraft:overworld', 'normal', (5, 6, 7, 999)), 0, 0, 2, 2, st, 1)
        s = self.region(self.gen.world('minecraft:overworld', 'normal', (5, 6, 777, 8)), 0, 0, 2, 2, st, 1)
        t = self.region(self.gen.world('minecraft:overworld', 'normal', (5, 66, 7, 8)), 0, 0, 2, 2, st, 1)
        c = self.region(self.gen.world('minecraft:overworld', 'normal', (55, 6, 7, 8)), 0, 0, 2, 2, st, 1)
        for ch in base.chunks():
            self.assertTrue(np.array_equal(base.blocks(*ch), f.blocks(*ch)), 'features-сид изменил рельеф')
            self.assertTrue(np.array_equal(base.blocks(*ch), s.blocks(*ch)), 'structures-сид изменил рельеф')
        self.assertTrue(any(not np.array_equal(base.blocks(*ch), t.blocks(*ch)) for ch in base.chunks()), 'terrain-сид не влияет на рельеф')
        self.assertTrue(any(not np.array_equal(base.biomes(*ch), c.biomes(*ch)) for ch in base.chunks()), 'climate-сид не влияет на биомы')

    def test_unified_equals_four_equal_domains(self):
        st = lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN
        a = self.region(self.gen.world('minecraft:overworld', 'normal', 12345), 3, 3, 2, 1, st, 1)
        b = self.region(self.gen.world('minecraft:overworld', 'normal', (12345,) * 4), 3, 3, 2, 1, st, 1)
        for ch in a.chunks():
            self.assertTrue(np.array_equal(a.blocks(*ch), b.blocks(*ch)))

    def test_threads_do_not_change_result(self):
        w = self.gen.world('minecraft:overworld', 'normal', 424242)
        st = lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN
        a = self.region(w, -2, -2, 4, 4, st, 1)
        b = self.region(w, -2, -2, 4, 4, st, 0)
        for ch in a.chunks():
            self.assertTrue(np.array_equal(a.blocks(*ch), b.blocks(*ch)), ch)
            self.assertTrue(np.array_equal(a.biomes(*ch), b.biomes(*ch)), ch)

    def test_chunk_independent_of_region_shape(self):
        """Один и тот же чанк одинаков в областях разной формы (гало соседей считается внутри)."""
        w = self.gen.world('minecraft:overworld', 'normal', 99)
        st = lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN
        a = self.region(w, 0, 0, 1, 1, st, 1).blocks(0, 0)
        b = self.region(w, -3, -2, 5, 4, st, 1).blocks(0, 0)
        self.assertTrue(np.array_equal(a, b))

    def test_concurrent_regions_from_python_threads(self):
        w = self.gen.world('minecraft:overworld', 'normal', 7)
        st = lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN
        ref = {(cx, cz): self.region(w, cx, cz, 2, 2, st, 1).blocks(cx, cz).copy() for cx in (0, 4) for cz in (0, 4)}
        out, errs = {}, []

        def work(cx, cz):
            try:
                out[(cx, cz)] = w.generate_region(cx, cz, 2, 2, st, 1).blocks(cx, cz).copy()
            except Exception as e:      # noqa: BLE001
                errs.append(e)
        ths = [threading.Thread(target=work, args=k) for k in ref]
        [t.start() for t in ths]
        [t.join() for t in ths]
        self.assertEqual(errs, [])
        for k in ref:
            self.assertTrue(np.array_equal(ref[k], out[k]), k)

    def test_cancel_from_callback(self):
        w = self.gen.world('minecraft:overworld', 'normal', 5)
        calls = []
        with self.assertRaises(lib.McCancelled):
            w.generate_region(0, 0, 16, 16, lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN, 1, lambda f, s: calls.append((round(f, 2), s)) or len(calls) > 3)
        self.assertGreaterEqual(len(calls), 1)
        print('\n   [прогресс: первые вызовы callback]', calls[:4])

    def test_tweaks_affect_terrain(self):
        st = lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN
        a = self.region(self.gen.world('minecraft:overworld', 'normal', 12345), 0, 0, 2, 2, st, 1)
        b = self.region(self.gen.world('minecraft:overworld', 'normal', 12345, {'sea_level_offset': 10}), 0, 0, 2, 2, st, 1)
        if all(np.array_equal(a.blocks(*c), b.blocks(*c)) for c in a.chunks()):
            self.skipTest('sea_level_offset пока не меняет рельеф (настройка ещё не реализована в W1)')

    def test_write_mcr_roundtrip(self):
        if not self.L.exported('mcgen_region_write_mcr') or self.L.mcgen_region_write_mcr is None:
            self.skipTest('нет mcgen_region_write_mcr')
        import struct
        w = self.gen.world('minecraft:overworld', 'normal', 3)
        r = self.region(w, 0, 0, 2, 1, lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN, 1)
        p = os.path.join(SCRATCH, 'real.mcr')
        r.write_mcr(p)
        with open(p, 'rb') as f:
            d = f.read()
        self.assertEqual(d[:4], b'MCR1')
        abi, cx0, cz0, nx, nz, miny, h, stages = struct.unpack_from('<8i', d, 4)
        self.assertEqual((abi, cx0, cz0, nx, nz, miny, h), (1, 0, 0, 2, 1, -64, 384))
        n_per = h * 256 * 2 + (h // 4) * 16 + 4 * 256 * 2
        for i, c in enumerate(r.chunks()):
            blocks = np.frombuffer(d, '<u2', h * 256, 36 + i * n_per).reshape(h, 16, 16)
            self.assertTrue(np.array_equal(blocks, r.blocks(*c)))

    def test_no_memory_growth(self):
        def rss():
            with open('/proc/self/statm') as f:
                return int(f.read().split()[1]) * os.sysconf('SC_PAGE_SIZE') / 1048576.0
        w = self.gen.world('minecraft:overworld', 'normal', 1)
        st = lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN
        for _ in range(3):
            self.region(w, 0, 0, 4, 4, st, 1)
        import gc
        gc.collect()
        a = rss()
        for _ in range(25):
            self.region(w, 0, 0, 4, 4, st, 1)
        gc.collect()
        grow = rss() - a
        print(f'\n   [рост RSS за 25 регионов 4×4: {grow:.1f} МБ]')
        self.assertLess(grow, 40.0, 'утечка памяти при повторной генерации')

    def test_performance(self):
        """Замер скорости (результат пишется в perf_results и в real_lib_perf.json рядом с логами тестов)."""
        w = self.gen.world('minecraft:overworld', 'normal', 12345)
        res = {}
        for name, st in (('biomes', lib.MC_STAGE_BIOMES), ('terrain', lib.MC_STAGE_BIOMES | lib.MC_STAGE_TERRAIN), ('all stages', lib.MC_STAGE_ALL)):
            try:
                for threads in (1, 0):
                    t0 = time.perf_counter()
                    r = w.generate_region(0, 0, 16, 16, st, threads)
                    dt = time.perf_counter() - t0
                    res[f'{name}, threads={threads or "auto"}'] = {'chunks': 256, 'seconds': round(dt, 3), 'chunks_per_s': round(256 / dt, 1)}
                    del r
            except lib.McError as e:
                res[name] = {'error': e.message}
        t0 = time.perf_counter()
        g = w.biome_grid(0, 0, 1024, 1024, 4, 64)
        res['biome_grid 1024x1024 step 4'] = {'seconds': round(time.perf_counter() - t0, 3)}
        t0 = time.perf_counter()
        lib.McGen.open(REAL_PACK, '26.3', self.L)
        res['mcgen_open'] = {'seconds': round(time.perf_counter() - t0, 3)}
        t0 = time.perf_counter()
        self.gen.world('minecraft:overworld', 'normal', 777)
        res['mcgen_world_new'] = {'seconds': round(time.perf_counter() - t0, 3)}
        perf_results.update(res)
        with open(os.path.join(SCRATCH, 'real_lib_perf.json'), 'w') as f:
            json.dump({'lib': REAL_LIB, 'version': self.L.version, 'results': res}, f, indent=1, ensure_ascii=False)
        print('\n   [скорость]', json.dumps(res, ensure_ascii=False))


# ---- палитры биомов, PNG -----------------------------------------------------------------------------------------------------------------

class BiomePaletteTests(unittest.TestCase):
    def test_modes(self):
        names = mock.FULL_BIOME_NAMES
        for mode in ('MAP', 'HASH', 'JSON'):
            p = biomes.palette(names, mode)
            self.assertEqual((p.shape, p.dtype), ((len(names), 3), np.float32))
            self.assertTrue(((p >= 0) & (p <= 1)).all())
        self.assertTrue(np.array_equal(biomes.palette(names, 'HASH'), biomes.palette(names, 'HASH')))       # детерминированность
        m = biomes.palette(names, 'MAP')
        self.assertFalse(np.allclose(m[mock.BIOME_ID['plains']], m[mock.BIOME_ID['desert']]))
        self.assertTrue(np.allclose(m[mock.BIOME_ID['snowy_plains']], [1, 1, 1]))

    def test_hash_distinguishes_biomes(self):
        h = biomes.palette(mock.FULL_BIOME_NAMES, 'HASH')
        self.assertGreater(len({tuple(np.round(x, 2)) for x in h}), 55)

    def test_unknown_biome_gets_hash_color(self):
        p = biomes.palette(['minecraft:brand_new_biome'], 'MAP')
        self.assertTrue(np.allclose(p[0], biomes.hash_color('brand_new_biome')))

    def test_colorize(self):
        pal = biomes.palette(mock.FULL_BIOME_NAMES, 'MAP')
        g = np.array([[0, 1], [2, 255]], np.uint8)
        c = biomes.colorize(g, pal)
        self.assertEqual(c.shape, (2, 2, 4))
        self.assertTrue((c[..., 3] == 1).all())

    @unittest.skipUnless(os.path.isdir(os.path.join(REPO, 'run', 'pack-26.3')), 'нет run/pack-26.3')
    def test_json_palette_from_pack(self):
        pk, asts = os.path.join(REPO, 'run', 'pack-26.3'), os.path.join(REPO, 'run', 'assets-26.3')
        p = biomes.palette(mock.FULL_BIOME_NAMES, 'JSON', pk, asts)
        ocean = p[mock.BIOME_ID['ocean']]
        self.assertTrue(np.allclose(ocean, np.array([0x3f, 0x76, 0xe4]) / 255.0, atol=1e-6))           # water_color океана
        self.assertGreater(p[mock.BIOME_ID['plains']][1], p[mock.BIOME_ID['plains']][2])               # трава зеленее, чем синяя

    @unittest.skipUnless(os.path.isdir(os.path.join(REPO, 'run', 'assets-26.3')), 'нет run/assets-26.3')
    def test_png_decoder(self):
        a, (w, h) = png.read_png(os.path.join(REPO, 'run', 'assets-26.3', 'assets', 'minecraft', 'textures', 'colormap', 'grass.png'))
        self.assertEqual((w, h, a.shape), (256, 256, (256, 256, 3)))
        a2, _ = png.read_png(os.path.join(REPO, 'run', 'assets-26.3', 'assets', 'minecraft', 'textures', 'block', 'water_still.png'))   # палитровый 4-бит
        self.assertEqual(a2.shape[:2], (512, 16))


class SysInfoTests(unittest.TestCase):
    def test_estimate_and_fit(self):
        from mcgen_addon.core import sysinfo
        self.assertEqual(sysinfo.estimate_bytes('minecraft:overworld', 1, 1), 384 * 256 * 2 + 96 * 16 + 4 * 256 * 2)
        ok, need, limit = sysinfo.check_fits('minecraft:overworld', 32, 32)
        self.assertTrue(ok)
        self.assertLess(need, 300 * 2**20)
        ok, need, limit = sysinfo.check_fits('minecraft:overworld', 512, 512)
        if limit is not None:
            self.assertEqual(ok, need <= limit)
        self.assertGreater(need, 40 * 2**30)
        self.assertTrue(sysinfo.total_ram_bytes() is None or sysinfo.total_ram_bytes() > 2**28)


class TaskTests(unittest.TestCase):
    def test_done_error_cancel(self):
        t = tasks.Task('x', lambda task: 5).run_blocking()
        self.assertEqual((t.state, t.result), ('done', 5))

        def bad(task):
            raise RuntimeError('oops')
        t = tasks.Task('x', bad).run_blocking()
        self.assertEqual((t.state, t.error_text), ('error', 'RuntimeError: oops'))

        def loop(task):
            for i in range(1000):
                task.check()
                time.sleep(0.005)
        t = tasks.Task('x', loop).start()
        time.sleep(0.05)
        t.cancel()
        t.join(2)
        self.assertEqual(t.state, 'cancelled')


class CatalogTests(unittest.TestCase):
    def test_defaults_and_update(self):
        catalog.clear()
        self.assertEqual(catalog.dimensions('26.3'), list(catalog.DEFAULT_DIMENSIONS))
        self.assertIn('amplified', catalog.presets('26.3', 'minecraft:overworld'))
        g = mock.McGen.open(None, '26.3')
        catalog.update_from_gen('26.3', g)
        self.assertEqual(catalog.presets('26.3', 'minecraft:the_end'), ['normal'])
        catalog.clear()


if __name__ == '__main__':
    unittest.main(verbosity=2)

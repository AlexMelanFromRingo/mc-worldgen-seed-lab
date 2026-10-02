"""Макет libmcgen на numpy: ТА ЖЕ поверхность, что у core/lib.py (McGen / McWorld / McRegion), но «мир» — правдоподобный шумовой
рельеф с плейсхолдерными блоками. Нужен, чтобы интерфейс, операторы, сцена и тесты работали до готовности настоящей библиотеки,
и как запасной режим («демо»), если библиотека не собрана под платформу.

НЕ воспроизводит Minecraft: рельеф, биомы, пещеры, руды, деревья и «постройки» — упрощённые шумы с теми же стадиями
(BIOMES < TERRAIN < SURFACE < CARVERS < FEATURES < STRUCTURES) и теми же тонкими настройками (tweaks.json), чтобы ползунки были живыми.
Блоки берутся из pack-каталога (reports/blocks.json: состояния по умолчанию), если он есть — тогда id совпадают с настоящими и сцена
показывается настоящими текстурами; иначе — встроенная мини-таблица.
"""
import json
import os
from collections import namedtuple

import numpy as np

from . import paths
from .lib import (ERROR_NAMES, MC_HM_MOTION_BLOCKING, MC_HM_MOTION_BLOCKING_NO_LEAVES, MC_HM_OCEAN_FLOOR, MC_HM_WORLD_SURFACE,  # noqa: F401
                  MC_STAGE_ALL, MC_STAGE_BIOMES, MC_STAGE_CARVERS, MC_STAGE_FEATURES, MC_STAGE_STRUCTURES, MC_STAGE_SURFACE,
                  MC_STAGE_TERRAIN, MCGEN_E_ARG, MCGEN_E_CANCEL, MCGEN_E_IO, MCGEN_E_VERSION, McCancelled, McError, RegionInfo,
                  TweakInfo)

NAME = 'mock'
SUPPORTED_VERSIONS = ('26.1', '26.2', '26.3', '26.4-snapshot-2')
DIMENSIONS = ('minecraft:overworld', 'minecraft:the_nether', 'minecraft:the_end')
PRESETS = {
    'minecraft:overworld': ('normal', 'large_biomes', 'amplified'),
    'minecraft:the_nether': ('normal',),
    'minecraft:the_end': ('normal',),
}

BIOME_NAMES = [
    'badlands', 'bamboo_jungle', 'basalt_deltas', 'beach', 'birch_forest', 'cherry_grove', 'cold_ocean', 'crimson_forest',
    'dappled_forest', 'dark_forest', 'deep_cold_ocean', 'deep_dark', 'deep_frozen_ocean', 'deep_lukewarm_ocean', 'deep_ocean',
    'desert', 'dripstone_caves', 'end_barrens', 'end_highlands', 'end_midlands', 'eroded_badlands', 'flower_forest', 'forest',
    'frozen_ocean', 'frozen_peaks', 'frozen_river', 'grove', 'ice_spikes', 'jagged_peaks', 'jungle', 'lukewarm_ocean', 'lush_caves',
    'mangrove_swamp', 'meadow', 'mushroom_fields', 'nether_wastes', 'ocean', 'old_growth_birch_forest', 'old_growth_pine_taiga',
    'old_growth_spruce_taiga', 'pale_garden', 'plains', 'river', 'savanna', 'savanna_plateau', 'small_end_islands', 'snowy_beach',
    'snowy_plains', 'snowy_slopes', 'snowy_taiga', 'soul_sand_valley', 'sparse_jungle', 'stony_peaks', 'stony_shore', 'sulfur_caves',
    'sunflower_plains', 'swamp', 'taiga', 'the_end', 'the_void', 'warm_ocean', 'warped_forest', 'windswept_forest',
    'windswept_gravelly_hills', 'windswept_hills', 'windswept_savanna', 'wooded_badlands',
]
BIOME_ID = {n: i for i, n in enumerate(BIOME_NAMES)}
FULL_BIOME_NAMES = ['minecraft:' + n for n in BIOME_NAMES]

# плейсхолдерные блоки (короткие имена); порядок = встроенные id, если pack-каталога нет
BLOCK_TABLE = [
    'air', 'stone', 'deepslate', 'bedrock', 'dirt', 'grass_block', 'sand', 'sandstone', 'gravel', 'water', 'lava', 'snow_block', 'ice',
    'packed_ice', 'netherrack', 'soul_sand', 'basalt', 'magma_block', 'end_stone', 'oak_log', 'oak_leaves', 'spruce_log', 'spruce_leaves',
    'cobblestone', 'stone_bricks', 'coal_ore', 'iron_ore', 'diamond_ore', 'terracotta', 'red_sand', 'mycelium', 'podzol', 'clay',
    'glowstone', 'crimson_nylium', 'warped_nylium', 'obsidian', 'coarse_dirt', 'cave_air', 'blackstone', 'red_sandstone',
]
_TWEAKS_CACHE = None


def load_tweak_infos():
    """TweakInfo из libmcgen/tweaks.json (копия в core/tweaks.json) — те же, что отдала бы настоящая библиотека."""
    global _TWEAKS_CACHE
    if _TWEAKS_CACHE is None:
        with open(paths.tweaks_json_path(), encoding='utf-8') as f:
            doc = json.load(f)
        _TWEAKS_CACHE = [TweakInfo(t['id'], t['label'], t['group'], t['description'], float(t['default']), float(t['min']), float(t['max']),
                                   float(t['soft_min']), float(t['soft_max']), t['type'] != 'float') for t in doc['tweaks']]
    return _TWEAKS_CACHE


# ---- хэш-шум -------------------------------------------------------------------------------------------------------------

_U = np.uint64
_C1, _C2 = _U(0xBF58476D1CE4E5B9), _U(0x94D049BB133111EB)
_GOLD = _U(0x9E3779B97F4A7C15)


def _mix(h):
    h = (h ^ (h >> _U(30))) * _C1
    h = (h ^ (h >> _U(27))) * _C2
    return h ^ (h >> _U(31))


def _u64(a):
    return np.asarray(a, dtype=np.int64).astype(np.uint64)


_M64 = 0xFFFFFFFFFFFFFFFF


def _salt(seed, n):
    """Производный 64-битный seed шума n из сида домена (арифметика на целых Python — без предупреждений о переполнении)."""
    h = (seed & _M64) ^ (((n + 1) * 0x9E3779B97F4A7C15) & _M64)
    h = ((h ^ (h >> 30)) * 0xBF58476D1CE4E5B9) & _M64
    h = ((h ^ (h >> 27)) * 0x94D049BB133111EB) & _M64
    return _U(h ^ (h >> 31))


def _quiet(fn):
    """Переполнение uint64 в хэшах — штатное: глушим предупреждения на время вызова (errstate — на поток)."""
    def wrapper(*a, **k):
        with np.errstate(over='ignore'):
            return fn(*a, **k)
    wrapper.__name__ = fn.__name__
    wrapper.__doc__ = fn.__doc__
    return wrapper


def _h01(seed, *coords):
    """Хэш целочисленных координат -> float64 в [-1, 1)."""
    h = seed
    for c in coords:
        h = _mix(h ^ (_u64(c) * _GOLD))
    return (h >> _U(11)).astype(np.float64) * (2.0 / 9007199254740992.0) - 1.0


def _sm(t):
    return t * t * (3.0 - 2.0 * t)


def vnoise2(seed, x, z):
    xi, zi = np.floor(x), np.floor(z)
    fx, fz = _sm(x - xi), _sm(z - zi)
    ix, iz = xi.astype(np.int64), zi.astype(np.int64)
    a, b = _h01(seed, ix, iz), _h01(seed, ix + 1, iz)
    c, d = _h01(seed, ix, iz + 1), _h01(seed, ix + 1, iz + 1)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz


def vnoise3(seed, x, y, z):
    xi, yi, zi = np.floor(x), np.floor(y), np.floor(z)
    fx, fy, fz = _sm(x - xi), _sm(y - yi), _sm(z - zi)
    ix, iy, iz = xi.astype(np.int64), yi.astype(np.int64), zi.astype(np.int64)

    def lx(dy, dz):
        a, b = _h01(seed, ix, iy + dy, iz + dz), _h01(seed, ix + 1, iy + dy, iz + dz)
        return a + (b - a) * fx
    l00, l10, l01, l11 = lx(0, 0), lx(1, 0), lx(0, 1), lx(1, 1)
    a = l00 + (l10 - l00) * fy
    b = l01 + (l11 - l01) * fy
    return a + (b - a) * fz


def fbm2(seed, x, z, octaves=4):
    out, amp, tot, f = 0.0, 1.0, 0.0, 1.0
    for o in range(octaves):
        out = out + amp * vnoise2(_salt(int(seed), o), x * f, z * f)
        tot += amp
        amp *= 0.5
        f *= 2.0
    return out / tot


def _lerp_matrix(n_out, n_in, step):
    """Матрица линейной интерполяции (n_out × n_in): выходная позиция p берёт узлы p/step и p/step+1."""
    m = np.zeros((n_out, n_in))
    p = np.arange(n_out) / step
    i0 = np.minimum(p.astype(int), n_in - 2)
    f = p - i0
    m[np.arange(n_out), i0] = 1 - f
    m[np.arange(n_out), i0 + 1] = f
    return m


# ---- McGen / McWorld / McRegion ------------------------------------------------------------------------------------------

class McGen:
    """Макет данных версии. pack_dir может отсутствовать (тогда встроенные таблицы)."""

    def __init__(self, pack_dir, version):
        self.pack_dir = pack_dir
        self.version = version
        self._closed = False
        self._names = None
        self._ids = None
        self._tweaks = load_tweak_infos()

    @classmethod
    def open(cls, pack_dir, version, lib=None):
        if version not in SUPPORTED_VERSIONS:
            raise McError(MCGEN_E_VERSION, 'Version {version} is not supported (demo generator: {supported})', 'mcgen_open', version=version, supported=', '.join(SUPPORTED_VERSIONS))
        return cls(pack_dir, version)

    backend = NAME

    def close(self):
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def dimensions(self):
        return list(DIMENSIONS)

    def presets(self, dimension):
        return list(PRESETS.get(dimension, ()))

    def _load_table(self):
        if self._names is not None:
            return
        blocks = None
        rep = os.path.join(self.pack_dir or '', 'reports', 'blocks.json')
        if self.pack_dir and os.path.isfile(rep):
            try:
                with open(rep, encoding='utf-8') as f:
                    blocks = json.load(f)
            except (OSError, ValueError):
                blocks = None
        ids = {}
        if blocks:
            names = [None] * (1 + max(s['id'] for b in blocks.values() for s in b['states']))
            for bname, b in blocks.items():
                props = list(b.get('properties', {}))
                for s in b['states']:
                    p = s.get('properties')
                    names[s['id']] = bname + ('[' + ','.join(f'{k}={p[k]}' for k in props) + ']' if p else '')
                    if s.get('default'):
                        ids[bname.split(':', 1)[1]] = s['id']
            self._names = names
        else:
            self._names = ['minecraft:' + n for n in BLOCK_TABLE]
            ids = {n: i for i, n in enumerate(BLOCK_TABLE)}
        missing = [n for n in BLOCK_TABLE if n not in ids]
        for n in missing:                       # блок мог отсутствовать в reports (старый pack): подменяем камнем
            ids[n] = ids.get('stone', 1)
        self._ids = ids

    @property
    def ids(self):
        self._load_table()
        return self._ids

    @property
    def block_state_count(self):
        self._load_table()
        return len(self._names)

    def block_state_name(self, i):
        self._load_table()
        return self._names[i] if 0 <= i < len(self._names) else None

    def block_state_from_name(self, name):
        self._load_table()
        if not hasattr(self, '_rev'):
            self._rev = {n: i for i, n in enumerate(self._names) if n}
        return self._rev.get(name, -1)

    def block_names(self):
        self._load_table()
        return self._names

    @property
    def biome_count(self):
        return len(BIOME_NAMES)

    def biome_name(self, i):
        return FULL_BIOME_NAMES[i] if 0 <= i < len(FULL_BIOME_NAMES) else None

    def biome_names(self):
        return FULL_BIOME_NAMES

    def tweaks(self):
        return list(self._tweaks)

    def world(self, dimension, preset, seeds, tweaks=None):
        return McWorld(self, dimension, preset, seeds, tweaks)


class McWorld:
    def __init__(self, gen, dimension, preset, seeds, tweaks):
        if dimension not in DIMENSIONS:
            raise McError(MCGEN_E_ARG, 'Unknown dimension {name}', 'mcgen_world_new', name=dimension)
        if preset not in PRESETS[dimension]:
            raise McError(MCGEN_E_ARG, 'Unknown preset {preset} for {dimension}', 'mcgen_world_new', preset=preset, dimension=dimension)
        if isinstance(seeds, int):
            seeds = (seeds,) * 4
        seeds = tuple(int(s) for s in seeds)
        if len(seeds) != 4 or any(not -(1 << 63) <= s < (1 << 63) for s in seeds):
            raise McError(MCGEN_E_ARG, 'Seeds: four int64 values are required', 'mcgen_world_new')
        known = {t.id: t for t in gen.tweaks()}
        tw = {t.id: t.default for t in gen.tweaks()}
        for k, v in (tweaks or {}).items():
            if k not in known:
                raise McError(MCGEN_E_ARG, 'Unknown world tweak {name}', 'mcgen_world_new', name=k)
            t = known[k]
            tw[k] = min(max(float(v), t.min), t.max)
        self.gen = gen
        self.dimension, self.preset, self.seeds = dimension, preset, seeds
        self.tweaks = tw
        self.dim = DIMENSIONS.index(dimension)
        self.min_y = -64 if self.dim == 0 else 0
        self.height = 384 if self.dim == 0 else 256
        self.sea_level = {0: 63, 1: 32, 2: 0}[self.dim] + int(tw['sea_level_offset'])
        self._s = [_salt(sd, i) for i, sd in enumerate(seeds)]       # хэш-сиды доменов
        self._climate_scale = tw['climate_scale_xz'] * (4.0 if preset == 'large_biomes' else 1.0)
        self._amp = tw['terrain_amplitude'] * (1.8 if preset == 'amplified' else 1.0)

    def close(self):
        pass

    # --- климат и высота (2D) ---
    def _climate(self, x, z):
        k = 1.0 / self._climate_scale
        c = np.clip(fbm2(self._s[0] ^ _U(11), x * k / 700.0, z * k / 700.0, 4) * 1.7, -1, 1)       # континентальность
        t = np.clip(fbm2(self._s[0] ^ _U(22), x * k / 600.0, z * k / 600.0, 3) * 1.8, -1, 1)       # температура
        h = np.clip(fbm2(self._s[0] ^ _U(33), x * k / 500.0, z * k / 500.0, 3) * 1.8, -1, 1)       # влажность
        r = fbm2(self._s[0] ^ _U(44), x * k / 400.0, z * k / 400.0, 3)                              # «русла»
        return c, t, h, r

    def _surface(self, x, z):
        """(высота верхнего твёрдого блока, c, t, h, река?) для массивов x, z."""
        sea = self.sea_level
        c, t, h, r = self._climate(x, z)
        base = np.interp(c, [-1, -0.45, -0.2, 0.0, 0.15, 0.5, 1.0], [-55, -28, -8, 2, 10, 30, 52])
        detail = fbm2(self._s[1] ^ _U(55), x / 60.0, z / 60.0, 4)
        ridge = 1.0 - np.abs(fbm2(self._s[1] ^ _U(66), x / 260.0, z / 260.0, 3) * 2.2)
        peaks = np.clip(ridge, 0, 1) ** 2 * np.clip(c - 0.1, 0, 1) * 70
        steep = self.tweaks['terrain_steepness']
        dev = (base + detail * 9 + peaks) * self._amp
        dev = np.sign(dev) * np.abs(dev) ** 1.0 * steep if steep != 1.0 else dev
        river = (np.abs(r) < 0.035) & (c > -0.15)
        dev = np.where(river, np.minimum(dev, -2.0), dev)
        top = np.floor(sea + dev).astype(np.int64)
        return np.clip(top, self.min_y + 8, self.min_y + self.height - 8), c, t, h, river

    def _biome_ids(self, x, z, surf=None):
        """Биомы поверхности (id) по климату — для Верхнего мира; Нижний мир и Энд — свои."""
        B = BIOME_ID
        if self.dim == 1:
            n = fbm2(self._s[0] ^ _U(77), x / (90.0 * self._climate_scale), z / (90.0 * self._climate_scale), 3)
            return np.select([n < -0.3, n < -0.08, n < 0.12, n < 0.3], [B['soul_sand_valley'], B['crimson_forest'], B['nether_wastes'], B['warped_forest']],
                             B['basalt_deltas']).astype(np.uint8)
        if self.dim == 2:
            d = np.sqrt(x.astype(np.float64) ** 2 + z.astype(np.float64) ** 2)
            return np.select([d < 120, d < 400, d < 700], [B['the_end'], B['end_highlands'], B['end_midlands']], B['small_end_islands']).astype(np.uint8)
        top, c, t, h, river = surf if surf is not None else self._surface(x, z)
        sea = self.sea_level
        height_above = top - sea
        cold, hot, wet, dry = t < -0.35, t > 0.4, h > 0.25, h < -0.25
        ocean_warm = np.select([t > 0.45, t > 0.0, t > -0.35], [B['warm_ocean'], B['lukewarm_ocean'], B['ocean']], B['cold_ocean'])
        deep = np.select([t > 0.45, t > 0.0, t > -0.35, t > -0.6], [B['deep_lukewarm_ocean'], B['deep_lukewarm_ocean'], B['deep_ocean'], B['deep_cold_ocean']], B['deep_frozen_ocean'])
        ocean = np.where(c < -0.45, deep, ocean_warm)
        ocean = np.where(t < -0.7, np.where(c < -0.45, B['deep_frozen_ocean'], B['frozen_ocean']), ocean)
        land = np.select(
            [cold & wet, cold, hot & dry, hot & wet, hot, dry, wet, h > 0.0],
            [B['snowy_taiga'], B['snowy_plains'], B['desert'], B['jungle'], B['savanna'], B['plains'], B['forest'], B['birch_forest']],
            B['plains'])
        land = np.where((t > -0.1) & (t < 0.3) & wet, B['dark_forest'], land)
        land = np.where((t > 0.0) & (t < 0.4) & (h > 0.5), B['swamp'], land)
        peak = np.where(cold, B['frozen_peaks'], np.where(h > 0, B['stony_peaks'], B['jagged_peaks']))
        beach = np.where(cold, B['snowy_beach'], B['beach'])
        out = np.where(top < sea - 1, ocean, np.where(height_above <= 2, np.where(river, np.where(cold, B['frozen_river'], B['river']), beach), land))
        out = np.where(river & (top >= sea - 1), np.where(cold, B['frozen_river'], B['river']), out)
        out = np.where(height_above > 78, peak, out)
        return out.astype(np.uint8)

    @_quiet
    def biome_at(self, x, y, z):
        return int(self._biome_ids(np.asarray([x], np.int64), np.asarray([z], np.int64))[0])

    @_quiet
    def biome_grid(self, x0, z0, nx, nz, step=1, y=63):
        xs = x0 + np.arange(nx, dtype=np.int64) * step
        zs = z0 + np.arange(nz, dtype=np.int64) * step
        X, Z = np.meshgrid(xs, zs)
        return self._biome_ids(X, Z).astype(np.uint8)

    # --- генерация чанка ---
    @_quiet
    def _gen_chunk(self, cx, cz, stages):
        gen, ids = self.gen, self.gen.ids
        H, miny, sea = self.height, self.min_y, self.sea_level
        tw = self.tweaks
        X = (cx * 16 + np.arange(16, dtype=np.int64))[None, :].repeat(16, 0)      # [z][x]
        Z = (cz * 16 + np.arange(16, dtype=np.int64))[:, None].repeat(16, 1)
        bq = self.biome_grid(cx * 16 + 2, cz * 16 + 2, 4, 4, 4)                      # клетки 4×4 (центры)
        biomes = np.broadcast_to(bq[None, :, :], (H // 4, 4, 4)).copy()
        blocks = np.zeros((H, 16, 16), np.uint16)
        surf = self._column_heights(X, Z)
        top = surf[0]
        bio = self._biome_ids(X, Z, surf) if self.dim == 0 else None
        hm = np.zeros((4, 16, 16), np.int16)
        if not stages & (MC_STAGE_TERRAIN | MC_STAGE_SURFACE | MC_STAGE_CARVERS | MC_STAGE_FEATURES | MC_STAGE_STRUCTURES):
            return blocks, biomes, hm
        Y = (miny + np.arange(H, dtype=np.int64))[:, None, None]
        solid = Y <= top[None]
        if self.dim == 0:
            blocks[...] = 0
            fluid = (~solid) & (Y <= sea)
            blocks[fluid] = ids['water']
        elif self.dim == 1:
            solid, lava = self._nether_solid(X, Z, Y)
            blocks[lava] = ids['lava']
        else:
            solid = self._end_solid(X, Z, Y)
        blocks[solid] = ids['stone'] if self.dim == 0 else (ids['netherrack'] if self.dim == 1 else ids['end_stone'])

        # кубические «сырные» пещеры входят в TERRAIN
        density = tw['cave_density']
        if density > 0 and self.dim <= 1:
            self._carve(blocks, solid, X, Z, top, cx, cz, kind=0)

        if stages & MC_STAGE_SURFACE:
            self._surface_rules(blocks, solid, Y, top, bio, X, Z)
        if stages & MC_STAGE_CARVERS and density > 0 and self.dim == 0:
            self._carve(blocks, solid, X, Z, top, cx, cz, kind=1)
        if stages & MC_STAGE_FEATURES:
            self._features(blocks, ids, cx, cz, top, bq)
        if stages & MC_STAGE_STRUCTURES:
            self._structures(blocks, ids, cx, cz, top)

        # карты высот (y первой свободной клетки над поверхностью)
        nonair = blocks != 0
        any_ = nonair.any(axis=0)
        top_idx = H - 1 - np.argmax(nonair[::-1], axis=0)
        wsurf = np.where(any_, miny + top_idx + 1, miny).astype(np.int16)
        water = (blocks == ids['water']) | (blocks == ids['lava'])
        solid_nw = nonair & ~water
        leaves = (blocks == ids['oak_leaves']) | (blocks == ids['spruce_leaves'])
        any_s = solid_nw.any(axis=0)
        floor = np.where(any_s, miny + H - np.argmax(solid_nw[::-1], axis=0), miny).astype(np.int16)
        mb = nonair & ~leaves | water
        any_m = mb.any(axis=0)
        mbn = np.where(any_m, miny + H - np.argmax(mb[::-1], axis=0), miny).astype(np.int16)
        hm[MC_HM_WORLD_SURFACE], hm[MC_HM_OCEAN_FLOOR], hm[MC_HM_MOTION_BLOCKING], hm[MC_HM_MOTION_BLOCKING_NO_LEAVES] = wsurf, floor, wsurf, mbn
        return blocks, biomes, hm

    def _column_heights(self, X, Z):
        if self.dim == 0:
            return self._surface(X, Z)
        z = np.zeros((16, 16))
        return (np.full((16, 16), self.min_y + 64, np.int64), z, z, z, z.astype(bool))

    def _nether_solid(self, X, Z, Y):
        s = self._s[1]
        n = vnoise3(s ^ _U(5), X[None] / 28.0, Y / 18.0, Z[None] / 28.0) * 0.7 + vnoise3(s ^ _U(6), X[None] / 9.0, Y / 7.0, Z[None] / 9.0) * 0.3
        ceil = Y > 120 - 6 * np.abs(vnoise3(s ^ _U(7), X[None] / 24.0, 0.0 * Y, Z[None] / 24.0))
        floor = Y < 30 + 10 * vnoise3(s ^ _U(8), X[None] / 40.0, 0.0 * Y, Z[None] / 40.0)
        solid = ((n > 0.15) | ceil | floor) & (Y >= 0) & (Y < 128)
        lava = (~solid) & (Y <= self.sea_level) & (Y >= 0)
        return solid, lava

    def _end_solid(self, X, Z, Y):
        s = self._s[1]
        d = np.sqrt(X.astype(np.float64) ** 2 + Z.astype(np.float64) ** 2)[None]
        island = 1.0 - d / 150.0
        isl2 = vnoise2(s ^ _U(9), X[None] / 90.0, Z[None] / 90.0)
        outer = (d > 550) & (isl2 > 0.35)
        m = ((island > 0) | outer)
        thick = np.where(island > 0, 38.0 * np.sqrt(np.clip(island, 0, 1)), 9.0 * (isl2 - 0.35) / 0.65 + 3)
        mid = 62 + 4 * vnoise2(s ^ _U(10), X[None] / 20.0, Z[None] / 20.0)
        return m & (np.abs(Y - mid) <= thick) & (Y - mid <= thick * 0.5)

    def _carve(self, blocks, solid, X, Z, top, cx, cz, kind):
        """Пещеры по крупной сетке (узлы 4×8×4, как ячейки интерполяции игры) + линейная интерполяция."""
        H, miny = self.height, self.min_y
        ny = H // 8 + 1
        gx = cx * 16 + np.arange(5) * 4
        gz = cz * 16 + np.arange(5) * 4
        gy = miny + np.arange(ny) * 8
        GX, GY, GZ = np.meshgrid(gx, gy, gz, indexing='ij')           # (5, ny, 5)
        s = self._s[1]
        size = self.tweaks['cave_size']
        dens = self.tweaks['cave_density']
        if kind == 0:
            n = vnoise3(s ^ _U(21), GX / (46.0 * size), GY / (28.0 * size), GZ / (46.0 * size)) + 0.4 * vnoise3(s ^ _U(22), GX / 20.0, GY / 14.0, GZ / 20.0)
            val = n
            thr = 1.0 - 0.32 * dens
        else:
            a = vnoise3(s ^ _U(23), GX / (34.0 * size), GY / (26.0 * size), GZ / (34.0 * size))
            b = vnoise3(s ^ _U(24), GX / (30.0 * size), GY / (22.0 * size), GZ / (30.0 * size))
            val = 1.0 - np.maximum(np.abs(a), np.abs(b)) / (0.045 * size * max(dens, 0.05))
            thr = 0.0
        # интерполяция: (x:5) (y:ny) (z:5) -> (z:16)(y:H)(x:16) блоков в порядке [y][z][x]
        mx, my, mz = _lerp_matrix(16, 5, 4), _lerp_matrix(H, ny, 8), _lerp_matrix(16, 5, 4)
        full = np.tensordot(my, val, axes=([1], [1]))                  # (H, 5x, 5z)
        full = np.tensordot(full, mx, axes=([1], [1]))                  # (H, 5z, 16x)
        full = np.tensordot(full, mz, axes=([1], [1]))                  # (H, 16x, 16z)
        full = full.transpose(0, 2, 1)                                  # (H, z, x)
        Y = (miny + np.arange(H, dtype=np.int64))[:, None, None]
        lava_y = -54 + int(self.tweaks['lava_level_offset']) if self.dim == 0 else self.sea_level
        cave = (full > thr) & (Y > miny + 5) & (Y < top[None] - 5 if self.dim == 0 else Y < 118)
        cave &= blocks != 0
        ids = self.gen.ids
        blocks[cave] = 0
        if self.dim == 0:
            blocks[cave & (Y <= lava_y)] = ids['lava']
        elif self.dim == 1:
            blocks[cave & (Y <= lava_y)] = ids['lava']

    def _surface_rules(self, blocks, solid, Y, top, bio, X, Z):
        ids = self.gen.ids
        H, miny, sea = self.height, self.min_y, self.sea_level
        if self.dim == 0:
            depth = top[None] - Y                                         # 0 = верхний блок
            B = BIOME_ID
            desert = np.isin(bio, [B['desert']])[None]
            beach = np.isin(bio, [B['beach'], B['snowy_beach']])[None]
            ocean = (top < sea - 1)[None]
            peak = np.isin(bio, [B['frozen_peaks'], B['stony_peaks'], B['jagged_peaks']])[None]
            snowy = np.isin(bio, [B['snowy_plains'], B['snowy_taiga'], B['frozen_river'], B['frozen_ocean'], B['ice_spikes']])[None]
            taiga = np.isin(bio, [B['snowy_taiga'], B['taiga']])[None]
            under = solid & (blocks == ids['stone'])
            top_block = np.where(desert | beach, ids['sand'], np.where(ocean, np.where(top[None] < sea - 6, ids['gravel'], ids['sand']), ids['grass_block']))
            top_block = np.where(peak, ids['stone'], top_block)
            top_block = np.where(snowy & ~ocean & ~beach, ids['grass_block'], top_block)
            sub = np.where(desert | beach, ids['sandstone'], np.where(peak, ids['stone'], ids['dirt']))
            rows = under & (depth == 0)
            blocks[rows] = np.broadcast_to(top_block, blocks.shape)[rows]
            under = solid & (blocks == ids['stone'])
            mid = under & (depth >= 1) & (depth <= 3)
            blocks[mid] = np.broadcast_to(sub, blocks.shape)[mid]
            # снег на вершинах/холодных биомах
            snow_cap = (depth == 0) & np.broadcast_to(peak | (snowy & ~ocean), blocks.shape) & (blocks != 0)
            blocks[snow_cap & (top[None] > sea + 90)] = ids['snow_block']
            # глубинный сланец
            ds = solid & (blocks == ids['stone']) & (Y < 0 + (vnoise2(self._s[1] ^ _U(31), X / 8.0, Z / 8.0) * 4)[None])
            blocks[ds] = ids['deepslate']
            # бедрок
            r = (_h01(self._s[1] ^ _U(32), X, Z) * 0.5 + 0.5)
            blocks[(Y == miny) & solid] = ids['bedrock']
            blocks[(Y > miny) & (Y < miny + 5) & solid & ((r[None] * 5) > (Y - miny))] = ids['bedrock']
        elif self.dim == 1:
            blocks[(Y >= 0) & (Y < 5) & (blocks != 0) & (blocks != ids['lava'])] = ids['bedrock']
            blocks[np.broadcast_to((Y >= 123) & (Y < 128), blocks.shape)] = ids['bedrock']
            n = vnoise2(self._s[1] ^ _U(33), X / 10.0, Z / 10.0)[None]
            ss = solid & (blocks == ids['netherrack']) & (Y < 40) & (n > 0.2)
            blocks[ss] = ids['soul_sand']
        # End: ничего

    def _features(self, blocks, ids, cx, cz, top, bq):
        tw = self.tweaks
        f = self._s[3]
        if self.dim == 0:
            # деревья: кандидаты по сетке 5×5 в глобальных координатах (детерминированно, без швов на границах чанков)
            self._trees(blocks, ids, cx, cz, 0.24 * tw['feature_density'], tw['tree_size'])
            # руды: случайные «капли» в камне (детерминированный rng на чанк)
            rng = np.random.default_rng([int(f) & 0xFFFFFFFF, (int(f) >> 32) & 0xFFFFFFFF, cx & 0xFFFFFFFF, cz & 0xFFFFFFFF])
            od, osz = tw['ore_density'], tw['ore_size']
            for ore, count, lo, hi, rad in (('coal_ore', 14, 0, 128, 1.9), ('iron_ore', 9, -24, 56, 1.7), ('diamond_ore', 2, -62, 14, 1.4)):
                for _ in range(int(round(count * od))):
                    x, z = int(rng.integers(0, 16)), int(rng.integers(0, 16))
                    y = int(rng.integers(lo, hi))
                    self._blob(blocks, ids, x, y, z, rad * osz, ids[ore])
        elif self.dim == 1:
            rng = np.random.default_rng([7, cx & 0xFFFFFFFF, cz & 0xFFFFFFFF])
            for _ in range(int(6 * tw['ore_density'])):
                self._blob(blocks, ids, int(rng.integers(0, 16)), int(rng.integers(10, 110)), int(rng.integers(0, 16)), 1.8 * tw['ore_size'], ids['glowstone'], host=ids['netherrack'])

    def _trees(self, blocks, ids, cx, cz, dens, size):
        if dens <= 0:
            return
        f, cell = self._s[3], 5
        rad = max(1, int(round(2 * size)))
        m = rad + 1
        gx = np.arange((cx * 16 - m) // cell, (cx * 16 + 16 + m) // cell + 1, dtype=np.int64)
        gz = np.arange((cz * 16 - m) // cell, (cz * 16 + 16 + m) // cell + 1, dtype=np.int64)
        GX, GZ = np.meshgrid(gx, gz)
        r1 = _h01(f ^ _U(41), GX, GZ) * 0.5 + 0.5
        tx = GX * cell + ((_h01(f ^ _U(42), GX, GZ) * 0.5 + 0.5) * cell).astype(np.int64)
        tz = GZ * cell + ((_h01(f ^ _U(43), GX, GZ) * 0.5 + 0.5) * cell).astype(np.int64)
        tx, tz, r1 = tx.ravel(), tz.ravel(), r1.ravel()
        top, c, t, h, river = self._surface(tx, tz)
        bio = self._biome_ids(tx, tz, (top, c, t, h, river))
        B = BIOME_ID
        dense = np.isin(bio, [B['forest'], B['birch_forest'], B['dark_forest'], B['taiga'], B['snowy_taiga'], B['jungle'], B['flower_forest'], B['old_growth_pine_taiga']])
        sparse = np.isin(bio, [B['plains'], B['savanna'], B['swamp'], B['sparse_jungle']])
        chance = np.where(dense, 1.6, np.where(sparse, 0.12, 0.0))
        ok = (r1 <= dens * chance) & (top > self.sea_level)
        spruce = np.isin(bio, [B['taiga'], B['snowy_taiga'], B['old_growth_pine_taiga']])
        for i in np.nonzero(ok)[0]:
            self._tree(blocks, ids, cx, cz, int(tx[i]), int(tz[i]), int(top[i]), bool(spruce[i]), size)

    def _tree(self, blocks, ids, cx, cz, tx, tz, top, spruce, size):
        lx, lz = tx - cx * 16, tz - cz * 16
        miny, H = self.min_y, self.height
        trunk, leaf = (ids['spruce_log'], ids['spruce_leaves']) if spruce else (ids['oak_log'], ids['oak_leaves'])
        th = max(2, int(round((5 + int(_h01(self._s[3] ^ _U(44), np.int64(tx), np.int64(tz)) * 1.4 + 1.4)) * size)))
        rad = max(1, int(round(2 * size)))
        y0 = top + 1 - miny
        for dy in range(th):
            y = y0 + dy
            if 0 <= y < H and 0 <= lx < 16 and 0 <= lz < 16:
                blocks[y, lz, lx] = trunk
        for dy in range(-1, rad + 1):
            r = rad if dy <= 0 else max(0, rad - dy)
            y = y0 + th - 1 + dy
            if not 0 <= y < H:
                continue
            xs, xe, zs, ze = max(0, lx - r), min(16, lx + r + 1), max(0, lz - r), min(16, lz + r + 1)
            if xs >= xe or zs >= ze:
                continue
            sub = blocks[y, zs:ze, xs:xe]
            if r > 1:
                zz, xx = np.ogrid[zs:ze, xs:xe]
                keep = ~((np.abs(xx - lx) == r) & (np.abs(zz - lz) == r))
                sub[(sub == 0) & keep] = leaf
            else:
                sub[sub == 0] = leaf

    def _blob(self, blocks, ids, x, y, z, rad, ore, host=None):
        H, miny = self.height, self.min_y
        r = int(np.ceil(rad))
        y_i = y - miny
        ys, ye = max(0, y_i - r), min(H, y_i + r + 1)
        xs, xe, zs, ze = max(0, x - r), min(16, x + r + 1), max(0, z - r), min(16, z + r + 1)
        if ys >= ye or xs >= xe or zs >= ze:
            return
        sub = blocks[ys:ye, zs:ze, xs:xe]
        yy, zz, xx = np.ogrid[ys:ye, zs:ze, xs:xe]
        m = ((yy - y_i) ** 2 + (zz - z) ** 2 + (xx - x) ** 2) <= rad * rad
        host_ids = [host] if host is not None else [ids['stone'], ids['deepslate']]
        m &= np.isin(sub, host_ids)
        sub[m] = ore

    def _structures(self, blocks, ids, cx, cz, top):
        """«Постройки» макета: каменная башенка в одном из чанков каждой ячейки 6×6 чанков (частота — structure_frequency)."""
        if self.dim != 0:
            return
        freq = self.tweaks['structure_frequency']
        if freq <= 0:
            return
        cell = 6
        gx, gz = cx // cell, cz // cell
        s = self._s[2]
        pick = (int((_h01(s ^ _U(51), np.int64(gx), np.int64(gz)) * 0.5 + 0.5) * cell) + gx * cell, int((_h01(s ^ _U(52), np.int64(gx), np.int64(gz)) * 0.5 + 0.5) * cell) + gz * cell)
        if (cx, cz) != pick:
            return
        if (_h01(s ^ _U(53), np.int64(gx), np.int64(gz)) * 0.5 + 0.5) > min(1.0, 0.7 * freq):
            return
        base = int(top[8, 8])
        if base < self.sea_level:
            return
        y0 = base + 1 - self.min_y
        H = self.height
        for dy in range(0, 9):
            y = y0 + dy
            if y >= H:
                break
            for z in range(5, 12):
                for x in range(5, 12):
                    edge = x in (5, 11) or z in (5, 11)
                    if dy == 0 or dy == 8:
                        blocks[y, z, x] = ids['stone_bricks']
                    elif edge:
                        door = dy in (1, 2) and x == 8 and z == 5
                        blocks[y, z, x] = 0 if door else ids['cobblestone']
                    else:
                        blocks[y, z, x] = 0

    def generate_region(self, cx0, cz0, nx, nz, stages=MC_STAGE_ALL, threads=0, progress=None):
        if nx <= 0 or nz <= 0 or nx > 4096 or nz > 4096:
            raise McError(MCGEN_E_ARG, 'Invalid region size', 'mcgen_generate_region')
        coords = [(cx0 + i, cz0 + j) for j in range(nz) for i in range(nx)]
        chunks = {}
        total = len(coords)
        # Один поток: numpy под GIL на мелких массивах с несколькими потоками работает МЕДЛЕННЕЕ (замерено: 8×8 чанков 1.8 с на 1 потоке,
        # 4.7–7 с на 4–8). Параметр threads принят ради совместимости с настоящей библиотекой и игнорируется.
        if progress and progress(0.0, 'biomes'):
            raise McCancelled(MCGEN_E_CANCEL, 'cancelled', 'mcgen_generate_region')
        for done, cc in enumerate(coords, 1):
            chunks[cc] = self._gen_chunk(cc[0], cc[1], stages)
            if progress and progress(done / total, 'terrain'):
                raise McCancelled(MCGEN_E_CANCEL, 'cancelled', 'mcgen_generate_region')
        return McRegion(self, RegionInfo(cx0, cz0, nx, nz, self.min_y, self.height), stages, chunks)


class McRegion:
    def __init__(self, world, info, stages, chunks):
        self.world = world
        self.info = info
        self.stages = stages
        self._ch = chunks
        self._closed = False

    def close(self):
        self._ch = {}
        self._closed = True

    def has_chunk(self, cx, cz):
        i = self.info
        return i.cx0 <= cx < i.cx0 + i.nx and i.cz0 <= cz < i.cz0 + i.nz

    def chunks(self):
        i = self.info
        return [(cx, cz) for cz in range(i.cz0, i.cz0 + i.nz) for cx in range(i.cx0, i.cx0 + i.nx)]

    def _get(self, cx, cz):
        try:
            return self._ch[(cx, cz)]
        except KeyError:
            raise IndexError('no data at this address (chunk outside the region?)')

    def blocks(self, cx, cz):
        return self._get(cx, cz)[0]

    def biomes(self, cx, cz):
        return self._get(cx, cz)[1]

    def heightmap(self, cx, cz, kind=MC_HM_WORLD_SURFACE):
        return self._get(cx, cz)[2][int(kind)]

    def memory_bytes(self):
        i = self.info
        return (i.height * 256 * 2 + (i.height // 4) * 16 + 4 * 256 * 2) * i.nx * i.nz

    def write_mcr(self, path):
        import struct
        i = self.info
        g = self.world.gen
        with open(path, 'wb') as f:
            f.write(b'MCR1' + struct.pack('<8i', 1, i.cx0, i.cz0, i.nx, i.nz, i.min_y, i.height, self.stages))
            for cx, cz in self.chunks():
                b, bi, hm = self._ch[(cx, cz)]
                f.write(b.astype('<u2').tobytes() + bi.tobytes() + hm.astype('<i2').tobytes())
            for names in (g.block_names(), g.biome_names()):
                f.write(struct.pack('<I', len(names)))
                for s in names:
                    e = (s or '').encode()
                    f.write(struct.pack('<H', len(e)) + e)


def version_string():
    return 'libmcgen MOCK (numpy macros, not Minecraft)'


def available():
    return True, version_string()

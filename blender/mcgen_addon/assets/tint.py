"""Оттенки биомов: colormaps (grass / foliage / dry_foliage), переопределения цветов в JSON биомов, цвет воды,
grass_color_modifier (swamp, dark_forest), правила тонировки блоков (BlockColors игры 26.x). Чистый Python, без bpy.

Тонировка в игре: цвет = среднее по окну (2r+1)² соседних XZ-позиций цвета биома в этой позиции (r = «Biome blend», по умолчанию 2).
Здесь биомы берутся из клеток 4×4×4 чанка без «размытия» BiomeManager (см. отличия в docs/blender/assets-mesh.md).
"""
import json
import os

import numpy as np

from . import jrand
from . import pngio

__all__ = ['NONE', 'CONST', 'GRASS', 'FOLIAGE', 'DRY_FOLIAGE', 'WATER', 'MOD_NONE', 'MOD_DARK_FOREST', 'MOD_SWAMP',
           'load_colormaps', 'colormap_get', 'load_biomes', 'BiomeColors', 'tint_sources', 'swamp_perm', 'simplex2', 'swamp_is_dark',
           'DEFAULT_BLEND_RADIUS']

NONE, CONST, GRASS, FOLIAGE, DRY_FOLIAGE, WATER = 0, 1, 2, 3, 4, 5
MOD_NONE, MOD_DARK_FOREST, MOD_SWAMP = 0, 1, 2
DEFAULT_BLEND_RADIUS = 2

# значения по умолчанию, если в датапаке нет биома/цвета
FOLIAGE_DEFAULT = (-12012264) & 0xFFFFFF
GRASS_DEFAULT = (-65281) & 0xFFFFFF
DRY_FOLIAGE_DEFAULT = (-10732494) & 0xFFFFFF
WATER_DEFAULT = 0x3F76E4


def load_colormaps(assets_dir):
    """-> {'grass': uint32[65536] (RGB, 0xRRGGBB), 'foliage': ..., 'dry_foliage': ...}. assets_dir — `.../assets/minecraft`."""
    out = {}
    for name in ('grass', 'foliage', 'dry_foliage'):
        p = os.path.join(assets_dir, 'textures', 'colormap', name + '.png')
        if os.path.isfile(p):
            a = pngio.read_png(p)
            if a.shape[:2] != (256, 256):
                raise ValueError('colormap %s: ожидается 256×256, получено %r' % (name, a.shape))
            out[name] = ((a[:, :, 0].astype(np.uint32) << 16) | (a[:, :, 1].astype(np.uint32) << 8) | a[:, :, 2].astype(np.uint32)).reshape(-1)
        else:
            out[name] = None
    return out


def colormap_get(pixels, temp, rain, default):
    """ColorMapColorUtil.get: rain *= temp; x = (int)((1 - temp) * 255); y = (int)((1 - rain) * 255); pixel[y << 8 | x]."""
    if pixels is None:
        return default
    rain = rain * temp
    x = int((1.0 - temp) * 255.0)
    y = int((1.0 - rain) * 255.0)
    i = (y << 8) | x
    return int(pixels[i]) if 0 <= i < len(pixels) else default


def _color(v, default=None):
    """'#rrggbb' | int (в т.ч. отрицательный ARGB) -> 0xRRGGBB."""
    if v is None:
        return default
    if isinstance(v, str):
        s = v.strip()
        if s.startswith('#'):
            s = s[1:]
        return int(s, 16) & 0xFFFFFF
    return int(v) & 0xFFFFFF


def load_biomes(pack_dir):
    """Читает worldgen/biome/*.json (пространство имён minecraft) -> {'minecraft:plains': {temperature, downfall, ...}}."""
    base = os.path.join(pack_dir, 'data', 'minecraft', 'worldgen', 'biome')
    out = {}
    if not os.path.isdir(base):
        return out
    for root, _, files in os.walk(base):
        for f in sorted(files):
            if not f.endswith('.json'):
                continue
            rel = os.path.relpath(os.path.join(root, f), base)[:-5].replace(os.sep, '/')
            with open(os.path.join(root, f), encoding='utf-8') as fh:
                d = json.load(fh)
            eff = d.get('effects', {})
            out['minecraft:' + rel] = {
                'temperature': float(d.get('temperature', 0.5)),
                'downfall': float(d.get('downfall', 0.5)),
                'water_color': _color(eff.get('water_color'), WATER_DEFAULT),
                'foliage_color': _color(eff.get('foliage_color')),
                'dry_foliage_color': _color(eff.get('dry_foliage_color')),
                'grass_color': _color(eff.get('grass_color')),
                'grass_color_modifier': eff.get('grass_color_modifier', 'none'),
            }
    return out


class BiomeColors:
    """Таблица цветов по id биома (u8): rgb[n, 4] = (трава, листва, сухая листва, вода), mod[n] — модификатор травы."""

    def __init__(self, biome_names, biomes, colormaps):
        n = len(biome_names)
        self.names = list(biome_names)
        self.rgb = np.zeros((max(n, 1), 4), dtype=np.uint32)
        self.mod = np.zeros(max(n, 1), dtype=np.uint8)
        gm, fm, dm = colormaps.get('grass'), colormaps.get('foliage'), colormaps.get('dry_foliage')
        for i, name in enumerate(biome_names):
            b = biomes.get(name if ':' in name else 'minecraft:' + name)
            if b is None:
                g, f, d, w = GRASS_DEFAULT, FOLIAGE_DEFAULT, DRY_FOLIAGE_DEFAULT, WATER_DEFAULT
                mod = MOD_NONE
            else:
                t = min(max(b['temperature'], 0.0), 1.0)
                r = min(max(b['downfall'], 0.0), 1.0)
                g = b['grass_color'] if b['grass_color'] is not None else colormap_get(gm, t, r, GRASS_DEFAULT)
                f = b['foliage_color'] if b['foliage_color'] is not None else colormap_get(fm, t, r, FOLIAGE_DEFAULT)
                d = b['dry_foliage_color'] if b['dry_foliage_color'] is not None else colormap_get(dm, t, r, DRY_FOLIAGE_DEFAULT)
                w = b['water_color']
                m = b['grass_color_modifier']
                mod = MOD_DARK_FOREST if m == 'dark_forest' else (MOD_SWAMP if m == 'swamp' else MOD_NONE)
                if mod == MOD_DARK_FOREST:
                    g = (((g & 0xFEFEFE) + 0x28340A) >> 1) & 0xFFFFFF
            self.rgb[i] = (g, f, d, w)
            self.mod[i] = mod

    def grass_swamp(self, dark):
        return 0x4C763C if dark else 0x6A7039


# ----------------------------------------------------------------------------------------------------------------------
#                                       шум болот (Biome.BIOME_INFO_NOISE: SimplexNoise 2D)
# ----------------------------------------------------------------------------------------------------------------------
_SQRT3 = 3.0 ** 0.5
_F2 = 0.5 * (_SQRT3 - 1.0)
_G2 = (3.0 - _SQRT3) / 6.0
_GRAD = ((1, 1, 0), (-1, 1, 0), (1, -1, 0), (-1, -1, 0), (1, 0, 1), (-1, 0, 1), (1, 0, -1), (-1, 0, -1),
         (0, 1, 1), (0, -1, 1), (0, 1, -1), (0, -1, -1), (1, 1, 0), (0, -1, 1), (-1, 1, 0), (0, -1, -1))


def swamp_perm(seed=2345):
    """Таблица перестановок SimplexNoise(WorldgenRandom(LegacyRandomSource(2345)), discardNoiseOffset=true)."""
    r = jrand.LegacyRandom(seed)
    r.next_double()
    r.next_double()
    r.next_double()
    p = list(range(256))
    for i in range(256):
        off = r.next_int(256 - i)
        p[i], p[i + off] = p[i + off], p[i]
    return np.array(p, dtype=np.uint8)


def simplex2(perm, xin, yin):
    """SimplexNoise.get(x, y) (результат приводится к float как в игре)."""
    import math
    s = (xin + yin) * _F2
    i = math.floor(xin + s)
    j = math.floor(yin + s)
    t = (i + j) * _G2
    x0 = xin - (i - t)
    y0 = yin - (j - t)
    if x0 > y0:
        i1, j1 = 1, 0
    else:
        i1, j1 = 0, 1
    x1 = x0 - i1 + _G2
    y1 = y0 - j1 + _G2
    x2 = x0 - 1.0 + 2.0 * _G2
    y2 = y0 - 1.0 + 2.0 * _G2
    ii = i & 0xFF
    jj = j & 0xFF

    def P(v):
        return int(perm[v & 0xFF])
    gi0 = P(ii + P(jj)) % 12
    gi1 = P(ii + i1 + P(jj + j1)) % 12
    gi2 = P(ii + 1 + P(jj + 1)) % 12

    def corner(gi, x, y):
        t0 = 0.5 - x * x - y * y
        if t0 < 0.0:
            return 0.0
        t0 *= t0
        g = _GRAD[gi]
        return t0 * t0 * (g[0] * x + g[1] * y)
    v = 70.0 * (corner(gi0, x0, y0) + corner(gi1, x1, y1) + corner(gi2, x2, y2))
    return float(np.float32(v))


def swamp_is_dark(perm, x, z):
    """GrassColorModifier.SWAMP: noise(x*0.0225, z*0.0225) < -0.1 -> тёмный (0x4C763C), иначе 0x6A7039."""
    return simplex2(perm, x * 0.0225, z * 0.0225) < -0.1


# ----------------------------------------------------------------------------------------------------------------------
#                                       правила тонировки блоков (BlockColors.createDefault)
# ----------------------------------------------------------------------------------------------------------------------
_WHITE = (CONST, 0xFFFFFF)


def _const(argb):
    return (CONST, argb & 0xFFFFFF)


def _redstone(power):
    p = np.float32(power) / np.float32(15.0)
    red = p * np.float32(0.6) + (np.float32(0.4) if p > 0 else np.float32(0.3))
    green = np.clip(p * p * np.float32(0.7) - np.float32(0.5), 0, 1)
    blue = np.clip(p * p * np.float32(0.6) - np.float32(0.7), 0, 1)
    f = lambda v: int(np.floor(np.float32(v) * np.float32(255.0)))
    return (CONST, (f(red) << 16) | (f(green) << 8) | f(blue))


_GRASS1 = ((GRASS, 0),)
_TINTS = {}
for _n in ('large_fern', 'tall_grass', 'fern', 'short_grass', 'potted_fern', 'bush', 'grass_block', 'sugar_cane'):
    _TINTS['minecraft:' + _n] = _GRASS1
for _n in ('pink_petals', 'wildflowers'):
    _TINTS['minecraft:' + _n] = (_WHITE, (GRASS, 0))
_TINTS['minecraft:spruce_leaves'] = (_const(-10380959),)
_TINTS['minecraft:birch_leaves'] = (_const(-8345771),)
for _n in ('oak_leaves', 'jungle_leaves', 'acacia_leaves', 'dark_oak_leaves', 'vine', 'mangrove_leaves'):
    _TINTS['minecraft:' + _n] = ((FOLIAGE, 0),)
_TINTS['minecraft:leaf_litter'] = ((DRY_FOLIAGE, 0),)
_TINTS['minecraft:water_cauldron'] = ((WATER, 0),)
for _n in ('water', 'bubble_column'):
    _TINTS['minecraft:' + _n] = ((WATER, 0),)
_TINTS['minecraft:attached_melon_stem'] = (_const(-2046180),)
_TINTS['minecraft:attached_pumpkin_stem'] = (_const(-2046180),)
_TINTS['minecraft:lily_pad'] = (_const(-14647248),)


def tint_sources(block_name, props):
    """Список источников оттенка по tintindex: [(вид, rgb)], вид — NONE/CONST/GRASS/FOLIAGE/DRY_FOLIAGE/WATER. Для CONST rgb = цвет."""
    t = _TINTS.get(block_name)
    if t is not None:
        return t
    if block_name.endswith('_banner'):
        from . import entity_models
        return (_const(entity_models.DYE_RGB[entity_models.banner_color(block_name[10:])]),)
    if block_name == 'minecraft:redstone_wire':
        return (_redstone(int(props.get('power', '0'))),)
    if block_name in ('minecraft:melon_stem', 'minecraft:pumpkin_stem'):
        age = int(props.get('age', '0'))
        return (_const((age * 32 << 16) | ((255 - age * 8) << 8) | (age * 4)),)
    return ()

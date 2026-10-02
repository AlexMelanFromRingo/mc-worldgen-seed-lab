"""Палитры цветов биомов для предпросмотра «Biome Map» (без bpy).

Режимы: 'MAP' — таблица узнаваемых цветов карты (для неизвестных биомов — хэш), 'HASH' — детерминированный цвет из имени,
'JSON' — цвет из JSON биома датапака (water_color для океанов/рек, иначе grass_color биома или значение из colormap grass.png по
температуре и влажности).
"""
import json
import os
import zlib

import numpy as np

CURATED = {
    'badlands': 0xD94515, 'bamboo_jungle': 0x768E14, 'basalt_deltas': 0x403636, 'beach': 0xFADE55, 'birch_forest': 0x307444,
    'cherry_grove': 0xF2A3C6, 'cold_ocean': 0x202070, 'crimson_forest': 0xDD0808, 'dappled_forest': 0x4F8A3A, 'dark_forest': 0x40511A,
    'deep_cold_ocean': 0x202038, 'deep_dark': 0x0A2E3A, 'deep_frozen_ocean': 0x404090, 'deep_lukewarm_ocean': 0x000040, 'deep_ocean': 0x000030,
    'desert': 0xFA9418, 'dripstone_caves': 0x7A5C48, 'end_barrens': 0x8E88C4, 'end_highlands': 0xB3AEE0, 'end_midlands': 0x9B96CC,
    'eroded_badlands': 0xFF6D3D, 'flower_forest': 0x2D8E49, 'forest': 0x056621, 'frozen_ocean': 0x7070D6, 'frozen_peaks': 0xB0C4D8,
    'frozen_river': 0xA0A0FF, 'grove': 0x6FA38C, 'ice_spikes': 0xB4DCDC, 'jagged_peaks': 0xDCDCC8, 'jungle': 0x537B09, 'lukewarm_ocean': 0x000090,
    'lush_caves': 0x5BA13C, 'mangrove_swamp': 0x67352B, 'meadow': 0x83BB6D, 'mushroom_fields': 0xFF00FF, 'nether_wastes': 0x572526,
    'ocean': 0x000070, 'old_growth_birch_forest': 0x589C6C, 'old_growth_pine_taiga': 0x596651, 'old_growth_spruce_taiga': 0x818E79,
    'pale_garden': 0xA9B2A0, 'plains': 0x8DB360, 'river': 0x0000FF, 'savanna': 0xBDB25F, 'savanna_plateau': 0xA79D64, 'small_end_islands': 0xA8A4D8,
    'snowy_beach': 0xFAF0C0, 'snowy_plains': 0xFFFFFF, 'snowy_slopes': 0xCCD8E4, 'snowy_taiga': 0x31554A, 'soul_sand_valley': 0x5E3830,
    'sparse_jungle': 0x628B17, 'stony_peaks': 0x888888, 'stony_shore': 0xA2A284, 'sulfur_caves': 0xC9B800, 'sunflower_plains': 0xB5DB88,
    'swamp': 0x07F9B2, 'taiga': 0x0B6659, 'the_end': 0x7B77B8, 'the_void': 0x000000, 'warm_ocean': 0x0000AC, 'warped_forest': 0x49907B,
    'windswept_forest': 0x589C6F, 'windswept_gravelly_hills': 0x789878, 'windswept_hills': 0x606060, 'windswept_savanna': 0xE5DA87,
    'wooded_badlands': 0xCA8C65,
}


def short(name):
    return name.split(':', 1)[-1]


def _rgb(h):
    return ((h >> 16) & 255) / 255.0, ((h >> 8) & 255) / 255.0, (h & 255) / 255.0


def hash_color(name):
    """Детерминированный цвет из имени биома: оттенок из crc32, насыщенность и яркость в приятных пределах."""
    import colorsys
    h = zlib.crc32(short(name).encode())
    hue = (h & 0xFFFF) / 65535.0
    sat = 0.45 + ((h >> 16) & 0xFF) / 255.0 * 0.40
    val = 0.62 + ((h >> 24) & 0xFF) / 255.0 * 0.33
    return colorsys.hsv_to_rgb(hue, sat, val)


def _hex(s):
    if isinstance(s, str) and s.startswith('#') and len(s) in (7, 9):
        return _rgb(int(s[1:7], 16))
    if isinstance(s, int):
        return _rgb(s & 0xFFFFFF)
    return None


def _load_colormap(assets_dir):
    if not assets_dir:
        return None
    p = os.path.join(assets_dir, 'assets', 'minecraft', 'textures', 'colormap', 'grass.png')
    if not os.path.isfile(p):
        return None
    from .png import read_png
    try:
        return read_png(p)[0]
    except (OSError, ValueError):
        return None


def json_color(name, pack_dir, colormap=None):
    """Цвет из JSON биома; None, если файла нет."""
    if not pack_dir:
        return None
    p = os.path.join(pack_dir, 'data', 'minecraft', 'worldgen', 'biome', short(name) + '.json')
    try:
        with open(p, encoding='utf-8') as f:
            d = json.load(f)
    except (OSError, ValueError):
        return None
    eff = d.get('effects', {})
    n = short(name)
    if 'ocean' in n or n.endswith('river'):
        c = _hex(eff.get('water_color'))
        if c:
            return c
    c = _hex(eff.get('grass_color'))
    if c:
        return c
    t = min(max(float(d.get('temperature', 0.5)), 0.0), 1.0)
    dn = min(max(float(d.get('downfall', 0.5)), 0.0), 1.0) * t
    if colormap is not None:
        px = colormap[int((1.0 - dn) * 255), int((1.0 - t) * 255)]
        return tuple(float(x) / 255.0 for x in px[:3])
    return _hex(eff.get('water_color'))


def palette(names, mode='MAP', pack_dir=None, assets_dir=None):
    """Палитра (n, 3) float32 sRGB по списку имён биомов (индекс = id биома)."""
    cm = _load_colormap(assets_dir) if mode == 'JSON' else None
    out = np.zeros((len(names), 3), np.float32)
    for i, name in enumerate(names):
        c = None
        n = short(name or '')
        if mode == 'MAP':
            c = _rgb(CURATED[n]) if n in CURATED else None
        elif mode == 'JSON':
            c = json_color(name, pack_dir, cm)
        out[i] = c if c is not None else hash_color(n or str(i))
    return out


def colorize(grid, pal):
    """Сетка id биомов (h, w) uint8 -> RGBA float32 (h, w, 4); id вне палитры — серый."""
    ids = np.minimum(grid.astype(np.int64), len(pal) - 1)
    rgba = np.ones(grid.shape + (4,), np.float32)
    rgba[..., :3] = pal[ids]
    return rgba

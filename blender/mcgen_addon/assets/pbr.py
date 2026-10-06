"""PBR-карты для атласа блоков: нормали, шероховатость, металличность, свечение. Чистый Python (numpy), без bpy.

В ванильных ресурсах PBR-текстур нет (ни одной normal/specular среди ≈4000 PNG), поэтому карты выводятся из самого атласа — одним проходом, без разбора
каждого блока вручную:

* класс спрайта — по имени (≈50 правил: камень, кирпич, дерево, металл, стекло, лёд, руды, листва, шерсть, светящиеся блоки…), всё остальное — класс
  по умолчанию; одно правило покрывает десятки спрайтов (`*_planks`, `*_ore`, `*_copper*`, `*_froglight*`);
* высота — яркость пикселя (кирпичный раствор темнее — углублён), нормализованная по спрайту; нормаль — свёртка Собеля по высоте с замыканием по краям
  спрайта (текстуры тайлятся), сила зависит от класса (камень и кора — сильно, стекло, лёд и металлические блоки — гладкие);
* шероховатость — база класса ± небольшая добавка от яркости (тёмные щели грубее); металличность — константа класса (медь по степени окисления);
* руды — «самоцветы» отделяются от камня по расстоянию цвета до медианы спрайта: блестят и (у железа/золота/меди) металлические только вкрапления;
* свечение — по классу: целиком (лава, светокамень, морской фонарь), по яркости пикселя (факелы, тыква-фонарь, печь) или по насыщенности (магма, плачущий обсидиан).

Результат — два изображения атласа (одна выборка текстуры = одна карта): normal (RGB, касательное пространство, OpenGL: +Y — вверх по v Blender) и orm
(R — шероховатость, G — металличность, B — свечение). Строки атласа — как в `table.atlas_image` (строка 0 — верх).
"""
import re

import numpy as np

__all__ = ['PBR_VERSION', 'RULES', 'classify', 'build_pbr']

PBR_VERSION = 1

# свойства класса: rough (база), metal (0..1), nrm (сила нормали), var (изменение шероховатости от яркости), emit ('none'|'full'|'lum'|'sat'), ore (самоцветы отдельно),
# ore_metal (металличность самоцветов руды), ore_rough (их шероховатость)
_DEF = dict(rough=0.8, metal=0.0, nrm=0.5, var=0.10, emit='none', ore=False, ore_metal=0.0, ore_rough=0.3)


def _r(pattern, **kw):
    return (re.compile(pattern), kw)


# порядок важен: первое совпавшее правило побеждает (имя — без `minecraft:` и каталога `block/`)
RULES = [
    # --- жидкости и лёд
    _r(r'^water_', rough=0.04, nrm=0.0, var=0.0),
    _r(r'^lava_', rough=0.55, nrm=0.25, emit='full', var=0.0),
    _r(r'^(ice|frosted_ice_\d)$', rough=0.06, nrm=0.0, var=0.0),
    _r(r'^blue_ice$', rough=0.1, nrm=0.05, var=0.0),
    _r(r'^packed_ice$', rough=0.25, nrm=0.1, var=0.0),
    # --- стекло
    _r(r'(^|_)(glass|tinted_glass)(_|$)|stained_glass', rough=0.03, nrm=0.0, var=0.0),
    # --- свечение: целиком / по яркости / по насыщенности
    _r(r'^(glowstone|sea_lantern|shroomlight|redstone_lamp_on|firefly_bush_emissive|open_eyeblossom_emissive)$|_froglight_|^(fire|soul_fire)_\d|campfire_fire', rough=0.5, nrm=0.3, emit='full', var=0.0),
    _r(r'^(torch|soul_torch|redstone_torch|end_rod|lantern|soul_lantern|jack_o_lantern|nether_portal|glow_lichen|beacon|magma)$|copper_(lantern|torch)|_candle_lit|copper_bulb_lit|_front_on$|'
       r'lightning_rod_on|_on$|^respawn_anchor_(side[1-4]|top)$|trial_spawner_.*_active|vault_.*_on|sculk_sensor_tendril_active|^creaking_heart_(top_)?awake$',
       rough=0.45, nrm=0.35, emit='lum', var=0.05),
    _r(r'^crying_obsidian$', rough=0.1, nrm=0.2, emit='sat', var=0.05),
    # --- руды (самоцветы отдельно) — раньше металлов, иначе copper_ore попал бы в «медь»
    _r(r'_ore$|^ancient_debris', rough=0.9, nrm=1.0, ore=True, ore_metal=0.0),
    # --- металлы (медь по степени окисления)
    _r(r'^(raw_(iron|gold|copper)_block)$', rough=0.8, metal=0.35, nrm=0.9, var=0.12),
    _r(r'^oxidized_.*copper', rough=0.6, metal=0.25, nrm=0.25, var=0.12),
    _r(r'^weathered_.*copper', rough=0.5, metal=0.5, nrm=0.22, var=0.12),
    _r(r'^exposed_.*copper', rough=0.4, metal=0.75, nrm=0.2, var=0.12),
    _r(r'copper', rough=0.3, metal=1.0, nrm=0.18, var=0.12),
    _r(r'^(iron_block|iron_bars|iron_chain|iron_door_|iron_trapdoor|netherite_block|gold_block|heavy_core|anvil|chipped_anvil_top|damaged_anvil_top|hopper_|cauldron_|lightning_rod|tripwire_hook|grindstone_pivot)', rough=0.32, metal=1.0, nrm=0.15, var=0.15),
    _r(r'^(rail|rail_corner|powered_rail|powered_rail_on|detector_rail|detector_rail_on|activator_rail|activator_rail_on)$', rough=0.5, metal=0.8, nrm=0.3, var=0.1),
    # --- гладкие минералы и самоцветы
    _r(r'^(diamond_block|emerald_block|lapis_block|amethyst_block|budding_amethyst)$|amethyst_(cluster|bud)|^calcite$', rough=0.22, nrm=0.2, var=0.1),
    _r(r'^(quartz_|chiseled_quartz|smooth_quartz)|^purpur', rough=0.3, nrm=0.2, var=0.08),
    _r(r'^(redstone_block|coal_block)$', rough=0.45, nrm=0.3),
    _r(r'^(obsidian)$', rough=0.1, nrm=0.2, var=0.05),
    _r(r'^(slime_block|honey_block_|resin_block|resin_bricks|chiseled_resin|resin_clump)', rough=0.12, nrm=0.1, var=0.05),
    _r(r'glazed_terracotta$', rough=0.18, nrm=0.2, var=0.05),
    # --- камень и кирпичи
    _r(r'^(polished_|smooth_|chiseled_)', rough=0.45, nrm=0.35, var=0.08),
    _r(r'bricks$|^(stone|cobblestone|mossy_cobblestone|cobbled_deepslate|deepslate|tuff|andesite|diorite|granite|blackstone|basalt_|netherrack|end_stone|prismarine|dark_prismarine|'
       r'sandstone|red_sandstone|cut_sandstone|cut_red_sandstone|dripstone_block|sulfur|cinnabar|bedrock|reinforced_deepslate_)', rough=0.88, nrm=1.0, var=0.12),
    # --- дерево
    _r(r'^stripped_', rough=0.6, nrm=0.35, var=0.1),
    _r(r'_(log|stem|wood)(_top)?$|^bamboo_block|_stem_top$|^mangrove_roots|^muddy_mangrove_roots', rough=0.75, nrm=0.9, var=0.12),
    _r(r'_(planks)$|^bamboo_mosaic|_(door|trapdoor|sign|hanging_sign|shelf)(_.*)?$|^(bookshelf|chiseled_bookshelf|barrel_|crafting_table_|note_block|jukebox_|loom_|lectern_|composter_|cartography_|fletching_|smithing_)',
       rough=0.65, nrm=0.5, var=0.1),
    # --- листва, ткань, земля
    _r(r'_leaves$|^(azalea_|flowering_azalea_|moss_block|pale_moss_block|hay_block|nether_wart_block|warped_wart_block)', rough=0.9, nrm=0.6, var=0.1),
    _r(r'_wool$|^pale_moss_carpet|_bed_', rough=1.0, nrm=0.35, var=0.05),
    _r(r'_concrete$', rough=0.82, nrm=0.12, var=0.05),
    _r(r'_concrete_powder$', rough=0.95, nrm=0.5, var=0.08),
    _r(r'terracotta$', rough=0.7, nrm=0.4, var=0.08),
    _r(r'^(sand|red_sand|gravel|dirt|coarse_dirt|rooted_dirt|podzol_|mycelium_|soul_sand|soul_soil|mud|packed_mud|clay|farmland|dirt_path_|grass_block_|crimson_nylium|warped_nylium|suspicious_)', rough=0.96, nrm=0.8, var=0.08),
    _r(r'^(snow|powder_snow)$', rough=0.75, nrm=0.3, var=0.05),
    _r(r'^(sculk|spore_|mushroom_|red_mushroom_block|brown_mushroom_block|shelf_mushroom)', rough=0.85, nrm=0.5),
    # --- устройства, воск, плоды, яйца (чтобы не попадать в класс по умолчанию)
    _r(r'(^|_)lightning_rod', rough=0.32, metal=0.8, nrm=0.15, var=0.15),
    _r(r'^(spawner|lever|tripwire|jigsaw_|structure_block|test_)', rough=0.5, metal=0.6, nrm=0.4, var=0.1),
    _r(r'^(pointed_dripstone|sulfur_spike|potent_sulfur)', rough=0.88, nrm=0.9, var=0.12),
    _r(r'^(furnace|smoker|blast_furnace|dispenser|dropper|observer|piston|lodestone|crafter|command_block|chain_command_block|repeating_command_block|vault|trial_spawner|target|'
       r'cracked_deepslate|gilded|bone_block|enchanting|end_portal_frame|respawn_anchor|grindstone|calibrated|daylight|tnt|redstone_lamp|comparator|repeater)', rough=0.85, nrm=0.8, var=0.1),
    _r(r'_?candle$', rough=0.45, nrm=0.3, var=0.05),
    _r(r'^(pumpkin_|carved_pumpkin|melon_)', rough=0.55, nrm=0.35, var=0.08),
    _r(r'egg|^dragon_egg', rough=0.4, nrm=0.3, var=0.08),
    _r(r'^(scaffolding_|ladder|bee_nest|beehive|honeycomb_block|straw_bed|brewing_stand|cake_)', rough=0.7, nrm=0.5, var=0.1),
    _r(r'^(sponge|wet_sponge|cobweb|frogspawn)', rough=0.95, nrm=0.3, var=0.05),
    # --- растения (плоские, без рельефа)
    _r(r'grass|fern|flower|tulip|poppy|dandelion|orchid|allium|azure|cornflower|daisy|lily|rose|peony|lilac|sapling|vine|kelp|seagrass|roots|sprouts|bush|crop|stage\d|wheat|carrots|potatoes|'
       r'beetroots|stem$|cactus|sugar_cane|bamboo|dripleaf|cocoa|pickle|coral|fungus|wart|torchflower|pitcher|petals|wildflowers|leaf_litter|mangrove_propagule|firefly', rough=0.8, nrm=0.2, var=0.06),
]
_CACHE = {}


def _short(name):
    n = name
    if n.startswith('minecraft:'):
        n = n[len('minecraft:'):]
    if n.startswith('block/'):
        n = n[len('block/'):]
    return n


def classify(name):
    """Свойства класса спрайта по имени (словарь; индекс правила в ключе `rule`, −1 — класс по умолчанию)."""
    n = _short(name)
    hit = _CACHE.get(n)
    if hit is not None:
        return hit
    p = dict(_DEF)
    p['rule'] = -1
    if not n.startswith('entity/'):
        for i, (rx, kw) in enumerate(RULES):
            if rx.search(n):
                p.update(kw)
                p['rule'] = i
                break
    elif 'copper' in n:
        p.update(rough=0.35, metal=0.7, nrm=0.15, rule=-2)
    _CACHE[n] = p
    return p


def _sobel(h):
    """Свёртка Собеля по высоте с замыканием по краям: (dh/dx вправо, dh/dy вниз) / 8."""
    p = np.pad(h, 1, mode='wrap')
    gx = (p[:-2, 2:] + 2 * p[1:-1, 2:] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[1:-1, :-2] + p[2:, :-2])
    gy = (p[2:, :-2] + 2 * p[2:, 1:-1] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[:-2, 1:-1] + p[:-2, 2:])
    return gx / 8.0, gy / 8.0


def _sprite_maps(rgba, p):
    """rgba — uint8 (h, w, 4) спрайта. Возвращает (normal uint8 (h,w,3), orm uint8 (h,w,3))."""
    h, w = rgba.shape[:2]
    rgb = rgba[:, :, :3].astype(np.float32) / 255.0
    a = rgba[:, :, 3]
    solid = a > 8
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    if solid.any():
        mean = float(lum[solid].mean())
        std = float(lum[solid].std())
    else:
        mean, std = 0.5, 0.0
    hn = (lum - mean) / max(std, 0.05)                      # нормализованная высота (в «сигмах»); спрайты без контраста не раздуваются
    hn = np.clip(hn, -2.5, 2.5)
    hn = np.where(solid, hn, 0.0)
    # нормаль: n = (−dh/du, −dh/dv_up, 1); v вверх = строки вниз → dv_up = −dy
    gx, gy = _sobel(hn)
    k = 0.55 * float(p['nrm'])
    nx, ny, nz = -gx * k, gy * k, np.ones_like(gx)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    nrm = np.stack([nx * inv, ny * inv, nz * inv], axis=-1)
    normal = np.clip((nrm * 0.5 + 0.5) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    # шероховатость / металличность
    rough = np.full((h, w), p['rough'], np.float32) + p['var'] * np.clip(mean - lum, -0.5, 0.5) * 2.0
    metal = np.full((h, w), p['metal'], np.float32)
    if p['ore'] and solid.sum() >= 4:
        med = np.median(rgb[solid], axis=0)
        gem = (np.sqrt(((rgb - med) ** 2).sum(axis=-1)) * 255.0 > 48.0) & solid
        rough = np.where(gem, p['ore_rough'], rough)
        metal = np.where(gem, p['ore_metal'], metal)
    # свечение
    mx = rgb.max(axis=-1)
    mn = rgb.min(axis=-1)
    sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)
    if p['emit'] == 'full':
        emit = np.where(solid, 1.0, 0.0)
    elif p['emit'] == 'lum':
        emit = np.clip((lum - 0.45) / 0.35, 0.0, 1.0) * solid
    elif p['emit'] == 'sat':
        emit = np.clip((sat - 0.35) / 0.3, 0.0, 1.0) * np.clip((mx - 0.35) / 0.3, 0.0, 1.0) * solid
    else:
        emit = np.zeros((h, w), np.float32)
    orm = np.stack([np.clip(rough, 0.02, 1.0), np.clip(metal, 0.0, 1.0), np.clip(emit, 0.0, 1.0)], axis=-1)
    orm = np.where(solid[:, :, None], orm, np.array([1.0, 0.0, 0.0], np.float32))
    return normal, np.clip(orm * 255.0 + 0.5, 0, 255).astype(np.uint8)


def build_pbr(atlas_image, atlas_rect, atlas_names, pad=2):
    """Строит карты для всего атласа. atlas_image uint8 (H, W, 4); atlas_rect float32 (n, 4) = (u0, v0, u1, v1) спрайтов в долях атласа (без поля pad);
    atlas_names — имена спрайтов. Возвращает (normal uint8 (H,W,3), orm uint8 (H,W,3), статистика {правило: число спрайтов})."""
    H, W = atlas_image.shape[:2]
    normal = np.empty((H, W, 3), np.uint8)
    normal[:] = (128, 128, 255)
    orm = np.empty((H, W, 3), np.uint8)
    orm[:] = (204, 0, 0)
    stats = {}
    done = set()
    for i, name in enumerate(atlas_names):
        u0, v0, u1, v1 = atlas_rect[i]
        x0, y0, x1, y1 = int(round(u0 * W)), int(round(v0 * H)), int(round(u1 * W)), int(round(v1 * H))
        if x1 <= x0 or y1 <= y0 or (x0, y0, x1, y1) in done:
            continue                                         # missingno занимает одну ячейку на все недостающие имена
        done.add((x0, y0, x1, y1))
        p = classify(name)
        key = p['rule']
        stats[key] = stats.get(key, 0) + 1
        n_img, o_img = _sprite_maps(atlas_image[y0:y1, x0:x1], p)
        if pad:                                              # поле спрайта — протяжка краёв (как у альбедо)
            n_img = np.pad(n_img, ((pad, pad), (pad, pad), (0, 0)), mode='edge')
            o_img = np.pad(o_img, ((pad, pad), (pad, pad), (0, 0)), mode='edge')
        ya, xa = max(y0 - pad, 0), max(x0 - pad, 0)
        yb, xb = min(y1 + pad, H), min(x1 + pad, W)
        normal[ya:yb, xa:xb] = n_img[ya - (y0 - pad):yb - (y0 - pad), xa - (x0 - pad):xb - (x0 - pad)]
        orm[ya:yb, xa:xb] = o_img[ya - (y0 - pad):yb - (y0 - pad), xa - (x0 - pad):xb - (x0 - pad)]
    return normal, orm, stats

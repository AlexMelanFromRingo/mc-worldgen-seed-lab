#!/usr/bin/env python3
"""Полный каталог правил генерации, зависящих от ВЫСОТЫ / ТЕМПЕРАТУРЫ / УРОВНЕЙ (26.x, версия по умолчанию 26.3).
Автоматически разбирает ВСЕ данные датапака (placed_feature, material_rule, density_function, carver, structure, biome, noise_settings)
и Java-классы (скан уровня моря/границ высоты/карт высот). Выход: docs/08-height-temperature-rules.md и data/height-rules-<V>.json.
"""
import json, os, re, sys, glob, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V = sys.argv[1] if len(sys.argv) > 1 else '26.3'
BASE = f'{ROOT}/src/data-{V}/data/minecraft'
WG = f'{BASE}/worldgen'
SRC = f'{ROOT}/src/dec/{V}/net/minecraft/world/level'

def strip(i): return i.split(':', 1)[1] if isinstance(i, str) and ':' in i else i
def load_dir(name):
    out = {}
    d = f'{WG}/{name}'
    if os.path.isdir(d):
        for p in sorted(glob.glob(f'{d}/**/*.json', recursive=True)):
            out[os.path.relpath(p, d)[:-5]] = json.load(open(p))
    return out

FEAT = load_dir('feature' if os.path.isdir(f'{WG}/feature') else 'configured_feature')
PLACED = load_dir('placed_feature'); BIOME = load_dir('biome'); STRUCT = load_dir('structure'); SSET = load_dir('structure_set')
CARV = load_dir('carver' if os.path.isdir(f'{WG}/carver') else 'configured_carver'); DFUN = load_dir('density_function')
NSET = load_dir('noise_settings'); MRULE = load_dir('material_rule'); MCOND = load_dir('material_condition')

# ----------------------------------------------------------------- вспомогательные описатели
def anchor(a):
    if isinstance(a, dict):
        for k, v in a.items():
            if k == 'absolute': return f'y={v}'
            if k == 'above_bottom': return f'низ{v:+d}'
            if k == 'below_top': return f'верх{-v:+d}'
    return str(a)

def provider(p):
    if isinstance(p, (int, float)): return str(p)
    if not isinstance(p, dict): return str(p)
    t = strip(p.get('type', ''))
    if t in ('uniform',):
        if 'max_exclusive' in p: return f"U[{p.get('min_inclusive')}..{p.get('max_exclusive')})"
        return f"U[{p.get('min_inclusive')}..{p.get('max_inclusive')}]"
    if t == 'constant': return str(p.get('value'))
    if t == 'weighted_list': return 'взвеш.(' + ', '.join(f"{e['data']}:{e['weight']}" for e in p.get('distribution', [])) + ')'
    if t == 'clamped_normal': return f"норм(μ={p.get('mean')},σ={p.get('deviation')},[{p.get('min_inclusive')};{p.get('max_inclusive')}])"
    if t == 'biased_to_bottom': return f"к_низу[{p.get('min_inclusive')}..{p.get('max_inclusive')}]"
    if t == 'very_biased_to_bottom': return f"очень_к_низу[{p.get('min_inclusive')}..{p.get('max_inclusive')}]"
    return t + ' ' + json.dumps({k: v for k, v in p.items() if k != 'type'}, ensure_ascii=False)[:60]

def heightprov(h):
    t = strip(h.get('type', ''))
    mn, mx = anchor(h.get('min_inclusive')), anchor(h.get('max_inclusive'))
    extra = f", плато {h['plateau']}" if 'plateau' in h else ''
    return f"{ {'uniform': 'равномерно', 'trapezoid': 'трапеция', 'biased_to_bottom': 'к низу', 'very_biased_to_bottom': 'сильно к низу', 'constant': 'константа'}.get(t, t) } {mn}…{mx}{extra}" if t != 'constant' else f"константа {anchor(h.get('value'))}"

def pred(p):
    if not isinstance(p, dict): return str(p)
    t = strip(p.get('type') or p.get('predicate_type') or '')
    if t == 'matching_blocks':
        b = p.get('blocks'); b = [b] if isinstance(b, str) else b
        return 'блок∈' + ','.join(strip(x) for x in b)
    if t == 'matching_block_tag': return 'тег#' + strip(p.get('tag'))
    if t == 'matching_fluids':
        f = p.get('fluids'); f = [f] if isinstance(f, str) else f
        return 'жидкость∈' + ','.join(strip(x) for x in f)
    if t == 'would_survive': return 'выживет(' + strip((p.get('state') or {}).get('Name', p.get('state')) if isinstance(p.get('state'), dict) else p.get('state')) + ')'
    if t in ('solid', 'replaceable', 'true', 'unobstructed'): return t
    if t == 'not': return 'НЕ(' + pred(p.get('predicate')) + ')'
    if t in ('all_of', 'any_of'): return ('И' if t == 'all_of' else 'ИЛИ') + '(' + '; '.join(pred(x) for x in p.get('predicates', [])) + ')'
    if t == 'inside_world_bounds': return f"внутри_мира{p.get('offset', '')}"
    if t == 'has_sturdy_face': return 'прочная_грань ' + str(p.get('direction'))
    if t == 'matching_biomes': return 'биом∈' + str(p.get('biomes'))
    return t + ' ' + json.dumps({k: v for k, v in p.items() if k not in ('type', 'predicate_type')}, ensure_ascii=False)[:60]

def mod(m):
    t = strip(m.get('type', ''))
    if t == 'count': return '×' + provider(m['count'])
    if t == 'rarity_filter': return f"1/{m['chance']}"
    if t == 'in_square': return 'в чанке'
    if t == 'height_range': return 'y: ' + heightprov(m['height'])
    if t == 'heightmap': return f"карта высот {m['heightmap']}"
    if t == 'environment_scan':
        s = f"поиск {m['direction_of_search']} ≤{m['max_steps']}"
        if 'allowed_search_condition' in m: s += f" (путь: {pred(m['allowed_search_condition'])})"
        return s + f" до: {pred(m['target_condition'])}"
    if t == 'surface_relative_threshold_filter':
        return f"относит. поверхности {m.get('min_inclusive', '−∞')}…{m.get('max_inclusive', '+∞')} ({m['heightmap']})"
    if t == 'surface_water_depth_filter': return f"глубина воды ≤ {m['max_water_depth']}"
    if t == 'block_predicate_filter': return 'фильтр блока: ' + pred(m['predicate'])
    if t == 'biome': return 'биом'
    if t == 'offset':
        return 'сдвиг(' + ','.join(provider(m.get(k, 0)) for k in ('x', 'y', 'z')) + ')'
    if t == 'count_on_every_layer': return '×' + provider(m['count']) + ' на каждом слое'
    if t in ('noise_threshold_count', 'noise_based_count'):
        return t + ' ' + json.dumps({k: v for k, v in m.items() if k != 'type'})[:80]
    return t + ' ' + json.dumps({k: v for k, v in m.items() if k != 'type'}, ensure_ascii=False)[:70]

# ----------------------------------------------------------------- A. размещаемые фичи
STEPS = ['raw_generation', 'lakes', 'local_modifications', 'underground_structures', 'surface_structures', 'strongholds', 'underground_ores', 'underground_decoration', 'fluid_springs', 'vegetal_decoration', 'top_layer_modification']
used = collections.defaultdict(lambda: collections.defaultdict(set))
for b, d in BIOME.items():
    for s, lst in enumerate(d.get('features', [])):
        for f in lst: used[strip(f)][s].add(b)

def cfg_type(fid):
    pf = PLACED[fid].get('feature')
    if isinstance(pf, str):
        c = FEAT.get(strip(pf)); return strip(c.get('type', '?')) if c else '?'
    if isinstance(pf, dict): return strip(pf.get('type', 'inline'))
    return '?'

def category(fid, ctype):
    if fid.startswith('ore_') or ctype in ('ore', 'scattered_ore'): return 'руды и жилы камня'
    if fid.startswith('trees_') or 'tree' in fid or ctype == 'tree' or 'fungi' in fid: return 'деревья и грибы-деревья'
    if fid.startswith(('flower', 'patch_', 'bamboo', 'forest_flowers')) or ctype in ('random_patch', 'flower'): return 'трава, цветы, кусты'
    if any(k in fid for k in ('kelp', 'seagrass', 'sea_pickle', 'coral', 'warm_ocean', 'iceberg', 'blue_ice', 'ocean')): return 'вода и океан'
    if any(k in fid for k in ('lush', 'dripstone', 'moss', 'glow', 'cave', 'sculk', 'azalea', 'vines', 'spore', 'sulfur', 'amethyst', 'geode', 'monster', 'fossil', 'lake', 'spring')): return 'пещеры, подземелья, жидкости'
    if any(k in fid for k in ('basalt', 'blackstone', 'delta', 'nether', 'glowstone', 'magma', 'crimson', 'warped', 'weeping', 'twisting', 'quartz', 'debris', 'soul')): return 'Незер'
    if any(k in fid for k in ('end_', 'chorus')): return 'Энд'
    return 'прочее'

P = {}
for fid, d in PLACED.items():
    steps = sorted(used[fid].keys()); biomes = sorted({b for s in used[fid].values() for b in s})
    ct = cfg_type(fid)
    P[fid] = {'category': category(fid, ct), 'type': ct, 'steps': [STEPS[s] for s in steps if s < len(STEPS)] or [], 'biomes': biomes,
              'rules': [mod(m) for m in d.get('placement', []) if strip(m.get('type')) not in ('biome', 'in_square')]}

# ----------------------------------------------------------------- B. правила поверхности (развёрнуто)
def mcond(c):
    if isinstance(c, str):
        n = strip(c)
        return n if n not in MCOND else n
    t = strip(c.get('type', ''))
    if t == 'biome':
        b = c['biome_is']; b = [b] if isinstance(b, str) else b
        return 'биом∈' + ','.join(strip(x) for x in b)
    if t == 'noise_threshold': return f"шум {strip(c['noise'])}∈[{c.get('min_threshold')}; {c.get('max_threshold')}]"
    if t in ('y_above', 'y_below'):
        return f"y {'≥' if t == 'y_above' else '<'} {anchor(c.get('anchor'))}" + (f" (+{c.get('surface_depth_multiplier')}×глубина слоя)" if c.get('surface_depth_multiplier') else '') + (' [+stone_depth]' if c.get('add_stone_depth') else '')
    if t == 'water': return f"вода(offset={c.get('offset')},×{c.get('surface_depth_multiplier')})"
    if t == 'not': return 'НЕ(' + mcond(c.get('invert') or c.get('predicate')) + ')'
    if t == 'stone_depth': return f"stone_depth({c.get('surface_type')},offset={c.get('offset')},range={c.get('secondary_depth_range')})"
    if t == 'vertical_gradient': return f"градиент y {anchor(c.get('true_at_and_below'))}→{anchor(c.get('false_at_and_above'))} [{strip(c.get('random_name'))}]"
    if t in ('all_of',): return ' И '.join(mcond(x) for x in c.get('predicates', []))
    if t in ('any_of',): return '(' + ' ИЛИ '.join(mcond(x) for x in c.get('predicates', [])) + ')'
    if t in ('steep', 'hole', 'above_preliminary_surface', 'temperature'): return t
    return t + ' ' + json.dumps({k: v for k, v in c.items() if k != 'type'}, ensure_ascii=False)[:70]

def flatten(r, conds, out, depth=0, name=''):
    if isinstance(r, str):
        n = strip(r)
        if n in MRULE and depth < 8:
            flatten(MRULE[n], conds, out, depth + 1, n); return
        if n in MCOND and not conds:
            out.append((conds, 'условие ' + n)); return
        out.append((conds, '→ ' + n)); return
    t = strip(r.get('type', ''))
    if t == 'sequence':
        for s in r['sequence']: flatten(s, conds, out, depth, name)
    elif t == 'condition':
        c = r['if_true']
        if isinstance(c, str) and strip(c) in MCOND:
            cc = f"{strip(c)}{{{mcond(MCOND[strip(c)])}}}"
        else: cc = mcond(c)
        flatten(r['then_run'], conds + [cc], out, depth, name)
    elif t == 'block':
        rs = r['result_state']; out.append((conds, '→ ' + (strip(rs) if isinstance(rs, str) else strip(rs.get('Name', '?')))))
    else:
        out.append((conds, '→ ' + t + ' ' + json.dumps({k: v for k, v in r.items() if k != 'type'}, ensure_ascii=False)[:90]))

RULES = {}
for rn in ['overworld', 'overworld_caves', 'overworld_floating_islands', 'nether', 'end', 'bedrock_floor', 'bedrock_roof']:
    if rn in MRULE:
        o = []; flatten(MRULE[rn], [], o, 0, rn); RULES[rn] = o

# ----------------------------------------------------------------- C. y-узлы density-функций
def ynodes(x, out, path=''):
    if isinstance(x, dict):
        t = strip(x.get('type', ''))
        if t in ('y_clamped_gradient', 'gradient') and (t == 'y_clamped_gradient' or x.get('axis') == 'y'):
            fy = x.get('from_y', x.get('from_coordinate')); ty = x.get('to_y', x.get('to_coordinate'))
            out.append(f"градиент по y: y={fy}→{x.get('from_value')}, y={ty}→{x.get('to_value')} (вне — константа)")
        if t == 'range_choice' and x.get('input') in ('minecraft:y',):
            out.append(f"если y ∈ [{x.get('min_inclusive')}; {x.get('max_exclusive')}) → вариант А, иначе вариант Б")
        if t == 'slice' and x.get('axis') == 'y': out.append(f"срез по y={x.get('coordinate')}")
        if t == 'find_top_surface': out.append(f"поиск верха поверхности: шаг {x.get('cell_height')}, нижняя граница {x.get('lower_bound')}")
        for v in x.values(): ynodes(v, out)
    elif isinstance(x, list):
        for v in x: ynodes(v, out)
DY = {}
for k, d in DFUN.items():
    o = []; ynodes(d, o)
    if o: DY[k] = sorted(set(o))
NY = {}
for k, d in NSET.items():
    o = []; ynodes(d.get('noise_router'), o); ynodes(d.get('aquifers'), o)
    NY[k] = {'sea_level': d.get('sea_level'), 'min_y': d['noise']['min_y'], 'height': d['noise']['height'], 'default_block': strip(d.get('default_block')) if isinstance(d.get('default_block'), str) else d.get('default_block'),
             'default_fluid': strip(d['default_fluid']) if isinstance(d.get('default_fluid'), str) else d.get('default_fluid'), 'legacy_random': d.get('legacy_random_source'), 'y_nodes': sorted(set(o))}

# ----------------------------------------------------------------- D. карверы
CAV = {}
for k, d in CARV.items():
    c = d.get('config', d)
    CAV[k] = {'type': strip(d.get('type', '')), 'probability': c.get('probability'), 'y': (heightprov(c['y']) if isinstance(c.get('y'), dict) else c.get('y')),
              'lava_level': c.get('lava_level'), 'extra': {kk: provider(vv) for kk, vv in {**c, **(c.get('shape') or {})}.items() if kk in ('distance_factor', 'horizontal_radius_factor', 'vertical_radius_center_factor', 'vertical_radius_default_factor', 'width_smoothness', 'vertical_rotation', 'start_vertical_radius_multiplier', 'vertical_radius_multiplier', 'horizontal_radius_multiplier', 'room_vertical_radius_multiplier', 'floor_level', 'count', 'thickness', 'y_scale', 'weird_thickness_bias')}}
CB = collections.defaultdict(list)
for b, d in BIOME.items():
    cl = d.get('carvers'); cl = [cl] if isinstance(cl, str) else (cl or [])
    for c in cl: CB[strip(c)].append(b)

# ----------------------------------------------------------------- E. структуры
STR = {}
for k, d in STRUCT.items():
    e = {'type': strip(d.get('type', '')), 'step': d.get('step'), 'terrain_adaptation': d.get('terrain_adaptation', 'none'), 'biomes': d.get('biomes')}
    for key in ('start_height', 'project_start_to_heightmap', 'max_distance_from_center', 'size', 'use_expansion_hack', 'height_filter', 'is_beached', 'biome_temperature', 'mineshaft_type', 'probability', 'setups', 'start_jigsaw_name', 'liquid_settings', 'dimension_padding', 'pool_aliases'):
        if key in d: e[key] = d[key]
    if 'setups' in e:
        e['setups'] = [{'placement': s.get('placement'), 'air_pocket_probability': s.get('air_pocket_probability'), 'mossiness': s.get('mossiness'), 'cold': s.get('can_be_cold'), 'overgrown': s.get('overgrown'), 'vines': s.get('vines'), 'replace_with_blackstone': s.get('replace_with_blackstone'), 'weight': s.get('weight')} for s in e['setups']]
    STR[k] = e

# ----------------------------------------------------------------- F. биомы: температура / осадки
BT = []
for b, d in BIOME.items():
    t = d['temperature']; prec = d.get('has_precipitation', True); mod_ = d.get('temperature_modifier')
    if not prec: snow = 'осадков нет'
    elif t < 0.15: snow = 'снег на любой высоте'
    else:
        y = 80 + 800 * (t - 0.15); snow = 'снег выше y ≈ %d ± 8' % round(y) if y <= 320 else 'снега нет (порог %d > 320)' % round(y)
    BT.append((t, b, d.get('downfall'), prec, mod_, snow))
BT.sort()

# ----------------------------------------------------------------- G. Java: классы с логикой высот/уровня моря
JV = {}
pat = {'уровень моря': r'getSeaLevel\(|seaLevel|SEA_LEVEL', 'границы мира minY/maxY': r'getMinY\(|getMaxY\(|getMinBuildHeight|getMaxBuildHeight|getMinGenY|getGenDepth',
       'карты высот': r'Heightmap\.Types\.[A-Z_]+', 'getHeight/getBaseHeight': r'\.getHeight\(|getBaseHeight\(|getFirstOccupiedHeight|getFirstFreeHeight',
       'температура/снег/лёд': r'getTemperature|warmEnoughToRain|coldEnoughToSnow|shouldSnow|shouldFreeze|getPrecipitationAt|getBaseTemperature|TEMPERATURE'}
for d in ['levelgen/feature', 'levelgen/structure', 'levelgen/structure/structures', 'levelgen/carver', 'levelgen/placement', 'levelgen/material', 'biome', 'levelgen']:
    base = f'{SRC}/{d}'
    if not os.path.isdir(base): continue
    for f in sorted(os.listdir(base)):
        if not f.endswith('.java') or f == 'package-info.java': continue
        t = open(f'{base}/{f}', encoding='utf-8', errors='ignore').read()
        t = re.sub(r'/\*.*?\*/', '', t, flags=re.S); t = re.sub(r'//.*', '', t)
        hits = {k: sorted(set(re.findall(p, t))) if k == 'карты высот' else len(re.findall(p, t)) for k, p in pat.items()}
        hits = {k: v for k, v in hits.items() if v}
        if hits: JV[f'{d}/{f[:-5]}'] = hits

json.dump({'placed_features': P, 'surface_rules': {k: [[c, r] for c, r in v] for k, v in RULES.items()}, 'density_y_nodes': DY, 'noise_settings': NY,
           'carvers': CAV, 'carver_biomes': CB, 'structures': STR, 'biome_temperature': BT, 'java_height_logic': JV}, open(f'{ROOT}/data/height-rules-{V}.json', 'w'), indent=1, ensure_ascii=False, default=str)

# ----------------------------------------------------------------- вывод Markdown
o = []; w = o.append
w(f'# 08. Каталог правил, зависящих от высоты, температуры и уровней ({V}) — генерируется автоматически\n')
w('Источник — **все** данные датапака и Java-код версии; скрипт `tools/audit_height_rules.py`, сырые данные `data/height-rules-%s.json`. Это полный перечень «если y … / если температура … / если вода …» из `placed_feature`, `material_rule`, `density_function`, `noise_settings`, `carver`, `structure`, `biome` и скан Java-классов. Пояснения к механикам — `docs/00-worldgen-guide.md` §10–15.\n' % V)
w('Обозначения высот: `y=N` — абсолютная; `низ±N` — от нижней границы мира (`above_bottom`); `верх∓N` — от верхней (`below_top`). Overworld: min_y −64, высота 384 (низ = −64, верх = 320); Nether/End: min_y 0, высота 128 (верх = 128).\n')

w('## 1. Базовые уровни по измерениям (`noise_settings`)\n')
w('| noise_settings | sea_level | min_y | высота | основной блок | основная жидкость | legacy RNG |\n|---|---|---|---|---|---|---|')
for k, e in NY.items():
    w(f"| `{k}` | {e['sea_level']} | {e['min_y']} | {e['height']} | {e['default_block']} | {e['default_fluid']} | {e['legacy_random']} |")
w('\nУровни жидкости в коде: Overworld — глобально `y < min(−54, sea_level)` → лава, иначе вода до sea_level (aquifer; `NoiseBasedChunkGenerator.createFluidPicker`); Nether — лава ниже y = 32 (`default_fluid = lava`, sea_level 32).\n')

w('## 2. y-зависимые узлы функций рельефа/пещер/aquifer\n')
w('Узлы density-функций, в которых явно участвует координата y (градиенты, `range_choice` по y, срезы). Включая `noise_router` и `aquifers` каждого `noise_settings`.\n')
w('### 2.1. По `noise_settings`\n')
for k, e in NY.items():
    if e['y_nodes']:
        w(f'* **`{k}`**')
        for n in e['y_nodes']: w(f'  * {n}')
w('\n### 2.2. По отдельным функциям (`density_function/*`)\n')
w('| Функция | y-узлы |\n|---|---|')
for k, v in sorted(DY.items()):
    w(f"| `{k}` | {'; '.join(v)} |")
w('')

w('## 3. Правила поверхности и материала (развёрнутые «условия → результат»)\n')
w('Полный разбор `material_rule/*` с раскрытием всех ссылок (26.3; в 26.1/26.2 то же в `SurfaceRules`). Порядок строк = приоритет (первое сработавшее правило выигрывает внутри своей `sequence`). Условия по высоте: `y ≥/< якорь`, `градиент y`, `вода(offset)` (под/над водой), `stone_depth` (глубина от поверхности), `steep` (крутой склон), `биом∈`, `шум∈` (пятна по шуму), `above_preliminary_surface`.\n')
for rn, lst in RULES.items():
    w(f'### 3.{list(RULES).index(rn) + 1}. `{rn}` ({len(lst)} правил)\n')
    w('```')
    for conds, res in lst:
        w(('если ' + '\n  И '.join(conds) + '\n    ' if conds else '') + res)
    w('```\n')

w('## 4. Размещаемые фичи: высота, карты высот, температура-зависимые фильтры (все %d)\n' % len(P))
w('«Правила» — модификаторы размещения по порядку (`биом` и `в чанке` опущены: они есть у каждой). Категория — эвристика по имени/типу. «Шаги» — шаги декорации биомов, где фича подключена; «Биомов» — сколько биомов её используют.\n')
cats = collections.defaultdict(list)
for fid, e in P.items(): cats[e['category']].append(fid)
for cat in sorted(cats):
    w(f'### 4.{sorted(cats).index(cat) + 1}. {cat} ({len(cats[cat])})\n')
    w('| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |\n|---|---|---|---|---|')
    for fid in sorted(cats[cat]):
        e = P[fid]
        w(f"| `{fid}` | {e['type']} | {','.join(e['steps']) or '—'} | {len(e['biomes'])} | {'; '.join(e['rules']) or '—'} |")
    w('')

w('## 5. Карверы (пещеры, разломы)\n')
w('| Карвер | Тип | Вероятность старта в чанке | y старта | lava_level (26.1/26.2) | Параметры | Биомов |\n|---|---|---|---|---|---|---|')
for k, c in CAV.items():
    w(f"| `{k}` | {c['type']} | {c['probability']} | {c['y']} | {c['lava_level'] if c['lava_level'] is not None else '—'} | {', '.join(f'{a}={b}' for a, b in c['extra'].items())} | {len(CB.get(k, []))} |")
w('')

w('## 6. Структуры: шаг, адаптация рельефа, высотные настройки (данные)\n')
w('Высотные ограничения, зашитые в **Java-код** структур (а не в JSON), перечислены в §8 (использование карт высот/уровня моря по классам).\n')
w('| Структура | Тип | Шаг | `terrain_adaptation` | Высотные/прочие настройки |\n|---|---|---|---|---|')
for k, e in sorted(STR.items()):
    ex = {kk: vv for kk, vv in e.items() if kk not in ('type', 'step', 'terrain_adaptation', 'biomes')}
    w(f"| `{k}` | {e['type']} | {e['step']} | {e['terrain_adaptation']} | {json.dumps(ex, ensure_ascii=False, default=str)[:260] if ex else '—'} |")
w('')

w('## 7. Температура, осадки, снег по биомам (все биомы)\n')
w('`T_base` — температура биома; снег/осадки по `Biome.getHeightAdjustedTemperature`: выше y = `sea_level + 17 = 80` температура падает: `T_adj = T_base − (v + y − 80)·0.05/40`, `v = 8·Simplex(x/8, z/8)` (константный шум 1234, ±8); снег при `T_adj < 0.15`. Порог высоты: `y ≈ 80 + 800·(T_base − 0.15) − v`.\n')
w('| T_base | Биом | downfall | Осадки | Модификатор | Снег |\n|---|---|---|---|---|---|')
for t, b, dn, prec, mm, snow in BT:
    w(f'| {t} | `{b}` | {dn} | {"да" if prec else "нет"} | {mm or "—"} | {snow} |')
w('')

w('## 8. Java-классы с логикой уровня моря / границ высоты / карт высот / температуры\n')
w('Скан исходников (`levelgen/feature`, `structure(s)`, `carver`, `placement`, `material`, `biome`, `levelgen`): сколько раз класс обращается к `getSeaLevel`, границам мира, `getHeight`/`Heightmap.Types.*`, функциям температуры/снега/льда. Это те места, где «высотная» логика живёт в коде, а не в JSON.\n')
w('| Класс | Использование |\n|---|---|')
for k, v in sorted(JV.items()):
    w(f"| `{k}` | {', '.join(f'{a}: {b}' for a, b in v.items())} |")
w('')
open(f'{ROOT}/docs/08-height-temperature-rules.md', 'w').write('\n'.join(o) + '\n')
print('docs/08-height-temperature-rules.md', len('\n'.join(o)), 'символов; placed', len(P), 'rules', {k: len(v) for k, v in RULES.items()}, 'java', len(JV))

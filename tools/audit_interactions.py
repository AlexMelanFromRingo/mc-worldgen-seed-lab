#!/usr/bin/env python3
"""Систематический аудит взаимосвязей worldgen (слой L1: данные датапака).

Что делает (для версии V, по умолчанию 26.3):
  1. Разбирает ВСЕ реестры worldgen из src/data-<V>: biome, placed_feature, feature(configured), carver, structure,
     structure_set, template_pool, noise_settings, density_function, noise, material_rule/material_condition, теги блоков.
  2. Для каждой фичи (включая вложенные/ссылочные) вычисляет множества БЛОКОВ, которые она
        - ЧИТАЕТ  (предикаты: matching_blocks / matching_block_tag / would_survive / replaceable / tag_match / target ...),
        - ПИШЕТ   (state / provider / fluid / barrier / result_state ...),
     а также используемые карты высот, шумы, scan-условия.
  3. По глобальному порядку фич (FeatureSorter, data/featureorder-<V>.json) строит потенциальные влияния
     «ранняя фича пишет блоки, которые поздняя читает» (с исключением вездесущих блоков).
  4. Сводит общие шумы/density-функции (кто их использует), структуры (шаг, terrain_adaptation, биомы), карверы.
Результат: data/interactions-<V>.json и docs/06-interactions.md (раздел L1; слои L2/L3 дописываются отдельно).
"""
import json, os, re, sys, collections, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V = sys.argv[1] if len(sys.argv) > 1 else '26.3'
BASE = f'{ROOT}/src/data-{V}/data/minecraft'
WG = f'{BASE}/worldgen'

UBIQ = {'air', 'cave_air', 'void_air', 'water', 'lava', 'stone', 'deepslate', 'dirt', 'grass_block', 'bedrock',
        'netherrack', 'end_stone', 'tuff', 'gravel', 'sand'}

def strip(i):
    return i.split(':', 1)[1] if ':' in i else i

def load_dir(name):
    out = {}
    d = f'{WG}/{name}'
    if not os.path.isdir(d):
        return out
    for p in glob.glob(f'{d}/**/*.json', recursive=True):
        rel = os.path.relpath(p, d)[:-5]
        try:
            out[rel] = json.load(open(p))
        except Exception:
            pass
    return out

BLOCKS = set(strip(l.strip()) for l in open(f'{ROOT}/data/blocks-{V}.txt') if l.strip())

# ----- теги блоков -----
TAGS = {}
for p in glob.glob(f'{BASE}/tags/block/**/*.json', recursive=True):
    rel = os.path.relpath(p, f'{BASE}/tags/block')[:-5]
    TAGS[rel] = json.load(open(p)).get('values', [])
_tag_cache = {}
def expand_tag(t):
    t = strip(t.lstrip('#'))
    if t in _tag_cache:
        return _tag_cache[t]
    _tag_cache[t] = set()
    res = set()
    for v in TAGS.get(t, []):
        v = v['id'] if isinstance(v, dict) else v
        if v.startswith('#'):
            res |= expand_tag(v)
        else:
            res.add(strip(v))
    _tag_cache[t] = res
    return res

feature_dir = 'feature' if os.path.isdir(f'{WG}/feature') else 'configured_feature'
carver_dir = 'carver' if os.path.isdir(f'{WG}/carver') else 'configured_carver'
FEAT = load_dir(feature_dir)
PLACED = load_dir('placed_feature')
BIOME = load_dir('biome')
STRUCT = load_dir('structure')
SSET = load_dir('structure_set')
CARV = load_dir(carver_dir)
DFUN = load_dir('density_function')
NOISE = load_dir('noise')
NSET = load_dir('noise_settings')
MRULE = load_dir('material_rule')

READ_KEYS = {'predicate', 'predicates', 'target', 'targets_test', 'rule', 'rules', 'allowed_tree_position', 'allowed_search_condition',
             'target_condition', 'can_replace_with_air_or_fluid', 'can_replace_with_barrier', 'can_place_feature', 'root_replaceable',
             'replaceable', 'base_block', 'ground', 'blocks', 'tag', 'filter', 'block_predicate', 'ignore_vines', 'valid_blocks'}
WRITE_HINT = {'state', 'to_place', 'result_state', 'provider', 'trunk_provider', 'foliage_provider', 'below_trunk_provider',
              'root_state_provider', 'hanging_root_state_provider', 'fluid', 'barrier', 'base_state', 'ceiling', 'floor',
              'dirt_provider', 'vegetation_feature', 'ground_state', 'replaceable_state'}

class Info:
    def __init__(self):
        self.reads = set(); self.writes = set(); self.read_tags = set(); self.noises = set(); self.heightmaps = set()
        self.types = set(); self.kids = set(); self.scan = []; self.modifiers = []

def walk(node, anc, info, depth=0):
    if depth > 40:
        return
    if isinstance(node, dict):
        t = node.get('type') or node.get('predicate_type') or ''
        if t:
            info.types.add(strip(t))
        if node.get('type') == 'minecraft:heightmap' or 'heightmap' in node and isinstance(node.get('heightmap'), str):
            info.heightmaps.add(node.get('heightmap'))
        if 'noise' in node and isinstance(node['noise'], str):
            info.noises.add(strip(node['noise']))
        for k, v in node.items():
            walk(v, anc + [k], info, depth + 1)
    elif isinstance(node, list):
        for v in node:
            walk(v, anc, info, depth + 1)
    elif isinstance(node, str):
        s = node
        is_tag = s.startswith('#') or (anc and anc[-1] == 'tag')
        name = strip(s.lstrip('#'))
        reading = any(a in READ_KEYS or a in ('predicate_type',) for a in anc)
        if is_tag:
            info.read_tags.add(name)
            info.reads |= expand_tag(name)
        elif name in BLOCKS:
            (info.reads if reading else info.writes).add(name)
        elif anc and anc[-1] in ('feature', 'features', 'configured_feature', 'placed_feature'):
            info.kids.add(strip(s))

def feature_info(fid, memo, stack=()):
    """Транзитивные reads/writes фичи (placed → configured → вложенные)."""
    if fid in memo:
        return memo[fid]
    info = Info(); memo[fid] = info
    if fid in stack:
        return info
    pf = PLACED.get(fid)
    cf = None
    if pf is not None:
        for m in pf.get('placement', []):
            info.modifiers.append(strip(m.get('type', '')))
            if m.get('type') == 'minecraft:heightmap':
                info.heightmaps.add(m.get('heightmap'))
            if m.get('type') == 'minecraft:environment_scan':
                info.scan.append((m.get('direction_of_search'), m.get('max_steps')))
            walk(m, ['placement'], info)
        feat = pf.get('feature')
        if isinstance(feat, str):
            cf = strip(feat)
            sub = feature_info_cfg(cf, memo, stack + (fid,))
            merge(info, sub)
        elif isinstance(feat, dict):
            walk(feat, ['feature'], info)
    elif fid in FEAT:
        merge(info, feature_info_cfg(fid, memo, stack))
    return info

def feature_info_cfg(cid, memo, stack):
    key = 'cfg:' + cid
    if key in memo:
        return memo[key]
    info = Info(); memo[key] = info
    node = FEAT.get(cid)
    if node is None:
        return info
    walk(node, [], info)
    for kid in list(info.kids):
        if kid in PLACED and kid != cid:
            merge(info, feature_info(kid, memo, stack + (cid,)))
        elif kid in FEAT and kid != cid:
            merge(info, feature_info_cfg(kid, memo, stack + (cid,)))
    return info

def merge(a, b):
    a.reads |= b.reads; a.writes |= b.writes; a.read_tags |= b.read_tags; a.noises |= b.noises
    a.heightmaps |= {h for h in b.heightmaps if h}; a.types |= b.types

def main():
    memo = {}
    order = json.load(open(f'{ROOT}/data/featureorder-{V}.json')) if os.path.exists(f'{ROOT}/data/featureorder-{V}.json') else None
    result = {'version': V}

    # ---- 1. информация по каждой размещаемой фиче ----
    infos = {fid: feature_info(fid, memo) for fid in PLACED}
    # biomes using feature
    used_by = collections.defaultdict(set)
    for b, d in BIOME.items():
        for step, lst in enumerate(d.get('features', [])):
            for f in lst:
                used_by[strip(f)].add((b, step))
    result['placed_features'] = {}
    for fid, i in infos.items():
        steps = sorted({s for (_, s) in used_by.get(fid, [])})
        result['placed_features'][fid] = {
            'steps': steps, 'biomes': sorted({b for (b, _) in used_by.get(fid, [])}),
            'modifiers': i.modifiers, 'heightmaps': sorted(h for h in i.heightmaps if h), 'noises': sorted(i.noises),
            'reads': sorted(i.reads - UBIQ), 'writes': sorted(i.writes - UBIQ), 'read_tags': sorted(i.read_tags),
            'scan': i.scan, 'types': sorted(i.types),
        }

    # ---- 2. потенциальные влияния «ранняя пишет → поздняя читает» по глобальному порядку (Overworld) ----
    edges = []
    if order and 'overworld' in order:
        seq = []
        for s, step in enumerate(order['overworld']['steps']):
            for idx, name in enumerate(step):
                seq.append((s, idx, strip(name)))
        for j in range(len(seq)):
            fj = infos.get(seq[j][2])
            if not fj:
                continue
            rj = fj.reads - UBIQ
            if not rj:
                continue
            for i in range(j):
                fi = infos.get(seq[i][2])
                if not fi:
                    continue
                common = (fi.writes - UBIQ) & rj
                if common:
                    edges.append({'writer': seq[i][2], 'writer_step': seq[i][0], 'reader': seq[j][2], 'reader_step': seq[j][0],
                                  'blocks': sorted(common)})
    result['feature_edges'] = edges

    # ---- 3. общие шумы и density-функции ----
    noise_users = collections.defaultdict(lambda: collections.defaultdict(set))
    def scan_noise(kind, name, node):
        inf = Info(); walk(node, [], inf)
        for n in inf.noises:
            noise_users[n][kind].add(name)
    for k, d in DFUN.items(): scan_noise('density_function', k, d)
    for k, d in NSET.items(): scan_noise('noise_settings', k, d)
    for k, d in FEAT.items(): scan_noise('feature', k, d)
    for k, d in PLACED.items(): scan_noise('placed_feature', k, d)
    for k, d in MRULE.items(): scan_noise('material_rule', k, d)
    for k, d in CARV.items(): scan_noise('carver', k, d)
    result['noise_users'] = {n: {k: sorted(v) for k, v in u.items()} for n, u in noise_users.items()}
    # density function fan-in
    refs = collections.defaultdict(lambda: collections.defaultdict(set))
    def scan_df_refs(kind, name, node):
        def w(x):
            if isinstance(x, dict):
                for v in x.values(): w(v)
            elif isinstance(x, list):
                for v in x: w(v)
            elif isinstance(x, str) and x.startswith('minecraft:') and strip(x) in DFUN:
                refs[strip(x)][kind].add(name)
        w(node)
    for k, d in DFUN.items(): scan_df_refs('density_function', k, d)
    for k, d in NSET.items(): scan_df_refs('noise_settings', k, d)
    for k, d in MRULE.items(): scan_df_refs('material_rule', k, d)
    for k, d in FEAT.items(): scan_df_refs('feature', k, d)
    for k, d in PLACED.items(): scan_df_refs('placed_feature', k, d)
    result['density_fan_in'] = {n: {k: sorted(v) for k, v in u.items()} for n, u in refs.items()}

    # ---- 4. структуры и наборы ----
    sinfo = {}
    for k, d in STRUCT.items():
        sinfo[k] = {'type': strip(d.get('type', '')), 'step': d.get('step'), 'terrain_adaptation': d.get('terrain_adaptation'),
                    'biomes': d.get('biomes'), 'start_pool': strip(d['start_pool']) if isinstance(d.get('start_pool'), str) else None,
                    'project_to_heightmap': d.get('project_start_to_heightmap'), 'start_height': d.get('start_height'),
                    'max_distance_from_center': d.get('max_distance_from_center'), 'use_expansion_hack': d.get('use_expansion_hack')}
    result['structures'] = sinfo
    ssinfo = {}
    for k, d in SSET.items():
        pl = d.get('placement', {})
        ssinfo[k] = {'placement': strip(pl.get('type', '')), 'spacing': pl.get('spacing'), 'separation': pl.get('separation'), 'salt': pl.get('salt'),
                     'spread': pl.get('spread_type'), 'frequency': pl.get('frequency'), 'reduction': pl.get('frequency_reduction_method'),
                     'exclusion': pl.get('exclusion_zone'), 'structures': [(strip(s['structure']), s['weight']) for s in d.get('structures', [])]}
    result['structure_sets'] = ssinfo

    # ---- 5. карверы ----
    cinfo = {}
    for k, d in CARV.items():
        c = d.get('config', d)
        cinfo[k] = {'type': strip(d.get('type', '')), 'probability': c.get('probability'), 'y': c.get('y'),
                    'lava_level': c.get('lava_level'), 'replaceable': c.get('replaceable')}
    result['carvers'] = cinfo
    cb = collections.defaultdict(list)
    for b, d in BIOME.items():
        cl = d.get('carvers')
        if isinstance(cl, str): cl = [cl]
        for c in cl or []:
            cb[strip(c)].append(b)
    result['carver_biomes'] = {k: sorted(v) for k, v in cb.items()}

    out = f'{ROOT}/data/interactions-{V}.json'
    json.dump(result, open(out, 'w'), indent=1, ensure_ascii=False)
    print('wrote', out, '| placed features', len(PLACED), '| feature edges', len(edges), '| noises', len(noise_users))

if __name__ == '__main__':
    main()

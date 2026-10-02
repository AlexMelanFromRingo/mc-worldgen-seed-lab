#!/usr/bin/env python3
"""Сборка датапака-изолятора стадий для варианта (tools/gt/variants/<имя>/variant.json) из pack-каталога версии.

Датапак НЕ хранится в репозитории (это производные данные Mojang): он строится из run/pack-<V>/data/minecraft/** при каждой
генерации мира и кладётся в <мир>/datapacks/gt/ ДО первого запуска сервера (механизм — docs/blender/ground-truth.md, §1).

  python3 tools/gt/datapack.py --version 26.3 --variant raw --out /tmp/pack      # собрать и показать сводку
  python3 tools/gt/datapack.py --list                                            # список вариантов

Что переопределяется (по полям variant.json):
  terrain_rule: "empty" | "veins" | "full"
      26.3+      : файлы worldgen/material_rule/<id>.json, на которые ссылаются noise_settings (бедрок, жилы, поверхность, сланец);
      26.1/26.2  : noise_settings.surface_rule (пустая sequence) и ore_veins_enabled.
  carvers=false  : у всех биомов carvers = []
  features=false : у всех биомов features = [[], ...] (все шаги пусты); features="only" — остаётся одна placed_feature
  structures="only": все structure_set, кроме одного, опустошаются (structures = [])
"""
import argparse, copy, glob, hashlib, json, os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, pack_dir, data_format, VERSIONS

VARIANTS_DIR = f'{ROOT}/tools/gt/variants'
EMPTY_RULE = {'type': 'minecraft:sequence', 'sequence': []}


def list_variants():
    return sorted(os.path.basename(os.path.dirname(p)) for p in glob.glob(f'{VARIANTS_DIR}/*/variant.json'))


def parse_variant(name):
    """'raw' | 'feature:minecraft:ore_diamond' | 'structure:minecraft:villages' -> (spec, param)"""
    base, _, param = name.partition(':')
    path = f'{VARIANTS_DIR}/{base}/variant.json'
    if not os.path.exists(path):
        raise SystemExit(f'неизвестный вариант {name!r}; доступны: {", ".join(list_variants())}')
    spec = json.load(open(path))
    if spec.get('parametric'):
        if not param:
            raise SystemExit(f'вариант {base} параметрический: {base}:<id>')
        if ':' not in param:
            param = 'minecraft:' + param
    elif param:
        raise SystemExit(f'вариант {base} без параметров')
    return spec, param or None


def safe_name(variant):
    return variant.replace(':', '_').replace('/', '-')


def _rid(name):
    return name.split(':', 1)[1] if ':' in name else name


def _load(path):
    return json.load(open(path))


def _dump(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        json.dump(obj, f, indent=1, sort_keys=False)
        f.write('\n')


def _has_vein(x):
    return 'ore_vein' in json.dumps(x)


def _vein_only(rule, tail=None):
    seq = [r for r in rule['sequence'] if _has_vein(r)] if isinstance(rule, dict) and rule.get('type') in ('minecraft:sequence', 'sequence') else []
    return {'type': 'minecraft:sequence', 'sequence': seq + ([tail] if tail else [])}


def _default_tail(rule):
    """26.4+: у noise_settings нет default_block — камень кладёт ПОСЛЕДНИЙ элемент material_rule (block stone/netherrack/end_stone)."""
    r = rule
    while isinstance(r, dict) and r.get('type') in ('minecraft:sequence', 'sequence') and r['sequence']:
        r = r['sequence'][-1]
    return copy.deepcopy(r) if isinstance(r, dict) and r.get('type') in ('minecraft:block', 'block') else None


def build(version, variant, out_dir):
    """Собирает датапак; возвращает сводку (dict) или None для варианта «ваниль»."""
    spec, param = parse_variant(variant)
    if spec.get('vanilla'):
        return None
    pk = pack_dir(version)
    wg = f'{pk}/data/minecraft/worldgen'
    if not os.path.isdir(wg):
        raise SystemExit(f'нет pack-каталога {pk}; создайте: tools/make_pack.py')
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    fmt = data_format(version)
    _dump(f'{out_dir}/pack.mcmeta', {'pack': {'description': f'gt isolation: {variant}', 'min_format': fmt, 'max_format': fmt}})
    dp = f'{out_dir}/data/minecraft/worldgen'
    summary = {'variant': variant, 'version': version, 'pack_format': fmt, 'overrides': {}}
    ov = summary['overrides']

    # --- правила материала / поверхности ------------------------------------------------------------------------------------
    rule = spec['terrain_rule']
    if rule != 'full':
        material_ids, n_ns = set(), 0
        for f in sorted(glob.glob(f'{wg}/noise_settings/*.json')):
            ns = _load(f)
            name = os.path.basename(f)
            if 'material_rule' in ns:                                   # 26.3+
                mr = ns['material_rule']
                if isinstance(mr, str):
                    material_ids.add(_rid(mr))
                else:
                    ns['material_rule'] = EMPTY_RULE if rule == 'empty' else _vein_only(mr)   # инлайн-правило (в данных версий не встречается)
                    _dump(f'{dp}/noise_settings/{name}', ns); n_ns += 1
            else:                                                       # 26.1 / 26.2
                ns['surface_rule'] = copy.deepcopy(EMPTY_RULE)
                if rule == 'empty':
                    ns['ore_veins_enabled'] = False
                _dump(f'{dp}/noise_settings/{name}', ns); n_ns += 1
        no_default = not any('default_block' in _load(f) for f in glob.glob(f'{wg}/noise_settings/*.json'))
        for rid in sorted(material_ids):
            src = _load(f'{wg}/material_rule/{rid}.json')
            tail = _default_tail(src) if no_default else None       # 26.4+: сохраняем заполняющий блок
            out = {'type': 'minecraft:sequence', 'sequence': [tail] if tail else []} if rule == 'empty' else _vein_only(src, tail)
            _dump(f'{dp}/material_rule/{rid}.json', out)
        summary['default_block_in_rule'] = no_default
        ov['material_rule'] = len(material_ids)
        ov['noise_settings'] = n_ns

    # --- биомы: карверы и фичи ----------------------------------------------------------------------------------------------
    n_biomes = 0
    if not spec['carvers'] or spec['features'] is not True:
        for f in sorted(glob.glob(f'{wg}/biome/*.json')):
            b = _load(f)
            name = os.path.basename(f)
            ch = False
            if not spec['carvers'] and b.get('carvers'):
                b['carvers'] = {k: [] for k in b['carvers']} if isinstance(b['carvers'], dict) else []
                ch = True
            if spec['features'] is False:
                if any(b.get('features', [])):
                    b['features'] = [[] for _ in b.get('features', [])]
                    ch = True
            elif spec['features'] == 'only':
                b['features'] = [[x for x in step if x == param] for step in b.get('features', [])]
                ch = True
            if ch:
                _dump(f'{dp}/biome/{name}', b); n_biomes += 1
        ov['biome'] = n_biomes
        if spec['features'] == 'only':
            pf = f'{wg}/placed_feature/{_rid(param)}.json'
            if not os.path.exists(pf):
                raise SystemExit(f'нет placed_feature {param} в {pk}')
            ov['only_feature'] = param
            summary['biomes_with_feature'] = sum(
                1 for f in glob.glob(f'{wg}/biome/*.json') if any(param in s for s in _load(f).get('features', [])))

    # --- наборы структур ----------------------------------------------------------------------------------------------------
    if spec['structures'] == 'only':
        keep = _rid(param)
        if not os.path.exists(f'{wg}/structure_set/{keep}.json'):
            raise SystemExit(f'нет structure_set {param} в {pk}')
        n = 0
        for f in sorted(glob.glob(f'{wg}/structure_set/*.json')):
            name = os.path.basename(f)
            if name[:-5] == keep:
                continue
            s = _load(f)
            s['structures'] = []
            _dump(f'{dp}/structure_set/{name}', s); n += 1
        ov['structure_set_emptied'] = n
        ov['only_structure_set'] = param

    # --- хэш содержимого (воспроизводимость) --------------------------------------------------------------------------------
    h = hashlib.sha1()
    for root, _, files in sorted(os.walk(out_dir)):
        for fn in sorted(files):
            p = os.path.join(root, fn)
            h.update(os.path.relpath(p, out_dir).encode()); h.update(open(p, 'rb').read())
    summary['sha1'] = h.hexdigest()
    summary['generate_structures'] = bool(spec['generate_structures'])
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='26.3', choices=VERSIONS)
    ap.add_argument('--variant', default='raw')
    ap.add_argument('--out', help='куда собрать датапак')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    if a.list:
        for v in list_variants():
            s = _load(f'{VARIANTS_DIR}/{v}/variant.json')
            print(f'{s["name"]:34s} {s["title"]}')
        return
    s = build(a.version, a.variant, a.out or f'/tmp/gtpack-{a.version}-{safe_name(a.variant)}')
    print(json.dumps(s, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()

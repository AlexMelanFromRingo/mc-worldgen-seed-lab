#!/usr/bin/env python3
"""Таблица placed_feature × статус реализации для docs/blender/features.md (выгрузка libmcgen + результаты G5i).
    python3 libmcgen/tests/g5_table.py [--version 26.3] [--results libmcgen/tests/results/g5i-26.3.json] > таблица.md
Статус: «проверена» — изолированная фича совпала с эталоном (≥1 блок эффекта); «реализована» — тип реализован, эталона нет/пустой; «частично» — тип
реализован, но вложенная фича не реализована (молча пропускается); «не начата» — тип не реализован."""
import argparse, collections, json, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
ap = argparse.ArgumentParser(); ap.add_argument('--version', default='26.3'); ap.add_argument('--results', default=''); a = ap.parse_args()
res = {}
if a.results and os.path.exists(a.results):
    for r in json.load(open(a.results))['rows']:
        res.setdefault(r['feature'], []).append(r)
rows = {}
for dn, dim in DIMS.items():
    out = f'/tmp/g5table-{a.version}-{dn}.json'
    env = dict(os.environ, MCGEN_FEATURES_DUMP_ORDER=out)
    subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', dim, '--seed', '1', '--cx0', '0', '--cz0', '0',
                    '--nx', '1', '--nz', '1', '--stages', '0x13', '--threads', '1', '--out', '/tmp/g5table.mcr'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = json.load(open(out))
    for st, lst in enumerate(d['steps']):
        for gi, fid in enumerate(lst):
            p = next(x for x in d['placed'] if x['id'] == fid)
            rows.setdefault(fid, dict(id=fid, type=p['type'], impl=p['impl'], dims=[], steps=set()))
            rows[fid]['dims'].append(dn); rows[fid]['steps'].add(st)
STEP = ['RAW_GENERATION', 'LAKES', 'LOCAL_MODIFICATIONS', 'UNDERGROUND_STRUCTURES', 'SURFACE_STRUCTURES', 'STRONGHOLDS', 'UNDERGROUND_ORES',
        'UNDERGROUND_DECORATION', 'FLUID_SPRINGS', 'VEGETAL_DECORATION', 'TOP_LAYER_MODIFICATION']
cnt = collections.Counter()
lines = ['| placed_feature | тип фичи | измерения | шаг | статус | G5i (изоляция) |', '|---|---|---|---|---|---|']
for fid in sorted(rows):
    r = rows[fid]
    rs = res.get(fid, [])
    ok = [x for x in rs if x['mismatch'] == 0 and (x.get('effect') or 0) > 0]
    bad = [x for x in rs if x['mismatch'] > 0]
    if bad: st, g = 'реализована (расхождения)', ', '.join(f'{x["match_pct"]:.4f} %' for x in bad)
    elif ok: st, g = 'проверена', ', '.join(f'100 % ({x["effect"]} блоков эффекта)' for x in ok)
    elif r['impl'] == 1: st, g = 'реализована', ('эталон без эффекта' if rs else '—')
    elif r['impl'] == 2: st, g = 'частично', '—'
    else: st, g = 'не начата', '—'
    cnt[st] += 1
    lines.append(f'| `{fid.split(":")[1]}` | `{r["type"].split(":")[1] if ":" in r["type"] else r["type"]}` | {",".join(sorted(set(r["dims"])))} | {",".join(STEP[s] for s in sorted(r["steps"]))} | {st} | {g} |')
print(f'Всего placed_feature в биомах измерений ({a.version}): {len(rows)}; ' + ', '.join(f'{k}: {v}' for k, v in cnt.most_common()) + '\n')
print('\n'.join(lines))

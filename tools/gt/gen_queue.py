#!/usr/bin/env python3
"""Очередь генерации эталонных миров по матрице tools/gt/matrix.json (последовательно, по одному серверу, с возобновлением).

  nohup python3 tools/gt/gen_queue.py --version 26.3 --variants raw --profile all >> run/gt/queue.log 2>&1 &

Профили:  core  — на каждый seed: overworld (0,0), nether (0,0), end (0,0), end внешние острова (outer_east);
          land  — core + остальные области Overworld (ocean/mountains/desert/jungle) — по умолчанию для лёгких вариантов;
          all   — вся матрица (+ nether offset, + end outer_north).
Порядок: сперва core всех seed (чтобы W1 мог начать), затем остальное. Готовые миры (manifest.json ok) пропускаются.
Записывает run/gt/queue-summary.json (время/число чанков по каждому миру).
"""
import argparse, json, os, sys, time, traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, GT, VERSIONS
import gen_world, datapack

CORE_LABELS = {('overworld', 'spawn'), ('nether', 'center'), ('end', 'main_island'), ('end', 'outer_east')}
LAND_LABELS = CORE_LABELS | {('overworld', 'ocean'), ('overworld', 'mountains'), ('overworld', 'desert'), ('overworld', 'jungle')}


def configs(profile, seeds=None, dims=None):
    m = json.load(open(f'{ROOT}/tools/gt/matrix.json'))
    if profile == 'extras':          # области (r=5) с редкими биомами для изоляции фич (pick_regions.py)
        return [c for c in m.get('extras', []) if (not seeds or c['seed'] in seeds) and (not dims or c['dim'] in dims)]
    out = []
    for c in m['configs']:
        key = (c['dim'], c['label'])
        if profile == 'core' and key not in CORE_LABELS: continue
        if profile == 'land' and key not in LAND_LABELS: continue
        if seeds and c['seed'] not in seeds: continue
        if dims and c['dim'] not in dims: continue
        out.append(c)
    # core сперва
    out.sort(key=lambda c: (0 if (c['dim'], c['label']) in CORE_LABELS else 1, ))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='26.3', choices=VERSIONS)
    ap.add_argument('--variants', default='raw', help='через запятую: raw,veins,surface,carvers,features,full,feature:minecraft:ore_diamond,...')
    ap.add_argument('--profile', default='land', choices=['core', 'land', 'all', 'extras'])
    ap.add_argument('--seeds', type=lambda s: [int(x) for x in s.split(',')], default=None)
    ap.add_argument('--dims', default=None, help='overworld,nether,end')
    ap.add_argument('--xmx', default='5g')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--timeout', type=int, default=7200)
    ap.add_argument('--plan', default=None, help='structures: области построек из tools/gt/structures_plan.json (structure_plan.py); в --variants `structure` = '
                                                 '`structure:<набор из плана>`, остальные варианты (full, surface, ...) — на тех же областях')
    ap.add_argument('--plan-radius', type=int, default=0, help='радиус областей плана (0: из плана; structures — 8)')
    ap.add_argument('--sets', default=None, help='с --plan: только эти наборы (через запятую, без minecraft:)')
    a = ap.parse_args()
    dims = a.dims.split(',') if a.dims else None
    cfgs = configs(a.profile, a.seeds, dims)
    if a.plan == 'features':
        fp = json.load(open(f'{ROOT}/tools/gt/features_plan.json'))
        only = set(a.sets.split(',')) if a.sets else None
        jobs = []
        for e in fp['plan']:
            if only and e['feature'].split(':')[1] not in only:
                continue
            if a.seeds and e['seed'] not in a.seeds:
                continue
            c = {'dim': e['dim'], 'seed': e['seed'], 'cx': e['cx'], 'cz': e['cz'], 'radius': a.plan_radius or e.get('radius', fp['radius']), 'label': e['feature'].split(':')[1]}
            for v in a.variants.split(','):
                jobs.append((f'feature:{e["feature"]}' if v == 'feature' else v, c))
    elif a.plan:
        plan = json.load(open(f'{ROOT}/tools/gt/structures_plan.json'))['plan']
        only = set(a.sets.split(',')) if a.sets else None
        jobs = []
        for e in plan:
            if only and e['set'].split(':')[1] not in only:
                continue
            if a.seeds and e['seed'] not in a.seeds:
                continue
            c = {'dim': e['dim'], 'seed': e['seed'], 'cx': e['cx'], 'cz': e['cz'], 'radius': a.plan_radius or 8, 'label': e['set'].split(':')[1]}
            for v in a.variants.split(','):
                jobs.append((f'structure:{e["set"]}' if v == 'structure' else v, c))
    else:
        jobs = [(v, c) for v in a.variants.split(',') for c in cfgs if not (v == 'veins' and c['dim'] != 'overworld')]   # в Nether/End жил руды нет: veins == raw
    sp = f'{GT}/queue-summary.json'
    summary = json.load(open(sp)) if os.path.exists(sp) else {}
    t_q = time.monotonic()
    print(time.strftime('%H:%M:%S'), f'очередь: {len(jobs)} задач ({a.version}, {a.variants}, профиль {a.profile})', flush=True)
    for i, (v, c) in enumerate(jobs):
        wd = gen_world.world_dir(a.version, v, c['dim'], c['seed'], c['cx'], c['cz'], c['radius'])
        print(time.strftime('%H:%M:%S'), f'[{i + 1}/{len(jobs)}] {v} {c["dim"]} s{c["seed"]} ({c["cx"]},{c["cz"]}) {c["label"]}', flush=True)
        try:
            m = gen_world.generate(a.version, v, c['dim'], c['seed'], c['cx'], c['cz'], c['radius'], xmx=a.xmx, force=a.force, timeout=a.timeout)
            summary[wd.replace(ROOT + '/', '')] = {k: m.get(k) for k in ('ok', 'chunks_full', 'chunks_area', 'server_start_s', 'generate_s', 'total_s',
                                                                         'region_bytes', 'finished')} | {'label': c['label']}
        except Exception as e:
            traceback.print_exc()
            summary[wd.replace(ROOT + '/', '')] = {'ok': False, 'error': f'{type(e).__name__}: {e}', 'label': c['label']}
        json.dump(summary, open(sp, 'w'), indent=1, ensure_ascii=False)
    print(time.strftime('%H:%M:%S'), f'очередь завершена за {time.monotonic() - t_q:.0f} с', flush=True)


if __name__ == '__main__':
    main()

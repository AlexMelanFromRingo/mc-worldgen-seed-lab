#!/usr/bin/env python3
"""G5: порядок фич по шагам (FeatureSorter) против выгрузки настоящего кода игры data/featureorder-<V>.json (все измерения).
    python3 libmcgen/tests/g5_order.py [--versions 26.1 26.2 26.3]"""
import argparse, json, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIMS = {'overworld': 'minecraft:overworld', 'the_nether': 'minecraft:the_nether', 'the_end': 'minecraft:the_end'}
ap = argparse.ArgumentParser(); ap.add_argument('--versions', nargs='*', default=['26.1', '26.2', '26.3']); a = ap.parse_args()
bad = 0
for v in a.versions:
    ref = json.load(open(f'{ROOT}/data/featureorder-{v}.json'))
    for k, dim in DIMS.items():
        out = f'/tmp/g5order-{v}-{k}.json'
        if os.path.exists(out): os.remove(out)
        env = dict(os.environ, MCGEN_FEATURES_DUMP_ORDER=out)
        subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{v}', '--version', v, '--dim', dim, '--seed', '1', '--cx0', '0', '--cz0', '0',
                        '--nx', '1', '--nz', '1', '--stages', '0x13', '--threads', '1', '--out', '/tmp/g5order.mcr'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not os.path.exists(out): print(f'{v} {k}: нет выгрузки'); bad += 1; continue
        ours = json.load(open(out))['steps']
        theirs = [[x if ':' in x else 'minecraft:' + x for x in s] for s in ref[k]['steps']]
        n = sum(len(s) for s in theirs)
        same = ours == theirs
        mism = sum(1 for s in range(min(len(ours), len(theirs))) for i, x in enumerate(theirs[s]) if i >= len(ours[s]) or ours[s][i] != x)
        print(f'{v} {k}: фич {n}, шагов {len(theirs)}/{len(ours)}  {"OK" if same else "РАСХОЖДЕНИЙ позиций: %d" % mism}')
        bad += 0 if same else 1
sys.exit(1 if bad else 0)

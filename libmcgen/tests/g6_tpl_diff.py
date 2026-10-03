#!/usr/bin/env python3
"""g6_tpl_diff.py — расхождения блоков одного эталонного мира с последним прогоном g6_blocks.py (файл $G6_TMP/<набор>-<мир>.mcr).

  g6_tpl_diff.py <набор> <подстрока имени мира> [--list N] [--pairs]
Печатает первые N расхождений (x y z наше -> эталон) и сводку пар.
"""
import argparse, glob, json, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools', 'gt'))
import diff
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
TMP = os.environ.get('G6_TMP', '/tmp/g6')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('set'); ap.add_argument('world'); ap.add_argument('--list', type=int, default=40)
    ap.add_argument('--version', default='26.3')
    a = ap.parse_args()
    w = [x for x in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/structure_minecraft_{a.set}/*')) if a.world in os.path.basename(x)][0]
    base = os.path.basename(w); dim = base.split('-s')[0]
    out = f'{TMP}/{a.set}-{base}.mcr'
    R, _ = diff.run(w, out, None, None, dim=dim, version=a.version, min_status='minecraft:full', ignore_state=[], ignore_y=[], only_y=None, mask_ext=False,
                    margin=2, top=12, biomes=False, heightmaps=False, min_match=0, allow_missing=False, region=None, no_cache=False,
                    list=a.list, flow_halo=0, stable_with=[], mask_flow=True)
    for s in R['samples']:
        print(s['x'], s['y'], s['z'], s['ours'], '->', s['ref'])
    for t in R['top_pairs']:
        print('  ', t)


if __name__ == '__main__':
    main()

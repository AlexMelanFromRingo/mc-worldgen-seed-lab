#!/usr/bin/env python3
"""G6o: старты построек libmcgen против oracle (`structstart`, настоящий Structure.generate) для версий без эталонных миров (26.1/26.2/26.4).

  g6_oracle_starts.py --version 26.1 --seed 12345 [--box -40 -40 80 80] [--dim overworld] [--max 40]
Для чанков с нашими стартами спрашивает oracle (режим serve) и сравнивает наборы структур, bounding box (oracle даёт его расширенным на 12 при terrain_adaptation)
и число/рамки частей. Печатает «совпало/всего»."""
import argparse, json, os, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
ap = argparse.ArgumentParser()
ap.add_argument('--version', default='26.1'); ap.add_argument('--seed', default='12345'); ap.add_argument('--dim', default='overworld')
ap.add_argument('--box', nargs=4, type=int, default=[-40, -40, 80, 80]); ap.add_argument('--max', type=int, default=40); ap.add_argument('-v', action='store_true')
a = ap.parse_args()
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
build = os.environ.get('G6_BUILD', 'build')
p = subprocess.run([f'{ROOT}/libmcgen/{build}/tests/g6_starts', f'{ROOT}/run/pack-{a.version}', a.version, DIMS[a.dim], a.seed, *map(str, a.box)], capture_output=True, text=True)
ours = [json.loads(l) for l in p.stdout.splitlines()]
ours = ours[:a.max]
print(f'наши старты: {len(ours)} ({sorted(set(o["id"] for o in ours))})', file=sys.stderr)
cmds = ''.join(f'structstart {a.dim} {a.seed} {o["cx"]} {o["cz"]}\n' for o in ours)
r = subprocess.run([f'{ROOT}/oracle/run.sh', a.version, 'serve'], input=cmds, capture_output=True, text=True, timeout=3000)
res = [json.loads(l) for l in r.stdout.splitlines() if l.startswith('{')]
ok = 0
for o, rr in zip(ours, res):
    sel = None
    for st in rr.get('results', []):
        for at in st.get('attempts', []):
            if at.get('valid'): sel = at
    if not sel:
        print('нет у oracle:', o['id'], o['cx'], o['cz']); continue
    ob = sel['bbox']; pc = sel.get('pieces', [])
    exp = o['bb'] if True else None
    # oracle bbox расширен на 12 при terrain_adaptation != none: сравниваем и так, и так
    cands = [exp, [exp[0] - 12, exp[1] - 12, exp[2] - 12, exp[3] + 12, exp[4] + 12, exp[5] + 12]]
    good = sel['structure'] == o['id'] and ob in cands and len(pc) == len(o['pieces']) and all(list(x['bbox']) == y['bb'] or True for x, y in zip(pc, o['pieces']))
    # рамки частей (без допуска по y для не-jigsaw)
    if good:
        for x, y in zip(pc, o['pieces']):
            xb = list(x['bbox']); yb = y['bb']
            if xb != yb and not ('pos' not in y and xb[0] == yb[0] and xb[2] == yb[2] and xb[3] == yb[3] and xb[5] == yb[5]):
                good = False; break
    ok += good
    if not good or a.v: print('совпало' if good else 'РАСХОЖДЕНИЕ', o['id'], o['cx'], o['cz'], 'oracle', sel['structure'], ob, len(pc), 'наши', o['bb'], len(o['pieces']))
print(f'ИТОГО {a.version} seed {a.seed}: {ok}/{len(ours)} стартов совпали с oracle')

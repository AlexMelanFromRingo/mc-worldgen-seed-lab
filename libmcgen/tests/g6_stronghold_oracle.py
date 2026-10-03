#!/usr/bin/env python3
"""G6 strongholds: старты крепостей libmcgen против oracle (настоящий Structure.generate игры) без эталонных миров.

  g6_stronghold_oracle.py [--version 26.3] [--seeds 12345,…] [--n 12] [--all] [-v]
Для каждого зерна берутся позиции колец (`oracle stronghold <seed>`), для N ближайших к 0,0 (или всех 128 при --all) —
`structstart overworld <seed> <cx> <cz> minecraft:strongholds` (типы и bounding box частей, первые 400) и сравниваются со
стартами `G6_BUILD/tests/g6_starts` в том же чанке (id части, BB). Ответы oracle кэшируются в G6_TMP/sh_oracle_<V>_<seed>.json.
"""
import argparse, json, os, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, f'{ROOT}/tools/gt')
BUILD = os.environ.get('G6_BUILD', 'build')
TMP = os.environ.get('G6_TMP', '/tmp/g6')
BIN = f'{ROOT}/libmcgen/{BUILD}/tests/g6_starts'
SEEDS = [12345, 8675309, -7048155917072976836]


class Lazy:
    def __init__(self, v):
        self.v = v; self.o = None

    def q(self, line):
        if self.o is None:
            from structure_plan import Oracle
            self.o = Oracle(self.v)
        return self.o.q(line)

    def close(self):
        if self.o:
            self.o.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--seeds', default=''); ap.add_argument('--n', type=int, default=12)
    ap.add_argument('--all', action='store_true'); ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    seeds = [int(s) for s in a.seeds.split(',') if s] or SEEDS
    os.makedirs(TMP, exist_ok=True)
    orc = Lazy(a.version)
    tot = ok = 0
    for seed in seeds:
        cf = f'{TMP}/sh_oracle_{a.version}_{seed}.json'
        cache = json.load(open(cf)) if os.path.exists(cf) else {}
        if 'rings' not in cache:
            cache['rings'] = orc.q(f'stronghold {seed}')['chunks']
        rings = sorted(cache['rings'], key=lambda c: abs(c[0]) + abs(c[1]))
        if not a.all:
            rings = rings[:a.n]
        starts = cache.setdefault('starts', {})
        for cx, cz in rings:
            k = f'{cx},{cz}'
            if k not in starts:
                r = orc.q(f'structstart overworld {seed} {cx} {cz} minecraft:strongholds')
                res = (r.get('results') or [{}])[0]
                att = [x for x in res.get('attempts', []) if x.get('valid')]
                starts[k] = {'bbox': att[0]['bbox'], 'pieces': att[0]['pieces'], 'n': att[0]['piece_count']} if att else None
                json.dump(cache, open(cf, 'w'))
        json.dump(cache, open(cf, 'w'))
        for cx, cz in rings:
            ref = starts[f'{cx},{cz}']
            p = subprocess.run([BIN, f'{ROOT}/run/pack-{a.version}', a.version, 'minecraft:overworld', str(seed), str(cx), str(cz), '1', '1'], capture_output=True, text=True)
            ours = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
            ours = [o for o in ours if o['id'] == 'minecraft:stronghold']
            tot += 1
            if ref is None and not ours:
                ok += 1; continue
            if ref is None or not ours:
                print(f'seed {seed} chunk {cx},{cz}: эталон {"есть" if ref else "нет"}, наш {"есть" if ours else "нет"}'); continue
            o = ours[0]
            rp = [(x['type'], x['bbox']) for x in ref['pieces']]
            op = [(x['id'], x['bb']) for x in o['pieces']][:400]
            adj = [o['bb'][i] - 12 if i < 3 else o['bb'][i] + 12 for i in range(6)]     # getBoundingBox: adjustBoundingBox (bury → +12)
            same = rp == op and ref['n'] == len(o['pieces']) and ref['bbox'] == adj
            if same:
                ok += 1
                if a.v:
                    print(f'seed {seed} chunk {cx},{cz}: OK, частей {ref["n"]}')
            else:
                i = next((i for i in range(min(len(rp), len(op))) if rp[i] != op[i]), min(len(rp), len(op)))
                print(f'seed {seed} chunk {cx},{cz}: РАСХОЖДЕНИЕ частей эталон {ref["n"]} наши {len(o["pieces"])}, bbox {ref["bbox"]} / {adj}, первая отличающаяся {i}')
                if a.v:
                    print('   эталон:', rp[i] if i < len(rp) else None); print('   наше  :', op[i] if i < len(op) else None)
    orc.close()
    print(f'ИТОГО: стартов {tot}, совпало {ok}')


if __name__ == '__main__':
    main()

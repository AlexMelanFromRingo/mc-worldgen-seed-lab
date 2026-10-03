#!/usr/bin/env python3
"""Дополнение плана (запрос W9): nether_fossils в долинах песка душ и woodland_mansions на других seed. Добавляет в structures_plan.json."""
import json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT
from structure_plan import Oracle, SEEDS
P = f'{ROOT}/tools/gt/structures_plan.json'


def biome_nether(seed, cx, cz):
    out = subprocess.run([f'{ROOT}/tools/mcquery', '26.3', 'nether', str(seed), str(cx * 4 + 2), str(cz * 4 + 2), '1', '1', '8'], capture_output=True, text=True).stdout.split()
    return out[-1] if out else ''


def main():
    d = json.load(open(P)); plan = d['plan']
    have = {(e['set'], e['dim'], e['seed'], e['cx'], e['cz']) for e in plan}
    new = []
    o = Oracle('26.3')
    for seed in SEEDS:
        c = o.q(f'structs nether {seed} minecraft:nether_fossils -400 -400 800 800')['sets'].get('minecraft:nether_fossils', [])
        ssv = [p for p in c if biome_nether(seed, *p) == 'minecraft:soul_sand_valley']
        scored = sorted(((sum(1 for q in ssv if abs(q[0] - p[0]) <= 8 and abs(q[1] - p[1]) <= 8), -(abs(p[0]) + abs(p[1])), p) for p in ssv), reverse=True)
        chosen = []
        for sc, _, p in scored:
            if sc and all(abs(p[0] - q[0]) + abs(p[1] - q[1]) > 40 for q in chosen):
                chosen.append(p); new.append({'set': 'minecraft:nether_fossils', 'dim': 'nether', 'seed': seed, 'cx': p[0], 'cz': p[1], 'structure': 'minecraft:nether_fossil', 'ssv_starts_nearby': sc})
                print(new[-1], flush=True)
            if len(chosen) >= 2:
                break
    for seed in SEEDS:
        R = 900
        cands = sorted(o.q(f'structs overworld {seed} minecraft:woodland_mansions {-R} {-R} {2 * R} {2 * R}')['sets'].get('minecraft:woodland_mansions', []), key=lambda c: abs(c[0]) + abs(c[1]))
        n = 0
        for cx, cz in cands[:300]:
            s = o.q(f'structstart overworld {seed} {cx} {cz} minecraft:woodland_mansions')
            if (s.get('results') or [{}])[0].get('generated') and ('minecraft:woodland_mansions', 'overworld', seed, cx, cz) not in have:
                new.append({'set': 'minecraft:woodland_mansions', 'dim': 'overworld', 'seed': seed, 'cx': cx, 'cz': cz, 'structure': 'minecraft:mansion'}); print(new[-1], flush=True)
                n += 1
                if n >= 2:
                    break
    o.close()
    d['plan'] = plan + new
    json.dump(d, open(P, 'w'), indent=1, ensure_ascii=False)
    print('добавлено', len(new))


if __name__ == '__main__':
    main()

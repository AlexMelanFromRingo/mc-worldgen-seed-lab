#!/usr/bin/env python3
"""Дополнение плана построек (запрос W9): strongholds, ruined_portals, деревни других биомов, fortress, nether_fossils, вторые миры для редких наборов.
Результат ДОБАВЛЯЕТСЯ в tools/gt/structures_plan.json (без дублей по (set, dim, seed, cx, cz)). Для наборов, где oracle structstart даёт пустой ответ
(ruined_portals, nether_fossils), берутся «потенциальные» чанки structs, а реальные старты проверяются по NBT сгенерированного мира (structures.starts).
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT
from structure_plan import Oracle, SEEDS

P = f'{ROOT}/tools/gt/structures_plan.json'


def near(cands, n, mind=25):
    out = []
    for c in sorted(cands, key=lambda c: abs(c[0]) + abs(c[1])):
        if all(abs(c[0] - o[0]) + abs(c[1] - o[1]) >= mind for o in out):
            out.append(c)
        if len(out) >= n:
            break
    return out


def main():
    d = json.load(open(P)); plan = d['plan']
    have = {(e['set'], e['dim'], e['seed'], e['cx'], e['cz']) for e in plan}
    new = []

    def add(st, dim, seed, c, structure=None):
        k = (f'minecraft:{st}', dim, seed, c[0], c[1])
        if k not in have:
            have.add(k); new.append({'set': k[0], 'dim': dim, 'seed': seed, 'cx': c[0], 'cz': c[1], 'structure': structure}); print(new[-1], flush=True)
    o = Oracle('26.3')
    for seed in SEEDS:
        r = o.q(f'stronghold {seed}')
        for c in near(r['chunks'], 3, 100):
            add('strongholds', 'overworld', seed, c)
        for dim, n in (('overworld', 3), ('nether', 2)):
            r = o.q(f'structs {dim} {seed} minecraft:ruined_portals -150 -150 300 300')
            for c in near(r['sets'].get('minecraft:ruined_portals', []), n):
                add('ruined_portals', dim, seed, c)
        r = o.q(f'structs nether {seed} minecraft:nether_fossils -150 -150 300 300')
        for c in near(r['sets'].get('minecraft:nether_fossils', []), 2):
            add('nether_fossils', 'nether', seed, c)
        # деревни других биомов, fortress, и вторые валидные старты редких наборов
        want = [('villages', 'overworld', {'minecraft:village_desert', 'minecraft:village_savanna', 'minecraft:village_snowy'}, 3),
                ('nether_complexes', 'nether', {'minecraft:fortress'}, 1)]
        for st in ('desert_pyramids', 'swamp_huts', 'buried_treasures', 'woodland_mansions', 'igloos'):
            want.append((st, 'overworld', None, 2))
        for st, dim, names, n in want:
            R = 250
            cands = sorted(o.q(f'structs {dim} {seed} minecraft:{st} {-R} {-R} {2 * R} {2 * R}')['sets'].get(f'minecraft:{st}', []), key=lambda c: abs(c[0]) + abs(c[1]))
            got = {}
            for cx, cz in cands[:150]:
                s = o.q(f'structstart {dim} {seed} {cx} {cz} minecraft:{st}')
                res = (s.get('results') or [{}])[0]
                if not res.get('generated'):
                    continue
                ok = [x['structure'] for x in res.get('attempts', []) if x.get('valid')]
                name = ok[0] if ok else None
                if names and name not in names:
                    continue
                key = name if names else (cx, cz)
                if key in got or (not names and len(got) >= n):
                    continue
                got[key] = (cx, cz)
                if not names and (f'minecraft:{st}', dim, seed, cx, cz) in have:
                    continue        # уже в плане: считается одним из n
                add(st, dim, seed, (cx, cz), name)
                if (names and len(got) >= len(names)) or (not names and len(got) >= n):
                    break
    o.close()
    d['plan'] = plan + new
    json.dump(d, open(P, 'w'), indent=1, ensure_ascii=False)
    print(f'добавлено {len(new)}')


if __name__ == '__main__':
    main()

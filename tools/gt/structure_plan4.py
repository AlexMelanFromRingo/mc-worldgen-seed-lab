#!/usr/bin/env python3
"""Эталоны для КОНКРЕТНЫХ структур внутри наборов (варианты лагеря по биомам, mineshaft_mesa, shipwreck_beached, ruined_portal_*): добавляет в structures_plan.json.

Кандидаты — потенциальные чанки набора (oracle structs); предфильтр по биому в центре чанка (tools/mcquery, y=68); для наборов с рабочим structstart
принимается только чанк, где сгенерировалась именно целевая структура. Для ruined_portals oracle structstart пуст — принимается по биому (старт проверять по NBT .mca).
"""
import glob, json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT
from structure_plan import Oracle, SEEDS
P = f'{ROOT}/tools/gt/structures_plan.json'
PK = f'{ROOT}/run/pack-26.3/data/minecraft/worldgen'


def biome_at(seed, cx, cz):
    o = subprocess.run([f'{ROOT}/tools/mcquery', '26.3', 'ow', str(seed), str(cx * 4 + 2), str(cz * 4 + 2), '1', '1', '17'], capture_output=True, text=True).stdout.split()
    return o[-1].split(':')[-1] if o else ''


def main():
    d = json.load(open(P)); plan = d['plan']
    have = {(e['set'], e['dim'], e['seed'], e['cx'], e['cz']) for e in plan}
    targets = {}   # структура -> (набор, набор биомов или None)
    for f in sorted(glob.glob(f'{PK}/structure/abandoned_camp_*.json')):
        n = os.path.basename(f)[:-5]
        targets['minecraft:' + n] = ('abandoned_camp', {n[len('abandoned_camp_'):]})
    targets['minecraft:mineshaft_mesa'] = ('mineshafts', {'badlands', 'eroded_badlands', 'wooded_badlands'})
    targets['minecraft:shipwreck_beached'] = ('shipwrecks', {'beach', 'snowy_beach'})
    targets['minecraft:ruined_portal_desert'] = ('ruined_portals', {'desert'})
    targets['minecraft:ruined_portal_jungle'] = ('ruined_portals', {'jungle', 'sparse_jungle', 'bamboo_jungle'})
    targets['minecraft:ruined_portal_swamp'] = ('ruined_portals', {'swamp', 'mangrove_swamp'})
    done = {e.get('structure') for e in plan}
    o = Oracle('26.3'); new = []
    for tgt, (st, biomes) in targets.items():
        if tgt in done:
            continue
        for seed in SEEDS:
            R = 600
            cands = sorted(o.q(f'structs overworld {seed} minecraft:{st} {-R} {-R} {2 * R} {2 * R}')['sets'].get(f'minecraft:{st}', []), key=lambda c: abs(c[0]) + abs(c[1]))
            hit = None
            for cx, cz in cands:
                if biome_at(seed, cx, cz) not in biomes:
                    continue
                if st != 'ruined_portals':
                    r = (o.q(f'structstart overworld {seed} {cx} {cz} minecraft:{st}').get('results') or [{}])[0]
                    ok = [x['structure'] for x in r.get('attempts', []) if x.get('valid')]
                    if not r.get('generated') or not ok or ok[0] != tgt:
                        continue
                hit = (cx, cz); break
            print(tgt, seed, hit, flush=True)
            if hit and (f'minecraft:{st}', 'overworld', seed, *hit) not in have:
                have.add((f'minecraft:{st}', 'overworld', seed, *hit))
                new.append({'set': f'minecraft:{st}', 'dim': 'overworld', 'seed': seed, 'cx': hit[0], 'cz': hit[1], 'structure': tgt})
                break           # одной области на структуру достаточно
    o.close()
    d['plan'] = plan + new
    json.dump(d, open(P, 'w'), indent=1, ensure_ascii=False)
    print('добавлено', len(new))


if __name__ == '__main__':
    main()

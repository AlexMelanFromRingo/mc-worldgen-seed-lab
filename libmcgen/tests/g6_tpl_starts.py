#!/usr/bin/env python3
"""g6_tpl_starts.py — старты шаблонных построек (shipwreck, ocean_ruin, ruined_portal, nether_fossil, end_city) против сохранённых стартов эталона
с полями шаблонных частей: Template, TPX/TPY/TPZ, Rot/Rotation, Mirror, GD, BB.

  g6_tpl_starts.py --sets shipwrecks[,…] [--version 26.3] [--seeds …] [-v]
Высота шаблона у кораблей/руин уточняется при ПЕРВОМ рисовании части (postProcess), поэтому у нарисованных стартов эталона TPY/BB.y
отличаются от стартовых; такие расхождения только по y считаются отдельно («только y») и проверяются g6_blocks.py.
Переменные окружения: G6_BUILD (каталог сборки, как у g6_starts.py).
"""
import argparse, glob, json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g6_starts as G

ROT = {'NONE': 'NONE', 'CLOCKWISE_90': 'CLOCKWISE_90', 'CLOCKWISE_180': 'CLOCKWISE_180', 'COUNTERCLOCKWISE_90': 'COUNTERCLOCKWISE_90'}
MIR = {'NONE': 0, 'LEFT_RIGHT': 1, 'FRONT_BACK': 2}


def ref_key(p, noy=False):
    bb = [int(x) for x in p['BB']]
    d = {'id': p.get('id'), 'o': p.get('O'), 'gd': p.get('GD'), 'tpl': p.get('Template'),
         'rot': p.get('Rot', p.get('Rotation', 'NONE')), 'mir': MIR.get(p.get('Mirror', 'NONE'), p.get('Mirror')),
         'bbxz': [bb[0], bb[2], bb[3], bb[5]], 'tpxz': [p.get('TPX'), p.get('TPZ')]}
    if 'Properties' in p:
        d['props'] = [int(p['Properties'].get('cold', 0)), int(p['Properties'].get('air_pocket', 0)), p.get('VerticalPlacement')]
    if not noy:
        d['y'] = [bb[1], bb[4], p.get('TPY')]
    return d


def our_key(p, noy=False):
    bb = p['bb']
    d = {'id': p['id'], 'o': p['o'], 'gd': p['gd'], 'tpl': p.get('tpl'), 'rot': p.get('trot', 'NONE'), 'mir': p.get('tmir', 0),
         'bbxz': [bb[0], bb[2], bb[3], bb[5]], 'tpxz': [p['tp'][0], p['tp'][2]] if 'tp' in p else None}
    if 'vp' in p:
        d['props'] = [p['cold'], p['air_pocket'], p['vp']]
    if not noy:
        d['y'] = [bb[1], bb[4], p['tp'][1] if 'tp' in p else None]
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--sets', default=''); ap.add_argument('--seeds', default='')
    ap.add_argument('-v', action='store_true'); ap.add_argument('--max-diff', type=int, default=4)
    a = ap.parse_args()
    sets = [s for s in a.sets.split(',') if s]; seeds = [s for s in a.seeds.split(',') if s]
    tot = full_ok = yonly = 0
    for d in sorted(glob.glob(f'{G.ROOT}/run/gt/{a.version}/structure_minecraft_*')):
        name = os.path.basename(d).replace('structure_minecraft_', '')
        if sets and name not in sets:
            continue
        for w in sorted(glob.glob(f'{d}/*-s*-c*-r*')):
            base = os.path.basename(w); dim = base.split('-s')[0]; seed = base.split('-s', 1)[1].rsplit('-c', 1)[0]
            if seeds and seed not in seeds:
                continue
            ref = G.ref_starts(w, dim)
            setids = {e['structure'] for e in json.load(open(f'{G.ROOT}/run/pack-{a.version}/data/minecraft/worldgen/structure_set/{name}.json'))['structures']}
            mf = json.load(open(f'{w}/manifest.json')); ac = mf['area_chunks']
            cx0, cz0, cx1, cz1 = ac[0] - 6, ac[1] - 6, ac[2] + 6, ac[3] + 6
            p = subprocess.run([G.BIN, f'{G.ROOT}/run/pack-{a.version}', a.version, G.DIMS[dim], seed, str(cx0), str(cz0), str(cx1 - cx0 + 1), str(cz1 - cz0 + 1)],
                               capture_output=True, text=True)
            if p.returncode:
                print(name, base, 'ОШИБКА', p.stderr[-300:]); continue
            ours = {}
            for line in p.stdout.splitlines():
                j = json.loads(line)
                if j['id'] in setids:
                    ours[(j['id'], j['cx'], j['cz'])] = j
            refk = {k: v for k, v in ref.items() if cx0 <= k[1] <= cx1 and cz0 <= k[2] <= cz1}
            ok = yo = 0; diffs = []
            missing = [k for k in refk if k not in ours]; extra = [k for k in ours if k not in refk]
            for k, v in refk.items():
                if k not in ours:
                    continue
                rp = [ref_key(x) for x in v['Children']]; op = [our_key(x) for x in ours[k]['pieces']]
                if rp == op:
                    ok += 1; continue
                rpn = [ref_key(x, True) for x in v['Children']]; opn = [our_key(x, True) for x in ours[k]['pieces']]
                if rpn == opn:
                    yo += 1; continue
                if len(diffs) < a.max_diff:
                    i = next((i for i in range(min(len(rpn), len(opn))) if rpn[i] != opn[i]), min(len(rpn), len(opn)))
                    diffs.append((k, len(rpn), len(opn), i, rp[i] if i < len(rp) else None, op[i] if i < len(op) else None))
            tot += len(refk); full_ok += ok; yonly += yo
            print(f'{name:16s} {base:46s} эталон {len(refk):3d} наши {len(ours):3d} совпало {ok:3d} только-y {yo:3d} иначе {len(refk) - ok - yo - len(missing):3d} нет {len(missing)} лишних {len(extra)}')
            if a.v:
                for k in missing[:5]: print('   нет:', k)
                for k in extra[:5]: print('   лишний:', k)
                for k, nr, no, i, r, o in diffs:
                    print('   расхождение', k, 'частей', nr, no, 'первая', i); print('     эталон:', json.dumps(r)); print('     наше  :', json.dumps(o))
    print(f'ИТОГО: стартов {tot}, полностью {full_ok}, только по y (уточнение высоты при рисовании) {yonly}')


if __name__ == '__main__':
    main()

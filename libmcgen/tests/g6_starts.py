#!/usr/bin/env python3
"""G6s: старты построек libmcgen против сохранённых стартов эталонных миров (structures.starts в .mca настоящего сервера).

  g6_starts.py [--version 26.3] [--sets villages,pillager_outposts,…] [--seeds 12345] [-v]
Для каждого мира run/gt/<V>/structure_minecraft_<набор>/<dim>-s<seed>-c<cx>_<cz>-r<R> берутся ВСЕ чанки с сохранённым стартом (включая внешние кольца
генерации), строится список стартов (id, чанк-источник, bounding box, части: id, BB, поворот, позиция, шаблон, соединения) и сравнивается с
выводом `build/tests/g6_starts`. Печатает таблицу: стартов в эталоне / у нас / полностью совпавших / частично; -v — подробности расхождений.
"""
import argparse, glob, json, os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools', 'gt'))
import anvil
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BUILD = os.environ.get('G6_BUILD', 'build')       # каталог сборки libmcgen (make B=…)
BIN = f'{ROOT}/libmcgen/{BUILD}/tests/g6_starts'
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
DIMDIR = {'overworld': 'overworld', 'nether': 'the_nether', 'end': 'the_end'}


def tolist(v):
    return [int(x) for x in v]


def ref_starts(world, dim):
    """все сохранённые старты мира: {(id,cx,cz): start-dict}"""
    rd = anvil.region_dir(world + '/world', DIMDIR[dim])
    out = {}
    for f in sorted(glob.glob(f'{rd}/r.*.mca')):
        rx, rz = map(int, os.path.basename(f).split('.')[1:3])
        rf = anvil.RegionFile(f)
        for lz in range(32):
            for lx in range(32):
                cx, cz = rx * 32 + lx, rz * 32 + lz
                if not rf.has(cx, cz):
                    continue
                n = anvil.parse_nbt(rf.raw(cx, cz))
                for k, v in n.get('structures', {}).get('starts', {}).items():
                    if v.get('id') == 'INVALID':
                        continue
                    out[(k, v['ChunkX'], v['ChunkZ'])] = v
        rf.close()
    return out


def piece_key(p):
    d = {'id': p.get('id'), 'bb': tolist(p['BB']), 'o': p.get('O'), 'gd': p.get('GD')}
    if 'pool_element' in p:
        pe = p['pool_element']
        d.update(pos=[p['PosX'], p['PosY'], p['PosZ']], rot=p['rotation'], gld=p['ground_level_delta'], loc=pe.get('location', pe.get('feature', 'list' if pe['element_type'].endswith('list_pool_element') else 'empty')),
                 junctions=[[j['source_x'], j['source_ground_y'], j['source_z'], j['delta_y'], j['dest_proj']] for j in p.get('junctions', [])])
    return d


def our_piece_key(p, ref=None):
    d = {'id': p['id'], 'bb': p['bb'], 'o': p['o'], 'gd': p['gd']}
    if 'pos' in p:
        # «expansion hack» растягивает BB по высоте; у эталона он сохранён то растянутым, то нет (части перечитываются из NBT по ходу
        # генерации сервера). Высота y1 на рельеф и размещение не влияет (Beardifier: kernel ±12 от земли), поэтому принимаем любой из двух вариантов
        d['bb'] = list(p['bb'])
        if ref is not None and ref['bb'][4] == p['bb0'][4]:
            d['bb'][4] = p['bb0'][4]
        d.update(pos=p['pos'], rot=p['rot'], gld=p['gld'], loc=p['loc'], junctions=p['junctions'])
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--sets', default=''); ap.add_argument('--seeds', default='')
    ap.add_argument('-v', action='store_true'); ap.add_argument('--max-diff', type=int, default=3)
    a = ap.parse_args()
    sets = [s for s in a.sets.split(',') if s]
    seeds = [s for s in a.seeds.split(',') if s]
    rows = []
    for d in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/structure_minecraft_*')):
        name = os.path.basename(d).replace('structure_minecraft_', '')
        if sets and name not in sets:
            continue
        for w in sorted(glob.glob(f'{d}/*-s*-c*-r*')):
            base = os.path.basename(w)
            dim = base.split('-s')[0]
            seed = base.split('-s', 1)[1].rsplit('-c', 1)[0]
            if seeds and seed not in seeds:
                continue
            ref = ref_starts(w, dim)
            setids = {e['structure'] for e in json.load(open(f'{ROOT}/run/pack-{a.version}/data/minecraft/worldgen/structure_set/{name}.json'))['structures']}
            if not ref:
                continue
            xs = [k[1] for k in ref]; zs = [k[2] for k in ref]
            # область: все чанки со стартами ±1
            cx0, cx1, cz0, cz1 = min(xs) - 40, max(xs) + 40, min(zs) - 40, max(zs) + 40
            mf = json.load(open(f'{w}/manifest.json'))
            ac = mf['area_chunks']      # [x0,z0,x1,z1] запрошенной области; кольца вокруг неё тоже сгенерированы
            R = mf['radius'] + 6
            cx0, cz0, cx1, cz1 = ac[0] - 6, ac[1] - 6, ac[2] + 6, ac[3] + 6
            p = subprocess.run([BIN, f'{ROOT}/run/pack-{a.version}', a.version, DIMS[dim], seed, str(cx0), str(cz0), str(cx1 - cx0 + 1), str(cz1 - cz0 + 1)],
                               capture_output=True, text=True)
            if p.returncode:
                print(name, w, 'ОШИБКА', p.stderr[-300:]); rows.append((name, base, -1, -1, -1, -1)); continue
            ours = {}
            for line in p.stdout.splitlines():
                j = json.loads(line)
                if setids and j['id'] not in setids:
                    continue
                ours[(j['id'], j['cx'], j['cz'])] = j
            # ограничим эталон той же областью
            refk = {k: v for k, v in ref.items() if cx0 <= k[1] <= cx1 and cz0 <= k[2] <= cz1}
            full = part = 0; missing = [k for k in refk if k not in ours]; extra = [k for k in ours if k not in refk]
            diffs = []
            for k in refk:
                if k not in ours:
                    continue
                rp = [piece_key(x) for x in refk[k]['Children']]; op = [our_piece_key(x, rp[i] if i < len(rp) else None) for i, x in enumerate(ours[k]['pieces'])]
                if rp == op:
                    full += 1
                else:
                    part += 1
                    if len(diffs) < a.max_diff:
                        i = next((i for i in range(min(len(rp), len(op))) if rp[i] != op[i]), min(len(rp), len(op)))
                        diffs.append((k, len(rp), len(op), i, rp[i] if i < len(rp) else None, op[i] if i < len(op) else None))
            rows.append((name, base, len(refk), len(ours), full, part))
            print(f'{name:22s} {base:46s} эталон {len(refk):3d}  наши {len(ours):3d}  совпало {full:3d}  частично {part:3d}  нет у нас {len(missing)}  лишних {len(extra)}')
            if a.v:
                for k in missing[:5]: print('   нет:', k)
                for k in extra[:5]: print('   лишний:', k)
                for k, nr, no, i, r, o in diffs:
                    print('   расхождение', k, 'частей', nr, no, 'первая отличающаяся', i); print('     эталон:', json.dumps(r)); print('     наше  :', json.dumps(o))
    tot = sum(r[2] for r in rows if r[2] > 0); ok = sum(r[4] for r in rows if r[2] > 0)
    print(f'ИТОГО: стартов в эталоне {tot}, полностью совпали {ok}')


if __name__ == '__main__':
    main()

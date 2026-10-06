#!/usr/bin/env python3
"""G6: блоки построек libmcgen (стадии BIOMES|TERRAIN|SURFACE|STRUCTURES = 0x27) против эталонов run/gt/<V>/structure_minecraft_<набор>/… блок-в-блок.

  g6_blocks.py [--version 26.3] [--sets villages,…] [--seeds 12345,…] [-v] [--list N] [--md файл]
Эталон: настоящий сервер с ОДНИМ набором построек, поверхность включена, карверы/фичи отключены датапаком (tools/gt). Сравнение — tools/gt/diff.py
(без масок построек; текущие жидкости маскируются как в ворота G2–G5: растекание вне стадий генерации). Метрики на мир:
  «область» — все блоки областей forceload (≥289 чанков) и результат diff.py (порог G6 99,9 % из спеки);
  «у построек» — только чанки, куда попадает bounding box старта эталона (расширенный на 12 блоков, как у Beardifier): там и видны построенные блоки.
Печатает таблицу и итог; коды возврата: 0 — у построек ≥ порога во всех мирах, 1 — ниже.
"""
import argparse, glob, json, os, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools', 'gt'))
import diff, anvil
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BUILD = os.environ.get('G6_BUILD', 'build')       # каталог сборки libmcgen (make B=…)
CLI = f'{ROOT}/libmcgen/{BUILD}/mcgen-cli'
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
DIMDIR = {'overworld': 'overworld', 'nether': 'the_nether', 'end': 'the_end'}
TMP = os.environ.get('G6_TMP', '/tmp/g6')       # временные дампы (для параллельной работы — свой каталог)


def affected_chunks(world, dim, box):
    """чанки области box=(x0,z0,x1,z1), в которые попадает bounding box стартов (+12) любых сохранённых стартов"""
    rd = anvil.region_dir(world + '/world', DIMDIR[dim])
    starts = []
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
                    if v.get('id') == 'INVALID' or not v.get('Children'):
                        continue
                    bbs = [list(map(int, c['BB'])) for c in v['Children']]
                    starts.append((k, [min(b[0] for b in bbs), min(b[2] for b in bbs), max(b[3] for b in bbs), max(b[5] for b in bbs)]))
        rf.close()
    res = set()
    for k, b in starts:
        for cx in range((b[0] - 12) >> 4, ((b[2] + 12) >> 4) + 1):
            for cz in range((b[1] - 12) >> 4, ((b[3] + 12) >> 4) + 1):
                if box[0] <= cx <= box[2] and box[1] <= cz <= box[3]:
                    res.add((cx, cz))
    return res, [s[0] for s in starts]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--sets', default=''); ap.add_argument('--seeds', default='')
    ap.add_argument('-v', action='store_true'); ap.add_argument('--list', type=int, default=0); ap.add_argument('--md', default=''); ap.add_argument('--json', default='', help='файл для JSON-результатов по мирам')
    ap.add_argument('--stages', default='0x27'); ap.add_argument('--threshold', type=float, default=99.9); ap.add_argument('--threads', default='0')
    a = ap.parse_args()
    os.makedirs(TMP, exist_ok=True)
    sets = [s for s in a.sets.split(',') if s]; seeds = [s for s in a.seeds.split(',') if s]
    rows = []; worst = 100.0
    for d in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/structure_minecraft_*')):
        name = os.path.basename(d).replace('structure_minecraft_', '')
        if sets and name not in sets:
            continue
        for w in sorted(glob.glob(f'{d}/*-s*-c*-r*')):
            base = os.path.basename(w); dim = base.split('-s')[0]; seed = base.split('-s', 1)[1].rsplit('-c', 1)[0]
            if seeds and seed not in seeds:
                continue
            if not os.path.exists(f'{w}/manifest.json'):          # мир не дозаписан (прерванная генерация) — пропуск
                continue
            mf = json.load(open(f'{w}/manifest.json')); ac = mf['area_chunks']; m = 2
            if not mf.get('ok', True):
                continue
            cx0, cz0, nx, nz = ac[0] - m, ac[1] - m, ac[2] - ac[0] + 1 + 2 * m, ac[3] - ac[1] + 1 + 2 * m
            out = f'{TMP}/{name}-{base}.mcr'
            t0 = time.time()
            p = subprocess.run([CLI, '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', DIMS[dim], '--preset', 'normal', '--seed', seed,
                                '--cx0', str(cx0), '--cz0', str(cz0), '--nx', str(nx), '--nz', str(nz), '--stages', a.stages, '--threads', a.threads, '--out', out],
                               capture_output=True, text=True, env=dict(os.environ, MCGEN_STRUCTURES_ONLY=f'minecraft:{name}'))
            secs = time.time() - t0
            if p.returncode:
                print(f'{name:20s} {base:46s} ОШИБКА CLI rc={p.returncode} {p.stderr[-200:]!r}'); rows.append((name, base, None)); worst = 0; continue
            R, o = diff.run(w, out, None, None, dim=dim, version=a.version, min_status='minecraft:full', ignore_state=[], ignore_y=[], only_y=None, mask_ext=False,
                            margin=m, top=8, biomes=False, heightmaps=False, min_match=a.threshold, allow_missing=False, region=None, no_cache=False,
                            list=a.list, flow_halo=0, stable_with=[], mask_flow=True)
            # блоки построек = блоки, которые строительство/Beardifier меняют в эталоне: расхождение эталона с прогоном БЕЗ построек (стадии 0x7)
            base0 = f'{TMP}/{name}-{base}.base.mcr'
            subprocess.run([CLI, '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', DIMS[dim], '--preset', 'normal', '--seed', seed, '--cx0', str(cx0),
                            '--cz0', str(cz0), '--nx', str(nx), '--nz', str(nz), '--stages', '0x7' if dim != 'the_end' else '0x7', '--threads', a.threads, '--out', base0], capture_output=True, text=True)
            Rb, _ = diff.run(w, base0, None, None, dim=dim, version=a.version, min_status='minecraft:full', ignore_state=[], ignore_y=[], only_y=None, mask_ext=False,
                             margin=m, top=0, biomes=False, heightmaps=False, min_match=0, allow_missing=False, region=None, no_cache=False, list=0, flow_halo=0, stable_with=[], mask_flow=True)
            os.remove(base0)
            nstruct = Rb['blocks_mismatch']
            aff, ids = affected_chunks(w, dim, (ac[0], ac[1], ac[2], ac[3]))
            cm = R['_cmap']
            bl = R['full']['blocks'] // max(1, R['chunks_compared'])
            env = dict(os.environ); env['MCGEN_STRUCTURES_ONLY'] = f'minecraft:{name}'
            amis = 0; ach = 0
            for k in aff:
                v = cm.get(f'{k[0]},{k[1]}')
                if v is not None:
                    ach += 1; amis += v[0]
            ablocks = ach * bl
            apct = 100.0 * (1 - amis / ablocks) if ablocks else 100.0
            spct = 100.0 * (1 - R['blocks_mismatch'] / nstruct) if nstruct else 100.0
            worst = min(worst, spct)
            rows.append((name, base, (R['match_pct'], R['blocks_mismatch'], ach, amis, apct, secs, ids, nstruct, spct)))
            print(f'{name:18s} {base:44s} область {R["match_pct"]:9.5f} % (расхожд. {R["blocks_mismatch"]:6d})  блоков построек {nstruct:7d}: совпало {spct:8.4f} %  (в {ach:3d} чанках {apct:8.4f} %)  {secs:5.1f} с  {",".join(sorted(set(i.replace("minecraft:", "") for i in ids)))}')
            if a.v and R['top_pairs']:
                for t in R['top_pairs'][:6]:
                    print('      ', t)
            if a.v and a.list and R.get('samples'):
                for sm in R['samples'][:a.list]:
                    print('       @', sm['x'], sm['y'], sm['z'], sm['ours'], '<-', sm['ref'])
    if a.json:
        out = [dict(set=r[0], world=r[1], error=True) if not r[2] else dict(set=r[0], world=r[1], area_pct=r[2][0], area_mismatch=r[2][1], chunks=r[2][2], chunk_mismatch=r[2][3],
                     chunk_pct=r[2][4], secs=r[2][5], structures=sorted(set(r[2][6])), struct_blocks=r[2][7], struct_pct=r[2][8]) for r in rows]
        json.dump(out, open(a.json, 'w'), ensure_ascii=False, indent=1)
    ok = [r for r in rows if r[2]]
    if ok:
        tm = sum(r[2][3] for r in ok); tb = sum(r[2][2] for r in ok)
        print(f'ИТОГО: миров {len(ok)}, худший по блокам построек {worst:.4f} %; расхождений {tm} в {tb} чанках у построек')
    sys.exit(0 if worst >= a.threshold else 1)


if __name__ == '__main__':
    main()

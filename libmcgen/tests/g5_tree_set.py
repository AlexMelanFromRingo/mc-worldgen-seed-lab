#!/usr/bin/env python3
"""G5-деревья: прогон libmcgen по эталонному миру (feature:<id> или featureset:<имя>) и разбор расхождений по «древесным» блокам.

    python3 libmcgen/tests/g5_tree_set.py --world run/gt/26.3/featureset_g5_overworld/overworld-s12345-c0_0-r10 [--only a,b] [--version 26.3] [--top 30] [--list 20]

Для featureset берутся placed_feature из tools/gt/featuresets/<имя>.json (нереализованные типы в libmcgen молча пропускаются).
Печатает: всего блоков сравнено/расхождений, пары «наше → эталон» среди бревён/листвы/лоз/грибов и доля расхождений вне «древесных» блоков.
"""
import argparse, glob, json, os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'the_nether': 'minecraft:the_nether', 'end': 'minecraft:the_end', 'the_end': 'minecraft:the_end'}
TREE_WORDS = ('log', 'leaves', 'wood', 'vine', 'stem', 'mushroom', 'wart', 'shroomlight', 'roots', 'propagule', 'bee_nest', 'cocoa', 'moss', 'litter', 'podzol',
              'dirt', 'azalea', 'nylium', 'creaking', 'hyphae', 'fungus', 'planks', 'mud')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--world', required=True); ap.add_argument('--version', default='26.3'); ap.add_argument('--only', default='')
    ap.add_argument('--top', type=int, default=25); ap.add_argument('--list', type=int, default=0); ap.add_argument('--stages', default='0x17')
    ap.add_argument('--threads', type=int, default=0); ap.add_argument('--keep', default=''); ap.add_argument('--pp-margin', type=int, default=1); ap.add_argument('--margin', type=int, default=0)
    ap.add_argument('--cli', default=f'{ROOT}/libmcgen/build/mcgen-cli')
    a = ap.parse_args()
    wd = a.world.rstrip('/')
    m = json.load(open(f'{wd}/manifest.json'))
    var = m['variant']
    if a.only: only = a.only
    elif var.startswith('featureset:'):
        only = ','.join(json.load(open(f'{ROOT}/tools/gt/featuresets/{var.split(":", 1)[1]}.json')))
    else:
        only = var[len('feature:'):]
    x0, z0, x1, z1 = m['area_chunks']
    out = a.keep or tempfile.mktemp(suffix='.mcr', dir='/tmp')
    env = dict(os.environ, MCGEN_FEATURES_ONLY=only)
    cmd = [a.cli, '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', DIMS[m['dim']], '--seed', str(m['seed']),
           '--cx0', str(x0), '--cz0', str(z0), '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', a.stages, '--threads', str(a.threads),
           '--pp-margin', str(a.pp_margin), '--out', out]
    import time
    t = time.time()
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        print('ОШИБКА mcgen-cli:', r.stderr[-400:]); sys.exit(2)
    print(f'mcgen-cli: {time.time() - t:.1f} с')
    js = out + '.json'
    dcmd = [sys.executable, f'{ROOT}/tools/gt/diff.py', '--ref', wd, '--mcr', out, '--json', js, '--margin', str(a.margin), '--dim', m['dim'].replace('the_', ''),
            '--version', a.version, '--top', str(max(a.top, 200)), '--list', str(a.list)]
    r = subprocess.run(dcmd, capture_output=True, text=True)
    if a.list: print(r.stdout[-6000:])
    d = json.load(open(js))
    print(f"сравнено {d['blocks_compared']:,}  расхождений {d['blocks_mismatch']:,}  {d['match_pct']:.6f} %")
    tree = other = 0
    rows = []
    for p in d.get('top_pairs', []):
        # формат пары: [наше, эталон, число] или словарь
        if isinstance(p, dict): ours, ref, n = p.get('ours') or p.get('ours_state'), p.get('ref') or p.get('ref_state'), p.get('count') or p.get('n')
        else: ours, ref, n = p[0], p[1], p[2]
        is_tree = any(w in (ours + ref) for w in TREE_WORDS)
        (rows if is_tree else []).append((n, ours, ref))
        if is_tree: tree += n
        else: other += n
    print(f'в топ-парах: «древесных» {tree:,}, прочих {other:,}')
    for n, o, rf in rows[:a.top]:
        print(f'  {n:>8,}  наше {o}  эталон {rf}')
    if not a.keep:
        for f in (out, js):
            if os.path.exists(f): os.remove(f)


if __name__ == '__main__':
    main()

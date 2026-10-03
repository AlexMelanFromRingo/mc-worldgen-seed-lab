#!/usr/bin/env python3
"""Таблица «placed_feature × статус» для деревьев/грибов-деревьев: изолированные миры feature:<id> (margin 0 / margin 2 / margin 2 с маской недетерминизма / шум игры ref↔rep1)
и одиночные деревья custom:<имя>.

    python3 libmcgen/tests/g5_tree_table.py [--version 26.3] [--out libmcgen/tests/results/g5-trees-26.3.json]

Результат — JSON и markdown-таблицы на stdout.
"""
import argparse, glob, json, os, subprocess, sys, tempfile
ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'the_nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
FEATS = ['birch_tall', 'trees_birch', 'trees_badlands', 'trees_taiga', 'trees_old_growth_pine_taiga', 'trees_old_growth_spruce_taiga', 'trees_jungle', 'trees_sparse_jungle',
         'trees_savanna', 'trees_swamp', 'trees_mangrove', 'trees_cherry', 'trees_flower_forest', 'trees_birch_and_oak_leaf_litter', 'dark_forest_vegetation', 'trees_snowy',
         'trees_windswept_hills', 'trees_grove', 'trees_meadow', 'trees_dappled_forest', 'trees_water', 'trees_plains', 'trees_windswept_savanna', 'mushroom_island_vegetation',
         'rooted_azalea_tree', 'crimson_fungi', 'warped_fungi']


def diff(args, dim, version, margin, extra=()):
    js = tempfile.mktemp(suffix='.json')
    subprocess.run([sys.executable, f'{ROOT}/tools/gt/diff.py'] + args + ['--margin', str(margin), '--dim', dim.replace('the_', ''), '--version', version, '--json', js, '--top', '0'] + list(extra),
                   capture_output=True, text=True)
    try:
        d = json.load(open(js)); os.remove(js)
        return d['blocks_compared'], d['blocks_mismatch']
    except Exception:
        return None, None


def gen(pack, version, m, only, out, stages='0x17'):
    x0, z0, x1, z1 = m['area_chunks']
    r = subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', pack, '--version', version, '--dim', D[m['dim']], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
                        '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', stages, '--pp-margin', '1', '--out', out], env=dict(os.environ, MCGEN_FEATURES_ONLY=only), capture_output=True, text=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--version', default='26.3'); ap.add_argument('--out', default='')
    a = ap.parse_args(); v = a.version
    rows = []
    for f in FEATS:
        ws = [d.rstrip('/') for d in sorted(glob.glob(f'{ROOT}/run/gt/{v}/feature_minecraft_{f}/*/')) if '_rep' not in d and os.path.exists(d + 'manifest.json')]
        if not ws: continue
        wd = ws[0]; m = json.load(open(wd + '/manifest.json'))
        if not m.get('ok'): continue
        out = tempfile.mktemp(suffix='.mcr')
        if not gen(f'{ROOT}/run/pack-{v}', v, m, f'minecraft:{f}', out): continue
        dim = m['dim']; reps = sorted(glob.glob(wd + '_rep*'))
        n0, m0 = diff(['--ref', wd, '--mcr', out], dim, v, 0)
        n2, m2 = diff(['--ref', wd, '--mcr', out], dim, v, 2)
        st = [x for r in reps for x in ('--stable-with', r)]
        _, s2 = diff(['--ref', wd, '--mcr', out], dim, v, 2, st) if reps else (None, None)
        noise = None
        if reps:
            x0, z0, x1, z1 = m['area_chunks']
            _, noise = diff(['--ref', wd, '--vs-world', reps[0], '--cx0', str(x0), '--cz0', str(z0), '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1)], dim, v, 0)
        os.remove(out)
        rows.append(dict(feature=f, dim=dim, seed=m['seed'], blocks0=n0, mism0=m0, blocks2=n2, mism2=m2, mism2_stable=s2, noise_ref_rep1=noise))
        print(f"{f:34s} m0 {m0:>8,}/{n0:>11,}  m2 {m2:>8,}  m2 stable {s2 if s2 is not None else '-':>8}  шум ref↔rep1 {noise if noise is not None else '-'}", flush=True)
    cust = []
    ov = f'{ROOT}/run/gt/_tree_overlay/{v}'
    if os.path.isdir(ov):
        for wdm in sorted(glob.glob(f'{ROOT}/run/gt/{v}/custom_*/*/manifest.json')):
            wd = os.path.dirname(wdm); name = wd.split('/custom_')[1].split('/')[0]
            m = json.load(open(wdm))
            if not m.get('ok'): continue
            out = tempfile.mktemp(suffix='.mcr')
            if not gen(ov, v, m, f'minecraft:gt_custom_{name}', out): continue
            n0, m0 = diff(['--ref', wd, '--mcr', out], m['dim'], v, 0)
            n1, m1 = diff(['--ref', wd, '--mcr', out], m['dim'], v, 1)
            os.remove(out)
            cust.append(dict(name=name, blocks0=n0, mism0=m0, blocks1=n1, mism1=m1))
            print(f'custom {name:34s} m0 {m0}  m1 {m1}', flush=True)
    res = dict(version=v, features=rows, custom=cust)
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True); json.dump(res, open(a.out, 'w'), indent=1, ensure_ascii=False)


if __name__ == '__main__':
    main()

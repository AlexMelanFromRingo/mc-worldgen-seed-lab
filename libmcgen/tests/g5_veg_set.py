#!/usr/bin/env python3
"""G5-veg «в сборе»: `featureset:veg_overworld` / `veg_the_nether` (W6; наборы — tools/gt/featuresets/veg_*.json): в эталоне у биомов оставлены только фичи набора
(на своих шагах), у нас — MCGEN_FEATURES_ONLY=<тот же список>. Сравнение блок-в-блок; для миров с повтором (`<мир>_rep1`) дополнительно считается шум самой ванили
(эталон против повтора) и результат с маской `--stable-with`.

    python3 libmcgen/tests/g5_veg_set.py [--version 26.3] [--report файл.json]
"""
import argparse, glob, json, os, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g5_features as G


def vs_vanilla(wd, rep, v):
    m = json.load(open(f'{wd}/manifest.json')); x0, z0, x1, z1 = m['area_chunks']
    out = tempfile.mktemp(suffix='.json')
    subprocess.run([sys.executable, f'{G.ROOT}/tools/gt/diff.py', '--ref', wd, '--vs-world', rep, '--cx0', str(x0), '--cz0', str(z0), '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1),
                    '--dim', m['dim'].replace('the_', ''), '--version', v, '--json', out], capture_output=True, text=True)
    d = json.load(open(out)) if os.path.exists(out) else {}
    if os.path.exists(out): os.remove(out)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--report', default=''); ap.add_argument('--stages', default='0x17')
    a = ap.parse_args()
    G.CLI = tempfile.mktemp(prefix='mcgen-cli-', dir='/tmp'); shutil.copy(os.environ.get('MCGEN_CLI') or f'{G.ROOT}/libmcgen/build/mcgen-cli', G.CLI)
    rows = []
    for fdir in sorted(glob.glob(f'{G.ROOT}/run/gt/{a.version}/featureset_veg_*')):
        name = os.path.basename(fdir)[len('featureset_'):]
        lst = json.load(open(f'{G.ROOT}/tools/gt/featuresets/{name}.json'))
        for wd in sorted(glob.glob(fdir + '/*/')):
            wd = wd.rstrip('/')
            if '_rep' in os.path.basename(wd) or not os.path.exists(f'{wd}/manifest.json'): continue
            m = json.load(open(f'{wd}/manifest.json'))
            if not m.get('ok'): continue
            d = G.run_one(a.version, wd, ','.join(lst), 1, 0, a.stages)
            ds = G.run_one(a.version, wd, ','.join(lst), 1, 0, a.stages, stable=True) if os.path.isdir(wd + '_rep1') else None
            vv = vs_vanilla(wd, wd + '_rep1', a.version) if os.path.isdir(wd + '_rep1') else None
            row = dict(set=name, world=os.path.basename(wd), blocks=d.get('blocks_compared'), mismatch=d.get('blocks_mismatch'), match_pct=d.get('match_pct'),
                       stable_mismatch=ds and ds.get('blocks_mismatch'), stable_pct=ds and ds.get('match_pct'), vanilla_vs_vanilla=vv and vv.get('blocks_mismatch'),
                       top=d.get('top_pairs', [])[:4])
            rows.append(row)
            print(f'{name:18s} {row["world"]:36s} {row["blocks"]:>12,} блоков  расхождений {row["mismatch"]:>8,}  {row["match_pct"]:.4f} %'
                  + (f'  | с маской rep1: {row["stable_mismatch"]:,} ({row["stable_pct"]:.4f} %); ваниль против ваниль: {row["vanilla_vs_vanilla"]:,}' if ds else ''), flush=True)
    if a.report: json.dump({'version': a.version, 'rows': rows}, open(a.report, 'w'), indent=1, ensure_ascii=False)


if __name__ == '__main__':
    main()

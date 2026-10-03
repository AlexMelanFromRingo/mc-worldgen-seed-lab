#!/usr/bin/env python3
"""G5i: изолированные фичи — libmcgen (стадии BIOMES|TERRAIN|SURFACE|FEATURES, фильтр MCGEN_FEATURES_ONLY=<placed_feature>) против настоящих
чанков сервера из run/gt/<V>/feature_<id>/<мир> (tools/gt, вариант feature:<id>).

    python3 libmcgen/tests/g5_features.py [--version 26.3] [--only ore_coal_upper,disk_clay] [--report libmcgen/tests/results/g5i-26.3.json]
Строка таблицы: фича, измерение, блоков сравнено, расхождений, % совпадения. Код возврата 1, если есть расхождения.
"""
import argparse, glob, json, os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMPL = set()
CLI = None   # копия mcgen-cli на время прогона (можно пересобирать библиотеку параллельно)
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'the_nether': 'minecraft:the_nether', 'end': 'minecraft:the_end', 'the_end': 'minecraft:the_end'}
def implemented(v):
    """placed_feature, полностью реализованные в libmcgen (impl=1 в выгрузке MCGEN_FEATURES_DUMP_ORDER), по всем измерениям"""
    ids = set()
    for dim in ('minecraft:overworld', 'minecraft:the_nether', 'minecraft:the_end'):
        f = tempfile.mktemp(suffix='.json', dir='/tmp')
        subprocess.run([CLI, '--pack', f'{ROOT}/run/pack-{v}', '--version', v, '--dim', dim, '--seed', '1', '--cx0', '0', '--cz0', '0', '--nx', '1', '--nz', '1', '--stages', '0x13',
                        '--threads', '1', '--out', f + '.mcr'], env=dict(os.environ, MCGEN_FEATURES_DUMP_ORDER=f), capture_output=True)
        if os.path.exists(f):
            ids |= {x['id'] for x in json.load(open(f))['placed'] if x['impl'] == 1}
            os.remove(f)
        if os.path.exists(f + '.mcr'): os.remove(f + '.mcr')
    return ids


def feature_type(v, fid):
    n = fid.split(':')[1]
    base = f'{ROOT}/run/pack-{v}/data/minecraft/worldgen'
    pf = json.load(open(f'{base}/placed_feature/{n}.json'))['feature']
    if not isinstance(pf, str):
        return pf['type']
    for d in ('feature', 'configured_feature'):
        fp = f'{base}/{d}/{pf.split(":")[1]}.json'
        if os.path.exists(fp): return json.load(open(fp))['type']
    return '?'


def run_one(v, wd, fid, pp_margin, threads, stages, keep=None):
    m = json.load(open(f'{wd}/manifest.json'))
    x0, z0, x1, z1 = m['area_chunks']
    out = keep or tempfile.mktemp(suffix='.mcr', dir='/tmp')
    env = dict(os.environ, MCGEN_FEATURES_ONLY=fid)
    cmd = [CLI, '--pack', f'{ROOT}/run/pack-{v}', '--version', v, '--dim', DIMS[m['dim']], '--seed', str(m['seed']),
           '--cx0', str(x0), '--cz0', str(z0), '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', stages, '--threads', str(threads),
           '--pp-margin', str(pp_margin), '--out', out]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        return {'error': r.stderr[-300:]}
    js = out + '.json'
    subprocess.run([sys.executable, f'{ROOT}/tools/gt/diff.py', '--ref', wd, '--mcr', out, '--json', js, '--margin', '0', '--dim', m['dim'].replace('the_', ''), '--version', v], capture_output=True, text=True)
    d = json.load(open(js)) if os.path.exists(js) else {'error': 'нет json'}
    for f in (out, js):
        if not keep and os.path.exists(f): os.remove(f)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--only', default=''); ap.add_argument('--report', default='')
    ap.add_argument('--pp-margin', type=int, default=1); ap.add_argument('--threads', type=int, default=0); ap.add_argument('--stages', default='0x17'); ap.add_argument('--control', type=int, default=1)
    a = ap.parse_args()
    global CLI
    import shutil
    CLI = tempfile.mktemp(prefix='mcgen-cli-', dir='/tmp'); shutil.copy(os.environ.get('MCGEN_CLI') or f'{ROOT}/libmcgen/build/mcgen-cli', CLI)
    only = {x if ':' in x else 'minecraft:' + x for x in a.only.split(',') if x}
    global IMPL
    IMPL = implemented(a.version)
    rows, bad = [], 0
    for fdir in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/feature_*')):
        for wd in sorted(glob.glob(fdir + '/*/')):
            wd = wd.rstrip('/')
            mp = f'{wd}/manifest.json'
            if not os.path.exists(mp): continue
            m = json.load(open(mp))
            if not m.get('ok') or not m['variant'].startswith('feature:'): continue
            fid = m['variant'][len('feature:'):]
            if only and fid not in only: continue
            if not only and fid not in IMPL: continue
            d = run_one(a.version, wd, fid, a.pp_margin, a.threads, a.stages)
            ctl = run_one(a.version, wd, fid, a.pp_margin, a.threads, hex(int(a.stages, 16) & ~16)) if a.control else {}   # без FEATURES: сколько блоков фича меняет в эталоне
            if 'error' in d and 'blocks_compared' not in d:
                print(f'{fid:45s} {m["dim"]:9s} ОШИБКА {d["error"]}'); bad += 1; continue
            row = dict(feature=fid, dim=m['dim'], world=os.path.basename(wd), blocks=d['blocks_compared'], mismatch=d['blocks_mismatch'], match_pct=d['match_pct'],
                       chunks=d['chunks_compared'], effect=ctl.get('blocks_mismatch'), top=d.get('top_pairs', [])[:3])
            rows.append(row); bad += d['blocks_mismatch'] > 0
            print(f'{fid:45s} {m["dim"]:9s} {d["blocks_compared"]:>12,} блоков  расхождений {d["blocks_mismatch"]:>8,}  {d["match_pct"]:.6f} %  эффект в эталоне {ctl.get("blocks_mismatch", "?")}', flush=True)
    if a.report:
        json.dump({'version': a.version, 'rows': rows}, open(a.report, 'w'), indent=1, ensure_ascii=False)
    print(f'итого: {len(rows)} фич, с расхождениями: {bad}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()

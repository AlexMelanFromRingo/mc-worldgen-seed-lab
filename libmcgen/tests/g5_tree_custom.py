#!/usr/bin/env python3
"""G5-деревья: одиночные деревья `custom:<имя>` против эталонов run/gt/<V>/custom_<имя>/ (tools/gt/custom/<имя>.json, делает W6).

    python3 libmcgen/tests/g5_tree_custom.py [имя …] [--version 26.3] [--margin 0] [--list N] [--keep]

Наша сторона: «оверлей-пак» run/gt/_tree_overlay/<V> (символические ссылки на run/pack-<V>; биомы — копии с features = 11 пустых шагов, шаг 9 = все gt_custom_*;
placed_feature gt_custom_<имя> из tools/gt/custom), MCGEN_FEATURES_ONLY=minecraft:gt_custom_<имя> — ровно то, что делает пак эталона.
"""
import argparse, glob, json, os, shutil, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'the_nether': 'minecraft:the_nether', 'end': 'minecraft:the_end', 'the_end': 'minecraft:the_end'}


def custom_dir(v):
    return f'{ROOT}/tools/gt/custom' + ('' if v == '26.3' else '-' + v)


def build_overlay(v):
    src = f'{ROOT}/run/pack-{v}'; dst = f'{ROOT}/run/gt/_tree_overlay/{v}'
    specs = {os.path.basename(f)[:-5]: json.load(open(f)) for f in sorted(glob.glob(custom_dir(v) + '/*.json'))}
    if os.path.exists(dst): shutil.rmtree(dst)
    os.makedirs(dst + '/data/minecraft/worldgen')
    for e in os.listdir(src):
        if e != 'data': os.symlink(f'{src}/{e}', f'{dst}/{e}')
    for e in os.listdir(f'{src}/data'):
        if e != 'minecraft': os.symlink(f'{src}/data/{e}', f'{dst}/data/{e}')
    for e in os.listdir(f'{src}/data/minecraft'):
        if e != 'worldgen': os.symlink(f'{src}/data/minecraft/{e}', f'{dst}/data/minecraft/{e}')
    wg = f'{src}/data/minecraft/worldgen'; dwg = f'{dst}/data/minecraft/worldgen'
    for e in os.listdir(wg):
        if e not in ('biome', 'placed_feature'): os.symlink(f'{wg}/{e}', f'{dwg}/{e}')
    os.makedirs(dwg + '/biome'); os.makedirs(dwg + '/placed_feature')
    ids = [f'minecraft:gt_custom_{n}' for n in specs]
    for f in glob.glob(wg + '/biome/*.json'):
        b = json.load(open(f))
        b['features'] = [[] for _ in range(11)]
        b['features'][9] = list(ids)
        json.dump(b, open(f'{dwg}/biome/{os.path.basename(f)}', 'w'))
    for f in glob.glob(wg + '/placed_feature/*.json'): os.symlink(f, f'{dwg}/placed_feature/{os.path.basename(f)}')
    for n, s in specs.items(): json.dump(s['placed_feature'], open(f'{dwg}/placed_feature/gt_custom_{n}.json', 'w'))
    return dst, specs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*'); ap.add_argument('--version', default='26.3'); ap.add_argument('--margin', type=int, default=0); ap.add_argument('--list', type=int, default=0)
    ap.add_argument('--rebuild', action='store_true'); ap.add_argument('--stages', default='0x17'); ap.add_argument('--keep', default='')
    a = ap.parse_args()
    v = a.version
    ov = f'{ROOT}/run/gt/_tree_overlay/{v}'
    if a.rebuild or not os.path.isdir(ov): ov, specs = build_overlay(v)
    else: specs = {os.path.basename(f)[:-5]: json.load(open(f)) for f in sorted(glob.glob(custom_dir(v) + '/*.json'))}
    names = a.names or sorted(specs)
    tot = bad = 0
    for n in names:
        wds = sorted(glob.glob(f'{ROOT}/run/gt/{v}/custom_{n}/*/manifest.json'))
        if not wds: print(f'{n:32s} нет эталона'); continue
        wd = os.path.dirname(wds[0]); m = json.load(open(wd + '/manifest.json'))
        if not m.get('ok'): print(f'{n:32s} эталон не готов'); continue
        x0, z0, x1, z1 = m['area_chunks']
        out = a.keep or tempfile.mktemp(suffix='.mcr', dir='/tmp')
        env = dict(os.environ, MCGEN_FEATURES_ONLY=f'minecraft:gt_custom_{n}')
        r = subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', ov, '--version', v, '--dim', DIMS[m['dim']], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
                            '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', a.stages, '--pp-margin', '1', '--out', out], env=env, capture_output=True, text=True)
        if r.returncode: print(f'{n:32s} ОШИБКА {r.stderr[-200:]}'); continue
        js = out + '.json'
        r = subprocess.run([sys.executable, f'{ROOT}/tools/gt/diff.py', '--ref', wd, '--mcr', out, '--json', js, '--margin', str(a.margin), '--dim', m['dim'].replace('the_', ''), '--version', v,
                            '--top', '8', '--list', str(a.list)], capture_output=True, text=True)
        d = json.load(open(js))
        top = d.get('top_pairs', [])[:3]
        tp = '; '.join(f'{p[0] if not isinstance(p, dict) else p}' for p in top)
        print(f"{n:32s} {d['blocks_compared']:>12,} блоков  расхождений {d['blocks_mismatch']:>7,}  {d['match_pct']:.6f} %  {tp[:140] if d['blocks_mismatch'] else ''}", flush=True)
        if a.list and d['blocks_mismatch']: print(r.stdout[-3000:])
        tot += 1; bad += d['blocks_mismatch'] > 0
        if not a.keep:
            for f in (out, js):
                if os.path.exists(f): os.remove(f)
    print(f'итого: {tot} фич, с расхождениями: {bad}')


if __name__ == '__main__':
    main()

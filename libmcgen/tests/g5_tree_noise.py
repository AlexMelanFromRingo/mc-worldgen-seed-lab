#!/usr/bin/env python3
"""Недетерминизм игры против нашей ошибки: для изолированной фичи сравнивает ref↔rep1 (игра с самой собой) и ours↔ref, ours↔rep1 (блоков расхождений).
    g5_tree_noise.py <feature_id> [--margin 0] [--version 26.3]"""
import argparse, glob, json, os, subprocess, sys, tempfile
ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser(); ap.add_argument('fid'); ap.add_argument('--margin', type=int, default=0); ap.add_argument('--version', default='26.3')
a = ap.parse_args()
base = f'{ROOT}/run/gt/{a.version}/feature_minecraft_{a.fid}'
refs = [d for d in sorted(glob.glob(base + '/*/')) if '_rep' not in d]
wd = refs[0].rstrip('/'); reps = sorted(glob.glob(wd + '_rep*'))
m = json.load(open(wd + '/manifest.json')); x0, z0, x1, z1 = m['area_chunks']; dim = m['dim'].replace('the_', '')
D = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
def run(args):
    r = subprocess.run([sys.executable, f'{ROOT}/tools/gt/diff.py'] + args + ['--margin', str(a.margin), '--dim', dim, '--version', a.version, '--top', '0'], capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        if ln.startswith('блоков сравнено'): return ln.split('расхождений')[1].split('->')[0].strip().replace(',', ''), ln
    return '?', r.stdout[-200:]
out = tempfile.mktemp(suffix='.mcr')
subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', D[dim], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
                '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', '0x17', '--pp-margin', '1', '--out', out], env=dict(os.environ, MCGEN_FEATURES_ONLY=f'minecraft:{a.fid}'), capture_output=True)
print(f'{a.fid} margin {a.margin}')
print('  ours ↔ ref :', run(['--ref', wd, '--mcr', out])[0])
for rp in reps:
    print(f'  ours ↔ {os.path.basename(rp)[-5:]} :', run(['--ref', rp, '--mcr', out])[0])
    print(f'  ref  ↔ {os.path.basename(rp)[-5:]} :', run(['--ref', wd, '--vs-world', rp, '--cx0', str(x0), '--cz0', str(z0), '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1)])[0])
os.remove(out)

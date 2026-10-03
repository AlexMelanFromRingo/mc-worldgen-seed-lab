#!/usr/bin/env python3
"""Расхождения в одном чанке: g5_tree_chunk.py <мир> <cx> <cz> [--only id] [--keep файл.mcr] — печатает блоки (x y z: наше -> эталон) этого чанка."""
import argparse, json, os, re, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser()
ap.add_argument('world'); ap.add_argument('cx', type=int, nargs='?'); ap.add_argument('cz', type=int, nargs='?'); ap.add_argument('--margin', type=int, default=0); ap.add_argument('--only', default=''); ap.add_argument('--version', default='26.3')
ap.add_argument('--n', type=int, default=400)
a = ap.parse_args()
wd = a.world.rstrip('/')
m = json.load(open(wd + '/manifest.json'))
only = a.only or m['variant'][len('feature:'):]
x0, z0, x1, z1 = m['area_chunks']
D = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'the_nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
out = tempfile.mktemp(suffix='.mcr', dir='/tmp')
r = subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', D[m['dim']], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
                    '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', '0x17', '--pp-margin', '1', '--out', out], env=dict(os.environ, MCGEN_FEATURES_ONLY=only), capture_output=True, text=True)
if r.returncode: print(r.stderr[-300:]); sys.exit(2)
if os.environ.get('SHOW_TRACE'): print(r.stderr[-20000:])
r = subprocess.run([sys.executable, f'{ROOT}/tools/gt/diff.py', '--ref', wd, '--mcr', out, '--margin', str(a.margin), '--dim', m['dim'].replace('the_', ''), '--version', a.version, '--list', '3000000', '--top', '0'], capture_output=True, text=True)
rows = []
if a.cx is None:
    import collections
    cnt = collections.Counter()
    for ln in r.stdout.splitlines():
        mm = re.match(r'\s+\((-?\d+), (-?\d+), (-?\d+)\)', ln)
        if mm: cnt[(int(mm.group(1)) >> 4, int(mm.group(3)) >> 4)] += 1
    print('чанки по возрастанию числа расхождений:', sorted(cnt.items(), key=lambda t: t[1])[:30]); os.remove(out); sys.exit(0)
for ln in r.stdout.splitlines():
    mm = re.match(r'\s+\((-?\d+), (-?\d+), (-?\d+)\)\s+(.*) -> (.*)$', ln)
    if mm:
        x, y, z = int(mm.group(1)), int(mm.group(2)), int(mm.group(3))
        if x >> 4 == a.cx and z >> 4 == a.cz: rows.append((y, x, z, mm.group(4), mm.group(5)))
rows.sort()
print(f'чанк ({a.cx},{a.cz}): {len(rows)} расхождений')
for y, x, z, o, rf in rows[:a.n]:
    sh = lambda s: s.replace('minecraft:', '').replace('persistent=false,waterlogged=false', '')
    print(f'  ({x},{y},{z})  {sh(o)} -> {sh(rf)}')
os.remove(out)

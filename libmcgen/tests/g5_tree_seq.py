#!/usr/bin/env python3
"""Последовательности попыток деревьев по чанкам: H — наша удачная попытка совпала с основанием ствола в эталоне, T — удачная у нас, но в эталоне ствола нет,
f — неудачная попытка. Первое T в чанке — место, где поток ГСЧ/условия разошлись с игрой.
    g5_tree_seq.py <мир> [--only id] [--chunk cx cz] [--version 26.3]"""
import argparse, collections, json, os, re, subprocess, sys
ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, f'{ROOT}/tools/gt')
import anvil
ap = argparse.ArgumentParser(); ap.add_argument('world'); ap.add_argument('--only', default=''); ap.add_argument('--chunk', type=int, nargs=2); ap.add_argument('--version', default='26.3')
ap.add_argument('--margin', type=int, default=1)
a = ap.parse_args()
wd = a.world.rstrip('/'); m = json.load(open(wd + '/manifest.json'))
only = a.only or m['variant'][len('feature:'):]
x0, z0, x1, z1 = m['area_chunks']
D = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether'}
import tempfile
out = tempfile.mktemp(suffix='.mcr')
r = subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', 'minecraft:the_nether' if 'nether' in m['dim'] else 'minecraft:overworld',
                    '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0), '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', '0x17', '--pp-margin', '1', '--out', out],
                   env=dict(os.environ, MCGEN_FEATURES_ONLY=only, MCGEN_TRACE_ATT='1'), capture_output=True, text=True)
w = anvil.World(wd + '/world', 'nether' if 'nether' in m['dim'] else 'overworld', a.version)
names = w.states.names
def isleaf(n): return '_leaves' in n or '_log' in n or '_stem' in n or 'wart_block' in n
bases = set()
for cz in range(z0, z1 + 1):
    for cx in range(x0, x1 + 1):
        c = w.chunk(cx, cz)
        if c is None: continue
        b = c.blocks
        for y in range(1, b.shape[0]):
            col = b[y]
            for z in range(16):
                for x in range(16):
                    n = names[int(col[z, x])]
                    if '_log' in n or '_stem' in n:
                        nb = names[int(b[y - 1, z, x])]
                        if '_log' not in nb and '_leaves' not in nb and 'air' not in nb and '_stem' not in nb: bases.add((cx * 16 + x, y + c.min_y, cz * 16 + z))
seq = collections.defaultdict(list)
fid = only.split(',')[0]
for mm in re.finditer(r'ATT chunk\((-?\d+),(-?\d+)\) (\S+) \((-?\d+),(-?\d+),(-?\d+)\) -> (\d)', r.stderr.replace('\r', '\n')):
    cx, cz, f, x, y, z, ok = int(mm.group(1)), int(mm.group(2)), mm.group(3), int(mm.group(4)), int(mm.group(5)), int(mm.group(6)), int(mm.group(7))
    if f == fid: seq[(cx, cz)].append(((x, y, z), ok))
tot = collections.Counter()
for cz in range(z0 + a.margin, z1 + 1 - a.margin):
    row = []
    for cx in range(x0 + a.margin, x1 + 1 - a.margin):
        s = ''.join(('H' if p in bases else 'T') if ok else 'f' for p, ok in seq.get((cx, cz), []))
        tot.update(s)
        row.append(s)
    if a.chunk is None: print(cz, ' '.join(f'{s:14s}' for s in row))
if a.chunk:
    for p, ok in seq.get(tuple(a.chunk), []): print(p, ok, 'H' if p in bases else '')
print('итого попыток H/T/f:', dict(tot))
if os.path.exists(out): os.remove(out)

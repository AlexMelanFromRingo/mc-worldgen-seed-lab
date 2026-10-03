#!/usr/bin/env python3
"""Проверка деревьев по трассе (MCGEN_TREE_TRACE): для каждого дерева чанка печатает блоки эталона/нашего в точке посадки и над ней.
    g5_tree_probe.py <мир> <cx> <cz> [--only id]"""
import argparse, json, os, re, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools/gt'))
import anvil, mcr
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser(); ap.add_argument('world'); ap.add_argument('cx', type=int); ap.add_argument('cz', type=int); ap.add_argument('--only', default=''); ap.add_argument('--version', default='26.3')
ap.add_argument('--dy', type=int, default=3)
a = ap.parse_args()
wd = a.world.rstrip('/'); m = json.load(open(wd + '/manifest.json'))
only = a.only or m['variant'][len('feature:'):]
x0, z0, x1, z1 = m['area_chunks']
D = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'the_nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}
import tempfile
out = os.path.join(tempfile.gettempdir(), 'mcgen_probe.mcr')
r = subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', D[m['dim']], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
                    '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', '0x17', '--pp-margin', '1', '--out', out], env=dict(os.environ, MCGEN_FEATURES_ONLY=only, MCGEN_TREE_TRACE='1'), capture_output=True, text=True)
trees = []
for ln in r.stderr.splitlines():
    mm = re.match(r'TREE chunk\((-?\d+),(-?\d+)\) at \((-?\d+),(-?\d+),(-?\d+)\) ok=(\d) res=(\d) trunks=(\d+) foliage=(\d+) decor=(\d+)', ln)
    if mm and int(mm.group(1)) == a.cx and int(mm.group(2)) == a.cz: trees.append(tuple(int(g) for g in mm.groups()[2:]))
w = anvil.World(wd + '/world', m['dim'].replace('the_', ''), a.version)
st = w.states
def refb(x, y, z):
    c = w.chunk(x >> 4, z >> 4)
    return st.names[int(c.blocks[y - c.min_y, z & 15, x & 15])] if c is not None and hasattr(c, 'min_y') else '?'
print(len(trees), 'деревьев в трассе чанка')
for t in trees:
    ox, oy, oz, ok, res, nt, nf, nd = t
    print(f'дерево ({ox},{oy},{oz}) ok={ok} res={res} trunks={nt} foliage={nf} decor={nd}')
    for dy in range(0, a.dy + 1):
        print('   y+%d эталон %s' % (dy, refb(ox, oy + dy, oz).replace('minecraft:', '')))

#!/usr/bin/env python3
"""Проверка, что расхождения биомов клеток с эталонными мирами (G2) — ничьи R-дерева (равный минимальный fitness двух
биомов в той же климатической точке; в игре такой выбор зависит от предыдущего запроса потока — Climate.RTree lastResult).

    python3 libmcgen/tests/biome_ties.py --version 26.3 [--variant raw]
"""
import argparse, glob, json, os, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import anvil, mcr  # noqa
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')
DIM_FULL = {'overworld': 'minecraft:overworld', 'the_nether': 'minecraft:the_nether', 'nether': 'minecraft:the_nether', 'the_end': 'minecraft:the_end', 'end': 'minecraft:the_end'}

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--version', default='26.3'); ap.add_argument('--variant', default='raw')
    ap.add_argument('--tmp', default=os.environ.get('TMPDIR', '/tmp')); a = ap.parse_args()
    pack = os.path.join(ROOT, 'run', f'pack-{a.version}')
    tot = ties = cells = 0
    for ref in sorted(glob.glob(os.path.join(ROOT, 'run', 'gt', a.version, a.variant, '*'))):
        m = json.load(open(os.path.join(ref, 'manifest.json')))
        dim, seed = m['dim'], m['seed']; x0, z0, x1, z1 = m['area_chunks']; nx, nz = x1 - x0 + 1, z1 - z0 + 1
        out = os.path.join(a.tmp, 'ties.mcr')
        subprocess.run([CLI, '--pack', pack, '--version', a.version, '--dim', DIM_FULL[dim], '--seed', str(seed), '--cx0', str(x0), '--cz0', str(z0),
                        '--nx', str(nx), '--nz', str(nz), '--stages', '0x1', '--out', out], check=True, capture_output=True)
        ours = mcr.Mcr(out); world = anvil.World(os.path.join(ref, 'world'), dim if dim in ('overworld',) else dim.replace('the_', ''), a.version)
        lines = []
        for cz in range(z0, z1 + 1):
            for cx in range(x0, x1 + 1):
                c = world.chunk(cx, cz)
                if c is None: continue
                ob = ours.biomes(cx, cz); rb = c.biomes
                cells += ob.size
                for qy, qz, qx in zip(*np.nonzero(np.array([[[ours.biome_names[ob[y, z, x]] != world.biomes.names[rb[y, z, x]] for x in range(4)] for z in range(4)] for y in range(ob.shape[0])]))):
                    lines.append(f'{cx * 4 + qx} {qy + ours.min_y // 4} {cz * 4 + qz} {ours.biome_names[ob[qy, qz, qx]]} {world.biomes.names[rb[qy, qz, qx]]}')
        if lines:
            r = subprocess.run([CLI, 'biometie', '--pack', pack, '--version', a.version, '--dim', DIM_FULL[dim], '--seed', str(seed)],
                               input='\n'.join(lines) + '\n', capture_output=True, text=True, check=True)
            t = [int(x) for x in r.stdout.split()]
            tot += len(t); ties += sum(t)
            bad = [l for l, x in zip(lines, t) if not x]
            print(f'{os.path.basename(ref):45s} расхождений биомов {len(t):5d}, из них ничьих {sum(t):5d}' + (f'  НЕ ничьи: {bad[:3]}' if bad else ''), flush=True)
        os.remove(out)
    print(f'\nИТОГО: клеток {cells}, расхождений {tot}, ничьих {ties}, не-ничьих {tot - ties}')
    return 0 if tot == ties else 1

if __name__ == '__main__':
    sys.exit(main())

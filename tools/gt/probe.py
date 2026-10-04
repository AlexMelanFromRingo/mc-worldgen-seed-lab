#!/usr/bin/env python3
"""Окрестность блока: наш дамп (.mcr) против эталонного мира, слоями по y.
    probe.py <мир> <файл.mcr> x y z [r=2] [--dim overworld] [--version 26.3]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import argparse, numpy as np, mcr, anvil


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('world'); ap.add_argument('mcr'); ap.add_argument('x', type=int); ap.add_argument('y', type=int); ap.add_argument('z', type=int)
    ap.add_argument('--r', type=int, default=2); ap.add_argument('--dim', default='overworld'); ap.add_argument('--version', default='26.3')
    a = ap.parse_args()
    m = mcr.Mcr(a.mcr); W = anvil.World(a.world + '/world', a.dim, a.version)
    def short(n): return n.replace('minecraft:', '')
    def ours(x, y, z):
        c = m.blocks(x >> 4, z >> 4); return short(m.state_names[c[y - m.min_y, z & 15, x & 15]])
    def ref(x, y, z):
        c = W.chunk(x >> 4, z >> 4); b = c.blocks[y - c.min_y, z & 15, x & 15]; return short(W.states.names[b]) if hasattr(W.states, 'names') else str(b)
    for dy in range(a.r, -a.r - 1, -1):
        print(f'y={a.y + dy}')
        for dz in range(-a.r, a.r + 1):
            print('  ' + ' '.join(f'{ours(a.x + dx, a.y + dy, a.z + dz)[:14]:14s}' for dx in range(-a.r, a.r + 1)) + '   |   ' +
                  ' '.join(f'{ref(a.x + dx, a.y + dy, a.z + dz)[:14]:14s}' for dx in range(-a.r, a.r + 1)))


if __name__ == '__main__':
    main()

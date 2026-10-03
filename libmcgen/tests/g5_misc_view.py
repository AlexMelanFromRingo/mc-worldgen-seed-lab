#!/usr/bin/env python3
"""Отладка G5-misc: вывести блоки эталона и libmcgen в окне вокруг точки.

    python3 libmcgen/tests/g5_misc_view.py --ref run/gt/26.3/feature_minecraft_ice_spike/<мир> --mcr ours.mcr --at X Y Z [--r 6] [--dy 0] [--mode col|slice]
col   — колонка (x,z) по y в диапазоне ±r: «y  эталон | наше»
slice — горизонтальный срез на y=Y в квадрате ±r: символы по таблице; различия помечены '#', справа — карта эталона и наша
"""
import argparse, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, f'{ROOT}/tools/gt')
import anvil
from mcr import Mcr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True); ap.add_argument('--mcr', required=True); ap.add_argument('--at', nargs=3, type=int, required=True)
    ap.add_argument('--r', type=int, default=6); ap.add_argument('--mode', default='col'); ap.add_argument('--dim', default='overworld'); ap.add_argument('--version', default='26.3')
    a = ap.parse_args()
    ref = anvil.World(anvil.find_world(a.ref) if hasattr(anvil, 'find_world') else (a.ref + '/world'), a.dim, a.version)
    ours = Mcr(a.mcr)
    X, Y, Z = a.at
    def r_at(x, y, z):
        c = ref.chunk(x >> 4, z >> 4)
        if c is None: return '?'
        return ref.states.names[c.blocks[y - c.min_y, z & 15, x & 15]] if hasattr(ref.states, 'names') else str(c.blocks[y - c.min_y, z & 15, x & 15])
    def o_at(x, y, z):
        cx, cz = x >> 4, z >> 4
        if not ours.has(cx, cz): return '?'
        n = ours.state_names[int(ours.blocks(cx, cz)[y - ours.min_y, z & 15, x & 15])]
        b, pr = anvil.parse_state_name(n)
        return anvil.canon_name(b, dict(pr))
    if a.mode == 'col':
        for y in range(Y + a.r, Y - a.r - 1, -1):
            r, o = r_at(X, y, Z), o_at(X, y, Z)
            print(f'{y:4d} {"  " if r == o else "##"} ref={r:45s} ours={o}')
    else:
        pal = {}
        def ch(n):
            if n == 'minecraft:air': return '.'
            if n not in pal: pal[n] = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'[len(pal) % 52]
            return pal[n]
        for z in range(Z - a.r, Z + a.r + 1):
            lr = ''.join(ch(r_at(x, Y, z)) for x in range(X - a.r, X + a.r + 1))
            lo = ''.join(ch(o_at(x, Y, z)) for x in range(X - a.r, X + a.r + 1))
            df = ''.join('#' if r_at(x, Y, z) != o_at(x, Y, z) else ' ' for x in range(X - a.r, X + a.r + 1))
            print(f'{z:6d} {lr}  {lo}  {df}')
        print({v: k for k, v in pal.items()})


if __name__ == '__main__':
    main()

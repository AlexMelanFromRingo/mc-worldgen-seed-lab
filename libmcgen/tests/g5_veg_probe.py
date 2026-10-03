#!/usr/bin/env python3
"""Отладка G5-veg: срез блоков эталона и нашего дампа рядом.

    python3 libmcgen/tests/g5_veg_probe.py <feature> <x> <y> <z> [--r 6] [--axis z|x] [--version 26.3] [--mcr файл.mcr] [--world <каталог эталона>]
Печатает срез плоскостью (axis=z: по x горизонтально, по y вертикально, z фиксирован) эталона и дампа; символ — первая буква блока,
расшифровка легенды ниже. Дамп по умолчанию — <scratch>/o_<feature>.mcr, который пишет dif.sh; можно указать свой --mcr.
"""
import argparse, glob, json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, f'{ROOT}/tools/gt')
import anvil
from mcr import Mcr


def sym(name, legend):
    base = name.split('[')[0].replace('minecraft:', '')
    if base in ('air', 'cave_air', 'void_air'):
        return '.'
    for k, v in legend.items():
        if v == base:
            return k
    pool = (base[0].upper() + base[0] + '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ')
    for ch in pool:
        if ch not in legend:
            legend[ch] = base
            return ch
    return '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('feature'); ap.add_argument('x', type=int); ap.add_argument('y', type=int); ap.add_argument('z', type=int)
    ap.add_argument('--r', type=int, default=6); ap.add_argument('--h', type=int, default=8)
    ap.add_argument('--axis', default='z'); ap.add_argument('--version', default='26.3')
    ap.add_argument('--mcr', default=''); ap.add_argument('--world', default='')
    a = ap.parse_args()
    fid = a.feature.replace('minecraft:', '')
    wd = a.world or sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/feature_minecraft_{fid}/*/'))[0].rstrip('/')
    m = json.load(open(f'{wd}/manifest.json'))
    ref = anvil.World(wd + '/world' if os.path.isdir(wd + '/world') else wd, m['dim'], a.version)
    mcr = Mcr(a.mcr or glob.glob('/tmp/claude-*/*/*/scratchpad/o_' + fid + '.mcr')[0])
    legend = {}

    def cell(x, y, z):
        cx, cz = x >> 4, z >> 4
        rc = ref.chunk(cx, cz)
        rn = ref.states.names[int(rc.blocks[y - rc.min_y][z & 15][x & 15])] if rc is not None else '?'
        on = mcr.state_names[int(mcr.blocks(cx, cz)[y - mcr.min_y][z & 15][x & 15])] if mcr.has(cx, cz) else '?'
        return rn, on
    out = {'ref': [], 'ours': []}
    for y in range(a.y + a.h, a.y - a.h - 1, -1):
        rr, oo = '', ''
        for d in range(-a.r, a.r + 1):
            x, z = (a.x + d, a.z) if a.axis == 'z' else (a.x, a.z + d)
            rn, on = cell(x, y, z)
            rr += sym(rn, legend); oo += sym(on, legend)
        out['ref'].append(f'{y:5d} {rr}'); out['ours'].append(f'{y:5d} {oo}')
    print(f'срез {a.axis}: {a.feature} центр ({a.x},{a.y},{a.z}); слева эталон, справа наш дамп')
    for r, o in zip(out['ref'], out['ours']):
        print(r + '   |   ' + o[6:] + ('   <<<' if r[6:] != o[6:] else ''))
    print('легенда: ' + ', '.join(f'{k}={v}' for k, v in legend.items()))


if __name__ == '__main__':
    main()

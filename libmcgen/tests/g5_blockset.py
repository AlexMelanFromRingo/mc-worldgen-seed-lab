#!/usr/bin/env python3
"""Сравнение «множеств позиций» блоков по именам (шаблон fnmatch) между эталоном и дампом libmcgen — оценка фич в сборе.
    python3 libmcgen/tests/g5_blockset.py ref.mcr ours.mcr 'minecraft:coal_ore' 'minecraft:poppy' --margin 2
Печатает: число блоков в эталоне / у нас, пересечение, доля пересечения от эталона и от нас (Jaccard)."""
import argparse, fnmatch, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools', 'gt'))
from mcr import Mcr
ap = argparse.ArgumentParser(); ap.add_argument('ref'); ap.add_argument('ours'); ap.add_argument('names', nargs='+'); ap.add_argument('--margin', type=int, default=2)
a = ap.parse_args()
R, O = Mcr(a.ref), Mcr(a.ours)
def ids(m, pat): return [i for i, n in enumerate(m.state_names) if fnmatch.fnmatch(n, pat)]
for pat in a.names:
    ir, io = ids(R, pat), ids(O, pat)
    nr = no = inter = 0
    for cz in range(R.cz0 + a.margin, R.cz0 + R.nz - a.margin):
        for cx in range(R.cx0 + a.margin, R.cx0 + R.nx - a.margin):
            br = np.isin(R.blocks(cx, cz), ir); bo = np.isin(O.blocks(cx, cz), io)
            nr += int(br.sum()); no += int(bo.sum()); inter += int((br & bo).sum())
    print(f'{pat:44s} эталон {nr:>9,}  наши {no:>9,}  общих {inter:>9,}  {100*inter/max(nr,1):6.2f} % эталона  Jaccard {100*inter/max(nr+no-inter,1):6.2f} %')

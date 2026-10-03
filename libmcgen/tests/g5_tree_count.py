#!/usr/bin/env python3
"""Счётчики блоков по подстрокам имён: эталон против нашего дампа. g5_tree_count.py <мир> <наш.mcr> <подстрока>[,<подстрока>…]  (margin 1 чанк)"""
import sys, json, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools/gt'))
import anvil, mcr, numpy as np
wd = sys.argv[1].rstrip('/'); path = sys.argv[2]; words = sys.argv[3].split(',')
m = json.load(open(wd + '/manifest.json')); dim = m['dim'].replace('the_', '')
w = anvil.World(wd + '/world', dim, '26.3'); mc = mcr.Mcr(path)
x0, z0, x1, z1 = m['area_chunks']
ref = collections.Counter(); our = collections.Counter()
names_ref = w.states.names
for cz in range(z0 + 1, z1):
    for cx in range(x0 + 1, x1):
        c = w.chunk(cx, cz)
        if c is None: continue
        u, n = np.unique(c.blocks, return_counts=True)
        for a, k in zip(u, n):
            nm = names_ref[int(a)].split('[')[0].replace('minecraft:', '')
            if any(x in nm for x in words): ref[nm] += int(k)
        b = mc.blocks(cx, cz); u, n = np.unique(b, return_counts=True)
        for a, k in zip(u, n):
            nm = mc.state_names[int(a)].split('[')[0].replace('minecraft:', '')
            if any(x in nm for x in words): our[nm] += int(k)
print('%-30s %10s %10s' % ('блок', 'эталон', 'наше'))
for nm in sorted(set(ref) | set(our)): print('%-30s %10d %10d' % (nm, ref[nm], our[nm]))

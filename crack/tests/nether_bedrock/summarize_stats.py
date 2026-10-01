#!/usr/bin/env python3
"""Сводка stats.sh: по каждому N — доля запусков, где истинный seed найден, где найден ровно один R (нет ложных), среднее число ложных R, медиана кандидатов F."""
import sys, csv, statistics as st
from collections import defaultdict

def fnum(x):
    try: return float(x)
    except Exception: return float('nan')

rows = defaultdict(list)
for fn in sys.argv[1:]:
    with open(fn) as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if r['truth'] == '' : continue
            rows[(r['mode'], int(r['N']))].append(r)
print('| режим | N блоков | запусков | инфо, бит | истинный найден | найден ровно 1 набор R (нет ложных) | ср. ложных R | ср. «близнецов» | медиана кандидатов F | макс. найдено seed |')
print('|---|---|---|---|---|---|---|---|---|---|')
for (mode, n) in sorted(rows):
    rs = rows[(mode, n)]
    k = len(rs)
    truth = sum(1 for r in rs if r['truth'] == '1')
    uniq = sum(1 for r in rs if r['distinctR'] == '1' and r['truth'] == '1')
    fals = [int(r['distinctR']) - 1 for r in rs if r['truth'] == '1']
    twins = [int(r['found']) - int(r['distinctR']) for r in rs if r['truth'] == '1']
    cand = [fnum(r['candF']) for r in rs]
    info = st.mean(fnum(r['info_bits']) for r in rs)
    mx = max(int(r['found']) for r in rs)
    print(f"| {mode} | {n} | {k} | {info:.1f} | {truth}/{k} | {uniq}/{k} | {st.mean(fals):.2f} | {st.mean(twins):.2f} | {st.median(cand):.3g} | {mx} |")

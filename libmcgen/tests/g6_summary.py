#!/usr/bin/env python3
"""Сводка вывода g6_blocks.py (текст) по наборам: g6_summary.py вывод.txt  [--md]"""
import re, sys, collections
rows = []
for l in open(sys.argv[1]):
    m = re.match(r'(\S+)\s+(\S+)\s+область\s+([\d.]+) % \(расхожд\.\s+(\d+)\)\s+блоков построек\s+(\d+): совпало\s+([\d.]+) %', l)
    if m:
        rows.append((m.group(1), m.group(2), float(m.group(3)), int(m.group(4)), int(m.group(5)), float(m.group(6))))
by = collections.defaultdict(list)
for r in rows: by[r[0]].append(r)
md = '--md' in sys.argv
if md: print('| набор | миров | блоков построек | расхождений | худший мир, % | в среднем по блокам, % |\n|---|---|---|---|---|---|')
for k, v in sorted(by.items()):
    nb = sum(x[4] for x in v); mis = sum(x[3] for x in v)
    worst = min(x[5] for x in v if x[4] > 0) if any(x[4] > 0 for x in v) else 100.0
    avg = 100.0 * (1 - mis / nb) if nb else 100.0
    if md: print(f'| {k} | {len(v)} | {nb} | {mis} | {worst:.4f} | {avg:.4f} |')
    else: print(f'{k:20s} миров {len(v):2d} худший {worst:8.4f} % блоков {nb:8d} расхожд {mis:6d} среднее {avg:8.4f} %')

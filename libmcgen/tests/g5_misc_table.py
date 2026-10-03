#!/usr/bin/env python3
"""Таблица G5i для группы G5-misc из отчёта g5_features.py (--report): по одной строке на placed_feature, миры суммируются.

    python3 libmcgen/tests/g5_misc_table.py libmcgen/tests/results/g5i-misc-26.3.json [--notes notes.json] > таблица.md
notes.json — {"<placed_feature без minecraft:>": "пояснение к расхождениям"} (необязательно).
"""
import argparse, collections, json, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report'); ap.add_argument('--notes', default='')
    a = ap.parse_args()
    d = json.load(open(a.report))
    notes = json.load(open(a.notes)) if a.notes else {}
    rows = collections.OrderedDict()
    for r in sorted(d['rows'], key=lambda r: (r['dim'], r['feature'])):
        k = r['feature'].replace('minecraft:', '')
        x = rows.setdefault(k, dict(dim=r['dim'], n=0, blocks=0, eff=0, bad=0, worst=100.0, effw=0))
        x['n'] += 1; x['blocks'] += r['blocks']; x['bad'] += r['mismatch']; x['worst'] = min(x['worst'], r['match_pct'])
        if r.get('effect') is not None: x['eff'] += r['effect']; x['effw'] += 1
    print('| placed_feature | измерение | миров | блоков сравнено | эффект в эталоне | расхождений | худший мир | примечание |')
    print('|---|---|---:|---:|---:|---:|---:|---|')
    for k, x in rows.items():
        eff = f"{x['eff']:,}".replace(',', ' ') if x['effw'] else '—'
        print(f"| `{k}` | {x['dim'].replace('the_', '')} | {x['n']} | {x['blocks']:,} | {eff} | {x['bad']:,} | {x['worst']:.6f} % | {notes.get(k, '')} |".replace(',', ' '))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Бюджет времени и объёма эталонных миров: агрегат по manifest.json (run/gt/<V>/<вариант>/*/manifest.json).

  budget.py [--version 26.3] [--md]
Время `generate_s` измеряется с шагом опроса ~3 с (forceload -> все чанки full), `server_start_s` — от запуска JVM до строки «Done».
"""
import argparse, collections, glob, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import GT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--version', default='26.3'); ap.add_argument('--md', action='store_true'); ap.add_argument('--update-doc', action='store_true', help='записать таблицу в docs/blender/ground-truth.md между маркерами budget')
    a = ap.parse_args()
    agg = collections.defaultdict(list)
    for mp in sorted(glob.glob(f'{GT}/{a.version}/*/*/manifest.json')):
        if '_t' in os.path.basename(os.path.dirname(mp)).rsplit('-r', 1)[-1] or any(t in mp for t in ('_rep/', '_rep1/', '_rep2/', '_tickn/', '_s1/', '_s2/')):
            continue
        m = json.load(open(mp))
        if not m.get('ok'):
            continue
        agg[(('structure:*' if m['variant'].startswith('structure:') else 'feature:*' if m['variant'].startswith('feature:') else m['variant']), m['dim'], m['radius'])].append(m)
    rows = []
    for (v, d, rad), ms in sorted(agg.items()):
        f = lambda k: sum(x[k] for x in ms) / len(ms)
        rows.append((v + f' r={rad}', d, len(ms), ms[0]['chunks_area'], f('server_start_s'), f('generate_s'), f('total_s'), f('region_bytes') / 1e6, f('chunks_world_total')))
    hdr = ['вариант', 'измерение', 'миров', 'чанков в области', 'старт сервера, с', 'генерация, с (шаг 3 с)', 'всего на мир, с', 'region-файлы, МБ', 'чанков в мире (с кольцами)']
    if a.update_doc:
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            a.md = True
            _print(rows, hdr, a)
        p = os.path.join(os.path.dirname(GT), '..', 'docs/blender/ground-truth.md')
        p = os.path.abspath(f'{GT}/../../docs/blender/ground-truth.md')
        t = open(p).read()
        i, j = t.index('<!-- budget begin -->'), t.index('<!-- budget end -->')
        open(p, 'w').write(t[:i] + '<!-- budget begin -->\n' + buf.getvalue() + t[j:])
        return
    _print(rows, hdr, a)


def _print(rows, hdr, a):
    if a.md:
        print('| ' + ' | '.join(hdr) + ' |'); print('|' + '---|' * len(hdr))
        for r in rows:
            print(f'| {r[0]} | {r[1]} | {r[2]} | {r[3]} | {r[4]:.1f} | {r[5]:.1f} | {r[6]:.1f} | {r[7]:.1f} | {r[8]:.0f} |')
    else:
        for r in rows:
            print(f'{r[0]:36s} {r[1]:10s} n={r[2]:3d} чанков={r[3]} старт={r[4]:5.1f} ген={r[5]:6.1f} всего={r[6]:6.1f} с  region={r[7]:6.1f} МБ  мир={r[8]:.0f}')


if __name__ == '__main__':
    main()

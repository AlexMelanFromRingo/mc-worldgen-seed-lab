#!/usr/bin/env python3
"""Сводная таблица «глина -> алмаз» по версиям: запускает l3_pairs для каждой версии, пишет JSON и markdown-строки.
  tools/l3_versions_report.py [версии...]   (по умолчанию все найденные в data/l3v/)
Результат: data/l3v/summary.json, data/l3v/summary.md"""
import sys, os, glob, json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_pairs as P

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORDER = ['1.7.10', '1.8.9', '1.12.2', '1.13.2', '1.14.4', '1.15.2', '1.16.5', '1.17.1', '1.18', '1.18.2', '1.19.4', '1.20.6', '1.21.11', '26.3']


def frame_offsets(ver):
    t = tuple(int(x) for x in ver.split('.')[:2])
    return (8, 0) if t <= (1, 12) else (0,)


def run(ver, K):
    files = sorted(glob.glob(f'{ROOT}/data/l3v/{ver}-w*.npz'))
    if not files: return None
    out = dict(version=ver, files=len(files), frames={})
    for off in frame_offsets(ver):
        res, sets = P.analyze(files, off, K)
        fr = dict(res)
        fr['sets'] = {}
        for nm, s in sets.items():
            A = P.stats(s['HA'], s['HAk'], K); B = P.chi2_stats(s['TB'], s['TBk']) if s['TBk'].sum() else None; C = P.stats(s['HC'], s['HCk'], min(K, 24))
            fr['sets'][nm] = dict(nclay=s['nclay'], ndia=s['ndia'], A=A, B=B, C=C, fmin=P.power_fraction(s['TB'], s['TBk']))
        out['frames'][off] = fr
    return out


def fmt_row(r):
    # основной кадр: для <=1.12 — «кадр популяции» (+8), для остальных — чанк (0); второй кадр остаётся в JSON
    off = frame_offsets(r['version'])[0]
    fr = r['frames'][off]
    s = fr['sets']['wet']
    return fr, off, s, s['A'], s['B'], s['C']


if __name__ == '__main__':
    K = 48
    vers = sys.argv[1:] or [v for v in ORDER if glob.glob(f'{ROOT}/data/l3v/{v}-w*.npz')]
    if vers == ['--compile']: vers = []          # только свести уже посчитанные summary-<версия>.json
    for v in vers:
        r = run(v, K)
        if not r: continue
        json.dump(r, open(f'{ROOT}/data/l3v/summary-{v}.json', 'w'), ensure_ascii=False, indent=1, default=str)
        fr, off, s, A, B, C = fmt_row(r)
        print(f"{v}: off={off} миров {r['files']}, чанков {fr['chunks_full']}, центров глины {s['nclay']}; (C) {C['obs']}/{C['ctrl_mean']:.0f}±{C['ctrl_sd']:.0f} maxz {C['maxz_obs']:.1f}/{C['maxz_ctrl_mean']:.1f}({C['maxz_ctrl_max']:.1f}); (A) maxz {A['maxz_obs']:.1f}/{A['maxz_ctrl_mean']:.1f}({A['maxz_ctrl_max']:.1f}); (B) chi2z {B['chi2_z']:.1f}", flush=True)
    # сводная таблица из всех summary-*.json
    lines = []
    for v in ORDER:
        fp = f'{ROOT}/data/l3v/summary-{v}.json'
        if not os.path.exists(fp): continue
        r = json.load(open(fp))
        r['frames'] = {int(k): val for k, val in r['frames'].items()}
        fr, off, s, A, B, C = fmt_row(r)
        line = (f"| {v} | {r['files']} | {fr['chunks_full']} | {fr['clay_blocks']} ({fr['clay_blocks_wet']}) | {fr['dia_blocks']} | {s['nclay']} | "
                f"{C['obs']} / {C['ctrl_mean']:.0f} ± {C['ctrl_sd']:.0f} | {C['maxz_obs']:.1f} / {C['maxz_ctrl_mean']:.1f} ({C['maxz_ctrl_max']:.1f}) | "
                f"{A['maxz_obs']:.1f} / {A['maxz_ctrl_mean']:.1f} ({A['maxz_ctrl_max']:.1f}) | {B['chi2_z']:.1f} (p={B['p_rank']:.2f}) | {('%.1f%%' % (100 * s['fmin'])) if s.get('fmin') else '-'} |")
        lines.append(line)
    open(f'{ROOT}/data/l3v/summary.md', 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))

#!/usr/bin/env python3
"""Проверка детерминизма эталона: повторная генерация той же конфигурации (тег `rep`) и сравнение «эталон против эталона» блок-в-блок.

  determinism.py --version 26.3 --variant raw --dim overworld --seed 12345 --cx 0 --cz 0 --radius 10 [--keep]
Пишет строку результата в run/gt/determinism.jsonl; код возврата 0 — идентичны, 1 — различаются.
"""
import argparse, json, os, shutil, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import GT, VERSIONS, DIMS
import gen_world, diff


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='26.3', choices=VERSIONS); ap.add_argument('--variant', default='raw')
    ap.add_argument('--dim', default='overworld', choices=sorted(DIMS)); ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--cx', type=int, default=0); ap.add_argument('--cz', type=int, default=0); ap.add_argument('--radius', type=int, default=10)
    ap.add_argument('--keep', action='store_true', help='оставить повторный мир')
    ap.add_argument('--ignore-y', action='append', default=[], type=diff.parse_range)
    ap.add_argument('--ignore-state', action='append', default=[])
    a = ap.parse_args()
    orig = gen_world.world_dir(a.version, a.variant, a.dim, a.seed, a.cx, a.cz, a.radius)
    if not os.path.exists(f'{orig}/manifest.json'):
        gen_world.generate(a.version, a.variant, a.dim, a.seed, a.cx, a.cz, a.radius)
    m = gen_world.generate(a.version, a.variant, a.dim, a.seed, a.cx, a.cz, a.radius, force=True, tag='rep')
    rep = gen_world.world_dir(a.version, a.variant, a.dim, a.seed, a.cx, a.cz, a.radius, 'rep')
    R, o = diff.run(orig, None, vs_world=rep, box=(a.cx - a.radius, a.cz - a.radius, 2 * a.radius + 1, 2 * a.radius + 1), dim=a.dim, version=a.version,
                    biomes=True, heightmaps=True, ignore_y=a.ignore_y, ignore_state=a.ignore_state, mask_flow=False)
    line = {'version': a.version, 'variant': a.variant, 'dim': a.dim, 'seed': a.seed, 'center': [a.cx, a.cz], 'radius': a.radius,
            'chunks': R['chunks_compared'], 'blocks': R['blocks_compared'], 'mismatch': R['blocks_mismatch'], 'biome_mismatch': R['biomes']['cells_mismatch'],
            'hm_mismatch': sum(v['mismatch'] for v in R['heightmaps'].values()), 'identical': R['blocks_mismatch'] == 0 and R['verdict'] != 'error',
            'time': time.strftime('%Y-%m-%d %H:%M:%S')}
    print(json.dumps(line, ensure_ascii=False))
    open(f'{GT}/determinism.jsonl', 'a').write(json.dumps(line, ensure_ascii=False) + '\n')
    if R['blocks_mismatch']:
        print(diff.text_report(R, o))
    if not a.keep:
        shutil.rmtree(rep, ignore_errors=True)
    sys.exit(0 if line['identical'] else 1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""G5-деревья в сборе: вариант `features` (все декорации, стадии 0x1f) против эталона; доля расхождений, относимых к деревьям/грибам-деревьям.

    python3 libmcgen/tests/g5_tree_assembly.py [--dim overworld|nether] [--seed 12345] [--margin 2] [--version 26.3] [--stable]

Считает: всего блоков, расхождений, из них «древесных» (брёвна, листва, грибные блоки, стебли, бородавки, пропагулы, ульи, корни мангров, подзол…) и их долю от блоков.
--stable маскирует клетки, где эталон не совпал с повторными мирами <мир>_rep1/_rep2 (недетерминизм порядка чанков игры).
"""
import argparse, glob, json, os, re, subprocess, sys, tempfile
ROOT = os.environ.get('MCGEN_ROOT') or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TREE_WORDS = ('_log', '_leaves', 'mushroom_block', 'mushroom_stem', 'nether_wart_block', 'propagule', 'bee_nest', 'mangrove_roots', 'crimson_stem', 'warped_stem', '_wood',
              'shelf_mushroom', 'hanging_roots', 'rooted_dirt', 'podzol', 'cocoa', 'shroomlight', 'weeping_vines', 'pale_hanging_moss', 'creaking_heart', 'poplar')
D = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'end': 'minecraft:the_end'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dim', default='overworld'); ap.add_argument('--seed', default='12345'); ap.add_argument('--margin', type=int, default=2)
    ap.add_argument('--version', default='26.3'); ap.add_argument('--stable', action='store_true')
    a = ap.parse_args()
    ws = [d for d in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/features/{a.dim}-s{a.seed}-c0_0-r10')) if os.path.exists(d + '/manifest.json')]
    if not ws: sys.exit('нет эталона features')
    wd = ws[0]
    m = json.load(open(wd + '/manifest.json')); x0, z0, x1, z1 = m['area_chunks']
    tmp = tempfile.mkdtemp()
    out = tmp + '/f.mcr'
    ext = 2
    r = subprocess.run([f'{ROOT}/libmcgen/build/mcgen-cli', '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', D[a.dim], '--seed', str(m['seed']),
                        '--cx0', str(x0 - ext), '--cz0', str(z0 - ext), '--nx', str(x1 - x0 + 1 + 2 * ext), '--nz', str(z1 - z0 + 1 + 2 * ext), '--stages', '0x1f', '--pp-margin', '1', '--out', out],
                       capture_output=True, text=True)
    if r.returncode: sys.exit(r.stderr[-300:])
    cmd = [sys.executable, f'{ROOT}/tools/gt/diff.py', '--ref', wd, '--mcr', out, '--margin', str(a.margin), '--dim', a.dim, '--version', a.version, '--list', '5000000', '--top', '0']
    if a.stable:
        for rep in sorted(glob.glob(wd + '_rep*')): cmd += ['--stable-with', rep]
    r = subprocess.run(cmd, capture_output=True, text=True)
    tot = tree = 0; cmp_ = 0
    for ln in r.stdout.splitlines():
        if ln.startswith('блоков сравнено'): cmp_ = int(ln.split()[2].replace(',', ''))
        mm = re.match(r'\s+\((-?\d+), (-?\d+), (-?\d+)\)\s+(.*) -> (.*)$', ln)
        if mm:
            tot += 1
            if any(w in mm.group(4) + mm.group(5) for w in TREE_WORDS): tree += 1
    print(f'{a.dim} s{a.seed} margin {a.margin}{" stable" if a.stable else ""}: сравнено {cmp_:,}  расхождений {tot:,} ({100 * (1 - tot / max(cmp_, 1)):.4f} %), '
          f'из них деревья/грибы {tree:,} = {100 * tree / max(cmp_, 1):.4f} % блоков')
    for f in glob.glob(tmp + '/*'): os.remove(f)
    os.rmdir(tmp)


if __name__ == '__main__':
    main()

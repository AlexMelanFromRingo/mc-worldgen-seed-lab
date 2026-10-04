#!/usr/bin/env python3
"""G5-misc: подбор порядка декорации чанков, при котором изолированная фича совпадает с эталоном побитно.

Ванильный сервер декорирует соседние чанки в порядке, который не сводится ни к «x внешний», ни к «z внешний» (планировщик задач), но для
конкретного мира он воспроизводим (эталон совпадает со своим повтором `_rep1`). Если после нескольких перестановок пар соседних чанков
расхождение с эталоном падает до нуля — логика фичи (поиск позиций, ГСЧ, порядок обхода HashSet, перекрытия патчей) точна, а остаток —
только порядок. Скрипт стартует с обхода `xz`/`zx` (или из файла «cx cz») и жадно двигает чанк непосредственно перед/после соседа, пока
число расхождений (вне жидкостей и их halo по y, как в g5_misc_order.py) уменьшается.

    python3 libmcgen/tests/g5_misc_ordsearch.py --feature minecraft:lush_caves_clay --world s12345-c0_0-r5 [--start zx] [--out order.txt] [--rounds 6]

Результат — файл порядка («cx cz» построчно): воспроизведение — MCGEN_FEATURES_ONLY=<id> MCGEN_FEATURES_SEQ=file MCGEN_FEATURES_ORDER=<файл>.
Переменная MCGEN_CLI — путь к mcgen-cli (по умолчанию libmcgen/build/mcgen-cli). Один прогон — секунды; полный раунд — сотни прогонов.
"""
import argparse, glob, json, os, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, f'{ROOT}/tools/gt'); sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anvil
from mcr import Mcr
import g5_misc_order as G

DIMS = G.DIMS


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--feature', required=True); ap.add_argument('--version', default='26.3'); ap.add_argument('--world', default='')
    ap.add_argument('--start', default='zx', help='xz | zx | путь к файлу порядка'); ap.add_argument('--out', default=''); ap.add_argument('--rounds', type=int, default=6)
    ap.add_argument('--ring', type=int, default=1, help='кольцо декорации вокруг области (как MCGEN_FEATURES_RING)')
    a = ap.parse_args()
    fid = a.feature if ':' in a.feature else 'minecraft:' + a.feature
    cli = os.environ.get('MCGEN_CLI') or f'{ROOT}/libmcgen/build/mcgen-cli'
    wds = [w.rstrip('/') for w in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/feature_{fid.replace(":", "_")}/*/')) if a.world in w and '_rep' not in w]
    if not wds: sys.exit('нет эталонного мира')
    wd = wds[0]; m = json.load(open(f'{wd}/manifest.json')); x0, z0, x1, z1 = m['area_chunks']; dim = m['dim'].replace('the_', '')
    states = anvil.StateTable.load(a.version); ref = anvil.World(f'{wd}/world', dim, a.version, states=states)
    ring = a.ring
    chunks = [(cx, cz) for cx in range(x0 - ring, x1 + ring + 1) for cz in range(z0 - ring, z1 + ring + 1)]
    tab_r, ids2 = G.canon_table([states.names.get(i, '') for i in range(states.count)])
    allnames = {}

    def remap(tab, idmap):
        inv = {v: k for k, v in idmap.items()}
        return np.array([allnames.setdefault(inv[int(t)], len(allnames)) for t in tab], dtype=np.int32)
    lut_r = remap(tab_r, ids2)
    R = {}
    for cx in range(x0, x1 + 1):
        for cz in range(z0, z1 + 1):
            c = ref.chunk(cx, cz)
            if c is not None and c.status == 'minecraft:full': R[(cx, cz)] = lut_r[c.blocks]
    tmp = tempfile.mkdtemp(prefix='g5ordsearch-', dir=os.environ.get('TMPDIR', '/tmp'))

    def evaluate(order):
        of, out = f'{tmp}/o.txt', f'{tmp}/o.mcr'
        open(of, 'w').write(''.join(f'{p} {q}\n' for p, q in order))
        env = dict(os.environ, MCGEN_FEATURES_ONLY=fid, MCGEN_FEATURES_SEQ='file', MCGEN_FEATURES_ORDER=of, MCGEN_FEATURES_RING=str(ring))
        cmd = [cli, '--pack', f'{ROOT}/run/pack-{a.version}', '--version', a.version, '--dim', DIMS[m['dim']], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
               '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', '0x17', '--pp-margin', '1', '--threads', '1', '--out', out]
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if r.returncode: raise RuntimeError(r.stderr[-300:])
        o = Mcr(out); tab_o, ids = G.canon_table(o.state_names); lut_o = remap(tab_o, ids)
        fl = np.zeros(len(allnames) + 1, dtype=bool)
        for k, i in allnames.items():
            if k.startswith('minecraft:water') or k.startswith('minecraft:lava') or k.startswith('minecraft:bubble_column'): fl[i] = True
        per = {}
        for (cx, cz), Rr in R.items():
            O = lut_o[o.blocks(cx, cz)]
            f = fl[Rr] | fl[O]; f2 = f.copy(); f2[1:] |= f[:-1]; f2[:-1] |= f[1:]
            per[(cx, cz)] = int(((O != Rr) & ~f2).sum())
        return sum(per.values()), per

    if a.start == 'xz': order = sorted(chunks)
    elif a.start == 'zx': order = sorted(chunks, key=lambda c: (c[1], c[0]))
    else: order = [tuple(map(int, l.split())) for l in open(a.start) if l.strip()]
    best, per = evaluate(order)
    print(f'старт {a.start}: {best}', flush=True)
    for rnd in range(a.rounds):
        bad = [c for c, v in per.items() if v > 0]
        cs = sorted({(cx + dx, cz + dz) for cx, cz in bad for dx in (-1, 0, 1) for dz in (-1, 0, 1) if (cx + dx, cz + dz) in chunks})
        improved = False
        print(f'раунд {rnd + 1}: кандидатов {len(cs)}', flush=True)
        for p in cs:
            for q in cs:
                if p == q or max(abs(p[0] - q[0]), abs(p[1] - q[1])) > 1: continue
                for mode in ('before', 'after'):
                    o2 = [c for c in order if c != p]; j = o2.index(q); o2.insert(j if mode == 'before' else j + 1, p)
                    if o2 == order: continue
                    v, p2 = evaluate(o2)
                    if v < best:
                        print(f'  {best} -> {v}: {p} {mode} {q}', flush=True); best, order, per, improved = v, o2, p2, True
        if not improved or best == 0: break
    print('итог', best)
    if a.out: open(a.out, 'w').write(''.join(f'{p} {q}\n' for p, q in order)); print('порядок записан в', a.out)


if __name__ == '__main__':
    main()

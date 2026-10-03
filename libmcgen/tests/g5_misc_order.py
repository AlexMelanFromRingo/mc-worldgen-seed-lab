#!/usr/bin/env python3
"""G5-misc: проверка изолированной фичи с учётом недетерминизма порядка чанков у ванили.

Игра декорирует соседние чанки в порядке, зависящем от планировщика (у границ чанков порядок разный в разных мирах: в одном мире верно «x внешний, z внутренний»,
в другом — «z внешний, x внутренний», а у отдельных пар — обратный). Поэтому для фич, чьи постройки пересекаются через границы чанков, побитное совпадение
с одним фиксированным порядком достижимо не всегда. Скрипт гоняет libmcgen в нескольких порядках обхода (MCGEN_FEATURES_SEQ) и считает:
  * расхождения с эталоном при каждом порядке (в блоках);
  * «неустойчивые» клетки — где результаты libmcgen в разных порядках различаются между собой (ими управляет только порядок, не логика фичи);
  * расхождения с эталоном ВНЕ неустойчивых клеток (и вне течения воды/лавы) — должны быть 0, иначе логика фичи неверна.

    python3 libmcgen/tests/g5_misc_order.py --feature minecraft:large_dripstone [--version 26.3] [--orders xz,zx,xz-,zx-] [--world <часть имени>]
Переменная MCGEN_CLI — путь к mcgen-cli (по умолчанию libmcgen/build/mcgen-cli).
"""
import argparse, glob, json, os, subprocess, sys, tempfile, shutil
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, f'{ROOT}/tools/gt')
import anvil
from mcr import Mcr
DIMS = {'overworld': 'minecraft:overworld', 'nether': 'minecraft:the_nether', 'the_nether': 'minecraft:the_nether', 'end': 'minecraft:the_end', 'the_end': 'minecraft:the_end'}


def run_ours(cli, v, m, fid, order, stages, out):
    x0, z0, x1, z1 = m['area_chunks']
    env = dict(os.environ, MCGEN_FEATURES_ONLY=fid)
    if order: env['MCGEN_FEATURES_SEQ'] = order
    cmd = [cli, '--pack', f'{ROOT}/run/pack-{v}', '--version', v, '--dim', DIMS[m['dim']], '--seed', str(m['seed']), '--cx0', str(x0), '--cz0', str(z0),
           '--nx', str(x1 - x0 + 1), '--nz', str(z1 - z0 + 1), '--stages', stages, '--pp-margin', '1', '--out', out]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode: raise RuntimeError(r.stderr[-300:])


def canon_table(names):
    """имя состояния -> канонический идентификатор (одинаков для ссылки и нас)"""
    ids = {}
    out = np.zeros(len(names), dtype=np.int32)
    for i, n in enumerate(names):
        b, pr = anvil.parse_state_name(n)
        key = anvil.canon_name(b, dict(pr))
        out[i] = ids.setdefault(key, len(ids))
    return out, ids


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--feature', required=True); ap.add_argument('--version', default='26.3'); ap.add_argument('--orders', default='xz,zx,xz-,zx-,' + ','.join(f'rand{i}' for i in range(1, 13)))
    ap.add_argument('--show', type=int, default=0, help='вывести первые N неустойчивых-вне-порядка расхождений (x y z: наше -> эталон)')
    ap.add_argument('--world', default=''); ap.add_argument('--stages', default='0x17'); ap.add_argument('--margin', type=int, default=0)
    a = ap.parse_args()
    fid = a.feature if ':' in a.feature else 'minecraft:' + a.feature
    cli = os.environ.get('MCGEN_CLI') or f'{ROOT}/libmcgen/build/mcgen-cli'
    orders = a.orders.split(',')
    states = anvil.StateTable.load(a.version)
    rc = 0
    for wd in sorted(glob.glob(f'{ROOT}/run/gt/{a.version}/feature_{fid.replace(":", "_")}/*/')):
        wd = wd.rstrip('/')
        if a.world and a.world not in wd: continue
        mp = f'{wd}/manifest.json'
        if not os.path.exists(mp): continue
        m = json.load(open(mp))
        if not m.get('ok') or m['variant'] != f'feature:{fid}': continue
        dim = m['dim'].replace('the_', '')
        ref = anvil.World(f'{wd}/world', dim, a.version, states=states)
        x0, z0, x1, z1 = m['area_chunks']
        outs = {}
        tmp = tempfile.mkdtemp(prefix='g5order-', dir='/tmp')
        try:
            for o in orders:
                p = f'{tmp}/{o}.mcr'; run_ours(cli, a.version, m, fid, o, a.stages, p); outs[o] = Mcr(p)
            m0 = outs[orders[0]]
            tab_o, ids = canon_table(m0.state_names)
            ref_names = [states.names.get(i, '') for i in range(states.count)]
            tab_r, ids2 = canon_table(ref_names)
            # единая нумерация: канонические имена ссылки и наших
            allnames = {}
            def remap(tab, idmap):
                inv = {v: k for k, v in idmap.items()}
                return np.array([allnames.setdefault(inv[int(t)], len(allnames)) for t in tab], dtype=np.int32)
            lut_o = remap(tab_o, ids); lut_r = remap(tab_r, ids2)
            fluid = np.zeros(len(allnames) + 1, dtype=bool)
            for k, i in allnames.items():
                if k.startswith('minecraft:water') or k.startswith('minecraft:lava') or k.startswith('minecraft:bubble_column'): fluid[i] = True
            shown = 0; tot = {o: 0 for o in orders}; stab_bad = 0; unstable = 0; compared = 0; effect_cells = 0
            for cz in range(z0 + a.margin, z1 + 1 - a.margin):
                for cx in range(x0 + a.margin, x1 + 1 - a.margin):
                    rc_ = ref.chunk(cx, cz)
                    if rc_ is None or rc_.status != 'minecraft:full': continue
                    R = lut_r[rc_.blocks]
                    O = {o: lut_o[outs[o].blocks(cx, cz)] for o in orders}
                    uns = np.zeros(R.shape, dtype=bool)
                    for o in orders[1:]: uns |= O[o] != O[orders[0]]
                    fl = fluid[R] | fluid[O[orders[0]]]
                    # гало жидкости 1 по y (течение воды/лавы порождает соседние источники)
                    fl2 = fl.copy(); fl2[1:] |= fl[:-1]; fl2[:-1] |= fl[1:]
                    compared += R.size; unstable += int(uns.sum())
                    for o in orders: tot[o] += int(((O[o] != R) & ~fl2).sum())
                    bad = (O[orders[0]] != R) & ~uns & ~fl2
                    stab_bad += int(bad.sum())
                    if a.show and bad.any() and shown < a.show:
                        inv_all = {v: k for k, v in allnames.items()}
                        for yy, zz, xx in np.argwhere(bad)[:a.show - shown]:
                            print(f'   ({cx * 16 + xx}, {yy + rc_.min_y}, {cz * 16 + zz}): {inv_all[int(O[orders[0]][yy, zz, xx])]} -> {inv_all[int(R[yy, zz, xx])]}')
                            shown += 1
            print(f'{fid} {os.path.basename(wd)}: сравнено {compared:,}; неустойчивых по порядку клеток {unstable}; расхождений вне неустойчивых и течения: {stab_bad}; ' +
                  '; '.join(f'{o}: {tot[o]}' for o in orders), flush=True)
            if stab_bad: rc = 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(rc)


if __name__ == '__main__':
    main()

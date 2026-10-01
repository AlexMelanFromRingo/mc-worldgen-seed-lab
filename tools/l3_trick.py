#!/usr/bin/env python3
"""Калькулятор «трюка глина -> алмаз» для версий 1.13.2 – 1.17.1 (где seed фичи = decorationSeed + index + 10000*step на java.util.Random).

  tools/l3_trick.py --ver 1.16.5 --seed 12345 --cx 3 --cz -7 [--biome 1|swamp|id]
печатает для чанка (cx,cz): абсолютные координаты ЦЕНТРА диска глины (x_c,z_c), НАЧАЛА жилы алмаза (x_d,z_d,y_d), смещение
(dx,dz) = (x_d-x_c, z_d-z_c) и целевой квадрат 2x2 колонок для копания (колонки x_d-1..x_d, z_d-1..z_d: там алмаз в ~62 % колонок, ~80 % квадратов).
  tools/l3_trick.py --ver 1.16.5 --seed 12345 --table          # таблица (dx,dz) по s0 = seed & 15 для обычных биомов и болота
Индексы фич (step, idx_алмаза, idx_глины) получены подгонкой по реальным мирам (docs/07, tools/l3_chunk_rule.py)."""
import sys, argparse
import numpy as np

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import l3_replay as R

# версия -> (step, порядок вызовов nextInt(16) алмаза, {биомы: idx_алмаза}, idx_алмаза по умолчанию, idx_глины обычная, idx_глины болото)
PARAMS = {
    '1.13.2': dict(step=4, order='xyz', dia_default=9, dia_by_biome={}, clay=12, clay_swamp=11),
    '1.14.4': dict(step=4, order='xyz', dia_default=9, dia_by_biome={133: 21}, clay=12, clay_swamp=11),
    '1.15.2': dict(step=4, order='xzy', dia_default=9, dia_by_biome={}, clay=12, clay_swamp=11),
    '1.16.5': dict(step=6, order='xzy', dia_default=9, dia_by_biome={131: 20}, clay=12, clay_swamp=11),
    '1.17.1': dict(step=6, order='xzy', dia_default=11, dia_by_biome={}, clay=15, clay_swamp=14),
}
SWAMP = {6, 134}


def predict(ver, seed, cx, cz, biome=1):
    p = PARAMS[ver]
    ic = p['clay_swamp'] if biome in SWAMP else p['clay']
    idd = p['dia_by_biome'].get(biome, p['dia_default'])
    dec = R.decoration_seeds(seed, np.array([cx]), np.array([cz]))
    xc, zc = R.feature_draws(dec, ic, p['step'])[:2]
    d = R.feature_draws(dec, idd, p['step'], n=3)
    xd = d[0]; zd, yd = (d[1], d[2]) if p['order'] == 'xzy' else (d[2], d[1])
    X0, Z0 = 16 * cx, 16 * cz
    return dict(clay=(int(X0 + xc[0]), int(Z0 + zc[0])), dia=(int(X0 + xd[0]), int(Z0 + zd[0]), int(yd[0])), idx=(ic, idd))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--ver', required=True, choices=sorted(PARAMS)); ap.add_argument('--seed', type=int, required=True)
    ap.add_argument('--cx', type=int, default=0); ap.add_argument('--cz', type=int, default=0)
    ap.add_argument('--biome', default='1', help='числовой id биома (1.13–1.17.1) либо swamp'); ap.add_argument('--table', action='store_true')
    a = ap.parse_args()
    biome = 6 if a.biome == 'swamp' else int(a.biome)
    if a.table:
        import l3_seed_dependence as D
        p = PARAMS[a.ver]
        for nm, ic in (('обычный биом', p['clay']), ('болото', p['clay_swamp'])):
            idd = p['dia_default']
            T = np.zeros((16, 16), np.int64)
            for par in (0, 1): T += D.table(ic, idd, p['step'], p['order'], a.seed & 15, par)
            fl = np.argsort(T.ravel())[::-1][:4]
            print(f'{a.ver}, seed={a.seed} (s0={a.seed & 15}), {nm} (idx_глины={ic}, idx_алмаза={idd}): (dx,dz) mod 16 = ' + '  '.join(f'({k // 16},{k % 16}) {T.ravel()[k] / T.sum():.2f}' for k in fl))
        sys.exit(0)
    r = predict(a.ver, a.seed, a.cx, a.cz, biome)
    (xc, zc), (xd, zd, yd) = r['clay'], r['dia']
    print(f"{a.ver} seed {a.seed} чанк ({a.cx},{a.cz}) биом {biome}: idx глины/алмаза = {r['idx']}")
    print(f'  центр диска глины: x={xc}, z={zc} (диск реально появится, только если центр под водой на дне из земли/глины)')
    print(f'  начало жилы алмаза: x={xd}, z={zd}, y={yd}  =>  смещение от центра диска: dx={xd - xc}, dz={zd - zc} (внутри чанка, возможен «перенос» на 16)')
    print(f'  копать: колонки x={xd - 1}..{xd}, z={zd - 1}..{zd} (квадрат 2x2) до y≈{max(yd - 2, 0)}..{yd + 2}')

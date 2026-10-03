#!/usr/bin/env python3
"""G5-veg: подбор областей для эталонов `featuresparse:<id>@K` — окно r=10 (21x21 чанков) с максимальной долей клеток биомов, где фича есть.

План W6 (tools/gt/features_plan.json) берёт область по числу клеток биомов фичи без учёта воды и «живости» (flower_default в океане, patch_tall_grass на
одной клетке саванны) — у многих растительных фич эффект в эталоне 0 и проверка бессмысленна. Здесь:
  * биомы фичи — из worldgen/biome/*.json пака (фича есть в списках шагов);
  * считаются клетки биома на высоте y=68 (Overworld; для Nether — y=40) сеткой шагом 48 блоков в ±--range блоков, по нескольким seed;
  * океанские биомы исключаются, если не указано --water для фичи (морская трава, ламинария, кораллы, кувшинка — по списку WATER);
  * выбирается окно с наибольшей долей клеток-мишеней (не менее --min-share).
Вывод: JSON-список {feature, dim, seed, cx, cz, radius} — его можно отдать W6 / использовать в tools/gt (gen_queue --plan).

    python3 libmcgen/tests/g5_veg_pick.py --features patch_tall_grass,flower_default [--seeds 12345,8675309] [--range 12000] [--out план.json]
"""
import argparse, glob, json, os, subprocess, sys
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.environ.get('MCGEN_CLI') or f'{ROOT}/libmcgen/build/mcgen-cli'
WATER = ('seagrass', 'kelp', 'sea_pickle', 'warm_ocean', 'waterlily')


def feature_biomes(version, fid):
    out = set()
    for f in glob.glob(f'{ROOT}/run/pack-{version}/data/minecraft/worldgen/biome/*.json'):
        j = json.load(open(f))
        if any(fid in step for step in j.get('features', [])):
            out.add('minecraft:' + os.path.basename(f)[:-5])
    return out


def grid(version, dim, seed, rng, step, y):
    n = 2 * rng // step
    r = subprocess.run([CLI, 'biome', '--pack', f'{ROOT}/run/pack-{version}', '--version', version, '--dim', dim, '--seed', str(seed), '--x0', str(-rng),
                        '--z0', str(-rng), '--nx', str(n), '--nz', str(n), '--step', str(step), '--y', str(y)], capture_output=True, text=True, check=True)
    names = r.stdout.split()
    return np.array(names, dtype=object).reshape(n, n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--features', required=True); ap.add_argument('--version', default='26.3'); ap.add_argument('--seeds', default='12345,8675309,-7048155917072976836')
    ap.add_argument('--range', type=int, default=12000); ap.add_argument('--step', type=int, default=48); ap.add_argument('--radius', type=int, default=10)
    ap.add_argument('--dim', default='minecraft:overworld'); ap.add_argument('--min-share', type=float, default=0.3); ap.add_argument('--out', default='')
    a = ap.parse_args()
    y = 68 if a.dim.endswith('overworld') else 40
    win = max(1, round((2 * a.radius + 1) * 16 / a.step))
    grids = {int(s): grid(a.version, a.dim, int(s), a.range, a.step, y) for s in a.seeds.split(',')}
    res = []
    for fid in a.features.split(','):
        fid = fid if ':' in fid else 'minecraft:' + fid
        bio = feature_biomes(a.version, fid)
        water_ok = any(w in fid for w in WATER)
        if not water_ok: bio = {b for b in bio if 'ocean' not in b}
        best = None
        for seed, g in grids.items():
            m = np.isin(g, list(bio)).astype(np.int32)
            c = np.pad(m.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
            s = c[win:, win:] - c[:-win, win:] - c[win:, :-win] + c[:-win, :-win]
            i, j = np.unravel_index(int(s.argmax()), s.shape)
            share = s[i, j] / (win * win)
            if best is None or share > best[0]:
                cx = (-a.range + (j + win // 2) * a.step) // 16; cz = (-a.range + (i + win // 2) * a.step) // 16
                best = (share, seed, cx, cz)
        ok = best and best[0] >= a.min_share
        print(f'{fid:45s} seed {best[1]:>20} c=({best[2]},{best[3]}) доля {best[0]:.2f}{"" if ok else "  (МАЛО)"}  биомов у фичи: {len(bio)}', file=sys.stderr)
        res.append(dict(feature=fid, dim=a.dim.split(':')[1].replace('the_', ''), seed=best[1], cx=int(best[2]), cz=int(best[3]), radius=a.radius, share=round(float(best[0]), 3)))
    if a.out:
        json.dump(res, open(a.out, 'w'), indent=1, ensure_ascii=False)
    else:
        print(json.dumps(res, ensure_ascii=False))


if __name__ == '__main__':
    main()

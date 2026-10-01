#!/usr/bin/env python3
"""Быстрый тест ядер crack-struct БЕЗ oracle: наблюдения генерирует host-реализация (`--gen-obs`, сверена с oracle через --selftest),
затем все пути поиска (lift/full × драйверы div/pow2/tri/generic × GPU/CPU × окна) должны найти исходный seed.
Запуск: crack/tests/test_kernels.py [--seeds 4]"""
import argparse, os, random, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CS = os.path.join(ROOT, 'crack', 'bin', 'crack-struct')
MASK48 = (1 << 48) - 1

CASES = [  # (имя, наборы, режим, окно(GPU), окно(CPU))
    ('lift: храмы+корабль+деревня', 'desert_pyramids,igloo,swamp_hut,shipwreck,trial_chambers,villages', 'lift', None, None),
    ('lift+exclusion: outposts', 'pillager_outposts,pillager_outposts,pillager_outposts,desert_pyramids,igloo,swamp_hut,jungle_temples,shipwreck', 'lift', None, None),
    ('full div-test: nether+ruined+camp', 'nether_complexes,ruined_portals,nether_complexes,ruined_portals,nether_complexes,ruined_portals,nether_complexes,ruined_portals,ruined_portals', 'full', 36, 28),
    ('full pow2: ancient_city', 'ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities', 'full', 36, 28),
    ('full triangular: end_city', 'end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities', 'full', 34, 26),
    ('full triangular: monument+mansion', 'ocean_monuments,woodland_mansions,ocean_monuments,woodland_mansions,ocean_monuments,woodland_mansions,ocean_monuments,ocean_monuments', 'full', 34, 26),
    ('full generic: mineshaft', 'mineshafts,mineshafts,mineshafts,mineshafts,mineshafts,mineshafts,mineshafts,mineshafts', 'full', 34, 26),
    ('full generic: buried_treasure', 'buried_treasures,buried_treasures,buried_treasures,buried_treasures,buried_treasures,buried_treasures,buried_treasures,buried_treasures,buried_treasures', 'full', 34, 26),
    ('full mix: pow2+tri+reducer', 'ancient_cities,end_cities,buried_treasures,ocean_monuments,ancient_cities,end_cities,ruined_portals,buried_treasures,mineshafts,ruined_portals,woodland_mansions,ocean_monuments', 'full', 34, 26),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', type=int, default=3)
    ap.add_argument('--versions', nargs='+', default=['26.1', '26.3'])
    ap.add_argument('--rng', type=int, default=4242)
    ap.add_argument('--no-cpu', action='store_true')
    a = ap.parse_args()
    rnd = random.Random(a.rng)
    fails = 0; total = 0
    tmp = tempfile.mkdtemp(prefix='ktest_')
    t00 = time.time()
    for V in a.versions:
        for (name, sets, mode, wg, wc) in CASES:
            if V != '26.3' and 'camp' in sets:
                continue
            for i in range(a.seeds):
                seed = rnd.randint(-2**63, 2**63 - 1); s48 = seed & MASK48
                obsf = os.path.join(tmp, 'o.txt')
                r = subprocess.run([CS, '--version', V, '--gen-obs', sets, '--seed', str(seed), '--gen-rng', str(rnd.randint(1, 10**6))], capture_output=True, text=True)
                if r.returncode: print('gen-obs:', r.stderr); fails += 1; continue
                open(obsf, 'w').write(r.stdout)
                devs = ['gpu'] + ([] if a.no_cpu else ['cpu'])
                for dev in devs:
                    cmd = [CS, '--version', V, '--dev', dev, '--mode', mode, obsf]
                    wb = wg if dev == 'gpu' else wc
                    if mode == 'full': cmd += ['--assume-seed', str(s48), '--window-bits', str(wb)]
                    if mode == 'lift' and dev == 'cpu': cmd += ['--assume-seed', str(s48), '--window-bits', '22']
                    if dev == 'gpu': cmd = ['flock', '/tmp/gpu.lock'] + cmd      # GPU общий: короткие прогоны под замком
                    t0 = time.time()
                    r = subprocess.run(cmd, capture_output=True, text=True)
                    cands = [int(x) for x in r.stdout.split() if x.isdigit()]
                    ok = s48 in cands
                    total += 1
                    if not ok:
                        fails += 1
                        print(f'FAIL {V} {name} dev={dev} seed={seed}\n{r.stderr[-600:]}')
                    else:
                        print(f'ok   {V} {name:40s} dev={dev} cands={len(cands):4d} {time.time()-t0:5.2f}s')
    print(f'test_kernels: {total - fails}/{total} успешно, {time.time()-t00:.0f} с')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()

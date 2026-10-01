#!/usr/bin/env python3
"""Тест режима --blocks: биом на БЛОК-уровне (как в F3 / BiomeManager.getBiome с hashed seed) против oracle `blockbiome`.
Для каждой (версия, измерение/пресет): несколько seed'ов x K случайных блоковых точек; oracle даёт биом; crack-biomes --blocks
на единственном кандидате (истинном seed) обязан пройти ВСЕ K наблюдений. Для сравнения тот же набор без --blocks
(биом ячейки кварта x>>2,y>>2,z>>2) — показывает, сколько наблюдений «размытие» реально меняет.
  test_blocks.py [--versions ..] [--seeds 4] [--k 48]
"""
import argparse, os, random, subprocess, sys, json, shlex
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from known_answer import Oracle, ROOT, BIN, SCRATCH

def run(ver, dim, preset, seed, obs_lines, blocks):
    of = f'{SCRATCH}/blk_obs.txt'; cf = f'{SCRATCH}/blk_cand.txt'
    open(of, 'w').write('\n'.join(obs_lines) + '\n'); open(cf, 'w').write(f'{seed}\n')
    cmd = shlex.split(BIN) + ['--version', ver, '--dim', dim, '--preset', preset, '--candidates', cf, '--obs', of, '--no-sort', '-v'] + (['--blocks'] if blocks else [])
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, MCGEN_ROOT=ROOT))
    reached = None
    for l in r.stderr.splitlines():
        if l.startswith('прошли ровно'):
            reached = [int(x.split('=')[1]) for x in l.split(':')[1].split()]
    return (len(r.stdout.split()) == 1), reached

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--versions', nargs='*', default=['26.1', '26.2', '26.3'])
    ap.add_argument('--dims', nargs='*', default=['overworld:normal', 'overworld:large_biomes', 'overworld:amplified', 'nether:-', 'end:-'])
    ap.add_argument('--seeds', type=int, default=4)
    ap.add_argument('--k', type=int, default=48)
    ap.add_argument('--rng', type=int, default=7)
    a = ap.parse_args()
    os.makedirs(SCRATCH, exist_ok=True)
    rnd = random.Random(a.rng); tot = bad = 0; sens_tot = sens = 0
    print(f'{"версия":6} {"конфиг":24} {"seed":>22} {"k":>3}  --blocks  без --blocks (первый провал)')
    for ver in a.versions:
        orc = Oracle(ver)
        for dd in a.dims:
            dim, preset = dd.split(':')
            for _ in range(a.seeds):
                seed = rnd.randint(-2**63, 2**63 - 1)
                lines = []; 
                for i in range(a.k):
                    if dim == 'overworld': x, y, z = rnd.randint(-30000, 30000), rnd.randint(-60, 300), rnd.randint(-30000, 30000)
                    elif dim == 'nether': x, y, z = rnd.randint(-30000, 30000), rnd.randint(1, 126), rnd.randint(-30000, 30000)
                    else:
                        x, z = rnd.randint(-40000, 40000), rnd.randint(-40000, 40000)
                        while x * x + z * z < 2000 * 2000: x, z = rnd.randint(-40000, 40000), rnd.randint(-40000, 40000)
                        y = 64
                    r = orc.call(f'blockbiome {dim} {seed} {x} {y} {z}' + (f' --preset {preset}' if dim == 'overworld' and preset != 'normal' else ''))
                    lines.append((x, y, z, r['biome']))
                ok_b, rb = run(ver, dim, preset, seed, [f'{x} {y} {z} {b}' for (x, y, z, b) in lines], True)
                ok_n, rn = run(ver, dim, preset, seed, [f'{x >> 2} {y >> 2} {z >> 2} {b}' for (x, y, z, b) in lines], False)
                first_fail = None if ok_n else (rn.index(1) if rn and 1 in rn else '?')
                # rn: reached[j] = число seed'ов, прошедших ровно j наблюдений; кандидат один => индекс j с единицей = число пройденных
                ff = None if ok_n else next((j for j, v in enumerate(rn) if v), '?')
                print(f'{ver:6} {dim + "/" + preset:24} {seed:22d} {a.k:3d}  {"OK" if ok_b else "FAIL":8} {"OK" if ok_n else "провал на #" + str(ff)}', flush=True)
                tot += 1; bad += (not ok_b); sens_tot += 1; sens += (not ok_n)
        orc.close()
    print(f'ИТОГО: {tot} наборов; --blocks не прошёл в {bad}; наивный кварт-режим провалился в {sens} из {sens_tot} (значит «размытие» существенно)')
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())

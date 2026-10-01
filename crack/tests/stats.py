#!/usr/bin/env python3
"""Статистика crack-struct БЕЗ oracle (наблюдения — host-реализация placement, сверенная с oracle через --selftest):
сколько структур нужно для однозначного structure seed и сколько ложных кандидатов остаётся.
Режимы:  lift  — пулы liftable-структур (реальный lifting на GPU), K=3..N;
         full  — пулы без liftable: эмпирический счёт ложных срабатываний в окне 2^W значений (масштабируется на 2^48) и формула 2^(48-info).
Запуск: crack/tests/stats.py [--n 40] [--version 26.3] [--modes lift full] [--json out.json]"""
import argparse, json, math, os, random, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CS = os.path.join(ROOT, 'crack', 'bin', 'crack-struct')
CS_GEN = CS      # генерация наблюдений (--gen-obs) — host-код, любой бинарник
CPU_MODE = False
MASK48 = (1 << 48) - 1

POOLS_LIFT = {
    'ow-lift (храмы, хижина, игло, корабль, деревня, trial, trail, руины)': ['desert_pyramids', 'igloos', 'swamp_huts', 'jungle_temples', 'shipwrecks', 'villages', 'trial_chambers', 'trail_ruins', 'ocean_ruins'],
    'ow-lift-храмы (4 храма/хижина по 24 + корабль)': ['desert_pyramids', 'igloos', 'swamp_huts', 'jungle_temples', 'shipwrecks'],
    'ow-mixed (+monument, mansion, ancient_city, ruined_portal)': ['desert_pyramids', 'igloos', 'swamp_huts', 'shipwrecks', 'villages', 'trial_chambers', 'ocean_monuments', 'woodland_mansions', 'ancient_cities', 'ruined_portals'],
}
POOLS_FULL = {
    'nether (nether_complexes + ruined_portals)': ['nether_complexes', 'ruined_portals'],
    'end (end_cities)': ['end_cities'],
    'ow-non-liftable (ancient_city, ruined_portal, monument, mansion)': ['ancient_cities', 'ruined_portals', 'ocean_monuments', 'woodland_mansions'],
}


def run(cmd):
    # GPU общий: поиск (crack-struct без --gen-obs) — под замком /tmp/gpu.lock короткими прогонами
    if cmd[0] == CS and '--gen-obs' not in cmd:
        if CPU_MODE: cmd = cmd[:1] + ['--dev', 'cpu'] + cmd[1:]          # CPU-режим: GPU не трогаем (окна полного перебора и lifting на CPU быстрые)
        else: cmd = ['flock', '/tmp/gpu.lock'] + cmd
    t0 = time.time(); r = subprocess.run(cmd, capture_output=True, text=True); return r, time.time() - t0


def info_bits(err):
    for l in err.splitlines():
        if 'информация ~' in l:
            return float(l.split('информация ~')[1].split('бит')[0])
    return float('nan')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=40)
    ap.add_argument('--version', default='26.3')
    ap.add_argument('--modes', nargs='+', default=['lift', 'full'])
    ap.add_argument('--kmax', type=int, default=10)
    ap.add_argument('--window', type=int, default=36)
    ap.add_argument('--rng', type=int, default=777)
    ap.add_argument('--json')
    ap.add_argument('--cpu', action='store_true', help='считать на CPU (--dev cpu), без GPU и замка')
    ap.add_argument('--only', nargs='*', default=[], help='подстроки названий пулов')
    ap.add_argument('--kmin', type=int, default=4)
    a = ap.parse_args()
    global CPU_MODE
    CPU_MODE = a.cpu
    rnd = random.Random(a.rng); out = {}
    tmp = '/tmp/crack_stats'; os.makedirs(tmp, exist_ok=True)
    if 'lift' in a.modes:
        for pname, pool in POOLS_LIFT.items():
            if a.only and not any(x in pname for x in a.only): continue
            print(f'\n== {pname}  [{a.version}, N={a.n} seed на K]')
            print(f'{"K":>2} {"инфо,бит":>9} {"кандидатов: среднее":>20} {"медиана":>8} {"макс":>6} {"P(единственный)":>16} {"истина найдена":>15} {"время,с":>8}')
            rows = []
            for K in range(a.kmin, a.kmax + 1):
                cnts = []; ok = 0; infos = []; tm = []
                for i in range(a.n):
                    seed = rnd.randint(-2**63, 2**63 - 1)
                    names = [rnd.choice(pool) for _ in range(K)]
                    r, _ = run([CS, '--version', a.version, '--gen-obs', ','.join(names), '--seed', str(seed), '--gen-rng', str(rnd.randint(1, 10**6))])
                    open(f'{tmp}/o.txt', 'w').write(r.stdout)
                    r, dt = run([CS, '--version', a.version, '--mode', 'lift', '--force', '--max-out', '100000000', f'{tmp}/o.txt'])
                    c = [int(x) for x in r.stdout.split() if x.isdigit()]
                    ok += (seed & MASK48) in c; cnts.append(len(c)); infos.append(info_bits(r.stderr)); tm.append(dt)
                row = (K, statistics.mean(infos), statistics.mean(cnts), statistics.median(cnts), max(cnts), sum(1 for c in cnts if c == 1) / a.n, ok / a.n, statistics.mean(tm))
                rows.append(row)
                print(f'{K:2d} {row[1]:9.1f} {row[2]:20.1f} {row[3]:8.0f} {row[4]:6d} {row[5]:16.2f} {row[6]:15.2f} {row[7]:8.2f}', flush=True)
            out['lift:' + pname] = rows
    if 'full' in a.modes:
        for pname, pool in POOLS_FULL.items():
            if a.only and not any(x in pname for x in a.only): continue
            print(f'\n== {pname}  [{a.version}]: ложные срабатывания в окне 2^{a.window} значений, масштабированные на 2^48 (N={a.n} окон на K)')
            print(f'{"K":>2} {"инфо,бит":>9} {"ожид. ложных 2^(48-инфо)":>26} {"измерено в окне*2^(48-W)":>26}')
            rows = []
            for K in range(2, a.kmax + 1):
                fp = 0; infos = []
                for i in range(a.n):
                    seed = rnd.randint(-2**63, 2**63 - 1)
                    names = [rnd.choice(pool) for _ in range(K)]
                    r, _ = run([CS, '--version', a.version, '--gen-obs', ','.join(names), '--seed', str(seed), '--gen-rng', str(rnd.randint(1, 10**6))])
                    open(f'{tmp}/o.txt', 'w').write(r.stdout)
                    r, dt = run([CS, '--version', a.version, '--mode', 'full', '--force', '--max-out', '1000000', '--assume-seed', str(seed & MASK48), '--window-bits', str(a.window), f'{tmp}/o.txt'])
                    c = [int(x) for x in r.stdout.split() if x.isdigit()]
                    fp += len([x for x in c if x != (seed & MASK48)]); infos.append(info_bits(r.stderr))
                bits = statistics.mean(infos)
                row = (K, bits, statistics.mean(2 ** (48 - b) for b in infos), fp / a.n * 2 ** (48 - a.window))
                rows.append(row)
                print(f'{K:2d} {row[1]:9.1f} {row[2]:26.3g} {row[3]:26.3g}', flush=True)
            out['full:' + pname] = rows
    if a.json:
        json.dump(out, open(a.json, 'w'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()

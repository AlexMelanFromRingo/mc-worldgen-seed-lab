#!/usr/bin/env python3
"""Сверка эмуляции java.util.HashSet<BlockPos> (libmcgen/src/feature_veg_hash.c) с настоящей Java: случайные «заплатки» растительности и
враждебные наборы (много столкновений корзин → деревья-корзины). Печатает число совпавших наборов.
    python3 libmcgen/tests/g5_veg_hash.py [--sets 3000]"""
import argparse, os, random, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser(); ap.add_argument('--sets', type=int, default=3000); ap.add_argument('--seed', type=int, default=1); a = ap.parse_args()
rnd = random.Random(a.seed)
tmp = tempfile.mkdtemp()
src = f'{ROOT}/libmcgen/tests/g5_veg'
subprocess.run(['javac', '-d', tmp, f'{src}/HashOrder.java'], check=True)
# C-тест
exe = f'{tmp}/hash_c'
subprocess.run(['gcc', '-O2', '-std=gnu11', f'-I{ROOT}/libmcgen/src', f'-I{ROOT}/libmcgen/include', f'-I{ROOT}/engine', '-o', exe, f'{ROOT}/libmcgen/tests/g5_veg_hash.c',
                f'{ROOT}/libmcgen/src/feature_veg_hash.c', '-lm', '-lpthread'] , check=False)
if not os.path.exists(exe):
    # feature_veg_hash.c тянет feature.h → нужна библиотека целиком
    subprocess.run(['gcc', '-O2', '-std=gnu11', f'-I{ROOT}/libmcgen/src', f'-I{ROOT}/libmcgen/include', f'-I{ROOT}/engine', '-o', exe, f'{ROOT}/libmcgen/tests/g5_veg_hash.c',
                    os.environ.get('MCGEN_LIB') or f'{ROOT}/libmcgen/build/libmcgen.a', '-lm', '-lpthread'], check=True)
sets = []
for k in range(a.sets):
    kind = k % 4
    pts, seen = [], set()
    ox, oy, oz = rnd.randint(-30000000, 30000000), rnd.randint(-60, 300), rnd.randint(-30000000, 30000000)
    if kind in (0, 1):                      # заплатка: колонки прямоугольника, y — пол/потолок с шумом
        xr, zr = rnd.randint(2, 9), rnd.randint(2, 9)
        base = oy
        for dx in range(-xr, xr + 1):
            for dz in range(-zr, zr + 1):
                if abs(dx) == xr and abs(dz) == zr: continue
                if rnd.random() < 0.15: continue
                y = base + (rnd.randint(-3, 3) if kind == 0 else rnd.randint(-1, 1))
                pts.append((ox + dx, y, oz + dz))
    elif kind == 2:                         # враждебный: все точки в нескольких корзинах (одинаковый хэш по модулю 64/128)
        n = rnd.randint(20, 300); step = rnd.choice([64, 128, 256, 512])
        for i in range(n):
            pts.append((ox + (i * step) % 5000 - 2500 + rnd.randint(0, 3), oy + rnd.randint(-5, 5), oz + (i // 7)))
    else:                                   # плотный куб
        s = rnd.randint(3, 8)
        for dx in range(s):
            for dy in range(s):
                for dz in range(s):
                    if rnd.random() < 0.8: pts.append((ox + dx, oy + dy, oz + dz))
    uniq = []
    for p in pts:
        if p not in seen: seen.add(p); uniq.append(p)
    if len(uniq) >= 2: sets.append(uniq)
data = ''.join(f'{len(s)}\n' + ''.join(f'{x} {y} {z}\n' for x, y, z in s) for s in sets)
j = subprocess.run(['java', '-cp', tmp, 'HashOrder'], input=data, capture_output=True, text=True, check=True).stdout.splitlines()
c = subprocess.run([exe], input=data, capture_output=True, text=True, check=True).stdout.splitlines()
ok = sum(1 for x, y in zip(j, c) if x.split() == y.split())
print(f'наборов {len(sets)}, совпало с Java: {ok}')
bad = [i for i, (x, y) in enumerate(zip(j, c)) if x.split() != y.split()]
if bad: print('несовпавшие (индекс, размер):', [(i, len(sets[i])) for i in bad[:15]])
sys.exit(0 if ok == len(sets) else 1)

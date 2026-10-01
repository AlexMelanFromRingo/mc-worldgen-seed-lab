#!/usr/bin/env python3
"""
Эталонный Python-прототип аналитического алгоритма crack-slime (docs/22 §2.3): W = l + (m<<18) + (g<<35), g (13 бит) находится в замкнутом виде.
Генерирует N положительных слайм-чанков из seed, находит кандидатов прототипом и сверяет с `crack-slime --algo brute` (перебор 2^30).
Использование: proto_slime_analytic.py [seed] [N]      (N=30: ~5 с)
"""
import os, random, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.abspath(os.path.join(HERE, "..", "..", "bin", "crack-slime"))
M = 0x5DEECE66D; MASK = (1 << 48) - 1; SALT = 987234911; KX = SALT ^ M; B = 0xB
M13 = M & 0x1FFF


def inv13(x):
    y = x
    for _ in range(5):
        y = (y * (2 - x * y)) & 0x1FFF
    return y


MINV13 = inv13(M13)


def i32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >> 31 else x


def cst(x, z):
    return (i32(x * x * 4987142) + i32(x * 5947611) + i32(z * z) * 4392871 + i32(z * 389711)) & MASK


def r_exact(W, c):
    s0 = ((W + c) & MASK) ^ KX
    return ((s0 * M + B) & MASK) >> 17


seed = int(sys.argv[1]) if len(sys.argv) > 1 else 4611686018427387904
N = int(sys.argv[2]) if len(sys.argv) > 2 else 30
W0 = seed & MASK
rnd = random.Random(seed)
obs = []
while len(obs) < N:
    x, z = rnd.randrange(-64, 64), rnd.randrange(-64, 64)
    c = cst(x, z)
    r = r_exact(W0, c)
    if r % 10 == 0 and r < 2147483640 and (x, z) not in obs:
        obs.append((x, z))
cs = [cst(x, z) for x, z in obs]
# lifting по чётности: 18 младших бит
m18 = (1 << 18) - 1
lows = [l for l in range(1 << 18) if all((((((l + c) & m18) ^ (KX & m18)) * M + B) & m18) >> 17 & 1 == 0 for c in cs)]
CL = [c & ((1 << 35) - 1) for c in cs]
CH = [c >> 35 for c in cs]
found = set()
for l in lows:
    for m in range(1 << 17):
        base = l + (m << 18)
        lo, hi, ok = 0, 8192, True
        for k in range(N):
            v = base + CL[k]; carry = v >> 35; vlo = v & ((1 << 35) - 1)
            A = ((vlo ^ KX) * M + B) & MASK
            a = A >> 17
            d = (((CH[k] + carry) & 0x1FFF) * M13) & 0x1FFF
            b = (a + (d << 18)) & 0x7FFFFFFF
            rho, be = b & 0x3FFFF, b >> 18
            if rho & 1 or rho >= 0x3FFF8:      # нечётное r / зона отклонения — в прототипе пропускаем пару (редкий случай)
                ok = False; break
            lam = rho % 5
            if k == 0:
                lam1, beta1 = lam, be
            else:
                D = (be - beta1) & 0x1FFF
                q = (lam1 + D + 5 - lam) % 5
                if q == 0: hi = min(hi, 8192 - D)
                elif q == 2: lo = max(lo, 8192 - D)
                else: ok = False; break
                if lo >= hi: ok = False; break
        if not ok:
            continue
        for j in range(lo, hi):
            if j % 5 == lam1:
                g = (((j - beta1) & 0x1FFF) * MINV13) & 0x1FFF
                found.add(base | (g << 35))
found = {W for W in found if all(r_exact(W, c) % 10 == 0 and r_exact(W, c) < 2147483640 for c in cs)}
f = os.path.join(os.environ.get("TMPDIR", "/tmp"), "proto_obs.txt")
open(f, "w").write("".join("%d %d slime\n" % p for p in obs))
out = subprocess.run([BIN, f, "--algo", "brute", "--limit", "10000000", "--quiet"], capture_output=True, text=True).stdout
ref = set(int(l.split()[0]) for l in out.splitlines() if l)
print("seed48=%d N=%d: низов %d, прототип: %d кандидатов, crack-slime brute: %d, совпадают: %s, истинный среди них: %s" % (W0, N, len(lows), len(found), len(ref), found == ref, W0 in found))
sys.exit(0 if found == ref and W0 in found else 1)

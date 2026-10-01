#!/usr/bin/env python3
"""Стресс-тест crack-slime: аналитический алгоритм (GPU и CPU) == перебор 2^30 (brute) на случайных конфигурациях (без oracle; формула слайма сверена отдельно).
Использование: test_slime_algos.py [число конфигураций] [суффикс бинарника, напр. -new]"""
import os, random, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.abspath(os.path.join(HERE, "..", "..", "bin"))
n = int(sys.argv[1]) if len(sys.argv) > 1 else 40
suf = sys.argv[2] if len(sys.argv) > 2 else ""
rnd = random.Random(20260930)
bad = 0; total_c = 0


def cands(exe, spec, extra):
    p = subprocess.run([BIN + "/" + exe, "--gen", spec, "--limit", "10000000", "--quiet"] + extra, capture_output=True, text=True)
    return sorted(int(l.split()[0]) for l in p.stdout.splitlines() if l), p.returncode


for i in range(n):
    S = rnd.randrange(1 << 64)
    N = rnd.choice([12, 13, 14, 16, 18, 20, 24, 30, 40, 64, 100, 200])
    M = rnd.choice([0, 0, 10, 100, 500])
    R = rnd.choice([32, 64, 128])
    spec = "%d,%d,%d,%d,%d" % (S, N, M, R, rnd.randrange(1000))
    mode = rnd.choice([[], ["--mode", "pos"]])
    a, rc1 = cands("crack-slime" + suf, spec, ["--algo", "brute"] + mode)
    b, rc2 = cands("crack-slime" + suf, spec, ["--algo", "analytic"] + mode)
    c, rc3 = cands("crack-slime-cpu" + suf, spec, ["--algo", "analytic"] + mode)
    ok = a == b == c and (S & ((1 << 48) - 1)) in a
    bad += (not ok); total_c += len(a)
    print("%s cfg%02d N=%d M=%d R=%d %s: brute=%d analytic(GPU)=%d analytic(CPU)=%d" % ("PASS" if ok else "FAIL", i, N, M, R, " ".join(mode), len(a), len(b), len(c)), flush=True)
print("== slime algos: %d конфигураций, расхождений %d, суммарно кандидатов %d ==" % (n, bad, total_c))
sys.exit(1 if bad else 0)

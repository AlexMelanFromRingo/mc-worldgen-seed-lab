#!/usr/bin/env python3
"""Сравнение версий 26.1/26.2/26.3 на сетках биомов + point/chunk в 26.3 + brute-force vs R-tree.
Использование: compare_versions.py [outdir_tmp]. Печатает таблицу (для docs/oracle.md)."""
import json, subprocess, sys, os, itertools
HERE = os.path.dirname(os.path.abspath(__file__))
TMP = sys.argv[1] if len(sys.argv) > 1 else "/tmp/oracle_cmp"
os.makedirs(TMP, exist_ok=True)
SEEDS = [12345, 8675309, 0]
N = 1024
JOBS = []  # (dim, seed, qy, mode, tag)
for s in SEEDS:
    JOBS += [("overworld", s, 16, "point"), ("overworld", s, -16, "point"), ("the_nether", s, 16, "point")]
def run(V):
    p = subprocess.Popen([os.path.join(HERE, "run.sh"), V, "serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    res = {}
    def q(c):
        p.stdin.write(c + "\n"); p.stdin.flush(); return json.loads(p.stdout.readline())
    modes = ["point"] + (["chunk"] if V == "26.3" else [])
    for (dim, s, qy, _m) in JOBS:
        for mode in modes:
            f = "%s/%s_%s_%d_%d_%s.bin" % (TMP, V, dim, s, qy, mode)
            r = q("biome %s %d %d %d %d %d %d --fmt bin --out %s --mode %s" % (dim, s, -N // 2, -N // 2, N, N, qy, f, mode))
            res[(dim, s, qy, mode)] = (f, r["palette"], r["elem"], r["ms"])
    # brute-force vs R-tree (256x256, overworld, qy=16)
    br = {}
    for s in SEEDS:
        r = q("biome overworld %d -128 -128 256 256 16 --fmt hash --brute" % s)
        br[s] = r["brute_mismatch"]
    p.stdin.write("quit\n"); p.stdin.flush(); p.wait()
    return res, br
def load(t):
    f, pal, el, _ = t
    b = open(f, "rb").read()
    if el == "u16le":
        idx = [b[i] | (b[i + 1] << 8) for i in range(0, len(b), 2)]
    else:
        idx = list(b)
    return [pal[i] for i in idx] if len(pal) < 4096 else None
out = {}
R = {}
B = {}
for V in ("26.1", "26.2", "26.3"):
    R[V], B[V] = run(V)
print("brute-force vs R-tree (256x256, overworld qy=16, число несовпадений):", B)
def cmp(a, b):
    n = len(a); d = sum(1 for x, y in zip(a, b) if x != y); return d, n
for key in [(d, s, qy) for (d, s, qy, _m) in JOBS]:
    g = {V: load(R[V][key + ("point",)]) for V in R}
    g263c = load(R["26.3"][key + ("chunk",)])
    row = []
    for (A, Bv) in (("26.1", "26.2"), ("26.2", "26.3"), ("26.1", "26.3")):
        d, n = cmp(g[A], g[Bv]); row.append("%s vs %s: %d (%.4f%%)" % (A, Bv, d, 100.0 * d / n))
    d, n = cmp(g["26.3"], g263c); row.append("26.3 point vs chunk: %d (%.4f%%)" % (d, 100.0 * d / n))
    print(key, "|", " | ".join(row))

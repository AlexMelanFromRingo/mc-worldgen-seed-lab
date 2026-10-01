#!/usr/bin/env python3
"""
Замеры crack-slime: число кандидатов и время vs число/состав наблюдений. Без oracle: наблюдения строит сам инструмент из известного seed
(--gen / --print-map); формулу слайм-чанка test_oracle.py сверяет с реальным кодом игры для 26.1/26.2/26.3.
Использование: bench_slime.py [gpu|cpu] [секции ABC]    Результат: results/bench-slime-<dev>.json (+ таблицы в stdout)
  A — только положительные (окно 128x128, режим pos); B — полная карта квадрата WxW; C — 30 положительных + M отрицательных.
"""
import json, os, random, re, statistics, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BIN = os.path.join(ROOT, "crack", "bin")
TMP = os.environ.get("CRACK_TEST_TMP") or tempfile.mkdtemp(prefix="slpb-")
os.makedirs(TMP, exist_ok=True)
dev = sys.argv[1] if len(sys.argv) > 1 else "gpu"
sections = sys.argv[2] if len(sys.argv) > 2 else "ABC"
algo = sys.argv[3] if len(sys.argv) > 3 else "analytic"          # analytic | brute
OUTJ = os.path.join(HERE, "results", "bench-slime-%s-%s.json" % (dev, algo))


def run(args, timeout=900):
    try:
        p = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return -9, None, None, {}, False, "timeout"
    err = p.stderr
    m = re.search(r"найдено кандидатов: (\d+)", err)
    cands = int(m.group(1)) if m else None
    m = re.search(r"lifting .*?: (\d+) из 262144", err)
    nlows = int(m.group(1)) if m else None
    m = re.search(r"время: lifting ([\d.]+) с, перебор ([\d.]+) с \((\w+); ([\d.e+]+) (?:кандидатов|пар)/с\), всего ([\d.]+) с", err)
    t = {"lift": float(m.group(1)), "search": float(m.group(2)), "dev": m.group(3), "rate": float(m.group(4)), "total": float(m.group(5))} if m else {}
    hit = "НАЙДЕН" in err and "НЕ найден" not in err
    return p.returncode, cands, nlows, t, hit, err


def seeds(n, salt):
    r = random.Random(salt)
    return [r.randrange(1 << 48) | (r.randrange(1 << 16) << 48) for _ in range(n)]


def gpu_util():
    try:
        return int(subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"], capture_output=True, text=True).stdout.split()[0])
    except Exception:
        return -1


res = json.load(open(OUTJ)) if os.path.exists(OUTJ) else {"positives_only": [], "full_window": [], "mixed": []}
res["gpu_util_start_%s" % "".join(sections)] = gpu_util()
print("# загрузка GPU (другие процессы) в начале: %d%%" % gpu_util(), flush=True)


def save():
    os.makedirs(os.path.dirname(OUTJ), exist_ok=True)
    json.dump(res, open(OUTJ, "w"), indent=1)


if "A" in sections:
    print("== A. только положительные (окно 128x128, режим pos), устройство %s ==" % dev)
    print("%5s %6s %10s %12s %14s %10s %10s" % ("N", "seeds", "nlows(med)", "cand(med)", "cand(min-max)", "t_search", "rate"), flush=True)
    plan = [(6, 1), (7, 1), (8, 2), (9, 3), (10, 4), (12, 5), (14, 6), (16, 6), (18, 6), (20, 8), (24, 8), (30, 8), (40, 8), (60, 8), (100, 8), (150, 8), (200, 8),
            (300, 6), (400, 6), (600, 4), (1000, 4)]
    if algo == "brute":
        plan = [(8, 1), (10, 2), (12, 2), (14, 3), (16, 3), (20, 3), (30, 3), (60, 3), (200, 3)]
    if dev == "cpu" and algo == "brute":
        plan = [(14, 1), (16, 2), (20, 2), (30, 2), (60, 2), (200, 2)]
    res["positives_only"] = []
    for N, ns in plan:
        cs, ts, nl, rates, hits = [], [], [], [], 0
        for S in seeds(ns, 100 + N):
            rc, c, nlows, t, hit, err = run([BIN + "/crack-slime", "--gen", "%d,%d,0,64,1" % (S, N), "--count-only", "--force", "--dev", dev, "--algo", algo])
            if c is None:
                print("ошибка", err[-300:]); continue
            cs.append(c); ts.append(t.get("search", 0)); nl.append(nlows); rates.append(t.get("rate", 0)); hits += hit
        if not cs:
            continue
        print("%5d %6d %10d %12d %6d-%-8d %10.3f %10.2e  истинный найден %d/%d" % (N, len(cs), statistics.median(nl), statistics.median(cs), min(cs), max(cs),
                                                                           statistics.median(ts), statistics.median(rates), hits, len(cs)), flush=True)
        res["positives_only"].append({"N": N, "seeds": len(cs), "cands": cs, "nlows": nl, "t_search": ts, "rate": rates, "hit": hits})
        save()

if "B" in sections:
    print("\n== B. полная карта слайм/не-слайм квадрата WxW (смешанный режим, окно в случайном месте) ==")
    print("%5s %7s %6s %10s %18s %10s" % ("W", "chunks", "seeds", "pos(med)", "cand(med; min-max)", "t_search"), flush=True)
    wplan = [(12, 4), (16, 8), (20, 8), (24, 8), (32, 8), (40, 6), (48, 6), (64, 6), (96, 4), (128, 4)]
    if algo == "brute":
        wplan = [(16, 2), (24, 2), (32, 2)]
    res["full_window"] = []
    for Wd, ns in wplan:
        cs, ts, ps = [], [], []
        for S in seeds(ns, 500 + Wd):
            r = random.Random(S)
            x0, z0 = r.randrange(-2000, 2000), r.randrange(-2000, 2000)
            out = subprocess.run([BIN + "/crack-slime", "--print-map", str(S), str(x0), str(z0), str(Wd), str(Wd)], capture_output=True, text=True).stdout.split()
            lines, npos = [], 0
            for iz, row in enumerate(out):
                for ix, ch in enumerate(row):
                    lines.append("%d %d %s" % (x0 + ix, z0 + iz, "slime" if ch == "1" else "noslime")); npos += ch == "1"
            if npos < 8:
                continue          # слишком мало положительных: lifting почти не работает, перебор огромный (см. §2.6)
            f = os.path.join(TMP, "win.txt"); open(f, "w").write("\n".join(lines) + "\n")
            rc, c, nlows, t, hit, err = run([BIN + "/crack-slime", f, "--count-only", "--force", "--dev", dev, "--algo", algo])
            if c is None:
                print("ошибка", err[-300:]); continue
            cs.append(c); ts.append(t.get("search", 0)); ps.append(npos)
        if not cs:
            continue
        print("%5d %7d %6d %10d %8d; %d-%d %10.3f" % (Wd, Wd * Wd, len(cs), statistics.median(ps), statistics.median(cs), min(cs), max(cs), statistics.median(ts)), flush=True)
        res["full_window"].append({"W": Wd, "seeds": len(cs), "cands": cs, "pos": ps, "t_search": ts})
        save()

if "C" in sections:
    print("\n== C. 30 положительных + M отрицательных (смешанный режим, окно 128x128) ==")
    res["mixed"] = []
    for M in ((0, 30, 100, 300, 1000, 3000) if algo != "brute" else (0, 300)):
        cs = []
        for S in seeds(6, 900 + M):
            rc, c, nlows, t, hit, err = run([BIN + "/crack-slime", "--gen", "%d,30,%d,64,1" % (S, M), "--count-only", "--force", "--dev", dev, "--algo", algo])
            if c is not None:
                cs.append(c)
        print("M=%5d: кандидатов медиана %d (min %d, max %d)" % (M, statistics.median(cs), min(cs), max(cs)), flush=True)
        res["mixed"].append({"M": M, "cands": cs})
        save()
save()

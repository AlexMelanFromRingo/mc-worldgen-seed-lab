#!/usr/bin/env python3
"""Замеры crack-pillars (шаг 2: 2^32 seed на ключ) на синтетике из известного seed. Результат: results/bench-pillars.json."""
import json, os, re, random, statistics, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BIN = os.path.join(ROOT, "crack", "bin")

CASES = [
    ("4 liftable (desert_pyramids+igloos+villages+shipwrecks)", "desert_pyramids+igloos+villages+shipwrecks"),
    ("6 liftable (+trial_chambers+jungle_temples)", "desert_pyramids+igloos+villages+shipwrecks+trial_chambers+jungle_temples"),
    ("4 без lifting (ocean_monuments+woodland_mansions+ancient_cities+ruined_portals)", "ocean_monuments+woodland_mansions+ancient_cities+ruined_portals"),
    ("3 liftable + 10 слайм-чанков", "desert_pyramids+igloos+villages+slime:10"),
    ("только 2 liftable (desert_pyramids+villages) — мало информации", "desert_pyramids+villages"),
]


def one(exe, S, sets, extra=()):
    p = subprocess.run([exe, "--gen", "%d,%s" % (S, sets), "--quiet" if False else "--limit", "10"] + list(extra), capture_output=True, text=True)
    e = p.stderr
    g = lambda pat: (re.search(pat, e).group(1) if re.search(pat, e) else None)
    return {
        "rc": p.returncode, "cands": int(g(r"итого кандидатов: (\d+)") or -1),
        "nx": g(r"lifting \(L=\d+\): (\d+) из"), "step1": float(g(r"шаг 1 ([\d.]+) с") or 0),
        "lift": float(g(r"lifting ([\d.]+) с, перебор") or 0), "search": float(g(r"перебор ([\d.]+) с") or 0),
        "kernel": float(g(r"чистое ядро ([\d.]+) с") or 0), "hit": "НАЙДЕН" in e and "НЕ найден" not in e,
    }


res = []
rr = random.Random(77)
for name, sets in CASES:
    row = {"case": name}
    for label, exe, extra in (("gpu_lift", BIN + "/crack-pillars", []), ("gpu_nolift", BIN + "/crack-pillars", ["--no-lift"]),
                              ("cpu_lift", BIN + "/crack-pillars-cpu", []), ("cpu_nolift", BIN + "/crack-pillars-cpu", ["--no-lift"])):
        runs = []
        for S in [rr.randrange(1 << 48) for _ in range(3)]:
            if label == "cpu_nolift" and "6 liftable" in name: continue
            runs.append(one(exe, S, sets, extra))
        if not runs: continue
        row[label] = {"search_min": min(r["search"] for r in runs), "search_med": statistics.median(r["search"] for r in runs),
                      "kernel_min": min(r["kernel"] for r in runs), "cands": [r["cands"] for r in runs], "hit": all(r["hit"] for r in runs), "nx": runs[0]["nx"]}
        print("%-70s %-11s поиск мин %.4f с (ядро мин %.4f) кандидатов %s nx=%s hit=%s" % (name, label, row[label]["search_min"], row[label]["kernel_min"], row[label]["cands"], row[label]["nx"], row[label]["hit"]), flush=True)
    res.append(row)
json.dump(res, open(os.path.join(HERE, "results", "bench-pillars.json"), "w"), indent=1, ensure_ascii=False)

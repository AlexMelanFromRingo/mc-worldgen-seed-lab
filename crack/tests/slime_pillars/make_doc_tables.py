#!/usr/bin/env python3
"""Формирует markdown-таблицы для docs/22-crack-slime-pillars.md из results/*.json и (с --apply) подставляет их вместо маркеров __NAME__."""
import json, os, re, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "results")
DOC = os.path.abspath(os.path.join(HERE, "..", "..", "..", "docs", "22-crack-slime-pillars.md"))


def jl(name):
    p = os.path.join(R, name)
    return json.load(open(p)) if os.path.exists(p) else None


def g3(x):
    return ("%.3g" % x)


def sp(n):
    return "{:,}".format(int(n)).replace(",", " ")


def med(xs):
    return statistics.median(xs)


tables = {}
ga = jl("bench-slime-gpu-analytic.json")
ca = jl("bench-slime-cpu-analytic.json")
gb = jl("bench-slime-gpu-brute.json")
cb = jl("bench-slime-cpu-brute.json")

if ga:
    rows = ["| N положит. | seed'ов | низов после lifting (мед.) | кандидатов (мед.) | мин – макс | время перебора GPU, с (мед.) | истинный среди найденных |", "|---|---|---|---|---|---|---|"]
    for e in ga["positives_only"]:
        trunc = min(e["cands"]) > 4194304
        rows.append("| %d | %d | %s | %s | %s – %s | %s | %s |" % (
            e["N"], e["seeds"], sp(med(e["nlows"])), sp(med(e["cands"])), sp(min(e["cands"])), sp(max(e["cands"])), g3(med(e["t_search"])),
            ("n/a (вывод усечён до 4·10^6)" if trunc else "%d/%d" % (e["hit"], e["seeds"]))))
    tables["BENCH_SLIME_A"] = "\n".join(rows)
    rows = ["| квадрат W×W | чанков | seed'ов | положит. (мед.) | кандидатов (мед.) | мин – макс |", "|---|---|---|---|---|---|"]
    for e in ga["full_window"]:
        rows.append("| %d×%d | %s | %d | %s | %s | %s – %s |" % (e["W"], e["W"], sp(e["W"] ** 2), e["seeds"], sp(med(e["pos"])), sp(med(e["cands"])), sp(min(e["cands"])), sp(max(e["cands"]))))
    tables["BENCH_SLIME_B"] = "\n".join(rows)
    rows = ["| M отрицательных (к 30 положительным) | кандидатов (мед.) | мин – макс |", "|---|---|---|"]
    for e in ga["mixed"]:
        rows.append("| %d | %s | %s – %s |" % (e["M"], sp(med(e["cands"])), sp(min(e["cands"])), sp(max(e["cands"]))))
    tables["BENCH_SLIME_C"] = "\n".join(rows)

# сравнение алгоритмов/устройств на общем префиксе seed'ов (seed'ы генерируются детерминированно => кандидаты должны совпадать)
if ga:
    def emap(j):
        return {e["N"]: e for e in j["positives_only"]} if j else {}
    Mga, Mca, Mgb, Mcb = emap(ga), emap(ca), emap(gb), emap(cb)
    def tk(M, N, k):
        return med(M[N]["t_search"][:k]) if N in M else None
    rows = ["| N положит. | seed'ов (общий префикс) | кандидатов (мед.) | brute GPU, с | brute CPU 12 потоков, с | analytic GPU, с | analytic CPU 12 потоков, с | ускорение GPU | ускорение CPU |", "|---|---|---|---|---|---|---|---|---|"]
    for N in (6, 7, 8, 10, 12, 14, 16, 20, 30, 60, 200):
        if N not in Mga: continue
        k = min([len(M[N]["cands"]) for M in (Mga, Mca, Mgb, Mcb) if N in M])
        same = all(M[N]["cands"][:k] == Mga[N]["cands"][:k] for M in (Mca, Mgb, Mcb) if N in M and min(M[N]["cands"]) < 4194304)
        a, b_, c_, d_ = tk(Mga, N, k), tk(Mgb, N, k), tk(Mcb, N, k), tk(Mca, N, k)
        fmt = lambda x: g3(x) if x is not None else "-"
        sg = ("%s×" % sp(b_ / a)) if (a and b_) else "-"
        sc = ("%s×" % sp(c_ / d_)) if (c_ and d_) else "-"
        rows.append("| %d | %d%s | %s | %s | %s | %s | %s | %s | %s |" % (N, k, "" if same else " (!расхождение)", sp(med(Mga[N]["cands"][:k])), fmt(b_), fmt(c_), fmt(a), fmt(d_), sg, sc))
    tables["BENCH_SLIME_CMP"] = "\n".join(rows)

p = jl("bench-pillars.json")
if p:
    rows = ["| набор наблюдений (на 1 ключ) | режим | x после lifting (из 65536) | кандидатов | поиск GPU, с (мин) | ядро GPU, с (мин) | поиск CPU 12 потоков, с (мин) |", "|---|---|---|---|---|---|---|"]
    for r in p:
        for lift, gk, ck in (("с lifting", "gpu_lift", "cpu_lift"), ("без lifting (полные 2^32)", "gpu_nolift", "cpu_nolift")):
            g, cc = r.get(gk), r.get(ck)
            if not g: continue
            rows.append("| %s | %s | %s | %s | %s | %s | %s |" % (r["case"], lift, g["nx"], g["cands"], g3(g["search_min"]), g3(g["kernel_min"]) if g["kernel_min"] else "-",
                                                                  g3(cc["search_min"]) if cc else "-"))
    tables["BENCH_PILLARS"] = "\n".join(rows)

for name, body in tables.items():
    print("== %s ==\n%s\n" % (name, body))
if "--apply" in sys.argv:
    doc = open(DOC).read()
    for name, body in tables.items():
        doc = doc.replace("__%s__" % name, body)
    open(DOC, "w").write(doc)
    print("подставлено:", list(tables))

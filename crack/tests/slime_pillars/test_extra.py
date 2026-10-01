#!/usr/bin/env python3
"""
Дополнительные тесты против oracle (одна версия): независимость от старших 16 бит seed, большие карты слайм-чанков,
50 раскладок башен, --expand-random (seed вида RandomSource.create().nextLong()), связка pillars+slime, учёт версии в таблице placement.
Использование: test_extra.py <26.1|26.2|26.3>; результат: results/extra-<V>.json
"""
import hashlib, json, os, random, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BIN = os.path.join(ROOT, "crack", "bin")
sys.path.insert(0, HERE)
from oracle_slp import Oracle

V = sys.argv[1]
TMP = os.environ.get("CRACK_TEST_TMP") or tempfile.mkdtemp(prefix="slpx-")
os.makedirs(TMP, exist_ok=True)
M, MASK = 0x5DEECE66D, (1 << 48) - 1
res = {"version": V, "checks": [], "metrics": {}}
failed = 0


def check(name, ok, info=""):
    global failed
    res["checks"].append({"name": name, "ok": bool(ok), "info": info})
    print("%-4s %s%s" % ("PASS" if ok else "FAIL", name, (" — " + info) if info else ""), flush=True)
    failed += (not ok)


def run(args, timeout=3600):
    p = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def write(name, text):
    p = os.path.join(TMP, name); open(p, "w").write(text); return p


def s64(x):
    x &= (1 << 64) - 1
    return x - (1 << 64) if x >> 63 else x


def next_long_seed(x):
    """RandomSource.create(x).nextLong() для LegacyRandomSource."""
    s = (x ^ M) & MASK
    s = (s * M + 0xB) & MASK; hi = s >> 16; hi = hi - (1 << 32) if hi >> 31 else hi
    s = (s * M + 0xB) & MASK; lo = s >> 16; lo = lo - (1 << 32) if lo >> 31 else lo
    return s64((hi << 32) + lo)


def main():
    rnd = random.Random(31337)
    with Oracle(V) as o:
        # 1. независимость от старших 16 бит (реальный код)
        for S in (12345, -8788534344520786540, 765906787396911863):
            T = s64(S ^ (0xABCD << 48))
            a = o.cmd("slime %d -64 -64 128 128" % S)["rows"]; b = o.cmd("slime %d -64 -64 128 128" % T)["rows"]
            check("slime: seed %d и %d (разные старшие 16 бит) дают одинаковую карту 128x128" % (S, T), a == b)
            pa = o.cmd("pillars %d" % S)["spikes"]; pb = o.cmd("pillars %d" % T)["spikes"]
            check("pillars: seed %d и %d дают одинаковые башни" % (S, T), pa == pb)
        # 2. большие карты слайм-чанков: сверка с нашей реализацией и хэши для сравнения версий
        big = {}
        for S in (2026, -777777777777):
            r = o.cmd("slime %d -256 -256 512 512" % S)
            rc, out, err = run([BIN + "/crack-slime", "--print-map", S, -256, -256, 512, 512])
            big[str(S)] = hashlib.sha256("".join(r["rows"]).encode()).hexdigest()[:16]
            check("slime: карта 512x512 (262144 чанков, %d слайм) seed %d == oracle" % (r["count"], S), out.split() == r["rows"])
        res["metrics"]["slime_big_sha"] = big
        # 3. 60 раскладок башен: oracle == crack-pillars --print-spikes, хэш для сравнения версий
        shas = {}; nbad = 0
        for i in range(60):
            S = rnd.randrange(-(1 << 63), 1 << 63)
            sp = o.cmd("pillars %d" % S)
            rc, out, err = run([BIN + "/crack-pillars", "--print-spikes", S])
            lines = out.splitlines()
            mine_key = int(lines[0][4:]); mh = [int(l.split()[3].split("=")[1]) for l in lines[1:]]
            ok = mine_key == sp["cache_key"] and mh == [s["height"] for s in sp["spikes"]]
            nbad += (not ok)
            shas[str(S)] = hashlib.sha256(json.dumps(sp["spikes"], sort_keys=True).encode()).hexdigest()[:16]
        check("pillars: 60 случайных seed: ключ и высоты == oracle", nbad == 0, "расхождений %d" % nbad)
        res["metrics"]["pillars_sha"] = shas
        # 4. --expand-random: seed, полученный как RandomSource.create(x).nextLong()
        okc = 0
        for t in range(3):
            x = rnd.randrange(1 << 48); w = next_long_seed(x)
            rows = o.cmd("slime %d -24 -24 48 48" % w)["rows"]
            lines = ["%d %d %s" % (-24 + ix, -24 + iz, "slime" if c == "1" else "noslime") for iz, row in enumerate(rows) for ix, c in enumerate(row)]
            f = write("ex.txt", "\n".join(lines) + "\n")
            rc, out, err = run([BIN + "/crack-slime", f, "--expand-random", "--limit", 10000])
            found = any(str(w) in l.split()[2:] for l in out.splitlines() if l and not l.startswith("#"))
            okc += found
            check("expand-random: 64-бит seed %d (автосид: nextLong от %d) восстановлен из карты слайм-чанков 48x48" % (w, x), found)
        # 5. pillars + слайм-чанки (ключ 16 бит + слайм-lifting)
        S = 424242; W = S & MASK
        sp = o.cmd("pillars %d" % S)["spikes"]; heights = ",".join(str(s["height"]) for s in sp)
        rows = o.cmd("slime %d -32 -32 64 64" % S)["rows"]
        pos = [(-32 + ix, -32 + iz) for iz, row in enumerate(rows) for ix, c in enumerate(row) if c == "1"]
        random.Random(5).shuffle(pos)
        f = write("pl_sl.txt", "".join("slime;%d;%d;1\n" % p for p in pos[:30]))
        rc, out, err = run([BIN + "/crack-pillars", "--heights", heights, "--with-structs", f, "--limit", 100000])
        c = [int(l.split()[0]) for l in out.splitlines() if l and not l.startswith("#")]
        check("pillars+slime: ключ башен + 30 слайм-чанков -> %d кандидат(ов), истинный среди них" % len(c), W in c)
        res["metrics"]["pillars_slime_cands"] = len(c)
        # 6. версия в таблице placement: abandoned_camp есть только в 26.3
        f = write("ac.txt", "abandoned_camp;10;20\n")
        rc, out, err = run([BIN + "/crack-pillars", "--check-seed", 0, "--with-structs", f, "--version", V])
        has = rc in (0, 1)
        check("версия %s: набор abandoned_camp %s" % (V, "известен" if has else "отсутствует (rc=%d)" % rc), has == (V == "26.3"))
        # 7. структуры: единственное число / id структуры как имя набора
        f = write("alias.txt", "desert_pyramid;11;-25\nvillage;0;0\nminecraft:end_city;3;4\n")
        rc, out, err = run([BIN + "/crack-pillars", "--check-seed", 12345, "--with-structs", f, "--version", V])
        check("алиасы имён (desert_pyramid, village, minecraft:end_city) разбираются", rc in (0, 1) and out.count("\n") == 3, out.replace("\n", " | "))
        # 9. слайм + структуры (--with-structs внутри ядра GPU) и CPU == GPU на смешанных данных с допуском ошибок
        S = 2718281828; W = S & MASK
        rows = o.cmd("slime %d -32 -32 64 64" % S)["rows"]
        pos = [(-32 + ix, -32 + iz) for iz, row in enumerate(rows) for ix, c in enumerate(row) if c == "1"]
        neg = [(-32 + ix, -32 + iz) for iz, row in enumerate(rows) for ix, c in enumerate(row) if c == "0"]
        rr = random.Random(9); rr.shuffle(pos); rr.shuffle(neg)
        f = write("sl_w.txt", "".join("%d %d slime\n" % p for p in pos[:30]))
        lines = []
        for st in ("desert_pyramids", "villages", "shipwrecks"):
            ch = o.cmd("structs overworld %d %s -60 -60 120 120" % (S, st))["sets"].get("minecraft:" + st, [])
            lines.append("%s;%d;%d" % (st, ch[0][0], ch[0][1]))
        g = write("sl_w_st.txt", "\n".join(lines) + "\n")
        rc, out0, err = run([BIN + "/crack-slime", f, "--limit", 100000])
        c0 = [int(l.split()[0]) for l in out0.splitlines() if l and not l.startswith("#")]
        rc, out1, err = run([BIN + "/crack-slime", f, "--with-structs", g, "--version", V, "--limit", 100000])
        c1 = [int(l.split()[0]) for l in out1.splitlines() if l and not l.startswith("#")]
        check("slime + 3 структуры (--with-structs): %d -> %d кандидатов, истинный среди них, подмножество" % (len(c0), len(c1)), W in c1 and set(c1) <= set(c0) and len(c1) <= len(c0))
        res["metrics"]["slime_structs"] = [len(c0), len(c1)]
        f2 = write("sl_mix.txt", "".join("%d %d slime\n" % p for p in pos[:40]) + "".join("%d %d noslime\n" % p for p in neg[:200]))
        rc, o_g, err = run([BIN + "/crack-slime", f2, "--max-errors", 1, "--limit", 100000])
        rc, o_c, err = run([BIN + "/crack-slime-cpu", f2, "--max-errors", 1, "--limit", 100000])
        cg = [int(l.split()[0]) for l in o_g.splitlines() if l and not l.startswith("#")]
        cc = [int(l.split()[0]) for l in o_c.splitlines() if l and not l.startswith("#")]
        check("slime GPU == CPU (OpenMP): 40+/200−, --max-errors 1: %d == %d кандидатов" % (len(cg), len(cc)), cg == cc and W in cg)
        # 10. pillars: --obs (x z значение) и --candidates
        sp = o.cmd("pillars %d" % S)["spikes"]
        lines = ["%d %d %d" % (sp_["centerX"] + (1 if i % 2 else -2), sp_["centerZ"], sp_["height"]) for i, sp_ in enumerate(sp)]
        ob = write("towers.txt", "\n".join(lines) + "\n")
        rc, out, err = run([BIN + "/crack-pillars", "--obs", ob, "--keys-only"])
        key = o.cmd("pillars %d" % S)["cache_key"]
        check("pillars --obs (x z высота, центры ±2): ключ %s" % out.split(), out.split() == [str(key)])
        cf = write("cand.txt", "%d\n%d\n%d\n" % (W, (W + 1) & MASK, (W ^ (1 << 40)) & MASK))
        rc, out, err = run([BIN + "/crack-pillars", "--key", key, "--candidates", cf, "--with-structs", g, "--version", V])
        check("pillars --candidates: из 3 кандидатов подходит только истинный", [int(l.split()[0]) for l in out.splitlines() if l and not l.startswith("#")] == [W])
        # 11. End: башни + End City (triangular, dim=the_end)
        S = -5550123456789; W = S & MASK
        sp = o.cmd("pillars %d" % S)["spikes"]; heights = ",".join(str(s["height"]) for s in sp)
        ch = o.cmd("structs end %d end_cities -150 -150 300 300" % S)["sets"].get("minecraft:end_cities", [])
        rr = random.Random(11); rr.shuffle(ch)
        f = write("end.txt", "".join("end_cities;%d;%d\n" % tuple(c) for c in ch[:8]))
        rc, out, err = run([BIN + "/crack-pillars", "--heights", heights, "--with-structs", f, "--version", V, "--limit", 100])
        c = [int(l.split()[0]) for l in out.splitlines() if l and not l.startswith("#")]
        check("End: 10 башен + 8 End City (triangular): %d кандидат(ов), истинный среди них" % len(c), W in c)
        res["metrics"]["end_cities_cands"] = len(c)
        # 12. предикаты (pred_check) для reducers legacy_type_1/2/3 на данных oracle: все PASS на истинном seed, почти все FAIL на чужом
        for S in (777, -31415926535):
            lines = []
            for st, (x0, n) in (("buried_treasures", (-30, 60)), ("pillager_outposts", (-150, 300)), ("mineshafts", (-30, 60)), ("ocean_monuments", (-60, 120)), ("woodland_mansions", (-150, 300)), ("ancient_cities", (-60, 120)), ("ruined_portals", (-60, 120))):
                ch = o.cmd("structs overworld %d %s %d %d %d %d" % (S, st, x0, x0, n, n))["sets"].get("minecraft:" + st, [])
                lines += ["%s;%d;%d" % (st, c[0], c[1]) for c in ch[:12]]
            f = write("chk.txt", "\n".join(lines) + "\n")
            rc, out, err = run([BIN + "/crack-pillars", "--check-seed", S & MASK, "--with-structs", f, "--version", V, "--quiet"])
            rc2, out2, err2 = run([BIN + "/crack-pillars", "--check-seed", (S + 1) & MASK, "--with-structs", f, "--version", V, "--quiet"])
            npass = out.count("PASS"); nfail2 = out2.count("FAIL")
            check("pred_check (%d наблюдений reducers/triangular из oracle): истинный seed %d — все PASS; чужой — FAIL %d/%d" % (len(lines), S, nfail2, len(lines)),
                  rc == 0 and npass == len(lines) and nfail2 >= 0.8 * len(lines))
        # 13. аналитический алгоритм слайма == перебор 2^30 на данных oracle (GPU и CPU)
        S = 31337; W = S & MASK
        rows = o.cmd("slime %d -32 -32 64 64" % S)["rows"]
        pos = [(-32 + ix, -32 + iz) for iz, row in enumerate(rows) for ix, c in enumerate(row) if c == "1"]
        neg = [(-32 + ix, -32 + iz) for iz, row in enumerate(rows) for ix, c in enumerate(row) if c == "0"]
        rr = random.Random(13); rr.shuffle(pos); rr.shuffle(neg)
        f = write("sl_alg.txt", "".join("%d %d slime\n" % p for p in pos[:24]) + "".join("%d %d noslime\n" % p for p in neg[:400]))
        lists = {}
        for exe, algo in (("crack-slime", "brute"), ("crack-slime", "analytic"), ("crack-slime-cpu", "analytic")):
            rc, out, err = run([BIN + "/" + exe, f, "--algo", algo, "--limit", 1000000])
            lists[exe + ":" + algo] = [int(l.split()[0]) for l in out.splitlines() if l and not l.startswith("#")]
        vals = list(lists.values())
        check("slime: brute == analytic(GPU) == analytic(CPU) на данных oracle (24+/400−): %s кандидатов" % [len(v) for v in vals], vals[0] == vals[1] == vals[2] and W in vals[0])
        # 8. CPU-вариант шахт: диапазон 2^30 вокруг seed == GPU-вариант
        S = 99991; W = S & MASK
        ch = o.cmd("structs overworld %d mineshafts -50 -50 100 100" % S)["sets"].get("minecraft:mineshafts", [])
        f = write("ms_cpu.txt", "".join("%d %d\n" % tuple(c) for c in ch[:8]))
        base = W & ~((1 << 30) - 1)
        outs = {}
        for exe in ("crack-mineshaft", "crack-mineshaft-cpu"):
            if os.path.exists(BIN + "/" + exe):
                rc, out, err = run([BIN + "/" + exe, f, "--range", "%d,30" % base, "--version", V])
                outs[exe] = [int(l.split()[0]) for l in out.splitlines() if l and not l.startswith("#")]
        check("mineshaft GPU и CPU (OpenMP) на диапазоне 2^30: %s" % {k: len(v) for k, v in outs.items()}, len(outs) == 2 and outs["crack-mineshaft"] == outs["crack-mineshaft-cpu"] and W in outs["crack-mineshaft"])
    return 0


try:
    main()
except Exception:
    import traceback; traceback.print_exc(); check("исключение", False)
json.dump(res, open(os.path.join(HERE, "results", "extra-%s.json" % V), "w"), indent=1, ensure_ascii=False)
print("\n== extra V=%s: %d проверок, провалено %d ==" % (V, len(res["checks"]), failed))
sys.exit(1 if failed else 0)

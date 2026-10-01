#!/usr/bin/env python3
"""
Тесты crack-slime / crack-pillars / crack-mineshaft против РЕАЛЬНОГО кода игры (oracle) для одной версии.
Использование: test_oracle.py <26.1|26.2|26.3> [--quick]
Результаты: results/oracle-<V>.json (числа: кандидаты, время), код возврата 0 — всё прошло.
"""
import hashlib, json, os, random, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BIN = os.path.join(ROOT, "crack", "bin")
sys.path.insert(0, HERE)
from oracle_slp import Oracle

V = sys.argv[1]
QUICK = "--quick" in sys.argv
TMP = os.environ.get("CRACK_TEST_TMP") or tempfile.mkdtemp(prefix="slp-")
os.makedirs(TMP, exist_ok=True)
MASK48 = (1 << 48) - 1

results = {"version": V, "checks": [], "metrics": {}}
failed = 0


def check(name, ok, info=""):
    global failed
    results["checks"].append({"name": name, "ok": bool(ok), "info": info})
    print("%-4s %s%s" % ("PASS" if ok else "FAIL", name, (" — " + info) if info else ""), flush=True)
    if not ok:
        failed += 1


def run(args, timeout=1800):
    t = time.time()
    p = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr, time.time() - t


def cand_list(stdout):
    return [int(l.split()[0]) for l in stdout.splitlines() if l and not l.startswith("#")]


def write(name, text):
    p = os.path.join(TMP, name)
    with open(p, "w") as f:
        f.write(text)
    return p


SEEDS = [0, 12345, -1, 123456789012345678, -8788534344520786540, 765906787396911863, 9223372036854775807, -9223372036854775808]
rnd = random.Random(2600)      # одинаковые «случайные» seed во всех версиях (для сравнения версий)
SEEDS += [rnd.randrange(-(1 << 63), 1 << 63) for _ in range(4)]
if QUICK:
    SEEDS = SEEDS[:4]


def tmpl(line_fmt):
    return line_fmt


def main():
    with Oracle(V) as o:
        info = o.cmd("info")
        print("# oracle:", info, flush=True)

        # ================= 1. СЛАЙМ: формула против реального кода =================
        maphash = {}
        for S in SEEDS:
            r = o.cmd("slime %d -64 -64 128 128" % S)
            rows = r["rows"]
            maphash[str(S)] = hashlib.sha256("".join(rows).encode()).hexdigest()[:16]
            rc, out, err, _ = run([BIN + "/crack-slime", "--print-map", S, -64, -64, 128, 128])
            check("slime-map V=%s S=%d (128x128 = 16384 чанков, %d слайм) == oracle" % (V, S, r["count"]), out.split() == rows)
        results["metrics"]["slime_map_sha"] = maphash

        # ================= 2. СЛАЙМ: восстановление seed =================
        rec = []
        for S in SEEDS[: (3 if QUICK else 6)]:
            W = S & MASK48
            r = o.cmd("slime %d -32 -32 64 64" % S)
            rows = r["rows"]
            pos = [(-32 + ix, -32 + iz) for iz, row in enumerate(rows) for ix, c in enumerate(row) if c == "1"]
            neg = [(-32 + ix, -32 + iz) for iz, row in enumerate(rows) for ix, c in enumerate(row) if c == "0"]
            rr = random.Random(S)
            rr.shuffle(pos); rr.shuffle(neg)
            scen = {}
            # (a) только положительные, 40 штук
            f = write("sl_a.txt", "".join("%d %d slime\n" % p for p in pos[:40]))
            rc, out, err, dt = run([BIN + "/crack-slime", f, "--limit", 100000, "--mode", "pos"])
            c = cand_list(out); scen["pos40"] = len(c)
            check("slime-recover V=%s S=%d 40 положительных: среди %d кандидатов истинный 48-бит seed" % (V, S, len(c)), W in c, "%.2fs" % dt)
            # (b) смешанный: 40 положительных + 1000 отрицательных
            f = write("sl_b.txt", "".join("%d %d slime\n" % p for p in pos[:40]) + "".join("%d %d noslime\n" % p for p in neg[:1000]))
            rc, out, err, dt = run([BIN + "/crack-slime", f, "--limit", 100000])
            c = cand_list(out); scen["pos40_neg1000"] = len(c)
            check("slime-recover V=%s S=%d 40+ / 1000- : %d кандидатов" % (V, S, len(c)), W in c, "%.2fs" % dt)
            # (c) вся карта 64x64 = 4096 чанков
            f = write("sl_c.txt", "".join("%d %d slime\n" % p for p in pos) + "".join("%d %d noslime\n" % p for p in neg))
            rc, out, err, dt = run([BIN + "/crack-slime", f, "--limit", 100000])
            c = cand_list(out); scen["full64x64"] = len(c)
            check("slime-recover V=%s S=%d полная карта 64x64 (%d слайм): %d кандидатов" % (V, S, len(pos), len(c)), W in c and len(c) >= 1, "%.2fs" % dt)
            # (d) допуск ошибок: 2 неверных наблюдения (положительный помечен как не-слайм, отрицательный как слайм)
            bad_pos, bad_neg = pos[:1], neg[:1]
            lines = "".join("%d %d slime\n" % p for p in pos[1:41]) + "%d %d noslime\n" % bad_pos[0] + "%d %d slime\n" % bad_neg[0] + \
                    "".join("%d %d noslime\n" % p for p in neg[1:301])
            f = write("sl_d.txt", lines)
            rc, out, err, dt = run([BIN + "/crack-slime", f, "--limit", 100000, "--max-errors", 2])
            c = cand_list(out); scen["err2"] = len(c)
            check("slime-errors V=%s S=%d --max-errors 2: истинный seed среди %d" % (V, S, len(c)), W in c, "%.2fs" % dt)
            rc, out, err, dt = run([BIN + "/crack-slime", f, "--limit", 100000, "--max-errors", 0])
            c0 = cand_list(out)
            check("slime-errors V=%s S=%d без допуска ошибок: истинного seed нет (ожидаемо)" % (V, S), W not in c0)
            rec.append({"seed": S, **scen})
        results["metrics"]["slime_recover"] = rec

        # CPU-fallback
        S = SEEDS[1]; W = S & MASK48
        r = o.cmd("slime %d -32 -32 64 64" % S)
        pos = [(-32 + ix, -32 + iz) for iz, row in enumerate(r["rows"]) for ix, c in enumerate(row) if c == "1"]
        random.Random(1).shuffle(pos)
        f = write("sl_cpu.txt", "".join("%d %d slime\n" % p for p in pos[:60]))
        if os.path.exists(BIN + "/crack-slime-cpu"):
            rc, out, err, dt = run([BIN + "/crack-slime-cpu", f, "--limit", 100000])
            c = cand_list(out)
            check("slime-cpu V=%s S=%d OpenMP-вариант: %d кандидатов" % (V, S, len(c)), W in c, "%.2fs" % dt)

        # ================= 3. БАШНИ КРАЯ =================
        prec = []
        PS = SEEDS[: (4 if QUICK else 10)]
        for S in PS:
            W = S & MASK48
            r = o.cmd("pillars %d" % S)
            sp = r["spikes"]; key = r["cache_key"]
            rc, out, err, _ = run([BIN + "/crack-pillars", "--print-spikes", S])
            mine = {}
            for line in out.splitlines():
                if line.startswith("key="):
                    mine["key"] = int(line[4:])
                else:
                    t = line.split()
                    mine[int(t[0])] = (int(t[1]), int(t[2]), int(t[3].split("=")[1]), int(t[4].split("=")[1]), int(t[5].split("=")[1]))
            ok = mine.get("key") == key and all(
                mine[i] == (sp[i]["centerX"], sp[i]["centerZ"], sp[i]["height"], sp[i]["radius"], 1 if sp[i]["guarded"] else 0) for i in range(10))
            check("pillars-layout V=%s S=%d: key=%d, центры/высоты/радиусы/клетки == oracle" % (V, S, key), ok)
            heights = ",".join(str(s["height"]) for s in sp)
            rc, out, err, dt = run([BIN + "/crack-pillars", "--heights", heights, "--keys-only"])
            keys = [int(x) for x in out.split()]
            check("pillars-key V=%s S=%d: по 10 высотам найден key (кандидатов ключей: %d)" % (V, S, len(keys)), key in keys, "%.3fs" % dt)
            # только радиусы; только клетки
            rc, out, err, dt = run([BIN + "/crack-pillars", "--heights", ",".join("r%d" % s["radius"] for s in sp), "--keys-only"])
            kr = [int(x) for x in out.split()]
            check("pillars-key V=%s S=%d: только радиусы -> %d ключей, среди них истинный" % (V, S, len(kr)), key in kr)
            rc, out, err, dt = run([BIN + "/crack-pillars", "--heights", ",".join(("cage" if s["guarded"] else "nocage") for s in sp), "--keys-only"])
            kc = [int(x) for x in out.split()]
            check("pillars-key V=%s S=%d: только клетки -> %d ключей, среди них истинный" % (V, S, len(kc)), key in kc)
            # 6 из 10 высот
            part = [str(s["height"]) if i in (0, 2, 3, 5, 7, 9) else "?" for i, s in enumerate(sp)]
            rc, out, err, dt = run([BIN + "/crack-pillars", "--heights", ",".join(part), "--keys-only"])
            k6 = [int(x) for x in out.split()]
            check("pillars-key V=%s S=%d: 6 из 10 высот -> %d ключей, среди них истинный" % (V, S, len(k6)), key in k6)
            prec.append({"seed": S, "key": key, "keys_10h": len(keys), "keys_radius": len(kr), "keys_cage": len(kc), "keys_6h": len(k6)})
        results["metrics"]["pillars_keys"] = prec

        # --- структуры из oracle: точность placement (scan-set == oracle structs) ---
        SETS_OW = ["desert_pyramids", "igloos", "jungle_temples", "swamp_huts", "shipwrecks", "villages", "trial_chambers", "ruined_portals",
                   "ocean_ruins", "ancient_cities", "ocean_monuments", "woodland_mansions", "buried_treasures", "mineshafts", "pillager_outposts", "trail_ruins"]
        if V == "26.3":
            SETS_OW.append("abandoned_camp")
        sets_seen = {}
        SS = SEEDS[:3] if QUICK else SEEDS[:5]
        for S in SS:
            for st in SETS_OW:
                nx = 100 if st not in ("woodland_mansions", "buried_treasures", "mineshafts", "pillager_outposts") else (300 if st in ("woodland_mansions", "pillager_outposts") else 60)
                x0 = -nx // 2
                r = o.cmd("structs overworld %d %s %d %d %d %d" % (S, st, x0, x0, nx, nx))
                got = sorted(map(tuple, r["sets"].get("minecraft:" + st, [])))
                rc, out, err, _ = run([BIN + "/crack-pillars", "--scan-set", S, st, x0, x0, nx, nx, "--version", V, "--quiet"])
                mine = sorted(tuple(map(int, l.split())) for l in out.splitlines())
                if st == "pillager_outposts":       # exclusion_zone не реализована: oracle ⊆ mine, а лишние — в радиусе 10 чанков от потенциальной деревни
                    ok = set(got) <= set(mine)
                    extra = set(mine) - set(got)
                    rcv, outv, _, _ = run([BIN + "/crack-pillars", "--scan-set", S, "villages", x0 - 11, x0 - 11, nx + 22, nx + 22, "--version", V, "--quiet"])
                    vill = [tuple(map(int, l.split())) for l in outv.splitlines()]
                    ok = ok and all(any(max(abs(e[0] - v[0]), abs(e[1] - v[1])) <= 10 for v in vill) for e in extra)
                    info = "oracle %d ⊆ мои %d; %d лишних — все в радиусе 10 чанков от потенциальной деревни (exclusion_zone)" % (len(got), len(mine), len(extra))
                else:
                    ok = got == mine
                    info = "%d чанков" % len(got)
                check("placement V=%s S=%d %s: потенциальные чанки == oracle structs" % (V, S, st), ok, info)
                sets_seen.setdefault(st, 0)
                sets_seen[st] += len(got)
        # End и Nether
        for S in SS[:3]:
            for dim, st, nx in (("end", "end_cities", 200), ("nether", "nether_complexes", 120), ("nether", "ruined_portals", 120)):
                x0 = -nx // 2
                r = o.cmd("structs %s %d %s %d %d %d %d" % (dim, S, st, x0, x0, nx, nx))
                got = sorted(map(tuple, r["sets"].get("minecraft:" + st, [])))
                rc, out, err, _ = run([BIN + "/crack-pillars", "--scan-set", S, st, x0, x0, nx, nx, "--version", V, "--quiet"])
                mine = sorted(tuple(map(int, l.split())) for l in out.splitlines())
                check("placement V=%s S=%d %s/%s: == oracle" % (V, S, dim, st), got == mine, "%d чанков" % len(got))
        results["metrics"]["placement_chunks_compared"] = sets_seen

        # --- pillars: шаг 2 с наблюдениями структур из oracle ---
        step2 = []
        for S in PS[:(3 if QUICK else 6)]:
            W = S & MASK48
            r = o.cmd("pillars %d" % S)
            key = r["cache_key"]; heights = ",".join(str(s["height"]) for s in r["spikes"])
            lines = []
            # 4 «liftable» структуры + 1 треугольная (36 + ~8 бит)
            for st in ("desert_pyramids", "igloos", "villages", "shipwrecks", "ocean_monuments"):
                nx = 120
                rr = o.cmd("structs overworld %d %s %d %d %d %d" % (S, st, -nx // 2, -nx // 2, nx, nx))
                ch = rr["sets"].get("minecraft:" + st, [])
                if ch:
                    c = ch[rnd.randrange(len(ch))]
                    lines.append("%s;%d;%d" % (st, c[0], c[1]))
            f = write("pl_s2.txt", "\n".join(lines) + "\n")
            rc, out, err, dt = run([BIN + "/crack-pillars", "--heights", heights, "--with-structs", f, "--version", V, "--limit", 1000])
            c = cand_list(out)
            check("pillars-step2 V=%s S=%d: %d структур -> %d кандидатов, истинный среди них" % (V, S, len(lines), len(c)), W in c, "%.2fs" % dt)
            # больше структур -> единственный кандидат
            for st in ("jungle_temples", "swamp_huts", "trial_chambers", "ruined_portals", "ocean_ruins"):
                rr = o.cmd("structs overworld %d %s -60 -60 120 120" % (S, st))
                ch = rr["sets"].get("minecraft:" + st, [])
                if ch:
                    c2 = ch[rnd.randrange(len(ch))]
                    lines.append("%s;%d;%d" % (st, c2[0], c2[1]))
            f = write("pl_s2b.txt", "\n".join(lines) + "\n")
            rc, out, err, dt = run([BIN + "/crack-pillars", "--heights", heights, "--with-structs", f, "--version", V, "--limit", 1000])
            c = cand_list(out)
            check("pillars-step2 V=%s S=%d: %d структур -> ровно 1 кандидат == seed mod 2^48" % (V, S, len(lines)), c == [W], "%.2fs" % dt)
            # CPU-вариант
            if os.path.exists(BIN + "/crack-pillars-cpu"):
                rc, out, err, dt2 = run([BIN + "/crack-pillars-cpu", "--heights", heights, "--with-structs", f, "--version", V, "--limit", 1000])
                check("pillars-step2-cpu V=%s S=%d OpenMP == GPU" % (V, S), cand_list(out) == c, "%.2fs" % dt2)
            step2.append({"seed": S, "n_struct": len(lines)})

        # ================= 4. ШАХТЫ =================
        mrec = []
        for S in SEEDS[: (2 if QUICK else 4)]:
            W = S & MASK48
            r = o.cmd("structs overworld %d mineshafts -50 -50 100 100" % S)
            ch = r["sets"].get("minecraft:mineshafts", [])
            check("mineshaft V=%s S=%d: oracle нашёл %d стартовых чанков в окне 100x100" % (V, S, len(ch)), len(ch) >= 8)
            f = write("ms.txt", "".join("%d %d\n" % tuple(c) for c in ch[:8]))
            base = W & ~((1 << 36) - 1)
            rc, out, err, dt = run([BIN + "/crack-mineshaft", f, "--range", "%d,36" % base, "--version", V, "--limit", 1000])
            c = cand_list(out)
            check("mineshaft-range V=%s S=%d: 8 стартов, перебор 2^36 вокруг seed -> %d кандидат(ов), истинный среди них" % (V, S, len(c)), W in c, "%.2fs" % dt)
            # полный поиск 2^48 с lifting за счёт liftable-структур
            lines = []
            for st in ("desert_pyramids", "igloos", "villages", "shipwrecks"):
                rr = o.cmd("structs overworld %d %s -60 -60 120 120" % (S, st))
                cc = rr["sets"].get("minecraft:" + st, [])
                if cc:
                    c3 = cc[rnd.randrange(len(cc))]; lines.append("%s;%d;%d" % (st, c3[0], c3[1]))
            g = write("ms_st.txt", "\n".join(lines) + "\n")
            rc, out, err, dt = run([BIN + "/crack-mineshaft", f, "--with-structs", g, "--version", V, "--limit", 1000])
            c = cand_list(out)
            check("mineshaft-full V=%s S=%d: 8 шахт + %d структур, полный 2^48 с lifting -> %d кандидат(ов)" % (V, S, len(lines), len(c)), W in c, "%.2fs" % dt)
            mrec.append({"seed": S, "full_with_lifting": len(c), "time": dt})
        results["metrics"]["mineshaft"] = mrec

        # ================= 5. --expand-random: сид из RandomSource.create().nextLong() =================
        S = 0x1234_5678_9ABC_DEF0 - (1 << 64) if False else SEEDS[3]
        # нерандомный seed: 64-бит seed «из LCG» строим на хосте: берём 48 бит и проверяем, что expand даёт исходный w, если w — nextLong
        # (самопроверка внутри crack-pillars --selftest)

    return 0


rc = 0
try:
    main()
except Exception as e:
    import traceback; traceback.print_exc()
    check("исключение в тесте", False, repr(e))
os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
with open(os.path.join(HERE, "results", "oracle-%s.json" % V), "w") as f:
    json.dump(results, f, indent=1, ensure_ascii=False)
n = len(results["checks"])
print("\n== V=%s: %d проверок, провалено %d ==" % (V, n, failed))
sys.exit(1 if failed else 0)

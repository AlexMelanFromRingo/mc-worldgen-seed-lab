#!/usr/bin/env python3
"""Генерация тест-векторов эталона: gen_vectors.py <26.1|26.2|26.3> [outdir]. Формат — см. docs/oracle.md."""
import json, subprocess, sys, os, random

V = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "tests", "vectors"), V)
os.makedirs(OUT, exist_ok=True)

def jhash(s):
    h = 0
    for ch in s:
        h = (31 * h + ord(ch)) & 0xFFFFFFFF
    return h - (1 << 32) if h >= (1 << 31) else h

rnd = random.Random(20260930)
SEEDS = [0, 1, -1, 2**63 - 1, -2**63, 12345, 8675309, jhash("hello"), jhash("Minecraft"), jhash("seed")]
SEEDS += [rnd.randrange(-2**63, 2**63) for _ in range(10)]
REGIONS = [(0, 0), (-3, -3), (2500, -2500), (-750000, 750000)]   # кварты: ~0, ~0(-), ~1e4 блоков, ~3e6 блоков
QY = {"overworld": [-16, 0, 16, 40], "the_nether": [0, 8, 16, 32], "the_end": [16]}
N = 4  # сетка N x N

p = subprocess.Popen([os.path.join(HERE, "run.sh"), V, "serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
def q(cmd):
    p.stdin.write(cmd + "\n"); p.stdin.flush()
    r = json.loads(p.stdout.readline())
    if not r.get("ok"):
        raise SystemExit("ошибка: %s -> %s" % (cmd, r))
    return r

# 1) climate + biome: seed qx qy qz T H C E D W biome   (End: seed qx qy qz height_value(double) height_fbits biome)
for dim in ("overworld", "the_nether", "the_end"):
    with open(os.path.join(OUT, "climate_%s.tsv" % dim), "w") as f:
        if dim == "the_end":
            f.write("# %s %s: seed qx qy qz height_value height_value_fbits biome   (height_value = erosion-функция в weirdBlock)\n" % (V, dim))
        else:
            f.write("# %s %s (mode=point): seed qx qy qz T H C E D W biome   (T..W квантованы x10000)\n" % (V, dim))
        for s in SEEDS:
            for (x0, z0) in REGIONS:
                for qy in QY[dim]:
                    if dim == "the_end":
                        for dz in range(N):
                            for dx in range(N):
                                r = q("climate the_end %d %d %d %d" % (s, x0 + dx, qy, z0 + dz))
                                f.write("%d\t%d\t%d\t%d\t%r\t%d\t%s\n" % (s, x0 + dx, qy, z0 + dz, r["height_value"], r["height_value_fbits"], r["biome"]))
                        continue
                    r = q("climategrid %s %d %d %d %d %d %d --biome" % (dim, s, x0, z0, N, N, qy))
                    names = r["palette"]
                    for iz in range(N):
                        for ix in range(N):
                            i = iz * N + ix
                            f.write("%d\t%d\t%d\t%d\t%s\t%s\n" % (s, x0 + ix, qy, z0 + iz, "\t".join(str(r["q"][k][i]) for k in "thcedw"), names[r["biome"][i]]))

# 2) хэши больших сеток биомов (FNV-1a64 по именам): seed mode qx0 qz0 n qy hash   (overworld 1024x1024, nether 1024x1024, end 512x512)
with open(os.path.join(OUT, "biome_grid_hashes.tsv"), "w") as f:
    f.write("# %s: dim seed mode qx0 qz0 n qy fnv1a64(names+'\\n', row-major iz*n+ix)\n" % V)
    modes = ["point"] + (["chunk"] if V == "26.3" else [])
    for dim, n in (("overworld", 1024), ("the_nether", 1024), ("the_end", 512)):
        for s in SEEDS[:6]:
            for mode in (modes if dim == "overworld" else ["point"]):
                r = q("biome %s %d %d %d %d %d 16 --fmt hash --mode %s" % (dim, s, -n // 2, -n // 2, n, n, mode))
                f.write("%s\t%d\t%s\t%d\t%d\t%d\t16\t%s\n" % (dim, s, mode, -n // 2, -n // 2, n, r["hash_fnv1a64_names"]))

# 3) noise / df (бит-в-бит): dim seed id x y z dbits fbits
pts = [(0, 0, 0), (100.5, 10.25, -200.75), (-1234.5, 64, 4321.25), (16, -32, 16), (3e5 + 0.5, 0, -3e5 + 0.25), (-7.123, 100, 8.456), (1000, 50, 1000), (0.1, 0.2, 0.3), (-64.5, -64, 64.5), (12345.678, 1.5, -9876.5)]
NOISES = ["minecraft:temperature", "minecraft:vegetation", "minecraft:continentalness", "minecraft:erosion", "minecraft:ridge", "minecraft:offset", "minecraft:surface", "minecraft:aquifer_barrier", "minecraft:cave_layer", "minecraft:jagged"]
with open(os.path.join(OUT, "noise.tsv"), "w") as f:
    f.write("# %s: dim seed noise_id x y z value dbits fbits   (Noises.instantiate как в RandomState; 26.3 значение нативно float)\n" % V)
    for dim in ("overworld", "the_nether", "the_end"):
        for s in SEEDS[:5]:
            ids = NOISES + (["minecraft:nether/temperature", "minecraft:nether/vegetation"] if dim == "the_nether" else [])
            for nid in ids:
                for (x, y, z) in pts:
                    r = q("noise %s %d %s %r %r %r" % (dim, s, nid, x, y, z))
                    f.write("%s\t%d\t%s\t%r\t%r\t%r\t%r\t%d\t%d\n" % (dim, s, nid, x, y, z, r["v"][0], r["dbits"][0], r["fbits"][0]))
DFS = {"overworld": ["minecraft:overworld/continents", "minecraft:overworld/erosion", "minecraft:overworld/ridges", "minecraft:overworld/offset", "minecraft:overworld/depth"],
       "the_nether": [], "the_end": []}
with open(os.path.join(OUT, "df.tsv"), "w") as f:
    f.write("# %s: dim seed df_id x y z value dbits fbits   (блоковая точка, без кэшей)\n" % V)
    for s in SEEDS[:5]:
        for did in DFS["overworld"]:
            for (x, y, z) in [(0, 64, 0), (100, 64, -200), (-1234, 0, 4321), (16, -32, 16), (300000, 100, -300000), (-7, 200, 8), (1000, 50, 1000), (55, 70, 66)]:
                r = q("df overworld %d %s %d %d %d" % (s, did, x, y, z))
                f.write("overworld\t%d\t%s\t%d\t%d\t%d\t%r\t%d\t%d\n" % (s, did, x, y, z, r["v"][0], r["dbits"][0], r["fbits"][0]))

# 4) структуры: seed set cx cz (окно 64x64 чанков около начала и ~1e4 блоков), плюс кольца strongholds
with open(os.path.join(OUT, "structs.tsv"), "w") as f:
    f.write("# %s: dim seed set cx cz   (StructurePlacement.isStructureChunk в окнах 64x64: [-32,32) и [600,664)x[-664,-600))\n" % V)
    for s in SEEDS[:6]:
        for (cx0, cz0) in [(-32, -32), (600, -664)]:
            r = q("structs overworld %d all %d %d 64 64" % (s, cx0, cz0))
            for sid, lst in r["sets"].items():
                for cx, cz in lst:
                    f.write("overworld\t%d\t%s\t%d\t%d\n" % (s, sid, cx, cz))
        for dim in ("the_nether", "the_end"):
            r = q("structs %s %d all -32 -32 64 64" % (dim, s))
            for sid, lst in r["sets"].items():
                for cx, cz in lst:
                    f.write("%s\t%d\t%s\t%d\t%d\n" % (dim, s, sid, cx, cz))
with open(os.path.join(OUT, "stronghold.tsv"), "w") as f:
    f.write("# %s: seed index cx cz  (ConcentricRings, 128 позиций; зависят от биомов)\n" % V)
    for s in SEEDS[:4]:
        r = q("stronghold %d" % s)
        for i, (cx, cz) in enumerate(r["chunks"]):
            f.write("%d\t%d\t%d\t%d\n" % (s, i, cx, cz))

# 5) мелочи: hashed / slime / pillars / blockbiome / defaultspawn
with open(os.path.join(OUT, "misc.jsonl"), "w") as f:
    for s in SEEDS:
        f.write(json.dumps(q("hashed %d" % s), ensure_ascii=False) + "\n")
        f.write(json.dumps(q("slime %d -16 -16 32 32" % s), ensure_ascii=False) + "\n")
        f.write(json.dumps(q("pillars %d" % s), ensure_ascii=False) + "\n")
    for s in SEEDS[:10]:
        f.write(json.dumps(q("defaultspawn overworld %d" % s), ensure_ascii=False) + "\n")
        for (x, y, z) in [(0, 64, 0), (100, 64, -200), (-5000, 70, 3000)]:
            f.write(json.dumps(q("blockbiome overworld %d %d %d %d" % (s, x, y, z)), ensure_ascii=False) + "\n")
            f.write(json.dumps(q("height overworld %d %d %d" % (s, x, z)), ensure_ascii=False) + "\n")
p.stdin.write("quit\n"); p.stdin.flush(); p.wait()
print("готово:", OUT)

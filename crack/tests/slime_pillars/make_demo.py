#!/usr/bin/env python3
"""Демо-данные из РЕАЛЬНОГО кода игры (oracle) для seed 12345 (версия по аргументу): башни, структуры, слайм-чанки, шахты -> data/demo/."""
import os, random, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from oracle_slp import Oracle
V = sys.argv[1] if len(sys.argv) > 1 else "26.3"
S = int(sys.argv[2]) if len(sys.argv) > 2 else 12345
D = os.path.join(HERE, "data", "demo")
os.makedirs(D, exist_ok=True)
rnd = random.Random(S)
with Oracle(V) as o:
    sp = o.cmd("pillars %d" % S)["spikes"]
    with open(os.path.join(D, "towers.txt"), "w") as f:
        f.write("# x z высота (Y блока бедрока под кристаллом) — реальный код игры %s, seed %d\n" % (V, S))
        for s in sp:
            f.write("%d %d %d\n" % (s["centerX"], s["centerZ"], s["height"]))
    with open(os.path.join(D, "heights.txt"), "w") as f:
        f.write(",".join(str(s["height"]) for s in sp) + "\n")
    lines = ["# <structure_set>;chunkX;chunkZ — по одному наблюдению (реальный код игры %s, seed %d)" % (V, S)]
    for st in ("desert_pyramids", "igloos", "villages", "shipwrecks", "trial_chambers", "swamp_huts"):
        ch = o.cmd("structs overworld %d %s -60 -60 120 120" % (S, st))["sets"].get("minecraft:" + st, [])
        c = ch[rnd.randrange(len(ch))]
        lines.append("%s;%d;%d" % (st, c[0], c[1]))
    open(os.path.join(D, "structs.txt"), "w").write("\n".join(lines) + "\n")
    rows = o.cmd("slime %d -16 -16 32 32" % S)["rows"]
    with open(os.path.join(D, "slime32.txt"), "w") as f:
        f.write("# chunkX chunkZ slime|noslime — полная карта 32x32 чанков, реальный код игры %s, seed %d\n" % (V, S))
        for iz, row in enumerate(rows):
            for ix, c in enumerate(row):
                f.write("%d %d %s\n" % (-16 + ix, -16 + iz, "slime" if c == "1" else "noslime"))
    ch = o.cmd("structs overworld %d mineshafts -40 -40 80 80" % S)["sets"].get("minecraft:mineshafts", [])
    with open(os.path.join(D, "mineshafts.txt"), "w") as f:
        f.write("# chunkX chunkZ стартовых чанков шахт (реальный код игры %s, seed %d)\n" % (V, S))
        for c in ch[:8]:
            f.write("%d %d\n" % (c[0], c[1]))
print("демо-данные в", D)

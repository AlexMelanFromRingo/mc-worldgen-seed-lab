#!/usr/bin/env python3
"""Сравнивает листья биомного дерева игры (LeafDump.java) с деревом cubiomes (tables/btreeXX.h).
Использование: cmp_leaves.py <leaves_game.txt> <btree-table.h> <biome_names.txt>"""
import re, sys, collections
game_f, tbl_f, names_f = sys.argv[1:4]
name2id = {}
id2name = {}
for l in open(names_f):
    i, n = l.split()
    name2id[n] = int(i); id2name[int(i)] = n
s = open(tbl_f).read()
pm = re.search(r'_param\[\]\[2\]\s*=\s*\{(.*?)\};', s, re.S)
param = [(int(a), int(b)) for a, b in re.findall(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', pm.group(1))]
nm = re.search(r'_nodes\[\]\s*=\s*\{(.*?)\};', s, re.S)
nodes = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', nm.group(1))]
leaves = collections.Counter()
for nd in nodes:
    if (nd >> 56) == 0xFF:
        biome = (nd >> 48) & 0xFF
        idx = [(nd >> (8 * i)) & 0xFF for i in range(6)]   # 0:t 1:h 2:c 3:e 4:d 5:w
        rng = tuple(param[i] for i in idx)
        leaves[(biome, rng)] += 1
game = collections.Counter()
for l in open(game_f):
    p = l.split()
    n = p[0]
    v = list(map(int, p[1:]))
    rng = tuple((v[2 * i], v[2 * i + 1]) for i in range(6))
    off = v[12]
    if n not in name2id:
        game[(-1 - hash(n) % 1000, rng, off)] += 1
        continue
    game[(name2id[n], rng, off)] += 1
cub = collections.Counter({(b, r, 0): c for (b, r), c in leaves.items()})
only_g = game - cub
only_c = cub - game
print(f"game leaves: {sum(game.values())}, cubiomes leaves: {sum(cub.values())}")
print(f"only in game: {sum(only_g.values())}; only in cubiomes: {sum(only_c.values())}")
def fmt(k):
    b, r, off = k
    nm = id2name.get(b, f"<unknown:{b}>")
    return f"{nm:26s} t[{r[0][0]},{r[0][1]}] h[{r[1][0]},{r[1][1]}] c[{r[2][0]},{r[2][1]}] e[{r[3][0]},{r[3][1]}] d[{r[4][0]},{r[4][1]}] w[{r[5][0]},{r[5][1]}] off={off}"
names_g = {}
for l in open(game_f):
    p = l.split()
    v = list(map(int, p[1:]))
    names_g[(tuple((v[2 * i], v[2 * i + 1]) for i in range(6)), v[12])] = p[0]
for k in sorted(only_g, key=lambda k: (id2name.get(k[0], "?"), k[1])):
    nm = names_g.get((k[1], k[2]), "?")
    print("  GAME-ONLY:", nm if k[0] < 0 else id2name.get(k[0]), fmt((k[0], k[1], k[2])) if k[0] >= 0 else fmt((k[0], k[1], k[2])))
for k in sorted(only_c, key=lambda k: (id2name.get(k[0], "?"), k[1])):
    print("  CUBIOMES-ONLY:", fmt(k))

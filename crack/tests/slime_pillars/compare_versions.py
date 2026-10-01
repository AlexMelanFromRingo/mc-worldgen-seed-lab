#!/usr/bin/env python3
"""Сравнение результатов oracle между версиями: слайм-карты и башни должны быть бит-в-бит одинаковы в 26.1/26.2/26.3."""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
vers = sys.argv[1:] or ["26.1", "26.2", "26.3"]


def load(kind, v):
    p = os.path.join(HERE, "results", "%s-%s.json" % (kind, v))
    return json.load(open(p)) if os.path.exists(p) else None


R = {v: load("oracle", v) for v in vers}
E = {v: load("extra", v) for v in vers}
if any(r is None for r in R.values()):
    print("нет результатов oracle для:", [v for v, r in R.items() if r is None]); sys.exit(1)
base = vers[0]
ok = True
for v in vers[1:]:
    a, b = R[base]["metrics"], R[v]["metrics"]
    common = sorted(set(a["slime_map_sha"]) & set(b["slime_map_sha"]))
    same_maps = all(a["slime_map_sha"][k] == b["slime_map_sha"][k] for k in common)
    ka = {str(x["seed"]): x["key"] for x in a["pillars_keys"]}
    kb = {str(x["seed"]): x["key"] for x in b["pillars_keys"]}
    ck = sorted(set(ka) & set(kb))
    same_keys = all(ka[k] == kb[k] for k in ck)
    pa, pb = a["placement_chunks_compared"], b["placement_chunks_compared"]
    same_pl = {k: pa[k] for k in pa if k in pb} == {k: pb[k] for k in pb if k in pa}
    print("версии %s vs %s: слайм-карты 128x128 (%d общих seed) %s; pillar key (%d общих seed) %s; число потенциальных чанков структур (общие наборы) %s; "
          "наборы только в одной версии: %s" % (base, v, len(common), "ИДЕНТИЧНЫ" if same_maps else "РАЗЛИЧАЮТСЯ", len(ck),
                                               "ИДЕНТИЧНЫ" if same_keys else "РАЗЛИЧАЮТСЯ", "совпадают" if same_pl else "РАЗЛИЧАЮТСЯ",
                                               sorted(set(pa) ^ set(pb))))
    ok = ok and same_maps and same_keys and same_pl
    if E[base] and E[v]:
        x, y = E[base]["metrics"], E[v]["metrics"]
        s1 = x["slime_big_sha"] == y["slime_big_sha"]
        s2 = x["pillars_sha"] == y["pillars_sha"]
        print("версии %s vs %s: карты слайм-чанков 512x512 (2 seed, 524288 чанков) %s; 60 раскладок башен (центры/радиусы/высоты/клетки) %s" % (
            base, v, "ИДЕНТИЧНЫ" if s1 else "РАЗЛИЧАЮТСЯ", "ИДЕНТИЧНЫ" if s2 else "РАЗЛИЧАЮТСЯ"))
        ok = ok and s1 and s2
sys.exit(0 if ok else 1)

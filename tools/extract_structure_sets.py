#!/usr/bin/env python3
"""Извлекает structure_set'ы (placement + структуры + биом-теги + измерение) из датапака игры.

Читает   src/data-<V>/data/minecraft/{worldgen/structure_set,worldgen/structure,tags/worldgen/biome}/**.json
Пишет    data/structure_sets-<V>.json      (V ∈ 26.1, 26.2, 26.3)

Использование:
    tools/extract_structure_sets.py                 # все версии, запись JSON
    tools/extract_structure_sets.py --diff          # + печать различий 26.1→26.2→26.3
    tools/extract_structure_sets.py --md 26.3       # markdown-таблица для docs/02

Нормализация placement (значения по умолчанию взяты из кода: 26.3 AbstractSpreadingStructurePlacement.placementCodec /
RandomSpreadStructurePlacement.CODEC; 26.1/26.2 StructurePlacement.placementCodec):
  spread_type = linear, frequency = 1.0, frequency_reduction_method = default, locate_offset = [0,0,0], exclusion_zone = null
Измерение структуры выводится из биом-тега (пересечение с tags/worldgen/biome/is_overworld|is_nether|is_end).
"""
import argparse
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
VERSIONS = ["26.1", "26.2", "26.3"]


def dp(v):
    return os.path.join(ROOT, "src", "data-" + v, "data", "minecraft")


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def strip_ns(s):
    return s.split(":", 1)[1] if ":" in s else s


class Tags:
    """Разворачивание биом-тегов (включая вложенные #tag и {id, required})."""

    def __init__(self, v):
        self.dir = os.path.join(dp(v), "tags", "worldgen", "biome")
        self.cache = {}

    def expand(self, tag):  # tag без '#', например 'minecraft:has_structure/village_plains'
        if tag in self.cache:
            return self.cache[tag]
        p = os.path.join(self.dir, *strip_ns(tag).split("/")) + ".json"
        res = []
        d = load(p)
        if d.get("replace"):
            pass
        for e in d["values"]:
            if isinstance(e, dict):
                e = e["id"]
            if e.startswith("#"):
                for b in self.expand(e[1:]):
                    if b not in res:
                        res.append(b)
            elif e not in res:
                res.append(e)
        self.cache[tag] = res
        return res

    def resolve(self, spec):
        """spec — значение поля `biomes` структуры/`preferred_biomes`: '#tag' | 'biome' | [..]."""
        if isinstance(spec, str):
            spec = [spec]
        out = []
        for e in spec:
            if isinstance(e, dict):
                e = e["id"]
            if e.startswith("#"):
                out += [b for b in self.expand(e[1:]) if b not in out]
            elif e not in out:
                out.append(e)
        return out


def norm_placement(p):
    t = strip_ns(p["type"])
    r = {"type": t}
    r["salt"] = p.get("salt", 0)
    r["locate_offset"] = p.get("locate_offset", [0, 0, 0])
    r["frequency"] = p.get("frequency", 1.0)
    r["frequency_reduction_method"] = p.get("frequency_reduction_method", "default")
    ez = p.get("exclusion_zone")
    r["exclusion_zone"] = None if ez is None else {"other_set": ez["other_set"], "chunk_count": ez["chunk_count"]}
    if t == "random_spread":
        r["spacing"] = p["spacing"]
        r["separation"] = p["separation"]
        r["spread_type"] = p.get("spread_type", "linear")
        r["limit"] = p["spacing"] - p["separation"]          # nextInt(limit) для каждой оси
    elif t == "concentric_rings":
        r["distance"] = p["distance"]
        r["spread"] = p["spread"]
        r["count"] = p["count"]
        r["preferred_biomes"] = p["preferred_biomes"]
    elif t == "dimension_origin":
        pass
    else:
        r["raw"] = p
    known = {"type", "salt", "locate_offset", "frequency", "frequency_reduction_method", "exclusion_zone",
             "spacing", "separation", "spread_type", "distance", "spread", "count", "preferred_biomes"}
    extra = {k: v for k, v in p.items() if k not in known}
    if extra:
        r["unknown_fields"] = extra
    return r


def extract(v):
    base = dp(v)
    tags = Tags(v)
    dims = {}
    for dim, tg in (("overworld", "is_overworld"), ("the_nether", "is_nether"), ("the_end", "is_end")):
        dims[dim] = set(tags.expand("minecraft:" + tg))
    structs = {}
    sdir = os.path.join(base, "worldgen", "structure")
    for fn in sorted(os.listdir(sdir)):
        if fn.endswith(".json"):
            structs[fn[:-5]] = load(os.path.join(sdir, fn))

    def dimensions_of(biomes):
        res = [d for d, s in dims.items() if any(b in s for b in biomes)]
        rest = [b for b in biomes if not any(b in s for s in dims.values())]
        return res, rest

    out = {"version": v, "structure_sets": {}, "structures": {}, "warnings": []}
    used = set()
    ssdir = os.path.join(base, "worldgen", "structure_set")
    for fn in sorted(os.listdir(ssdir)):
        if not fn.endswith(".json"):
            continue
        sid = fn[:-5]
        d = load(os.path.join(ssdir, fn))
        ent = {"id": "minecraft:" + sid, "placement": norm_placement(d["placement"]), "structures": [], "dimensions": []}
        if ent["placement"]["type"] == "concentric_rings":
            pb = ent["placement"]["preferred_biomes"]
            ent["placement"]["preferred_biomes_expanded"] = tags.resolve(pb)
        for s in d["structures"]:
            name = strip_ns(s["structure"])
            used.add(name)
            sj = structs[name]
            biomes = tags.resolve(sj["biomes"])
            dl, rest = dimensions_of(biomes)
            if rest:
                out["warnings"].append("%s: биомы вне is_overworld/is_nether/is_end: %s" % (name, rest))
            info = {"id": s["structure"], "weight": s["weight"], "type": strip_ns(sj["type"]), "step": sj.get("step"),
                    "biomes_spec": sj["biomes"], "biomes": biomes, "dimensions": dl,
                    "terrain_adaptation": sj.get("terrain_adaptation", "none")}
            for k in ("start_height", "project_start_to_heightmap", "size", "max_distance_from_center", "start_pool",
                      "use_expansion_hack", "dimension_padding", "liquid_settings", "pool_aliases"):
                if k in sj:
                    info[k] = sj[k]
            ent["structures"].append(info)
            for x in dl:
                if x not in ent["dimensions"]:
                    ent["dimensions"].append(x)
        out["structure_sets"][sid] = ent
    for name, sj in structs.items():
        biomes = tags.resolve(sj["biomes"])
        dl, _ = dimensions_of(biomes)
        out["structures"][name] = {"type": strip_ns(sj["type"]), "step": sj.get("step"), "biomes_spec": sj["biomes"],
                                   "dimensions": dl, "in_structure_set": name in used,
                                   "params": {k: v for k, v in sj.items() if k not in ("type", "biomes", "spawn_overrides", "step")}}
        if name not in used:
            out["warnings"].append("структура %s не входит ни в один structure_set" % name)
    return out


def flat(ent):
    """Плоское представление для diff."""
    p = ent["placement"]
    r = {("placement." + k): v for k, v in p.items() if k not in ("preferred_biomes_expanded",)}
    for i, s in enumerate(ent["structures"]):
        r["structure[%d]" % i] = "%s w=%s" % (s["id"], s["weight"])
        r["structure[%d].biomes" % i] = ",".join(s["biomes"])
        r["structure[%d].type" % i] = s["type"]
        r["structure[%d].step" % i] = s["step"]
    if "preferred_biomes_expanded" in p:
        r["placement.preferred_biomes_expanded"] = ",".join(p["preferred_biomes_expanded"])
    return r


def diff(a, b, na, nb):
    lines = []
    sa, sb = a["structure_sets"], b["structure_sets"]
    for k in sorted(set(sa) | set(sb)):
        if k not in sa:
            lines.append("+ %s: НОВЫЙ structure_set в %s: %s" % (k, nb, json.dumps(sb[k]["placement"], ensure_ascii=False)))
            for s in sb[k]["structures"]:
                lines.append("    + %s (weight %s, %s, биомы: %s)" % (s["id"], s["weight"], s["type"], s["biomes_spec"]))
            continue
        if k not in sb:
            lines.append("- %s: УДАЛЁН в %s" % (k, nb))
            continue
        fa, fb = flat(sa[k]), flat(sb[k])
        for f in sorted(set(fa) | set(fb)):
            if fa.get(f) != fb.get(f):
                if f.endswith(".biomes") or f.endswith("preferred_biomes_expanded"):
                    xa = set((fa.get(f) or "").split(",")) - {""}
                    xb = set((fb.get(f) or "").split(",")) - {""}
                    lines.append("~ %s %s: +%s -%s" % (k, f, sorted(xb - xa), sorted(xa - xb)))
                else:
                    lines.append("~ %s %s: %s -> %s" % (k, f, fa.get(f), fb.get(f)))
    for k in sorted(set(a["structures"]) | set(b["structures"])):
        if k not in a["structures"]:
            lines.append("+ structure %s (%s) НОВАЯ в %s" % (k, b["structures"][k]["type"], nb))
        elif k not in b["structures"]:
            lines.append("- structure %s УДАЛЕНА в %s" % (k, nb))
        else:
            pa, pb = a["structures"][k], b["structures"][k]
            for f in sorted(set(pa) | set(pb)):
                if pa.get(f) != pb.get(f):
                    if f == "params" and isinstance(pa.get(f), dict) and isinstance(pb.get(f), dict):
                        for pk in sorted(set(pa[f]) | set(pb[f])):
                            if pa[f].get(pk) != pb[f].get(pk):
                                lines.append("~ structure %s params.%s: %s -> %s" % (k, pk, json.dumps(pa[f].get(pk), ensure_ascii=False), json.dumps(pb[f].get(pk), ensure_ascii=False)))
                    else:
                        lines.append("~ structure %s %s: %s -> %s" % (k, f, json.dumps(pa.get(f), ensure_ascii=False)[:300], json.dumps(pb.get(f), ensure_ascii=False)[:300]))
    return lines


def md(o):
    rows = ["| set | структуры (вес) | placement | spacing | sep | limit | spread | salt | freq | freq-метод | exclusion | locate_offset | измерение | биом-тег(и) |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for k, e in o["structure_sets"].items():
        p = e["placement"]
        st = ", ".join("%s(%s)" % (strip_ns(s["id"]), s["weight"]) for s in e["structures"])
        tg = "; ".join(sorted({s["biomes_spec"] if isinstance(s["biomes_spec"], str) else "list" for s in e["structures"]}))
        ez = p["exclusion_zone"]
        ezs = "-" if not ez else "%s r=%d" % (strip_ns(ez["other_set"]), ez["chunk_count"])
        if p["type"] == "random_spread":
            rows.append("| %s | %s | random_spread | %d | %d | %d | %s | %d | %s | %s | %s | %s | %s | %s |" % (
                k, st, p["spacing"], p["separation"], p["limit"], p["spread_type"], p["salt"], p["frequency"],
                p["frequency_reduction_method"], ezs, tuple(p["locate_offset"]), "/".join(e["dimensions"]), tg))
        else:
            rows.append("| %s | %s | %s (distance=%s spread=%s count=%s pref=%s) | - | - | - | - | %d | %s | %s | %s | %s | %s | %s |" % (
                k, st, p["type"], p.get("distance"), p.get("spread"), p.get("count"), p.get("preferred_biomes"), p["salt"],
                p["frequency"], p["frequency_reduction_method"], ezs, tuple(p["locate_offset"]), "/".join(e["dimensions"]), tg))
    return "\n".join(rows)


def md_all(res):
    """Объединённые таблицы по всем версиям: столбец «версии» = где присутствует и совпадает ли placement."""
    vs = sorted(res)
    allsets = []
    for v in vs:
        for k in res[v]["structure_sets"]:
            if k not in allsets:
                allsets.append(k)
    rows = ["| set | структуры в сете (вес) | placement | spacing | sep | limit=sp-sep | spread | salt | frequency | freq-метод | exclusion_zone | locate_offset | измерение | версии |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for k in sorted(allsets):
        present = [v for v in vs if k in res[v]["structure_sets"]]
        e = res[present[-1]]["structure_sets"][k]
        strip_pb = lambda pl: {a: b for a, b in pl.items() if a != "preferred_biomes_expanded"}
        same = all(strip_pb(res[v]["structure_sets"][k]["placement"]) == strip_pb(e["placement"]) for v in present)
        names = [strip_ns(x["id"]) for x in e["structures"]]
        wts = [x["weight"] for x in e["structures"]]
        if len(names) > 4:
            pre = names[0].rsplit("_", 1)[0] if k == "abandoned_camp" else names[0]
            st = "%d структур %s_* (вес 1 каждая)" % (len(names), "abandoned_camp") if k == "abandoned_camp" else ", ".join("%s(%s)" % (n, w) for n, w in zip(names, wts))
        else:
            st = ", ".join("%s(%s)" % (n, w) for n, w in zip(names, wts))
        p = e["placement"]
        ez = p["exclusion_zone"]
        ezs = "-" if not ez else "%s r=%d" % (strip_ns(ez["other_set"]), ez["chunk_count"])
        ver = ",".join(present) + ("" if same else " (placement РАЗЛИЧАЕТСЯ!)")
        dim = "/".join(e["dimensions"])
        if p["type"] == "random_spread":
            rows.append("| %s | %s | random_spread | %d | %d | %d | %s | %d | %s | %s | %s | %s | %s | %s |" % (
                k, st, p["spacing"], p["separation"], p["limit"], p["spread_type"], p["salt"], p["frequency"],
                p["frequency_reduction_method"], ezs, tuple(p["locate_offset"]), dim, ver))
        else:
            rows.append("| %s | %s | %s: distance=%s spread=%s count=%s preferred=%s | - | - | - | - | %d | %s | %s | %s | %s | %s | %s |" % (
                k, st, p["type"], p.get("distance"), p.get("spread"), p.get("count"), p.get("preferred_biomes"), p["salt"],
                p["frequency"], p["frequency_reduction_method"], ezs, tuple(p["locate_offset"]), dim, ver))
    out = "\n".join(rows)
    out += "\n\n| structure | type | step | terrain_adaptation | биом-тег | измерение | в каких версиях | изменился тег/состав биомов? |\n|---|---|---|---|---|---|---|---|\n"
    allst = []
    for v in vs:
        for k in res[v]["structures"]:
            if k not in allst:
                allst.append(k)
    for k in sorted(allst):
        present = [v for v in vs if k in res[v]["structures"]]
        sj = res[present[-1]]["structures"][k]
        # состав биомов берём из structure_sets (там развёрнутый список)
        bl = {}
        for v in present:
            for sid, e in res[v]["structure_sets"].items():
                for x in e["structures"]:
                    if strip_ns(x["id"]) == k:
                        bl[v] = x["biomes"]
        chg = "нет"
        if len({tuple(b) for b in bl.values()}) > 1:
            first = None
            parts = []
            for v in present:
                if first is not None and set(bl[v]) != set(bl[first]):
                    parts.append("%s: +%s -%s" % (v, sorted(strip_ns(b) for b in set(bl[v]) - set(bl[first])), sorted(strip_ns(b) for b in set(bl[first]) - set(bl[v]))))
                first = v
            chg = "; ".join(parts)
        x0 = None
        for e in res[present[-1]]["structure_sets"].values():
            for x in e["structures"]:
                if strip_ns(x["id"]) == k:
                    x0 = x
        out += "| %s | %s | %s | %s | %s | %s | %s | %s |\n" % (k, sj["type"], sj["step"], x0["terrain_adaptation"], x0["biomes_spec"] if isinstance(x0["biomes_spec"], str) else "list", "/".join(sj["dimensions"]), ",".join(present), chg)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diff", action="store_true")
    ap.add_argument("--md", metavar="V")
    ap.add_argument("--md-all", action="store_true", help="объединённые markdown-таблицы по всем версиям")
    ap.add_argument("--versions", nargs="*", default=VERSIONS)
    a = ap.parse_args()
    res = {}
    os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
    for v in a.versions:
        res[v] = extract(v)
        path = os.path.join(ROOT, "data", "structure_sets-%s.json" % v)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(res[v], f, indent=1, ensure_ascii=False)
        print("wrote", path, "(%d sets, %d structures, %d warnings)" % (len(res[v]["structure_sets"]), len(res[v]["structures"]), len(res[v]["warnings"])), file=sys.stderr)
        for w in res[v]["warnings"]:
            print("  WARN", v, w, file=sys.stderr)
    if a.diff:
        vs = a.versions
        for i in range(len(vs) - 1):
            print("=== %s -> %s ===" % (vs[i], vs[i + 1]))
            ls = diff(res[vs[i]], res[vs[i + 1]], vs[i], vs[i + 1])
            print("\n".join(ls) if ls else "(без различий в structure_set / structure)")
    if a.md_all:
        print(md_all(res))
    if a.md:
        print(md(res[a.md] if a.md in res else extract(a.md)))


if __name__ == "__main__":
    main()

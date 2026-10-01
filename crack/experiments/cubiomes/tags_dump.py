#!/usr/bin/env python3
"""Раскрывает теги биомов (tags/worldgen/biome/**.json) датапака версии V и печатает JSON {tag: [biomes]}.
Использование: tags_dump.py 26.3 > out.json"""
import json, sys, os
V = sys.argv[1]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
root = f"{ROOT}/src/data-{V}/data/minecraft/tags/worldgen/biome"
def load(tag):
    p = os.path.join(root, tag + ".json")
    return json.load(open(p))
cache = {}
def expand(tag):
    if tag in cache: return cache[tag]
    out = []
    for v in load(tag)["values"]:
        if isinstance(v, dict): v = v["id"]
        if v.startswith("#minecraft:"):
            out += expand(v[len("#minecraft:"):])
        else:
            out.append(v.replace("minecraft:", ""))
    cache[tag] = sorted(set(out))
    return cache[tag]
res = {}
for dp, dn, fn in os.walk(root):
    for f in fn:
        if f.endswith(".json"):
            rel = os.path.relpath(os.path.join(dp, f), root)[:-5]
            res[rel] = expand(rel)
json.dump(res, sys.stdout, indent=0, sort_keys=True)

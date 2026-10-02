"""Замеры C-ядра меширования на настоящих чанках (вне Blender): время одного чанка, число граней, разные режимы.

    python3 blender/tests/bench_mesher.py [--chunks 200] [--out файл.json]
"""
import argparse
import json
import os
import statistics
import sys
import time

import numpy as np

import _boot
import anvil_util
import common
from mcgen_addon.mesh import mesher


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--chunks', type=int, default=200)
    ap.add_argument('--out', default=None)
    ap.add_argument('--reps', type=int, default=3)
    a = ap.parse_args()
    t = common.table()
    rd = os.path.join(_boot.SERVER_DIR, 'w12345', 'dimensions', 'minecraft', 'overworld', 'region')
    bn = common.biome_names()
    bidx = {n: i for i, n in enumerate(bn)}
    # квадрат чанков вокруг (0, 0) со статусом full
    side = int(np.ceil(np.sqrt(a.chunks)))
    c0 = -side // 2
    blocks, bio = {}, {}
    for cz in range(c0 - 1, c0 + side + 1):
        for cx in range(c0 - 1, c0 + side + 1):
            r = anvil_util.load_chunk(rd, cx, cz, t.state_from_props, lambda n: bidx.get(n, 0))
            if r is not None:
                blocks[(cx, cz)], bio[(cx, cz)] = r
    keys = [k for k in blocks if c0 <= k[0] < c0 + side and c0 <= k[1] < c0 + side][:a.chunks]
    res = {'chunks': len(keys)}
    for name, opt in (('обычный', mesher.MeshOptions()), ('merge', mesher.MeshOptions(merge=True)), ('оттенок r=0', mesher.MeshOptions(blend_radius=0)),
                      ('без жидкостей', mesher.MeshOptions(no_fluids=True))):
        m = mesher.Mesher(t, t.biome_colors, opt)
        best = []
        quads = []
        for ck in keys:
            ts = []
            for _ in range(a.reps):
                t0 = time.perf_counter()
                d = m.mesh_chunk(ck[0], ck[1], blocks, bio, -64, 384)
                ts.append((time.perf_counter() - t0) * 1000)
            best.append(min(ts))
            quads.append(d.n_quads)
        best.sort()
        res[name] = {'quads_mean': round(float(np.mean(quads)), 0), 'quads_max': int(max(quads)), 'ms_median': round(statistics.median(best), 2),
                     'ms_p95': round(best[int(len(best) * 0.95)], 2), 'ms_max': round(best[-1], 2),
                     'us_per_quad': round(float(np.sum(best)) * 1000.0 / max(1, sum(quads)), 3)}
    print(json.dumps(res, ensure_ascii=False, indent=1))
    if a.out:
        with open(a.out, 'w') as f:
            json.dump(res, f, ensure_ascii=False, indent=1)


main()

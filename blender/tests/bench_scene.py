"""Замеры сцены в Blender (headless): сборка N×N чанков из настоящих .mca, обновление одного чанка (ворота G7/G8).

    blender -b --python blender/tests/bench_scene.py -- --n 32 [--per-object 1] [--threads 12] [--updates 30]
"""
import argparse
import json
import os
import resource
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402
import anvil_util  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402

from mcgen_addon.assets import state_table  # noqa: E402
from mcgen_addon.render import scene as scene_mod  # noqa: E402


def rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=32)
    ap.add_argument('--cx0', type=int, default=-16)
    ap.add_argument('--cz0', type=int, default=-16)
    ap.add_argument('--per-object', type=int, default=1)
    ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--updates', type=int, default=30)
    ap.add_argument('--merge', action='store_true')
    ap.add_argument('--lod', action='store_true')
    ap.add_argument('--lod-distance', type=int, default=6)
    ap.add_argument('--lod-stride', type=int, default=2)
    ap.add_argument('--cache', default=os.path.join(_boot.SCRATCH, 'cache'))
    ap.add_argument('--out', default=None)
    a = ap.parse_args(argv)
    rd = os.path.join(_boot.SERVER_DIR, 'w12345', 'dimensions', 'minecraft', 'overworld', 'region')
    t0 = time.time()
    table = state_table.load(_boot.ASSETS_DIR, _boot.PACK_DIR, a.cache, version=_boot.VERSION)
    t_table = time.time() - t0
    bn = sorted('minecraft:' + f[:-5] for f in os.listdir(os.path.join(_boot.PACK_DIR, 'data', 'minecraft', 'worldgen', 'biome')) if f.endswith('.json'))
    bidx = {n: i for i, n in enumerate(bn)}
    t0 = time.time()
    blocks, bio = {}, {}
    for cz in range(a.cz0, a.cz0 + a.n):
        for cx in range(a.cx0, a.cx0 + a.n):
            r = anvil_util.load_chunk(rd, cx, cz, table.state_from_props, lambda n: bidx.get(n, 0))
            if r is not None:
                blocks[(cx, cz)], bio[(cx, cz)] = r
    t_load = time.time() - t0
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    vs = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=a.cache, chunks_per_object=a.per_object,
                                threads=a.threads, merge_flat=a.merge, lod=a.lod, lod_distance=a.lod_distance, lod_stride=a.lod_stride)
    sb = scene_mod.SceneBuilder(vs)
    stats = sb.build(blocks, bio, {'min_y': -64, 'height': 384, 'cx0': a.cx0, 'cz0': a.cz0, 'nx': a.n, 'nz': a.n}, None, bn)
    mem_after_build = rss_mb()
    # обновление одного чанка (правка блока в центре)
    keys = sorted(blocks.keys())
    mid = keys[len(keys) // 2]
    air = table.state_id('minecraft:air')
    stone = table.state_id('minecraft:stone')
    times = []
    import random
    rnd = random.Random(1)
    for i in range(a.updates):
        ck = rnd.choice(keys)
        arr = blocks[ck]
        idx = rnd.randrange(64 * 256, 128 * 256)
        old = int(arr[idx])
        arr[idx] = air if old != air else stone
        t0 = time.time()
        sb.update_chunk(*ck)
        times.append((time.time() - t0) * 1000)
    times_s = sorted(times)
    res = {'blender': bpy.app.version_string, 'merge': a.merge, 'lod': a.lod, 'chunks': len(blocks), 'per_object': a.per_object, 'table_load_s': round(t_table, 2),
           'mca_load_s': round(t_load, 2), 'build': {k: (round(v, 3) if isinstance(v, float) else v) for k, v in stats.items()},
           'update_ms': {'median': round(times_s[len(times_s) // 2], 1), 'max': round(times_s[-1], 1), 'min': round(times_s[0], 1), 'n': len(times)},
           'peak_rss_mb': round(mem_after_build, 0), 'objects': len(bpy.data.objects), 'meshes': len(bpy.data.meshes)}
    print('RESULT ' + json.dumps(res))
    if a.out:
        with open(a.out, 'w') as f:
            json.dump(res, f, indent=1)


main()

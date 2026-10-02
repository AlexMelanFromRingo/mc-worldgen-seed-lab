"""Ворота G7/G8 на настоящих чанках libmcgen: сцена из дампа MCR1 (mcgen-cli --out region.mcr), замеры сборки и обновления чанка.

    blender -b --factory-startup --python blender/tests/bench_mcr.py -- --mcr /tmp/r32.mcr [--per-object 1] [--merge] [--lod] [--threads 12]
"""
import argparse
import json
import os
import random
import resource
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'gt'))
import _boot  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402
import mcr as mcrmod  # noqa: E402  (tools/gt/mcr.py)

from mcgen_addon.render import scene as scene_mod  # noqa: E402


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--mcr', required=True)
    ap.add_argument('--per-object', type=int, default=1)
    ap.add_argument('--threads', type=int, default=0)
    ap.add_argument('--updates', type=int, default=40)
    ap.add_argument('--merge', action='store_true')
    ap.add_argument('--lod', action='store_true')
    ap.add_argument('--lod-distance', type=int, default=6)
    ap.add_argument('--cache', default=os.path.join(_boot.SCRATCH, 'cache'))
    ap.add_argument('--out', default=None)
    a = ap.parse_args(argv)
    t0 = time.time()
    m = mcrmod.Mcr(a.mcr)
    blocks, bio = {}, {}
    for cz in range(m.cz0, m.cz0 + m.nz):
        for cx in range(m.cx0, m.cx0 + m.nx):
            if m.has(cx, cz):
                blocks[(cx, cz)] = m.blocks(cx, cz).reshape(-1)
                bio[(cx, cz)] = m.biomes(cx, cz).reshape(-1)
    t_read = time.time() - t0
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    vs = scene_mod.ViewSettings(assets_dir=_boot.ASSETS_DIR, pack_dir=_boot.PACK_DIR, cache_dir=a.cache, chunks_per_object=a.per_object,
                                threads=a.threads, merge_flat=a.merge, lod=a.lod, lod_distance=a.lod_distance)
    sb = scene_mod.SceneBuilder(vs)
    st = sb.build(blocks, bio, {'min_y': m.min_y, 'height': m.height, 'cx0': m.cx0, 'cz0': m.cz0, 'nx': m.nx, 'nz': m.nz},
                  list(m.state_names), list(m.biome_names))
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0
    # обновления: правка блока в копии массива чанка
    keys = sorted(blocks)
    rnd = random.Random(7)
    air = sb.table.state_id('minecraft:air')
    stone = sb.table.state_id('minecraft:stone')
    times = []
    for i in range(a.updates):
        ck = rnd.choice(keys)
        arr = sb.blocks[ck]
        if not arr.flags['WRITEABLE'] or isinstance(arr, np.memmap):
            arr = np.array(arr)
            sb.blocks[ck] = arr
        idx = rnd.randrange(80 * 256, 140 * 256)
        arr[idx] = air if int(arr[idx]) != air else stone
        times.append(sb.update_chunk(*ck) * 1000)
    times.sort()
    res = {'blender': bpy.app.version_string, 'chunks': len(blocks), 'per_object': a.per_object, 'merge': a.merge, 'lod': a.lod,
           'threads': st['threads'], 'read_s': round(t_read, 2), 'table_s': round(sb.stats.get('table_seconds', 0), 2),
           'build_s': round(st['seconds'], 2), 'mesh_wait_s': round(st['mesh_wait_seconds'], 2), 'blender_s': round(st['blender_seconds'], 2),
           'lod_s': round(st['lod_seconds'], 2), 'quads': st['quads'], 'objects': len(bpy.data.objects), 'peak_rss_mb': round(rss),
           'update_ms': {'median': round(times[len(times) // 2], 1), 'p95': round(times[int(len(times) * 0.95)], 1), 'max': round(times[-1], 1),
                         'n': len(times)}}
    print('RESULT ' + json.dumps(res))
    if a.out:
        with open(a.out, 'w') as f:
            json.dump(res, f, indent=1)


main()

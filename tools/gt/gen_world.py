#!/usr/bin/env python3
"""Генерация эталонного мира ванильным сервером с изоляцией стадий датапаком.

  tools/gt/gen_world.py --version 26.3 --variant raw --dim overworld --seed 12345 --cx 0 --cz 0 --radius 10

Создаёт run/gt/<версия>/<вариант>/<dim>-s<seed>-c<cx>_<cz>-r<R>/ :
  world/                 мир (level-name=world), датапак — world/datapacks/gt/ (кладётся ДО первого запуска)
  server.properties, server.log, manifest.json
Сервер запускается под flock /tmp/mcgen-server.lock (один ванильный сервер на машину). Область — квадрат (2R+1)^2 чанков вокруг
чанка (cx, cz); чанки удерживаются через `forceload add` окнами <=16x16, готовность — по Status=full в region-файлах.
Варианты: см. `datapack.py --list` (raw, veins, surface, carvers, features, full, feature:<id>, structure:<id>).
"""
import argparse, json, os, platform, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, GT, DIMS, DIM_SHORT, VERSIONS, Server, server_lock, read_props, write_props, pack_dir, version_info
import datapack
import anvil


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def world_name(dim, seed, cx, cz, radius, tag=''):
    return f'{DIM_SHORT[DIMS[dim]]}-s{seed}-c{cx}_{cz}-r{radius}' + (f'_{tag}' if tag else '')


def world_dir(version, variant, dim, seed, cx, cz, radius, tag=''):
    return f'{GT}/{version}/{datapack.safe_name(variant)}/{world_name(dim, seed, cx, cz, radius, tag)}'


def free_port():
    s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close()
    return p


def windows(x0, x1, z0, z1, size=16):
    return [(ax, az, min(ax + size - 1, x1), min(az + size - 1, z1)) for ax in range(x0, x1 + 1, size) for az in range(z0, z1 + 1, size)]


def statuses_of(rdir, chunks):
    """{(cx,cz): status|None} для списка чанков; region-файлы открываются по одному разу."""
    out, by = {}, {}
    for cx, cz in chunks:
        by.setdefault((cx >> 5, cz >> 5), []).append((cx, cz))
    for (rx, rz), lst in by.items():
        p = f'{rdir}/r.{rx}.{rz}.mca'
        try:
            rf = anvil.RegionFile(p)
        except OSError:
            for c in lst: out[c] = None
            continue
        try:
            for cx, cz in lst:
                try:
                    raw = rf.raw(cx, cz) if rf.has(cx, cz) else None
                except Exception:
                    raw = None
                m = anvil._STATUS_RE.search(raw) if raw else None
                out[(cx, cz)] = raw[m.end():m.end() + m.group(1)[0]].decode() if m else None
        finally:
            rf.close()
    return out


GAMERULES = ['random_tick_speed 0', 'mob_griefing false', 'spawn_mobs false', 'advance_time false', 'advance_weather false']


def generate(version, variant, dim, seed, cx, cz, radius, xmx='5g', batch=512, force=False, tick='freeze', timeout=7200,
             extra_props=None, tag='', stall=300):
    dim = DIMS[dim]
    spec, param = datapack.parse_variant(variant)
    wd = world_dir(version, variant, dim, seed, cx, cz, radius, tag)
    man_path = f'{wd}/manifest.json'
    if os.path.exists(man_path) and not force:
        m = json.load(open(man_path))
        if m.get('ok'):
            log(f'уже есть: {wd} ({m["chunks_full"]}/{m["chunks_area"]} full); --force чтобы пересоздать')
            return m
    if os.path.exists(wd):
        shutil.rmtree(wd)
    os.makedirs(f'{wd}/world/datapacks', exist_ok=True)
    t_all = time.monotonic()
    # --- датапак ДО первого запуска ---
    dp = datapack.build(version, variant, f'{wd}/world/datapacks/gt')
    props = {
        'level-name': 'world', 'level-seed': str(seed), 'level-type': 'minecraft\\:normal',
        'generate-structures': str(bool(spec['generate_structures'])).lower(),
        'pause-when-empty-seconds': '-1', 'view-distance': '3', 'simulation-distance': '2', 'spawn-protection': '0',
        'online-mode': 'false', 'enable-status': 'false', 'max-tick-time': '-1', 'sync-chunk-writes': 'false',
        'region-file-compression': 'deflate', 'white-list': 'false', 'enforce-secure-profile': 'false',
        'difficulty': 'easy', 'server-port': str(free_port()), 'motd': f'gt {version} {variant}',
        'initial-enabled-packs': 'vanilla,file/gt' if dp else 'vanilla',
    }
    props.update(extra_props or {})
    write_props(f'{wd}/server.properties', props)
    x0, x1, z0, z1 = cx - radius, cx + radius, cz - radius, cz + radius
    area = [(x, z) for x in range(x0, x1 + 1) for z in range(z0, z1 + 1)]
    rdir = anvil.region_dir(f'{wd}/world', dim)
    srv = Server(version, wd, xmx=xmx)
    man = {'ok': False, 'tool': 'tools/gt/gen_world.py', 'version': version, 'variant': variant, 'dim': dim, 'seed': seed,
           'center_chunk': [cx, cz], 'radius': radius, 'area_chunks': [x0, z0, x1, z1], 'chunks_area': len(area),
           'datapack': dp, 'properties': props, 'java_xmx': xmx, 'tick': tick, 'world_version': version_info(version)['world_version'],
           'host': {'cores': os.cpu_count(), 'platform': platform.platform()}}
    with server_lock(log):
        t_lock = time.monotonic()
        log(f'старт сервера {version} -> {wd}')
        man['server_start_s'] = round(srv.start(), 1)
        try:
            man['java'] = subprocess.run(['java', '-version'], capture_output=True, text=True).stderr.splitlines()[0]
        except Exception:
            pass
        for g in GAMERULES:
            srv.cmd('gamerule ' + g)
        if tick == 'freeze':
            srv.cmd('tick freeze')
        time.sleep(1)
        t_gen = time.monotonic()
        wins = windows(x0, x1, z0, z1)
        phases, cur, n = [], [], 0
        for w in wins:
            sz = (w[2] - w[0] + 1) * (w[3] - w[1] + 1)
            if cur and n + sz > batch:
                phases.append(cur); cur, n = [], 0
            cur.append(w); n += sz
        if cur:
            phases.append(cur)
        done, t_prog = 0, time.monotonic()
        for pi, ph in enumerate(phases):
            need = [(x, z) for (ax, az, bx, bz) in ph for x in range(ax, bx + 1) for z in range(az, bz + 1)]
            for (ax, az, bx, bz) in ph:
                srv.cmd(f'execute in minecraft:{dim} run forceload add {ax * 16} {az * 16} {bx * 16 + 15} {bz * 16 + 15}')
            t0, ok_set, t_prog = time.monotonic(), set(), time.monotonic()
            while True:
                time.sleep(2)
                if not srv.alive():
                    raise RuntimeError(f'сервер упал; см. {srv.log_path}')
                srv.cmd('save-all flush')
                time.sleep(1)
                st = statuses_of(rdir, [c for c in need if c not in ok_set])
                n0 = len(ok_set)
                ok_set |= {c for c, s in st.items() if s == 'minecraft:full'}
                if len(ok_set) == len(need):
                    break
                if len(ok_set) > n0:
                    t_prog = time.monotonic()
                # сторожок: сервер держит глобальную блокировку, зависать нельзя
                if time.monotonic() - t_prog > stall or time.monotonic() - t_all > timeout:
                    raise TimeoutError(f'нет прогресса {time.monotonic() - t_prog:.0f} с в фазе {pi}: {len(ok_set)}/{len(need)} full')
            done += len(need)
            srv.cmd(f'execute in minecraft:{dim} run forceload remove all')
            log(f'фаза {pi + 1}/{len(phases)}: {done}/{len(area)} чанков full, {time.monotonic() - t_gen:.0f} с')
        man['generate_s'] = round(time.monotonic() - t_gen, 1)
        srv.cmd('save-all flush'); time.sleep(3)
        srv.stop()
    # --- итог ---
    st = statuses_of(rdir, area)
    full = sum(1 for s in st.values() if s == 'minecraft:full')
    man['chunks_full'] = full
    man['status_hist'] = {}
    for s in st.values():
        man['status_hist'][str(s)] = man['status_hist'].get(str(s), 0) + 1
    allst = anvil.World(f'{wd}/world', dim, version).statuses()
    man['chunks_world_total'] = len(allst)
    man['total_s'] = round(time.monotonic() - t_lock, 1)       # без ожидания глобальной блокировки
    man['lock_wait_s'] = round(t_lock - t_all, 1)
    man['region_bytes'] = sum(os.path.getsize(p) for p in anvil.World(f'{wd}/world', dim, version).region_files().values())
    man['ok'] = full == len(area)
    man['finished'] = time.strftime('%Y-%m-%d %H:%M:%S')
    json.dump(man, open(man_path, 'w'), indent=1, ensure_ascii=False)
    log(f'готово: {full}/{len(area)} full; сервер {man["server_start_s"]} с, генерация {man["generate_s"]} с, всего {man["total_s"]} с; {wd}')
    return man


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='26.3', choices=VERSIONS)
    ap.add_argument('--variant', default='raw')
    ap.add_argument('--dim', default='overworld', choices=sorted(DIMS))
    ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--cx', type=int, default=0, help='центр области, чанки')
    ap.add_argument('--cz', type=int, default=0)
    ap.add_argument('--radius', type=int, default=10, help='радиус в чанках: область (2R+1)^2')
    ap.add_argument('--xmx', default='5g')
    ap.add_argument('--batch', type=int, default=512, help='чанков за фазу forceload')
    ap.add_argument('--tick', default='freeze', choices=['freeze', 'normal'], help='freeze: /tick freeze (никаких случайных тиков и потоков жидкости)')
    ap.add_argument('--timeout', type=int, default=7200)
    ap.add_argument('--force', action='store_true', help='пересоздать, даже если manifest.json готов')
    ap.add_argument('--tag', default='', help='суффикс имени каталога (повторные прогоны для проверки детерминизма)')
    a = ap.parse_args()
    m = generate(a.version, a.variant, a.dim, a.seed, a.cx, a.cz, a.radius, a.xmx, a.batch, a.force, a.tick, a.timeout, tag=a.tag)
    sys.exit(0 if m.get('ok') else 1)


if __name__ == '__main__':
    main()

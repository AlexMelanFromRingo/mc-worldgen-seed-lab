#!/usr/bin/env python3
"""L3: генерация настоящих чанков ванильным сервером (26.3) через /forceload и ожидание записи в region-файлы.

  tools/l3_gen.py --seed 12345 --radius 24 [--cx 0 --cz 0]     # квадрат (2r+1)^2 чанков вокруг (cx,cz)

Каталог мира: run/server-26.3/w<seed>/ (Overworld: region/). Перед запуском создайте run/server-26.3/ с server.jar и eula.txt (`eula=true` — ваше согласие с Minecraft EULA).
"""
import argparse, os, subprocess, sys, time, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRV = f'{ROOT}/run/server-26.3'


def present(world, dim_dir, cx, cz):
    rx, rz = cx >> 5, cz >> 5
    p = f'{world}/dimensions/minecraft/{dim_dir}/region/r.{rx}.{rz}.mca'
    if not os.path.exists(p):
        return False
    with open(p, 'rb') as f:
        idx = (cx & 31) + (cz & 31) * 32
        f.seek(idx * 4)
        b = f.read(4)
    return len(b) == 4 and b != b'\0\0\0\0'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--radius', type=int, default=16)
    ap.add_argument('--cx', type=int, default=0)
    ap.add_argument('--cz', type=int, default=0)
    ap.add_argument('--dim', default='overworld')  # overworld | the_nether | the_end
    ap.add_argument('--timeout', type=int, default=3600)
    a = ap.parse_args()
    name = f'w{a.seed}'
    props = open(f'{SRV}/server.properties').read().splitlines()
    kv = dict(l.split('=', 1) for l in props if '=' in l and not l.startswith('#'))
    kv['level-seed'] = str(a.seed); kv['level-name'] = name; kv['pause-when-empty-seconds'] = '-1'
    open(f'{SRV}/server.properties', 'w').write('\n'.join(f'{k}={v}' for k, v in kv.items()) + '\n')
    world = f'{SRV}/{name}'
    p = subprocess.Popen(['java', '-Xmx8g', '-jar', 'server.jar', 'nogui'], cwd=SRV, stdin=subprocess.PIPE,
                         stdout=open(f'{SRV}/gen-{name}.log', 'w'), stderr=subprocess.STDOUT, text=True, bufsize=1)

    def cmd(s):
        p.stdin.write(s + '\n'); p.stdin.flush()
    # ждём готовности сервера (команды до 'Done' дают «unexpected error»)
    logp = f'{SRV}/gen-{name}.log'; t_wait = time.time()
    while time.time() - t_wait < 180:
        time.sleep(2)
        try:
            if 'Done (' in open(logp, errors='ignore').read(): break
        except Exception: pass
    time.sleep(2)
    x0, x1 = a.cx - a.radius, a.cx + a.radius
    z0, z1 = a.cz - a.radius, a.cz + a.radius
    t_start = time.time(); done = 0; total = (x1 - x0 + 1) * (z1 - z0 + 1)
    dimsel = 'minecraft:' + a.dim
    for ax in range(x0, x1 + 1, 16):
        for az in range(z0, z1 + 1, 16):
            bx1, bz1 = min(ax + 15, x1), min(az + 15, z1)
            cmd(f'execute in {dimsel} run forceload add {ax * 16} {az * 16} {bx1 * 16 + 15} {bz1 * 16 + 15}')
            need = [(cx, cz) for cx in range(ax, bx1 + 1) for cz in range(az, bz1 + 1)]
            t0 = time.time()
            while True:
                time.sleep(4)
                cmd('save-all flush')
                time.sleep(2)
                if all(present(world, a.dim, cx, cz) for cx, cz in need):
                    break
                if time.time() - t0 > 600 or time.time() - t_start > a.timeout:
                    print('timeout in area', ax, az, file=sys.stderr); break
            done += len(need)
            cmd(f'execute in {dimsel} run forceload remove all')
            print(f'{done}/{total} chunks, {time.time() - t_start:.0f}s', flush=True)
    cmd('save-all flush'); time.sleep(3); cmd('stop')
    try:
        p.wait(timeout=60)
    except Exception:
        p.kill()
    print('done', name)


if __name__ == '__main__':
    main()

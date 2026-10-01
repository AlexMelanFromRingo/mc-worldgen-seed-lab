#!/usr/bin/env python3
"""Генерация настоящих чанков ванильными серверами Java Edition 1.7.10 … 1.21.x (для аудита «глина → алмаз по версиям»).

  tools/l3_gen_old.py --ver 1.12.2 --seeds 101 102 103 ...        # <= 1.13: область спавна (≈ 625 / 529 чанков на seed)
  tools/l3_gen_old.py --ver 1.16.5 --seeds 12345 --radius 40      # >= 1.14: /forceload квадрата (2r+1)^2 чанков + save-all

Каталог сервера: run/oldver/<версия>/ (server.jar, eula.txt — создаётся l3_fetch_server.py --accept-eula),
миры: run/oldver/<версия>/w<seed>/ ; готовность помечается файлом w<seed>/GEN_DONE.  Java выбирается по версии:
<=1.16.5 -> 8, 1.17–1.20.4 -> 17, >=1.20.5 -> 21 (системные JDK из /usr/lib/jvm).
"""
import argparse, os, re, socket, subprocess, sys, time, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l3_anvil as A
import l3_anvil_legacy as L


def vtuple(v):
    p = [int(x) for x in re.findall(r'\d+', v)]
    while len(p) < 3: p.append(0)
    return tuple(p[:3])


def java_for(v):
    t = vtuple(v)
    if t >= (1, 20, 5): j = 'java-21-openjdk-amd64'
    elif t >= (1, 17, 0): j = 'java-17-openjdk-amd64'
    else: j = 'java-8-openjdk-amd64'
    return f'/usr/lib/jvm/{j}/bin/java'


def region_dir(srv, ver, seed):
    w = f'{srv}/w{seed}'
    return f'{w}/dimensions/minecraft/overworld/region' if vtuple(ver) >= (26, 1, 0) else f'{w}/region'


def free_port():
    s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p


def write_props(srv, ver, seed, port):
    t = vtuple(ver)
    kv = {'level-seed': str(seed), 'level-name': f'w{seed}', 'online-mode': 'false', 'spawn-protection': '0',
          'max-tick-time': '-1', 'pause-when-empty-seconds': '-1', 'view-distance': '10', 'spawn-monsters': 'false',
          'spawn-animals': 'false', 'spawn-npcs': 'false', 'allow-nether': 'false', 'server-port': str(port),
          'generate-structures': 'true', 'difficulty': 'peaceful' if t >= (1, 14, 0) else '0', 'max-players': '1',
          'sync-chunk-writes': 'false', 'snooper-enabled': 'false', 'enable-status': 'false', 'motd': 'worldgen-audit'}
    if t < (1, 14, 0): kv['difficulty'] = '0'
    open(f'{srv}/server.properties', 'w').write('\n'.join(f'{k}={v}' for k, v in kv.items()) + '\n')


def present_full(reg, cx, cz):
    """(есть_запись, статус_full)"""
    p = f'{reg}/r.{cx >> 5}.{cz >> 5}.mca'
    if not os.path.exists(p): return False
    with open(p, 'rb') as f:
        f.seek(((cx & 31) + (cz & 31) * 32) * 4); b = f.read(4)
    if len(b) != 4 or b == b'\0\0\0\0': return False
    try:
        nbt = A.read_chunk_nbt(p, cx, cz)
        return L.chunk_status(nbt)[0] if nbt else False
    except Exception:
        return False


def wait_done(logf, proc, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if proc.poll() is not None:
            return False
        try:
            s = open(logf, errors='replace').read()
        except Exception:
            s = ''
        if 'Done (' in s: return True
        time.sleep(1)
    return False


def gen_seed(ver, seed, radius, xmx, timeout):
    srv = f'{ROOT}/run/oldver/{ver}'
    world = f'{srv}/w{seed}'
    if os.path.exists(f'{world}/GEN_DONE'):
        print(f'[{ver}] seed {seed}: уже готово'); return True
    if os.path.exists(world): shutil.rmtree(world)
    t = vtuple(ver)
    write_props(srv, ver, seed, free_port())
    logf = f'{srv}/gen-w{seed}.log'
    cmdline = [java_for(ver), f'-Xmx{xmx}', '-Xms512m', f'-DbundlerRepoDir={srv}', '-jar', 'server.jar', 'nogui']
    p = subprocess.Popen(cmdline, cwd=srv, stdin=subprocess.PIPE, stdout=open(logf, 'w'), stderr=subprocess.STDOUT, text=True, bufsize=1)

    def cmd(s):
        try:
            p.stdin.write(s + '\n'); p.stdin.flush()
        except Exception:
            pass
    t_start = time.time()
    ok = wait_done(logf, p, 900)
    if not ok:
        print(f'[{ver}] seed {seed}: сервер не стартовал (см. {logf})', file=sys.stderr); p.kill(); return False
    print(f'[{ver}] seed {seed}: сервер готов за {time.time() - t_start:.0f}s', flush=True)
    reg = region_dir(srv, ver, seed)
    if t >= (1, 14, 0) and radius > 0:
        save = 'save-all flush' if t >= (1, 16, 0) else 'save-all'
        x0, x1, z0, z1 = -radius, radius, -radius, radius
        total = (x1 - x0 + 1) * (z1 - z0 + 1); done = 0
        for ax in range(x0, x1 + 1, 16):
            for az in range(z0, z1 + 1, 16):
                bx1, bz1 = min(ax + 15, x1), min(az + 15, z1)
                cmd(f'forceload add {ax * 16} {az * 16} {bx1 * 16 + 15} {bz1 * 16 + 15}')
                need = [(cx, cz) for cx in range(ax, bx1 + 1) for cz in range(az, bz1 + 1)]
                t0 = time.time(); ready = set()
                while True:
                    time.sleep(5)
                    cmd(save)
                    time.sleep(2)
                    for c in need:
                        if c not in ready and present_full(reg, *c): ready.add(c)
                    if len(ready) == len(need): break
                    if time.time() - t0 > 900 or time.time() - t_start > timeout or p.poll() is not None:
                        print(f'[{ver}] таймаут в области {ax},{az}: готово {len(ready)}/{len(need)}', file=sys.stderr); break
                done += len(need)
                cmd('forceload remove all')
                print(f'[{ver}] seed {seed}: {done}/{total} чанков, {time.time() - t_start:.0f}s', flush=True)
                if p.poll() is not None: break
    else:
        time.sleep(2); cmd('save-all'); time.sleep(5)
    cmd('save-all'); time.sleep(3); cmd('stop')
    try:
        p.wait(timeout=180)
    except Exception:
        p.kill()
    open(f'{world}/GEN_DONE', 'w').write(str(time.time()))
    print(f'[{ver}] seed {seed}: готово за {time.time() - t_start:.0f}s', flush=True)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ver', required=True)
    ap.add_argument('--seeds', type=int, nargs='+', required=True)
    ap.add_argument('--radius', type=int, default=40)
    ap.add_argument('--xmx', default='3g')
    ap.add_argument('--timeout', type=int, default=10800)
    a = ap.parse_args()
    for s in a.seeds:
        gen_seed(a.ver, s, a.radius, a.xmx, a.timeout)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Пресеты и поля роутера против настоящих классов игры (эталон libmcgen/tests/java, без сервера):

 1) поля NoiseRouter (rf:*) и Aquifer.Config (aq:*, 26.3+) каждой noise_settings — бит-в-бит в случайных точках
    (дополняет G1: oracle `df` знает только зарегистрированные функции и мировые пресеты);
 2) заполнение шумом чанков («сырое», до пост-обработки жидкостей) для КАЖДОЙ noise_settings, включая caves и
    floating_islands, у которых нет мирового пресета (их нельзя получить с сервера без своего датапака).

    python3 libmcgen/tests/presets_check.py --version 26.3 [--points 20000] [--chunks 32]
"""
import argparse, base64, os, random, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')
REF = os.path.join(HERE, 'java', 'run.sh')

# пресет libmcgen → (измерение, noise_settings)
PRESETS = [('minecraft:overworld', 'normal', 'minecraft:overworld'), ('minecraft:overworld', 'large_biomes', 'minecraft:large_biomes'),
           ('minecraft:overworld', 'amplified', 'minecraft:amplified'), ('minecraft:overworld', 'caves', 'minecraft:caves'),
           ('minecraft:overworld', 'floating_islands', 'minecraft:floating_islands'), ('minecraft:the_nether', 'normal', 'minecraft:nether'),
           ('minecraft:the_end', 'normal', 'minecraft:end')]
RF_NEW = ['temperature', 'vegetation', 'continents', 'erosion', 'depth', 'ridges', 'chunk_surface_level', 'final_density']
AQ_NEW = ['barrier', 'fluid_level_floodedness', 'fluid_level_spread', 'lava', 'exclusion', 'surface_level']
RF_OLD = ['barrier', 'fluid_level_floodedness', 'fluid_level_spread', 'lava', 'temperature', 'vegetation', 'continents', 'erosion', 'depth',
          'ridges', 'preliminary_surface_level', 'final_density', 'vein_toggle', 'vein_ridged', 'vein_gap']
DIMY = {'minecraft:overworld': (-64, 384), 'minecraft:the_nether': (0, 256), 'minecraft:the_end': (0, 256)}


class Ref:
    def __init__(self, v):
        self.p = subprocess.Popen([REF, v], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def cmd(self, s):
        self.p.stdin.write(s + '\n'); self.p.stdin.flush()
        r = self.p.stdout.readline().strip()
        if not r.startswith('ok'):
            raise RuntimeError(r[:300])
        return r[3:]

    def close(self):
        try:
            self.p.stdin.write('quit\n'); self.p.stdin.flush(); self.p.wait(10)
        except Exception:
            self.p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--points', type=int, default=20000); ap.add_argument('--chunks', type=int, default=32)
    ap.add_argument('--seeds', nargs='+', type=int, default=[12345, -4172144997902289642])
    a = ap.parse_args()
    newf = not (a.version.startswith('26.1') or a.version.startswith('26.2'))
    pack = os.path.join(ROOT, 'run', f'pack-{a.version}')
    ref = Ref(a.version)
    rng = random.Random(5)
    t0 = time.time()
    tot_pts = bad_pts = tot_blk = bad_blk = tot_ch = 0
    try:
        for dim, preset, ns in PRESETS:
            lo, h = DIMY[dim]
            fields = ['rf:' + f for f in (RF_NEW if newf else RF_OLD)]
            if newf and preset in ('normal', 'large_biomes', 'amplified') and dim == 'minecraft:overworld':
                fields += ['aq:' + f for f in AQ_NEW]
            for seed in a.seeds:
                # 1) поля роутера
                for f in fields:
                    pts = []
                    for _ in range(a.points // len(a.seeds)):
                        R = 20000 if rng.random() < 0.8 else 2000000
                        pts.append((rng.randint(-R, R), rng.randint(lo - 16, lo + h + 16), rng.randint(-R, R)))
                    ob = []
                    for i in range(0, len(pts), 2000):
                        ob += ref.cmd(f'df {ns} {seed} {f} ' + ' '.join(f'{x} {y} {z}' for x, y, z in pts[i:i + 2000])).split()
                    r = subprocess.run([CLI, 'df', '--pack', pack, '--version', a.version, '--dim', dim, '--preset', preset, '--seed', str(seed), '--id', f],
                                       input='\n'.join(f'{x} {y} {z}' for x, y, z in pts) + '\n', capture_output=True, text=True)
                    if r.returncode:
                        print(f'  {preset} {f}: ошибка {r.stderr.strip()}'); bad_pts += len(pts); tot_pts += len(pts); continue
                    mb = [l.split()[1].lstrip('0') or '0' for l in r.stdout.splitlines()]
                    ob = [x.lstrip('0') or '0' for x in ob]
                    bad = [i for i in range(len(pts)) if ob[i] != mb[i]]
                    tot_pts += len(pts); bad_pts += len(bad)
                    if bad:
                        i = bad[0]; print(f'  {dim} {preset} seed {seed} {f}: {len(bad)}/{len(pts)} расхождений, пример {pts[i]} эталон {ob[i]} наше {mb[i]}', flush=True)
                # 2) заполнение чанков
                for k in range(a.chunks // len(a.seeds)):
                    R = 300 if k % 2 == 0 else 60000
                    cx, cz = rng.randint(-R, R), rng.randint(-R, R)
                    o = base64.b64decode(ref.cmd(f'fill {ns} {seed} {cx} {cz} {lo} {h}'))
                    r = subprocess.run([CLI, 'fillraw', '--pack', pack, '--version', a.version, '--dim', dim, '--preset', preset, '--seed', str(seed),
                                        '--cx0', str(cx), '--cz0', str(cz)], capture_output=True)
                    if r.returncode:
                        print(f'  {preset} fill: ошибка {r.stderr.decode()[-200:]}'); continue
                    ob = struct.unpack(f'<{len(o) // 2}H', o); mb = struct.unpack(f'<{len(r.stdout) // 2}H', r.stdout)
                    nb = sum(1 for x, y in zip(ob, mb) if x != y) + abs(len(ob) - len(mb))
                    tot_blk += len(ob); bad_blk += nb; tot_ch += 1
                    if nb:
                        i = next(i for i in range(min(len(ob), len(mb))) if ob[i] != mb[i])
                        print(f'  {dim} {preset} seed {seed} чанк ({cx},{cz}): {nb} блоков, первый y={lo + i // 256} z={(i // 16) % 16} x={i % 16} '
                              f'эталон {ob[i]} наше {mb[i]}', flush=True)
            print(f'{dim:22s} {preset:17s} ns={ns}: готово ({time.time() - t0:.0f} с)', flush=True)
    finally:
        ref.close()
    print(f'\nИТОГО {a.version}: поля роутера — точек {tot_pts}, расхождений {bad_pts}; заполнение — чанков {tot_ch}, блоков {tot_blk}, '
          f'расхождений {bad_blk}; {time.time() - t0:.0f} с')
    return 0 if bad_pts == 0 and bad_blk == 0 else 1


if __name__ == '__main__':
    sys.exit(main())

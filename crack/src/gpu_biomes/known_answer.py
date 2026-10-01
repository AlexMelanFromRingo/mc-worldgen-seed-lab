#!/usr/bin/env python3
"""Тест (b): известный ответ. Берём реальный seed, спрашиваем у oracle (настоящий код Mojang) биомы в K точках,
строим файл наблюдений и проверяем, что crack-biomes находит исходный seed среди 2^16 кандидатов
(`--structure-seed`) — и считаем ложные срабатывания.

Для Nether/End верхние 16 бит seed не влияют на биомы (нужны только младшие 48), поэтому вместо 2^16 кандидатов
перебирается 2^N случайных 48-битных seed'ов (+ истинный) через --candidates.

  known_answer.py [--versions 26.1 26.2 26.3] [--seeds 3] [--k 4 8 12 16] [--radius 5000] [--rng 1] [--dims overworld:normal ...]
"""
import argparse, json, os, random, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import shlex
BIN = os.environ.get('CRACK_BIOMES', f'{ROOT}/crack/bin/crack-biomes')    # можно задать "flock /tmp/gpu.lock <путь>" (GPU общая)
SCRATCH = os.path.join(tempfile.gettempdir(), 'ka')


class Oracle:
    def __init__(self, version):
        self.p = subprocess.Popen([f'{ROOT}/oracle/run.sh', version, 'serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def call(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        while True:
            out = self.p.stdout.readline()
            if not out:
                raise RuntimeError('oracle died')
            if out.startswith('{'):
                return json.loads(out)

    def biome(self, dim, seed, qx, qy, qz, preset):
        r = self.call(f'biome {dim} {seed} {qx} {qz} 1 1 {qy}' + (f' --preset {preset}' if dim == 'overworld' and preset != 'normal' else ''))
        if not r.get('ok'):
            raise RuntimeError(r)
        return r['palette'][r['idx'][0]]

    def blockbiome(self, dim, seed, x, y, z, preset):
        r = self.call(f'blockbiome {dim} {seed} {x} {y} {z}' + (f' --preset {preset}' if dim == 'overworld' and preset != 'normal' else ''))
        if not r.get('ok'):
            raise RuntimeError(r)
        return r['biome']

    def close(self):
        try:
            self.p.stdin.close(); self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def run_crack(ver, dim, preset, mode_args, obsfile, extra=()):
    cmd = shlex.split(BIN) + ['--version', ver, '--dim', dim, '--preset', preset] + mode_args + ['--obs', obsfile, '-v'] + list(extra)
    t = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, MCGEN_ROOT=ROOT))
    dt = time.time() - t
    found = [int(x) for x in r.stdout.split()]
    return found, r.stderr, dt, r.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--versions', nargs='*', default=['26.1', '26.2', '26.3'])
    ap.add_argument('--dims', nargs='*', default=['overworld:normal', 'overworld:large_biomes', 'overworld:amplified', 'nether:-', 'end:-'])
    ap.add_argument('--seeds', type=int, default=3)
    ap.add_argument('--k', nargs='*', type=int, default=[4, 8, 12, 16])
    ap.add_argument('--radius', type=int, default=5000, help='радиус выбора точек в квартах')
    ap.add_argument('--rng', type=int, default=1)
    ap.add_argument('--ncand', type=int, default=1 << 22, help='для nether/end: число случайных кандидатов')
    ap.add_argument('--blocks', action='store_true', help='наблюдения БЛОКОВЫЕ (oracle blockbiome, как F3); crack-biomes --blocks')
    a = ap.parse_args()
    os.makedirs(SCRATCH, exist_ok=True)
    rnd = random.Random(a.rng)
    seeds = [rnd.randint(-2**63, 2**63 - 1) for _ in range(a.seeds)]
    total = fails = 0
    print(f'{"версия":6} {"измерение":22} {"seed":>22} {"k":>3} {"найден":>7} {"всего прошло":>12} {"ложных":>7}  {"GPU мс":>8}  ожид. ложных')
    for ver in a.versions:
        orc = Oracle(ver)
        for dd in a.dims:
            dim, preset = dd.split(':')
            for seed in seeds:
                # 16 точек: случайные в радиусе, qy как у игрока на поверхности (Overworld: qy=16 => y=64)
                pts = []
                R = a.radius if dim != 'end' else 4000       # End: внешние острова начинаются с >1024 блоков (256 кварт)
                for i in range(max(a.k)):
                    qx, qz = rnd.randint(-R, R), rnd.randint(-R, R)
                    if dim == 'end':
                        while qx * qx + qz * qz < 400 * 400:    # вне центрального острова (там всё the_end)
                            qx, qz = rnd.randint(-R, R), rnd.randint(-R, R)
                    qy = 16 if dim in ('overworld', 'end') else rnd.randint(2, 30)
                    if a.blocks:
                        bx, bz = qx * 4 + rnd.randint(0, 3), qz * 4 + rnd.randint(0, 3); by = qy * 4 + rnd.randint(0, 3)
                        pts.append((bx, by, bz, orc.blockbiome(dim, seed, bx, by, bz, preset)))
                    else:
                        pts.append((qx, qy, qz, orc.biome(dim, seed, qx, qy, qz, preset)))
                for k in a.k:
                    of = f'{SCRATCH}/obs_{ver}_{dim}_{preset}_{seed & 0xffff}_{k}{"_b" if a.blocks else ""}.txt'
                    with open(of, 'w') as f:
                        f.write(f'# seed={seed} {ver} {dim} {preset} (oracle)\n')
                        for (qx, qy, qz, b) in pts[:k]:
                            f.write(f'{qx} {qy} {qz} {b}\n')
                    s48 = seed & ((1 << 48) - 1)
                    if dim == 'overworld' or a.blocks:     # в режиме блоков hashed seed зависит от всех 64 бит => перебор hi осмыслен и для Nether/End
                        found, err, dt, rc = run_crack(ver, dim, preset, ['--structure-seed', str(s48)], of, ['--blocks'] if a.blocks else [])
                        ok = seed in found
                    else:
                        cf = f'{SCRATCH}/cand_{ver}_{dim}_{seed & 0xffff}.txt'
                        cf = cf + ('_b' if a.blocks else '')
                        if not os.path.exists(cf + f'.{a.ncand}'):
                            rr = random.Random(seed)
                            with open(cf + f'.{a.ncand}', 'w') as f:
                                f.write(f'{seed if a.blocks else s48}\n')
                                for _ in range(a.ncand):
                                    f.write(f'{rr.getrandbits(48)}\n')
                        found, err, dt, rc = run_crack(ver, dim, preset, ['--candidates', cf + f'.{a.ncand}'], of, ['--blocks'] if a.blocks else [])
                        ok = (seed if a.blocks else s48) in found
                    # мс GPU и ожидаемое число ложных — из stderr
                    ms = exp = '?'
                    for line in err.splitlines():
                        if 'ожидание ложных' in line:
                            exp = line.split('≈')[1].split(';')[0].strip()
                            ms = line.split('GPU')[1].split('мс')[0].strip()
                            tot = line.split('кандидатов')[1].split(',')[0].strip()
                    print(f'{ver:6} {dim + "/" + preset:22} {seed:22d} {k:3d} {"ДА" if ok else "НЕТ":>7} {len(found):12d} {len(found) - (1 if ok else 0):7d}  {ms:>8}  {exp}', flush=True)
                    total += 1; fails += (not ok)
                    if not ok:
                        print('   ПРОПУСК: ', err.strip().splitlines()[-3:], flush=True)
        orc.close()
    print(f'ИТОГО: {total} запусков, истинный seed не найден в {fails}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

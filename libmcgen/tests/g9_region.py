#!/usr/bin/env python3
"""g9_region.py — ворота G9 (часть 2б): регион на GPU ≡ регион на CPU побитно (дампы MCR1 mcgen-cli).

    python3 libmcgen/tests/g9_region.py [--cli build/mcgen-cli] [--lib build/gpu/libmcgen_cuda.so] [--run run]
        [--versions 26.3,26.4-snapshot-2] [--seeds 3] [--size 16] [--stages 0x3,0xf] [--dims overworld,the_nether,the_end]

Для каждой версии × измерения × seed × маске стадий генерирует область size×size чанков дважды: MCGEN_COMPUTE=cpu и
MCGEN_COMPUTE=gpu (рельеф на видеокарте), сравнивает sha256 дампов. Версии 26.1/26.2 рельеф на GPU не считают (проверяется,
что дамп всё равно совпадает: путь возврата на CPU).  Код возврата 0 — все пары идентичны. Замеры идут под flock /tmp/gpu.lock."""
import argparse, hashlib, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', default=os.path.join(ROOT, 'libmcgen/build/mcgen-cli'))
    ap.add_argument('--lib', default=os.path.join(ROOT, 'libmcgen/build/gpu/libmcgen_cuda.so'))
    ap.add_argument('--run', default=os.path.join(ROOT, 'run'))
    ap.add_argument('--versions', default='26.3,26.4-snapshot-2')
    ap.add_argument('--seeds', type=int, default=3)
    ap.add_argument('--size', type=int, default=16)
    ap.add_argument('--stages', default='0x3')
    ap.add_argument('--dims', default='overworld,the_nether,the_end')
    ap.add_argument('--presets', default='normal')
    ap.add_argument('--threads', default='0')
    a = ap.parse_args()
    seeds = [12345, -4172144997902289642, 987654321987, 0, 7][:a.seeds]
    pos = [(-8, -8), (320, -777), (-1500, 1200)]
    tmp = tempfile.mkdtemp(prefix='g9reg_')
    total = bad = 0
    for ver in a.versions.split(','):
        pack = os.path.join(a.run, 'pack-' + ver)
        for dim in a.dims.split(','):
            for preset in a.presets.split(','):
                for si, seed in enumerate(seeds):
                    for st in a.stages.split(','):
                        cx0, cz0 = pos[si % len(pos)]
                        out = {}
                        times = {}
                        for mode in ('cpu', 'gpu'):
                            f = os.path.join(tmp, mode + '.mcr')
                            env = dict(os.environ, MCGEN_COMPUTE=mode, MCGEN_CUDA_LIB=a.lib)
                            cmd = ['flock', '/tmp/gpu.lock', a.cli, '--pack', pack, '--version', ver, '--dim', 'minecraft:' + dim, '--preset', preset,
                                   '--seed', str(seed), '--cx0', str(cx0), '--cz0', str(cz0), '--nx', str(a.size), '--nz', str(a.size),
                                   '--stages', st, '--threads', a.threads, '--out', f]
                            t0 = time.time()
                            r = subprocess.run(cmd, env=env, capture_output=True, text=True)
                            times[mode] = time.time() - t0
                            if r.returncode:
                                print('ОШИБКА', ver, dim, preset, seed, st, mode, r.stderr[-300:])
                                out[mode] = None
                                continue
                            out[mode] = sha(f)
                            os.remove(f)
                        total += 1
                        ok = out['cpu'] is not None and out['cpu'] == out['gpu']
                        bad += 0 if ok else 1
                        print('%-16s %-10s %-9s seed %-20d стадии %-5s %s  CPU %.1f с, GPU %.1f с' % (ver, dim, preset, seed, st, 'ИДЕНТИЧНО' if ok else 'РАЗЛИЧАЕТСЯ', times['cpu'], times['gpu']), flush=True)
    print('ИТОГО: пар %d, различающихся %d' % (total, bad))
    return 1 if bad or not total else 0


if __name__ == '__main__':
    sys.exit(main())

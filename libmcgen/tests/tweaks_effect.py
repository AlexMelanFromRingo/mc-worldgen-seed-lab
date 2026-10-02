#!/usr/bin/env python3
"""Тонкие настройки: (1) значения по умолчанию, переданные явно, дают побитово тот же регион, что и без них;
(2) каждое не-умолчательное значение стадий biomes/terrain меняет результат (число отличающихся блоков и клеток биомов).

    python3 libmcgen/tests/tweaks_effect.py [--version 26.3] [--dim minecraft:overworld] [--n 8]
"""
import argparse, json, os, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gt'))
import mcr  # noqa: E402
CLI = os.path.join(ROOT, 'libmcgen', 'build', 'mcgen-cli')

# не-умолчательные значения для проверки (id -> значение)
PROBE = {'sea_level_offset': 10, 'terrain_amplitude': 1.5, 'terrain_steepness': 2.0, 'climate_scale_xz': 2.0, 'climate_scale_y': 2.0,
         'cave_density': 0.5, 'cave_size': 2.0, 'lava_level_offset': 40, 'aquifers': 0, 'fluid_flow': 0, 'ore_veins': 0}


def gen(a, tmp, tweaks):
    out = os.path.join(tmp, 'r.mcr')
    cmd = [CLI, '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version, '--dim', a.dim, '--seed', str(a.seed),
           '--cx0', str(a.cx0), '--cz0', str(a.cz0), '--nx', str(a.n), '--nz', str(a.n), '--stages', '0x3', '--out', out]
    for k, v in tweaks.items():
        cmd += ['--tweak', f'{k}={v}']
    subprocess.run(cmd, check=True, capture_output=True)
    m = mcr.Mcr(out)
    return (np.stack([m.blocks(cx, cz) for cx, cz in m.chunks()]), np.stack([m.biomes(cx, cz) for cx, cz in m.chunks()]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='26.3'); ap.add_argument('--dim', default='minecraft:overworld'); ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--cx0', type=int, default=-4); ap.add_argument('--cz0', type=int, default=-4); ap.add_argument('--n', type=int, default=8)
    a = ap.parse_args()
    info = subprocess.run([CLI, 'info', '--pack', os.path.join(ROOT, 'run', f'pack-{a.version}'), '--version', a.version], capture_output=True, text=True).stdout
    defaults = {l.split()[1]: l.split()[3] for l in info.splitlines() if l.startswith('tweak ')}
    with tempfile.TemporaryDirectory() as tmp:
        b0, m0 = gen(a, tmp, {})
        bd, md = gen(a, tmp, defaults)
        same = np.array_equal(b0, bd) and np.array_equal(m0, md)
        print(f'{a.version} {a.dim} seed {a.seed}, {a.n}x{a.n} чанков ({b0.size:,} блоков): все {len(defaults)} настроек = умолчание явно -> '
              f'{"побитово то же" if same else "ОТЛИЧАЕТСЯ"}')
        bad = 0 if same else 1
        for k, v in PROBE.items():
            if k not in defaults:
                continue
            b, m = gen(a, tmp, {k: v})
            db, dm = int((b != b0).sum()), int((m != m0).sum())
            print(f'  {k:18s} = {v:<5}: блоков отличается {db:>9,} ({100.0 * db / b0.size:6.3f} %), клеток биомов {dm:,}')
    return bad


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Заглушка mcgen-cli для самопроверки run_gate.py/diff.py ДО появления libmcgen (НЕ генератор мира).

Принимает те же аргументы региона, что и libmcgen/cli/mcgen-cli.c, но «генерирует» дамп, копируя чанки из эталонного мира:
  GT_FAKE_SRC=<каталог запуска или мира>   откуда копировать (обязательно; для seed/измерения не проверяется)
  GT_FAKE_PERTURB=N                        исказить N случайных блоков в каждом чанке (проверка, что diff их находит)
  GT_FAKE_SEED_SHIFT=1                     при желании сдвинуть область на 1 чанк по X (грубое расхождение)
"""
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anvil, mcr, diff


def main():
    a = sys.argv[1:]
    kv = {a[i]: a[i + 1] for i in range(0, len(a) - 1, 2) if a[i].startswith('--')}
    ver, dim = kv.get('--version', '26.3'), kv['--dim'].split(':')[-1]
    cx0, cz0, nx, nz = int(kv['--cx0']), int(kv['--cz0']), int(kv['--nx']), int(kv['--nz'])
    w = anvil.World(diff.find_world(os.environ['GT_FAKE_SRC']), dim, ver)
    shift = int(os.environ.get('GT_FAKE_SEED_SHIFT', '0'))
    pert = int(os.environ.get('GT_FAKE_PERTURB', '0'))
    rng = np.random.default_rng(7)
    names = [w.states.names.get(i, '') for i in range(w.states.count)]
    first = w.chunk(cx0, cz0)

    def fn(cx, cz):
        c = w.chunk(cx + shift, cz)
        b = c.blocks.copy()
        for _ in range(pert):
            b[rng.integers(0, b.shape[0]), rng.integers(0, 16), rng.integers(0, 16)] = (w.states.air + 1) % w.states.count
        return b, c.biomes, c.heightmaps
    mcr.write_mcr(kv['--out'], cx0, cz0, nx, nz, first.min_y, first.blocks.shape[0], int(kv.get('--stages', '0'), 0), fn, names, w.biomes.names)


if __name__ == '__main__':
    main()

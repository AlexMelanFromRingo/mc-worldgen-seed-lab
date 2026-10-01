#!/usr/bin/env python3
"""Модель механизма «глина -> алмаз» до 1.17.1 включительно: у каждой фичи собственный java.util.Random с seed = decorationSeed + index + 10000*step.
Для двух фич с разностью индексов d их первые два nextInt(16) (x и z внутри чанка у SQUARE-размещения) связаны детерминированно:
state1 = A*state0 + C (mod 2^48), x = state1 >> 44; разность состояний двух фич после k шагов = A^k * (разность начальных seed'ов) (mod 2^48).
Скрипт печатает для d = 1..N распределение (dx, dz) = (x_{i+d} - x_i, z_{i+d} - z_i) mod 16 на случайных decorationSeed.

  tools/l3_lcg_model.py [N=40] [число_выборок=200000]
"""
import sys
import numpy as np

A = 0x5DEECE66D; C = 0xB; M48 = (1 << 48) - 1


def lcg_first_two(seeds, bound=16):
    """seeds: uint64[] -> (x,z) = первые два nextInt(16) у new Random(seed)"""
    s = (seeds ^ np.uint64(A)) & np.uint64(M48)
    s1 = (s * np.uint64(A) + np.uint64(C)) & np.uint64(M48)
    s2 = (s1 * np.uint64(A) + np.uint64(C)) & np.uint64(M48)
    return (s1 >> np.uint64(44)).astype(np.int64), (s2 >> np.uint64(44)).astype(np.int64)


def draws(seeds, n=6):
    """первые n значений nextInt(16)"""
    s = (seeds ^ np.uint64(A)) & np.uint64(M48)
    out = []
    for _ in range(n):
        s = (s * np.uint64(A) + np.uint64(C)) & np.uint64(M48)
        out.append((s >> np.uint64(44)).astype(np.int64))
    return out


def diff_table(d, n, rng, step_off=0):
    dec = rng.integers(0, 1 << 63, size=n, dtype=np.uint64) * np.uint64(2) + rng.integers(0, 2, size=n, dtype=np.uint64)
    i = rng.integers(0, 60, size=n).astype(np.uint64)
    x0, z0 = lcg_first_two(dec + i + np.uint64(step_off))
    x1, z1 = lcg_first_two(dec + i + np.uint64(d) + np.uint64(step_off))
    T = np.zeros((16, 16), np.int64)
    np.add.at(T, ((x1 - x0) % 16, (z1 - z0) % 16), 1)
    return T


if __name__ == '__main__':
    N = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
    rng = np.random.default_rng(1)
    print('d: топ-пики (dx,dz) mod 16 [вероятность]   ; dx,dz = (x_{i+d}-x_i, z_{i+d}-z_i)')
    for d in range(1, N + 1):
        T = diff_table(d, n, rng)
        flat = np.argsort(T.ravel())[::-1][:4]
        print(f'{d:3d}: ' + '  '.join(f'({k // 16},{k % 16}) {T.ravel()[k] / n:.3f}' for k in flat) + f'   доля в топ-4: {T.ravel()[flat].sum() / n:.2f}')

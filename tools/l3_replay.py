#!/usr/bin/env python3
"""Точное воспроизведение случайных чисел размещения фич по seed мира (java.util.Random, версии 1.13 – 1.17.1).

decorationSeed(chunk) = setDecorationSeed(worldSeed, 16*cx, 16*cz):
    r = Random(worldSeed); a = r.nextLong() | 1; b = r.nextLong() | 1; c = (x*a + z*b) ^ worldSeed
seed фичи = c + index + 10000*step (setFeatureSeed); первые два nextInt(16) — позиция (x, z) внутри чанка для SQUARE-размещения.

Режим «найти индексы»: для чанков, где наблюдается жила алмаза (центр жилы), перебирает (step, index) и считает долю совпадений
наблюдаемого центра с предсказанным (x,z) (допуск 2 блока по кругу mod 16) — правильные (step, index) дают долю >> фона (≈ 10 %).

  tools/l3_replay.py find <версия> [--field bio_center|bio_00] data/l3v/<версия>-w*.npz
"""
import sys, os, argparse, re
import numpy as np

A = 0x5DEECE66D; C = 0xB; M48 = (1 << 48) - 1; M64 = (1 << 64) - 1


def _step(s):
    return (s * np.uint64(A) + np.uint64(C)) & np.uint64(M48)


def next_long_py(seed48):
    """seed48 — внутреннее состояние (уже ^A & mask). -> (long как python int без знака, новое состояние)"""
    s = (seed48 * A + C) & M48; hi = (s >> 16) & 0xFFFFFFFF; hi = hi - (1 << 32) if hi >= (1 << 31) else hi
    s = (s * A + C) & M48; lo = (s >> 16) & 0xFFFFFFFF; lo = lo - (1 << 32) if lo >= (1 << 31) else lo
    return ((hi << 32) + lo) & M64, s


def world_mults(world_seed):
    st = (world_seed ^ A) & M48
    a, st = next_long_py(st); b, st = next_long_py(st)
    return a | 1, b | 1


def decoration_seeds(world_seed, cx, cz):
    """векторно: cx,cz — int64[] координаты чанков -> uint64[] decorationSeed"""
    a, b = world_mults(world_seed)
    x = (cx.astype(np.int64) * 16).view(np.uint64); z = (cz.astype(np.int64) * 16).view(np.uint64)
    c = (x * np.uint64(a) + z * np.uint64(b)) ^ np.uint64(world_seed & M64)
    return c


def feature_draws(dec, index, step, n=2, bound=16):
    """первые n значений nextInt(bound=16) для seed = dec + index + 10000*step -> список int64[]"""
    seed = dec + np.uint64(index + 10000 * step)
    s = (seed ^ np.uint64(A)) & np.uint64(M48)
    out = []
    for _ in range(n):
        s = _step(s)
        out.append((s >> np.uint64(44)).astype(np.int64))
    return out


if __name__ == '__main__':
    # самопроверка на известном значении: Random(0).nextLong() = -4962768465676381896
    st = (0 ^ A) & M48
    v, _ = next_long_py(st)
    v = v - (1 << 64) if v >= (1 << 63) else v
    assert v == -4962768465676381896, v
    print('nextLong самопроверка: ok')

#!/usr/bin/env python3
"""Моделирование «связанной генерации»: для двух фич с seed'ами decSeed+i и decSeed+i+d (WorldgenRandom.setFeatureSeed)
распределение разности позиций первых in_square (x=nextInt(16), z=nextInt(16)) в чанке.
  LCG  (java.util.Random)  — как в WorldgenRandom до 1.17.1
  XORO (Xoroshiro128++, upgrade+Stafford) — как с 1.18 (и в 26.x)
"""
import random, collections, sys
M = 0x5DEECE66D; MASK = (1 << 48) - 1; U64 = (1 << 64) - 1
def lcg_xz(seed):
    st = (seed ^ M) & MASK
    st = (st * M + 0xB) & MASK; x = (16 * (st >> 17)) >> 31
    st = (st * M + 0xB) & MASK; z = (16 * (st >> 17)) >> 31
    return x, z
def mix(z):
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & U64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & U64
    return z ^ (z >> 31)
def rotl(x, k): return ((x << k) | (x >> (64 - k))) & U64
def xoro_xz(seed):
    lo = (seed & U64) ^ 0x6A09E667F3BCC909; hi = (lo + 0x9E3779B97F4A7C15) & U64
    s0, s1 = mix(lo), mix(hi)
    out = []
    for _ in range(2):
        res = (rotl((s0 + s1) & U64, 17) + s0) & U64
        s1 ^= s0; s0 = rotl(s0, 49) ^ s1 ^ ((s1 << 21) & U64); s1 = rotl(s1, 28)
        r = res & 0xFFFFFFFF                      # nextInt(): (int)nextLong()
        m = r * 16; out.append(m >> 32)           # Лемир для bound=16 (остаток 0, без отбраковки)
    return out[0], out[1]
rng = random.Random(1); N = 40000
print('d | LCG: самая частая (Δx,Δz) mod 16 и доля | Xoroshiro: то же')
for d in list(range(1, 25)) + [30, 40, 60, 100]:
    cl = collections.Counter(); cx = collections.Counter()
    for _ in range(N):
        s = rng.getrandbits(62)
        a = lcg_xz(s); b = lcg_xz(s + d); cl[((b[0] - a[0]) % 16, (b[1] - a[1]) % 16)] += 1
        a = xoro_xz(s); b = xoro_xz(s + d); cx[((b[0] - a[0]) % 16, (b[1] - a[1]) % 16)] += 1
    (pl, nl), (px, nx) = cl.most_common(1)[0], cx.most_common(1)[0]
    top2 = ', '.join(f'{k}:{v / N:.0%}' for k, v in cl.most_common(3))
    print(f'{d:3d} | LCG {top2:34s} | XORO {px}:{nx / N:.1%}')

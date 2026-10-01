/*
 * mcrand.h — минимальная host/device реализация того, что нужно для восстановления seed:
 * Java LCG (LegacyRandomSource / SingleThreadedRandomSource), позиции структур random_spread,
 * слайм-чанки, mineshaft (legacy_type_3), End pillars, бедрок Nether (legacy random).
 * Каждая формула сверена с реальным кодом игры 26.1/26.2/26.3 через vectors/gameref-*.txt
 * (см. selftest.c). Идентификаторы совпадают с исходниками Mojang, где это возможно.
 */
#ifndef MCRAND_H
#define MCRAND_H

#include <stdint.h>

#ifdef __CUDACC__
#define HD __host__ __device__ __forceinline__
#else
#define HD static inline
#endif

typedef uint64_t u64;
typedef int64_t  i64;
typedef uint32_t u32;
typedef int32_t  i32;

#define MASK48 0xFFFFFFFFFFFFULL
#define LCG_MUL 0x5DEECE66DULL
#define LCG_ADD 0xBULL

/* ---------- Java LCG ---------- */
HD u64 lcg_step(u64 s) { return (s * LCG_MUL + LCG_ADD) & MASK48; }
HD u64 lcg_scramble(u64 seed) { return (seed ^ LCG_MUL) & MASK48; }          /* setSeed(seed) */
/* next(bits) для bits<=32: возвращает знаковое int как в Java */
HD i32 lcg_next(u64 *s, int bits) { *s = lcg_step(*s); return (i32)(i64)((i64)*s >> (48 - bits)); }

HD i32 lcg_nextInt(u64 *s, i32 bound) {
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * (i64)lcg_next(s, 31)) >> 31);
    i32 sample, modulo;
    do {
        sample = lcg_next(s, 31);
        modulo = sample % bound;
    } while ((i32)((u32)sample - (u32)modulo + (u32)(bound - 1)) < 0);
    return modulo;
}
HD i64 lcg_nextLong(u64 *s) {
    i32 hi = lcg_next(s, 32);
    i32 lo = lcg_next(s, 32);
    return (i64)((u64)(i64)hi << 32) + (i64)lo;
}
HD float lcg_nextFloat(u64 *s) { return (float)lcg_next(s, 24) * 5.9604645E-8f; }
HD double lcg_nextDouble(u64 *s) {
    i64 hi = lcg_next(s, 26);
    i64 lo = lcg_next(s, 27);
    return (double)((hi << 27) + lo) * 1.1102230246251565E-16;
}

/* ---------- random_spread (RandomSpreadStructurePlacement.getPotentialStructureChunk) ---------- */
#define REGION_A 341873128712LL
#define REGION_B 132897987541LL
/* setLargeFeatureWithSalt(seed, x, z, salt): seed + x*A + z*B + salt, затем setSeed */
HD u64 large_feature_state(u64 seed, i32 x, i32 z, i32 salt) {
    u64 v = seed + (u64)((i64)x * REGION_A) + (u64)((i64)z * REGION_B) + (u64)(i64)salt;
    return lcg_scramble(v);
}
/* type: 0 = linear, 1 = triangular. Возвращает смещение внутри региона (offX, offZ), 0..spacing-sep-1 */
HD void spread_offset(u64 seed, i32 rx, i32 rz, i32 spacing, i32 sep, i32 salt, int type, i32 *ox, i32 *oz) {
    u64 s = large_feature_state(seed, rx, rz, salt);
    i32 lim = spacing - sep;
    if (type == 0) { *ox = lcg_nextInt(&s, lim); *oz = lcg_nextInt(&s, lim); }
    else {
        i32 a = lcg_nextInt(&s, lim), b = lcg_nextInt(&s, lim); *ox = (a + b) / 2;
        a = lcg_nextInt(&s, lim); b = lcg_nextInt(&s, lim); *oz = (a + b) / 2;
    }
}

/* ---------- слайм-чанк: WorldgenRandom.seedSlimeChunk(x, z, seed, 987234911).nextInt(10) == 0 ---------- */
HD u64 slime_state(u64 seed, i32 x, i32 z) {
    i32 t1 = (i32)((u32)x * (u32)x * 4987142u);      /* int*int*int, переполнение как в Java */
    i32 t2 = (i32)((u32)x * 5947611u);
    i64 t3 = (i64)(i32)((u32)z * (u32)z) * 4392871LL; /* (z*z) — int, затем *long */
    i32 t4 = (i32)((u32)z * 389711u);
    u64 v = seed + (u64)(i64)t1 + (u64)(i64)t2 + (u64)t3 + (u64)(i64)t4;
    v ^= 987234911ULL;
    return lcg_scramble(v);
}
HD int is_slime_chunk(u64 seed, i32 x, i32 z) { u64 s = slime_state(seed, x, z); return lcg_nextInt(&s, 10) == 0; }

/* ---------- mineshaft: legacy_type_3 (setLargeFeatureSeed + nextDouble < 0.004) ---------- */
HD int mineshaft_start(u64 seed, i32 cx, i32 cz) {
    u64 s = lcg_scramble(seed);
    i64 a = lcg_nextLong(&s), b = lcg_nextLong(&s);
    u64 r = (u64)((i64)cx * a) ^ (u64)((i64)cz * b) ^ seed;
    u64 t = lcg_scramble(r);
    return lcg_nextDouble(&t) < 0.004;
}

/* ---------- End pillars ---------- */
HD u32 pillar_seed_of(u64 seed) { u64 s = lcg_scramble(seed); return (u32)((u64)lcg_nextLong(&s) & 65535ULL); }
HD void pillar_sizes(u32 pseed, int out[10]) {
    for (int i = 0; i < 10; i++) out[i] = i;
    u64 s = lcg_scramble(pseed);
    for (int i = 10; i > 1; i--) { int j = lcg_nextInt(&s, i); int t = out[i - 1]; out[i - 1] = out[j]; out[j] = t; }
}

/* ---------- Mth.getSeed(x,y,z) и бедрок Nether (legacy random) ---------- */
HD i64 mth_getSeed(i32 x, i32 y, i32 z) {
    i64 i = (i64)(i32)((u32)x * 3129871u) ^ ((i64)z * 116129781LL) ^ (i64)y;
    i = (i64)((u64)i * (u64)i * 42317861ULL + (u64)i * 11ULL);
    return i >> 16;
}
/* factorySeed — 64-битное F (LegacyPositionalRandomFactory.seed). Значимы только младшие 48 бит. */
HD float bedrock_float(u64 factorySeed, i32 x, i32 y, i32 z) {
    u64 s = lcg_scramble((u64)mth_getSeed(x, y, z) ^ factorySeed);
    return lcg_nextFloat(&s);
}
/* factory для random_name: base = Random(worldSeed).nextLong(); F = Random(hash ^ base).nextLong() */
HD u64 nether_bedrock_factory(u64 worldSeed, i32 nameHash) {
    u64 s = lcg_scramble(worldSeed);
    u64 base = (u64)lcg_nextLong(&s);
    u64 s2 = lcg_scramble((u64)(i64)nameHash ^ base);
    return (u64)lcg_nextLong(&s2);
}
#define HASH_BEDROCK_ROOF  343340730
#define HASH_BEDROCK_FLOOR 2042456806

#endif

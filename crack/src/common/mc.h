/*
 * mc.h — host/device реализация RNG-части Minecraft 26.x, нужной для восстановления 48-битного structure seed.
 *
 * Все формулы сверены с РЕАЛЬНЫМ кодом игры (oracle/, 26.1 / 26.2 / 26.3), см. crack/tests/.
 * Идентификаторы и ссылки — на src/dec/<V>/net/minecraft/world/level/levelgen/...
 *
 *   LegacyRandomSource / SingleThreadedRandomSource  — java.util.Random (48-бит LCG)
 *   WorldgenRandom.seedSlimeChunk                    — WorldgenRandom.java:71-73 (+ Slime.java: nextInt(10)==0)
 *   RandomSpreadStructurePlacement                   — RandomSpreadStructurePlacement.java:67-76
 *   AbstractSpreadingStructurePlacement reducers     — AbstractSpreadingStructurePlacement.java:101-126
 *   EndSpikeFeature.getSpikesForLevel                — EndSpikeFeature.java:48-52 (+ SpikeCacheLoader, Util.toShuffledList(IntStream))
 *
 * Собирается и nvcc (HD = __host__ __device__), и gcc (HD = static inline).
 */
#ifndef CRACK_MC_H
#define CRACK_MC_H

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

#define MASK48  0xFFFFFFFFFFFFULL
#define LCG_MUL 0x5DEECE66DULL
#define LCG_ADD 0xBULL

/* ---------------- Java LCG ---------------- */
HD u64 lcg_step(u64 s) { return (s * LCG_MUL + LCG_ADD) & MASK48; }
HD u64 lcg_scramble(u64 seed) { return (seed ^ LCG_MUL) & MASK48; }          /* setSeed(seed) */
/* next(bits), bits<=32: знаковое int, как в Java */
HD i32 lcg_next(u64 *s, int bits) { *s = lcg_step(*s); return (i32)(i64)((i64)*s >> (48 - bits)); }

/* BitRandomSource.nextInt(bound), bound>0 (BitRandomSource.java:17-33) */
HD i32 lcg_nextInt(u64 *s, i32 bound) {
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * (i64)lcg_next(s, 31)) >> 31);
    i32 sample, modulo;
    do {
        sample = lcg_next(s, 31);
        modulo = sample % bound;
    } while ((i32)((u32)sample - (u32)modulo + (u32)(bound - 1)) < 0);      /* int-переполнение = отклонение */
    return modulo;
}
HD i64 lcg_nextLong(u64 *s) {                                                  /* (hi<<32) + (int)lo, lo знаково расширяется */
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

/* ---------------- Слайм-чанки ----------------
 * WorldgenRandom.seedSlimeChunk(x,z,seed,salt) = SingleThreadedRandomSource(
 *     seed + x*x*4987142 + x*5947611 + z*z*4392871L + z*389711 ^ salt )          (WorldgenRandom.java:71-73)
 * x*x*4987142, x*5947611, z*z (int!) * 4392871L, z*389711: 32-битное целочисленное переполнение,
 * слагаемые знаково расширяются до long; затем XOR с salt=987234911L (Slime.java:93 / 26.1: Slime.java:291).
 * Слайм-чанк <=> nextInt(10)==0.  Зависит только от seed mod 2^48.
 */
#define SLIME_SALT 987234911ULL
/* c(x,z) = t1+t2+t3+t4 mod 2^48 — слагаемое, не зависящее от seed */
HD u64 slime_cst(i32 x, i32 z) {
    i32 t1 = (i32)((u32)x * (u32)x * 4987142u);
    i32 t2 = (i32)((u32)x * 5947611u);
    i64 t3 = (i64)(i32)((u32)z * (u32)z) * 4392871LL;
    i32 t4 = (i32)((u32)z * 389711u);
    return ((u64)(i64)t1 + (u64)(i64)t2 + (u64)t3 + (u64)(i64)t4) & MASK48;
}
/* начальное состояние LCG: ((W + c) ^ salt) ^ MUL  (mod 2^48) */
#define SLIME_KX (SLIME_SALT ^ LCG_MUL)
HD u64 slime_state(u64 W, u64 cst) { return (((W + cst) & MASK48) ^ SLIME_KX); }
HD int slime_from_cst(u64 W, u64 cst) { u64 s = slime_state(W, cst); return lcg_nextInt(&s, 10) == 0; }
HD int is_slime_chunk(u64 seed, i32 x, i32 z) { return slime_from_cst(seed & MASK48, slime_cst(x, z)); }

/* ---------------- random_spread ----------------
 * getPotentialStructureChunk: setLargeFeatureWithSalt(seed, gx, gz, salt): seed + gx*A + gz*B + salt, затем setSeed
 */
#define REGION_A 341873128712LL
#define REGION_B 132897987541LL

HD i32 floordiv_i32(i32 a, i32 b) { i32 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }

/* type: 0 = linear, 1 = triangular. Смещение внутри региона (RandomSpreadType.evaluate, RandomSpreadType.java:23-28) */
HD void spread_offset(u64 seed, i32 gx, i32 gz, i32 spacing, i32 sep, i32 salt, int type, i32 *ox, i32 *oz) {
    u64 v = seed + (u64)((i64)gx * REGION_A) + (u64)((i64)gz * REGION_B) + (u64)(i64)salt;
    u64 s = lcg_scramble(v);
    i32 lim = spacing - sep;
    if (type == 0) { *ox = lcg_nextInt(&s, lim); *oz = lcg_nextInt(&s, lim); }
    else {
        i32 a = lcg_nextInt(&s, lim), b = lcg_nextInt(&s, lim); *ox = (a + b) / 2;
        a = lcg_nextInt(&s, lim); b = lcg_nextInt(&s, lim); *oz = (a + b) / 2;
    }
}

/* ---------------- frequency reducers (AbstractSpreadingStructurePlacement.java:101-126) ---------------- */
#define RED_NONE  0
#define RED_DEFAULT 1   /* probabilityReducer: setLargeFeatureWithSalt(seed, salt, sx, sz); nextFloat() < p */
#define RED_TYPE1 2     /* legacy_type_1: setSeed((sx>>4 ^ (sz>>4)<<4) ^ seed); nextInt(); nextInt((int)(1/p))==0 */
#define RED_TYPE2 3     /* legacy_type_2: setLargeFeatureWithSalt(seed, sx, sz, 10387320); nextFloat() < p */
#define RED_TYPE3 4     /* legacy_type_3: setLargeFeatureSeed(seed, sx, sz); nextDouble() < (double)p */

/* setLargeFeatureSeed(seed, cx, cz): setSeed(seed); xs=nextLong(); zs=nextLong(); setSeed(cx*xs ^ cz*zs ^ seed) */
HD u64 large_feature_seed_state(u64 W, i32 cx, i32 cz) {
    u64 s = lcg_scramble(W);
    i64 a = lcg_nextLong(&s), b = lcg_nextLong(&s);
    u64 r = (u64)((i64)cx * a) ^ (u64)((i64)cz * b) ^ W;
    return lcg_scramble(r);
}
/* float p; inv1 = (int)(1.0F/p) для type1 */
HD int reducer_pass(u64 W, int red, i32 salt, i32 sx, i32 sz, float p, i32 inv1) {
    if (red == RED_NONE) return 1;
    u64 s;
    switch (red) {
    case RED_DEFAULT:
        s = lcg_scramble(W + (u64)((i64)salt * REGION_A) + (u64)((i64)sx * REGION_B) + (u64)(i64)sz);
        return lcg_nextFloat(&s) < p;
    case RED_TYPE2:
        s = lcg_scramble(W + (u64)((i64)sx * REGION_A) + (u64)((i64)sz * REGION_B) + 10387320ULL);
        return lcg_nextFloat(&s) < p;
    case RED_TYPE3:
        s = large_feature_seed_state(W, sx, sz);
        return lcg_nextDouble(&s) < (double)p;
    case RED_TYPE1: {
        i32 cx = sx >> 4, cz = sz >> 4;
        i32 t = cx ^ (i32)((u32)cz << 4);
        s = lcg_scramble((u64)(i64)t ^ W);
        s = lcg_step(s);                        /* random.nextInt() = next(32): результат не нужен, только шаг состояния */
        return lcg_nextInt(&s, inv1) == 0;
    }
    }
    return 1;
}

/* ---------------- End pillars (EndSpikeFeature.getSpikesForLevel + SpikeCacheLoader) ----------------
 * key = SingleThreadedRandomSource(levelSeed).nextLong() & 65535   (EndSpikeFeature.java:48-52)
 * sizes = Util.toShuffledList(IntStream.range(0,10), SingleThreadedRandomSource(key))   (Util.java:1092-1102)
 * высота башни i = 76 + 3*sizes[i], радиус = 2 + sizes[i]/3, клетка (guarded) = sizes[i] in {1,2}
 */
HD u32 pillar_key_of(u64 W) { u64 s = lcg_scramble(W); return (u32)((u64)lcg_nextLong(&s) & 65535ULL); }
HD void pillar_sizes(u32 key, int out[10]) {
    for (int i = 0; i < 10; i++) out[i] = i;
    u64 s = lcg_scramble((u64)key);
    for (int i = 10; i > 1; i--) { int j = lcg_nextInt(&s, i); int t = out[i - 1]; out[i - 1] = out[j]; out[j] = t; }
}

#endif

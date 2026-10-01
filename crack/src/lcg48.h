/*
 * lcg48.h — java.util.Random / LegacyRandomSource (48-битный LCG) для host и device.
 * Все формулы сверены с реальным кодом игры (WorldgenRandom, LegacyRandomSource) — см. docs/20-crack-struct-lift.md.
 */
#ifndef CRACK_LCG48_H
#define CRACK_LCG48_H

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

#define K_MASK48 0xFFFFFFFFFFFFULL
#define K_MUL    0x5DEECE66DULL
#define K_ADD    0xBULL
#define K_REGION_A 341873128712LL
#define K_REGION_B 132897987541LL

HD u64 j_step(u64 s) { return (s * K_MUL + K_ADD) & K_MASK48; }
HD u64 j_scramble(u64 seed) { return (seed ^ K_MUL) & K_MASK48; }              /* setSeed(seed) */
/* next(bits), bits<=32: знаковое int как в Java */
HD i32 j_next(u64 *s, int bits) { *s = j_step(*s); return (i32)(i64)((i64)*s >> (48 - bits)); }

HD i32 j_nextInt(u64 *s, i32 bound) {
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * (i64)j_next(s, 31)) >> 31);
    i32 sample, modulo;
    do {
        sample = j_next(s, 31);
        modulo = sample % bound;
    } while ((i32)((u32)sample - (u32)modulo + (u32)(bound - 1)) < 0);
    return modulo;
}
HD i64 j_nextLong(u64 *s) {
    i32 hi = j_next(s, 32);
    i32 lo = j_next(s, 32);
    return (i64)((u64)(i64)hi << 32) + (i64)lo;
}
HD float j_nextFloat(u64 *s) { return (float)j_next(s, 24) * 5.9604645E-8f; }
HD double j_nextDouble(u64 *s) {
    i64 hi = j_next(s, 26);
    i64 lo = j_next(s, 27);
    return (double)((hi << 27) + lo) * 1.1102230246251565E-16;
}

/* WorldgenRandom.setLargeFeatureWithSalt(seed, x, z, blend): seed + x*A + z*B + blend; возвращает состояние после setSeed */
HD u64 large_feature_with_salt(u64 seed, i32 x, i32 z, i32 blend) {
    u64 v = seed + (u64)((i64)x * K_REGION_A) + (u64)((i64)z * K_REGION_B) + (u64)(i64)blend;
    return j_scramble(v & K_MASK48);
}
/* WorldgenRandom.setLargeFeatureSeed(seed, cx, cz) */
HD u64 large_feature_seed(u64 seed, i32 cx, i32 cz) {
    u64 s = j_scramble(seed & K_MASK48);
    i64 xs = j_nextLong(&s), zs = j_nextLong(&s);
    u64 r = (u64)((i64)cx * xs) ^ (u64)((i64)cz * zs) ^ seed;
    return j_scramble(r & K_MASK48);
}

#endif

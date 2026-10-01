/* mc_rng.h — генераторы случайных чисел Minecraft (Java Random / LegacyRandomSource и Xoroshiro128++),
 * позиционные фабрики, MD5-хэш имён. Точный порт net.minecraft.world.level.levelgen.* (26.1–26.3; логика та же).
 * Источники: LegacyRandomSource.java, BitRandomSource.java, XoroshiroRandomSource.java,
 *            Xoroshiro128PlusPlus.java, RandomSupport.java, WorldgenRandom.java
 */
#ifndef MC_RNG_H
#define MC_RNG_H
#include "mc_common.h"

/* ---------------- Legacy (java.util.Random-совместимый 48-битный LCG) ---------------- */
typedef struct { u64 s; } McLcg;

#define MC_LCG_MUL 0x5DEECE66DULL
#define MC_LCG_ADD 0xBULL
#define MC_LCG_MASK ((1ULL << 48) - 1)

MC_HD MC_INLINE void lcg_set_seed(McLcg *r, i64 seed) { r->s = ((u64)seed ^ MC_LCG_MUL) & MC_LCG_MASK; }
MC_HD MC_INLINE McLcg lcg_new(i64 seed) { McLcg r; lcg_set_seed(&r, seed); return r; }
/* next(bits): Java возвращает (int)(newSeed >> (48-bits)) — со знаком при bits=32 */
MC_HD MC_INLINE i32 lcg_next(McLcg *r, int bits) {
    r->s = (r->s * MC_LCG_MUL + MC_LCG_ADD) & MC_LCG_MASK;
    return (i32)(i64)((i64)r->s >> (48 - bits));   /* s<2^48 => знак не теряется в i64 */
}
MC_HD MC_INLINE i32 lcg_next_int(McLcg *r) { return lcg_next(r, 32); }
MC_HD MC_INLINE i32 lcg_next_int_bound(McLcg *r, i32 bound) {
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * (i64)lcg_next(r, 31)) >> 31);
    i32 sample, modulo;
    do {
        sample = lcg_next(r, 31);
        modulo = sample % bound;
    } while ((i32)((u32)sample - (u32)modulo + (u32)(bound - 1)) < 0);   /* переполнение int как в Java */
    return modulo;
}
MC_HD MC_INLINE i64 lcg_next_long(McLcg *r) {
    i32 upper = lcg_next(r, 32);
    i32 lower = lcg_next(r, 32);
    return (i64)((u64)(i64)upper << 32) + (i64)lower;
}
MC_HD MC_INLINE double lcg_next_double(McLcg *r) {
    i32 upper = lcg_next(r, 26);
    i32 lower = lcg_next(r, 27);
    i64 combined = ((i64)upper << 27) + lower;
    return (double)combined * 1.1102230246251565e-16;    /* = 2^-53, как (double)1.110223E-16F */
}
MC_HD MC_INLINE float lcg_next_float(McLcg *r) { return (float)lcg_next(r, 24) * 5.9604645E-8f; }
MC_HD MC_INLINE void lcg_skip(McLcg *r, int n) { for (int i = 0; i < n; i++) lcg_next(r, 32); }

/* быстрый скачок LCG на n шагов (для перебора): s' = A^n * s + C_n mod 2^48 */
MC_HD MC_INLINE void lcg_jump_params(u64 n, u64 *mul, u64 *add) {
    u64 m = 1, a = 0, cm = MC_LCG_MUL, ca = MC_LCG_ADD;
    while (n) {
        if (n & 1) { m = (m * cm) & MC_LCG_MASK; a = (a * cm + ca) & MC_LCG_MASK; }
        ca = ((cm + 1) * ca) & MC_LCG_MASK; cm = (cm * cm) & MC_LCG_MASK;
        n >>= 1;
    }
    *mul = m; *add = a;
}

/* ---------------- Xoroshiro128++ ---------------- */
typedef struct { u64 lo, hi; } McXoro;

MC_HD MC_INLINE u64 mc_rotl64(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

MC_HD MC_INLINE u64 mc_mix_stafford13(u64 z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;   /* -4658895280553007687 */
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;   /* -7723592293110705685 */
    return z ^ (z >> 31);
}
#define MC_GOLDEN_RATIO_64 0x9E3779B97F4A7C15ULL
#define MC_SILVER_RATIO_64 0x6A09E667F3BCC909ULL

/* new XoroshiroRandomSource(long seed): upgradeSeedTo128bit = mixStafford13 от (seed ^ SILVER, +GOLDEN) */
MC_HD MC_INLINE McXoro xoro_from_long_seed(i64 seed) {
    u64 lo = (u64)seed ^ MC_SILVER_RATIO_64;
    u64 hi = lo + MC_GOLDEN_RATIO_64;
    McXoro r; r.lo = mc_mix_stafford13(lo); r.hi = mc_mix_stafford13(hi);
    if ((r.lo | r.hi) == 0) { r.lo = MC_GOLDEN_RATIO_64; r.hi = MC_SILVER_RATIO_64; }   /* как в конструкторе Xoroshiro128PlusPlus */
    return r;
}
MC_HD MC_INLINE McXoro xoro_new(u64 lo, u64 hi) {
    McXoro r; r.lo = lo; r.hi = hi;
    if ((lo | hi) == 0) { r.lo = MC_GOLDEN_RATIO_64; r.hi = MC_SILVER_RATIO_64; }
    return r;
}
MC_HD MC_INLINE u64 xoro_next_long(McXoro *r) {
    u64 s0 = r->lo, s1 = r->hi;
    u64 result = mc_rotl64(s0 + s1, 17) + s0;
    s1 ^= s0;
    r->lo = mc_rotl64(s0, 49) ^ s1 ^ (s1 << 21);
    r->hi = mc_rotl64(s1, 28);
    return result;
}
MC_HD MC_INLINE i32 xoro_next_int(McXoro *r) { return (i32)xoro_next_long(r); }
MC_HD MC_INLINE i32 xoro_next_int_bound(McXoro *r, i32 bound) {
    u64 randomBits = (u64)(u32)xoro_next_int(r);
    u64 mult = randomBits * (u64)bound;
    u64 frac = mult & 0xFFFFFFFFULL;
    if (frac < (u64)bound) {
        u32 unbiased = (u32)(0u - (u32)bound) % (u32)bound;    /* Integer.remainderUnsigned(~bound + 1, bound) */
        while (frac < unbiased) {
            randomBits = (u64)(u32)xoro_next_int(r);
            mult = randomBits * (u64)bound;
            frac = mult & 0xFFFFFFFFULL;
        }
    }
    return (i32)(mult >> 32);
}
MC_HD MC_INLINE u64 xoro_next_bits(McXoro *r, int bits) { return xoro_next_long(r) >> (64 - bits); }
MC_HD MC_INLINE double xoro_next_double(McXoro *r) { return (double)xoro_next_bits(r, 53) * 1.1102230246251565e-16; }
MC_HD MC_INLINE float xoro_next_float(McXoro *r) { return (float)xoro_next_bits(r, 24) * 5.9604645E-8f; }
MC_HD MC_INLINE void xoro_skip(McXoro *r, int n) { for (int i = 0; i < n; i++) xoro_next_long(r); }

/* Позиционная фабрика (forkPositional): 2 nextLong */
typedef struct { u64 lo, hi; } McXoroPos;
MC_HD MC_INLINE McXoroPos xoro_fork_positional(McXoro *r) {
    McXoroPos p; p.lo = xoro_next_long(r); p.hi = xoro_next_long(r); return p;
}
/* fromHashOf(name): seed128 = md5(name) (lo=первые 8 байт big-endian, hi=следующие 8) xor (posLo,posHi).
 * Хэши имён вычисляются на хосте (mc_md5_seed128) и кладутся в таблицы. */
MC_HD MC_INLINE McXoro xoro_from_hash(const McXoroPos *p, u64 hashLo, u64 hashHi) {
    return xoro_new(hashLo ^ p->lo, hashHi ^ p->hi);
}
/* at(x,y,z): Mth.getSeed(x,y,z) ^ seedLo, seedHi */
MC_HD MC_INLINE i64 mc_get_seed_xyz(i32 x, i32 y, i32 z) {
    i64 seed = (i64)(i32)((u32)x * 3129871u) ^ ((i64)z * 116129781LL) ^ (i64)y;
    seed = (i64)((u64)seed * (u64)seed * 42317861ULL + (u64)seed * 11ULL);
    return seed >> 16;
}
MC_HD MC_INLINE McXoro xoro_at(const McXoroPos *p, i32 x, i32 y, i32 z) {
    return xoro_new((u64)mc_get_seed_xyz(x, y, z) ^ p->lo, p->hi);
}

/* ---------------- MD5 (только для предвычисления таблиц на хосте) ---------------- */
#ifndef __CUDA_ARCH__
static inline void mc_md5(const u8 *msg, size_t len, u8 out[16]) {
    static const u32 K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const int R[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                              4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    u32 a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    size_t newlen = ((len + 8) / 64 + 1) * 64;
    u8 buf[256];                      /* имён длиннее ~180 байт у нас нет */
    if (newlen > sizeof(buf)) { memset(out, 0, 16); return; }
    memset(buf, 0, newlen);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    u64 bits = (u64)len * 8;
    for (int i = 0; i < 8; i++) buf[newlen - 8 + i] = (u8)(bits >> (8 * i));
    for (size_t off = 0; off < newlen; off += 64) {
        u32 M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (u32)buf[off + i*4] | ((u32)buf[off + i*4+1] << 8) | ((u32)buf[off + i*4+2] << 16) | ((u32)buf[off + i*4+3] << 24);
        u32 A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            u32 F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i + 5) & 15; }
            else             { F = C ^ (B | ~D);       g = (7*i) & 15; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + ((F << R[i]) | (F >> (32 - R[i])));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    u32 v[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i*4+j] = (u8)(v[i] >> (8*j));
}
/* RandomSupport.seedFromHashOf(name): два big-endian long из MD5 (UTF-8, имена ASCII) */
static inline void mc_md5_seed128(const char *name, u64 *lo, u64 *hi) {
    u8 h[16]; mc_md5((const u8 *)name, strlen(name), h);
    u64 a = 0, b = 0;
    for (int i = 0; i < 8; i++) { a = (a << 8) | h[i]; b = (b << 8) | h[8 + i]; }
    *lo = a; *hi = b;
}
/* String.hashCode() (для LegacyPositionalRandomFactory.fromHashOf) */
static inline i32 mc_java_string_hash(const char *s) {
    u32 h = 0; for (; *s; s++) h = 31u * h + (u32)(u8)*s; return (i32)h;
}
#endif

#endif /* MC_RNG_H */

/* mc_common.h — общие макросы host+device (C99 / CUDA).
 *
 * ВАЖНО для бит-точности с Java: fp-операции в Java строго IEEE-754 без слияния (FMA).
 * Поэтому компилируем с  gcc/clang: -ffp-contract=off   nvcc: --fmad=false
 * (и не использовать -ffast-math). float-арифметику 26.3 нельзя повышать до double.
 */
#ifndef MC_COMMON_H
#define MC_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#if defined(__CUDACC__)
#define MC_HD __host__ __device__
#define MC_D  __device__
#define MC_INLINE __forceinline__
#else
#define MC_HD
#define MC_D
#define MC_INLINE static inline
#endif

typedef uint64_t u64;
typedef int64_t  i64;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef int16_t  i16;

/* Версии игры. Нумерация — по порядку выхода, расширяемая. */
enum {
    MC_26_1 = 0,
    MC_26_2 = 1,
    MC_26_3 = 2,
    MC_26_4 = 3,      /* 26.4-snapshot-2 (данные = 26.3, отличаются 2 подземных записи) */
    MC_VERSION_COUNT
};

/* Измерения */
enum { MC_OVERWORLD = 0, MC_NETHER = 1, MC_END = 2 };

/* Java: (int)Math.floor(double) — усечение к -inf. Значения приходят в разумных пределах. */
MC_HD MC_INLINE i32 mc_floor_d(double v) {
    i32 i = (i32)v;                    /* усечение к нулю */
    return (v < (double)i) ? i - 1 : i;
}
MC_HD MC_INLINE i64 mc_lfloor_d(double v) {
    i64 i = (i64)v;
    return (v < (double)i) ? i - 1 : i;
}

#endif /* MC_COMMON_H */

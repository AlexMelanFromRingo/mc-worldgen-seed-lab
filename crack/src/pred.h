/*
 * pred.h — «предикат» над 48-битным structure seed W: одно наблюдение = 1–2 предиката.
 *   PK_LIN      random_spread linear:      nextInt(lim)==ox && nextInt(lim)==oz   (state0 = ((W+cst)&M48)^MUL)
 *   PK_TRI      random_spread triangular:  (nextInt+nextInt)/2==ox && (nextInt+nextInt)/2==oz
 *   PK_RED_DEF  frequency_reduction default:   setLargeFeatureWithSalt(seed,salt,cx,cz); nextFloat() < f
 *   PK_RED_L1   legacy_type_1 (outposts):      setSeed((cx>>4)^((cz>>4)<<4)^seed); nextInt(); nextInt((int)(1/f))==0
 *   PK_RED_L2   legacy_type_2 (buried):        setLargeFeatureWithSalt(seed,cx,cz,10387320); nextFloat() < f
 *   PK_RED_L3   legacy_type_3 (mineshafts):    setLargeFeatureSeed(seed,cx,cz); nextDouble() < (double)f
 * Источник: AbstractSpreadingStructurePlacement.java (26.3) / StructurePlacement.java (26.1, 26.2), RandomSpreadStructurePlacement.java.
 */
#ifndef CRACK_PRED_H
#define CRACK_PRED_H

#include <math.h>
#include "lcg48.h"

enum { PK_LIN = 0, PK_TRI = 1, PK_RED_DEF = 2, PK_RED_L1 = 3, PK_RED_L2 = 4, PK_RED_L3 = 5,
       PK_EXCL = 6,     /* exclusion_zone: в квадрате (2r+1)^2 вокруг (cx,cz) нет потенциального чанка другого набора (random_spread без reducer'а) */
       PK_RING = 7 };   /* strongholds: (cx,cz) в пределах ex_range чанков от одной из позиций колец (надмножество; точная проверка на хосте) */

struct Pred {
    i32 kind;
    i32 lim, ox, oz;      /* spread: lim = spacing - separation; наблюдаемое смещение в регионе */
    i32 tz;               /* linear: число младших бит r, доступных для lifting (0 — нельзя) */
    i32 pow2k;            /* log2(lim), если lim — степень двойки, иначе -1 */
    i32 t2;               /* число нулевых младших бит lim (для теста делимости) */
    u32 invq;             /* обратный к нечётной части lim по модулю 2^32 */
    u32 divbound;         /* floor((2^32-1)/lim) */
    u32 thr_rej;          /* r >= thr_rej -> возможна отбраковка nextInt: считаем точным путём */
    u64 cst;              /* (rx*A + rz*B + salt) mod 2^48 (spread, RED_DEF, RED_L2) */
    u64 magic;            /* Lemire fastmod: floor(2^64/lim)+1 */
    i32 cx, cz;           /* RED_L1 / RED_L3: координаты чанка */
    i32 nmod;             /* RED_L1: (int)(1/f) */
    u32 T24;              /* RED_DEF/L2: next(24) < T24 */
    u64 T53;              /* RED_L3: ((next(26)<<27)+next(27)) < T53 */
    i32 ex_spacing, ex_lim, ex_salt, ex_tri, ex_range;     /* PK_EXCL: параметры другого набора; PK_RING: ex_range — допуск (чанки) */
    i32 ring_distance, ring_spread, ring_count;            /* PK_RING */
};

HD i32 fdiv(i32 a, i32 b) { i32 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }

#ifdef __CUDA_ARCH__
#define MULHI64(a, b) __umul64hi((a), (b))
#else
#define MULHI64(a, b) ((u64)(((unsigned __int128)(a) * (b)) >> 64))
#endif

/* r % lim для 0 <= r < 2^31 (Lemire fastmod) */
HD u32 fastmod31(u32 r, const Pred &P) { return (u32)MULHI64(P.magic * (u64)r, (u64)(u32)P.lim); }

/* nextInt(lim): быстрый путь без отбраковки, иначе — точный */
HD i32 draw_lim(u64 *s, const Pred &P) {
    if (P.pow2k >= 0) return (i32)(((i64)P.lim * (i64)j_next(s, 31)) >> 31);
    u64 st = j_step(*s);
    u32 r = (u32)(st >> 17);
    if (r >= P.thr_rej) return j_nextInt(s, P.lim);
    *s = st;
    return (i32)fastmod31(r, P);
}

/* Точная проверка предиката. W — structure seed (используются младшие 48 бит). */
HD bool pred_eval(u64 W, const Pred &P) {
    switch (P.kind) {
    case PK_LIN: {
        u64 s = ((W + P.cst) & K_MASK48) ^ K_MUL;
        if (draw_lim(&s, P) != P.ox) return false;
        return draw_lim(&s, P) == P.oz;
    }
    case PK_TRI: {
        u64 s = ((W + P.cst) & K_MASK48) ^ K_MUL;
        i32 a = draw_lim(&s, P), b = draw_lim(&s, P);
        if ((a + b) / 2 != P.ox) return false;
        a = draw_lim(&s, P); b = draw_lim(&s, P);
        return (a + b) / 2 == P.oz;
    }
    case PK_RED_DEF:
    case PK_RED_L2: {
        u64 s = ((W + P.cst) & K_MASK48) ^ K_MUL;
        return (u32)j_next(&s, 24) < P.T24;
    }
    case PK_RED_L3: {
        u64 s = large_feature_seed(W & K_MASK48, P.cx, P.cz);
        i64 hi = j_next(&s, 26), lo = j_next(&s, 27);
        return (u64)((hi << 27) + lo) < P.T53;
    }
    case PK_RED_L1: {
        i32 a = (P.cx >> 4) ^ ((P.cz >> 4) << 4);
        u64 s = j_scramble(((u64)(i64)a ^ W) & K_MASK48);
        j_next(&s, 32);
        return j_nextInt(&s, P.nmod) == 0;
    }
    case PK_EXCL: {
        i32 g0x = fdiv(P.cx - P.ex_range, P.ex_spacing), g1x = fdiv(P.cx + P.ex_range, P.ex_spacing);
        i32 g0z = fdiv(P.cz - P.ex_range, P.ex_spacing), g1z = fdiv(P.cz + P.ex_range, P.ex_spacing);
        for (i32 gx = g0x; gx <= g1x; gx++) for (i32 gz = g0z; gz <= g1z; gz++) {
            u64 s = large_feature_with_salt(W, gx, gz, P.ex_salt);
            i32 sx, sz;
            if (!P.ex_tri) { sx = j_nextInt(&s, P.ex_lim); sz = j_nextInt(&s, P.ex_lim); }
            else { i32 a = j_nextInt(&s, P.ex_lim), b = j_nextInt(&s, P.ex_lim); sx = (a + b) / 2; a = j_nextInt(&s, P.ex_lim); b = j_nextInt(&s, P.ex_lim); sz = (a + b) / 2; }
            i32 dx = gx * P.ex_spacing + sx - P.cx, dz = gz * P.ex_spacing + sz - P.cz;
            if (dx >= -P.ex_range && dx <= P.ex_range && dz >= -P.ex_range && dz <= P.ex_range) return false;
        }
        return true;
    }
    case PK_RING: {
        u64 s = j_scramble(W & K_MASK48);
        const double PI = 3.141592653589793;
        double angle = j_nextDouble(&s) * PI * 2.0;
        int pic = 0, circle = 0, spread = P.ring_spread;
        const double robs = sqrt((double)P.cx * P.cx + (double)P.cz * P.cz);
        for (int i = 0; i < P.ring_count; i++) {
            double dist = (double)(4 * P.ring_distance + P.ring_distance * circle * 6) + (j_nextDouble(&s) - 0.5) * (P.ring_distance * 2.5);
            j_nextLong(&s);
            double dd = dist - robs;
            if (dd <= P.ex_range * 1.5 + 2.0 && dd >= -(P.ex_range * 1.5 + 2.0)) {       /* тригонометрия только если радиус подходит */
                i32 ix = (i32)floor(cos(angle) * dist + 0.5), iz = (i32)floor(sin(angle) * dist + 0.5);
                i32 dx = P.cx - ix, dz = P.cz - iz;
                if (dx >= -P.ex_range && dx <= P.ex_range && dz >= -P.ex_range && dz <= P.ex_range) return true;
            }
            angle += (PI * 2) / spread;
            if (++pic == spread) {
                circle++; pic = 0;
                spread += 2 * spread / (circle + 1);
                spread = spread < P.ring_count - i ? spread : P.ring_count - i;
                angle += j_nextDouble(&s) * PI * 2.0;
            }
        }
        return false;
    }
    }
    return false;
}

/* Подготовка констант fastmod/divisibility для lim (host). */
static inline void pred_set_lim(Pred &P, i32 lim) {
    P.lim = lim;
    P.pow2k = -1;
    if ((lim & (lim - 1)) == 0) { int k = 0; while ((1 << k) < lim) k++; P.pow2k = k; }
    P.magic = (~0ULL) / (u64)(u32)lim + 1;
    int t = 0; while (((lim >> t) & 1) == 0) t++;
    P.t2 = t;
    u32 q = (u32)lim >> t, x = q;
    for (int i = 0; i < 6; i++) x *= 2u - q * x;           /* Ньютон: обратный к нечётному q mod 2^32 */
    P.invq = x;
    P.divbound = 0xFFFFFFFFu / (u32)lim;
    P.thr_rej = (u32)(0x80000000u - (u32)lim);
}

#endif

/*
 * pred.h — «предикаты наблюдений» над 48-битным seed W (host + device).
 *   PK_SPREAD: потенциальный чанк random_spread структуры + (опц.) frequency-reducer  (RandomSpreadStructurePlacement + AbstractSpreading...)
 *   PK_SLIME : слайм-чанк / не-слайм-чанк
 * Предикат — POD (копируется в __constant__ память). pred_check() — точная проверка (bit-exact с игрой).
 * Низкобитовый lifting (host): pred_lowbits_ok() — необходимое условие на младшие L бит W.
 */
#ifndef CRACK_PRED_H
#define CRACK_PRED_H
#include "mc.h"

#define PK_SPREAD 0
#define PK_SLIME  1

struct Pred {
    i32 kind;
    /* spread */
    i32 type;        /* 0 linear, 1 triangular */
    i32 lim;         /* spacing - separation */
    i32 salt;
    i32 cx, cz;      /* наблюдаемый чанк (для reducer) */
    i32 ox, oz;      /* смещение в регионе */
    u64 rcst;        /* (gx*A + gz*B + salt) mod 2^48 */
    u64 magic;       /* Lemire: 2^64/lim + 1 */
    i32 np2;         /* lim не степень двойки */
    i32 red;         /* RED_* */
    float freq;
    i32 inv1;        /* (int)(1/freq) для legacy_type_1 */
    i32 tz;          /* число доступных для lifting бит r (0 — нет lifting) */
    /* slime */
    u64 scst;
    i32 neg;         /* 1: наблюдение «не слайм» */
    i32 pad;
    double pass;     /* оценка вероятности прохождения случайным W (для порядка проверки) — только host */
};

/* быстрый nextInt(lim) для lim не степени двойки (Lemire fastmod); на редкий случай отклонения — точный путь */
HD i32 nextInt_fast(u64 *s, i32 lim, u64 magic) {
    u64 st = lcg_step(*s);
    u32 r = (u32)(st >> 17);
    if (r >= 0x80000000u - (u32)lim) return lcg_nextInt(s, lim);
#ifdef __CUDA_ARCH__
    u32 m = (u32)__umul64hi(magic * (u64)r, (u64)lim);
#else
    u32 m = (u32)(((unsigned __int128)(magic * (u64)r) * (u64)lim) >> 64);
#endif
    *s = st;
    return (i32)m;
}

HD i32 pred_nextInt(u64 *s, const Pred &p) { return p.np2 ? nextInt_fast(s, p.lim, p.magic) : lcg_nextInt(s, p.lim); }

HD bool pred_check(u64 W, const Pred &p) {
    if (p.kind == PK_SLIME) {
        int sl = slime_from_cst(W, p.scst);
        return p.neg ? !sl : (bool)sl;
    }
    if (p.lim > 1) {
        u64 s = ((W + p.rcst) & MASK48) ^ LCG_MUL;
        if (p.type == 0) {
            if (pred_nextInt(&s, p) != p.ox) return false;
            if (pred_nextInt(&s, p) != p.oz) return false;
        } else {
            i32 a = pred_nextInt(&s, p), b = pred_nextInt(&s, p);
            if ((a + b) / 2 != p.ox) return false;
            a = pred_nextInt(&s, p); b = pred_nextInt(&s, p);
            if ((a + b) / 2 != p.oz) return false;
        }
    }
    return p.red == RED_NONE || reducer_pass(W, p.red, p.salt, p.cx, p.cz, p.freq, p.inv1);
}

/* Необходимое условие на младшие L бит W (L >= 17). true, если lo (L бит) не противоречит предикату. */
static inline bool pred_lowbits_ok(u64 lo, int L, const Pred &p) {
    u64 mask = (L >= 48) ? MASK48 : ((1ULL << L) - 1);
    if (p.kind == PK_SLIME) {
        if (p.neg || L < 18) return true;
        u64 s = ((lo + p.scst) & mask) ^ (SLIME_KX & mask);
        u64 s1 = (s * LCG_MUL + LCG_ADD) & mask;
        return ((s1 >> 17) & 1) == 0;                 /* nextInt(10)==0 => next(31) чётно => бит 17 состояния == 0 */
    }
    int k = p.tz < (L - 17) ? p.tz : (L - 17);
    if (p.tz <= 0 || k <= 0) return true;
    u64 s = ((lo + p.rcst) & mask) ^ (LCG_MUL & mask);
    u64 s1 = (s * LCG_MUL + LCG_ADD) & mask;
    u64 s2 = (s1 * LCG_MUL + LCG_ADD) & mask;
    u64 m = (1ULL << k) - 1;
    if (((s1 >> 17) & m) != ((u64)(u32)p.ox & m)) return false;
    return ((s2 >> 17) & m) == ((u64)(u32)p.oz & m);
}

#endif

/* mc_end.h — измерение End: острова (SimplexNoise на LegacyRandomSource), биом по TheEndBiomeSource.
 *
 * Источники (26.2): DensityFunctions.EndIslandDensityFunction (getHeightValue, compute), TheEndBiomeSource.getNoiseBiome
 * Источники (26.3): densityfunction/generator/EndIslandFunction.java, DistanceToPointFunction.java, data/density_function/end/islands.json
 *
 * Зависимость от seed: ТОЛЬКО младшие 48 бит (LegacyRandomSource(seed), consumeCount(17292)) => структура «structure seed».
 */
#ifndef MC_END_H
#define MC_END_H
#include "mc_noise.h"

/* perm-таблица SimplexNoise (нужна только p[], смещения xo/yo/zo не используются в 2D) */
typedef struct { McImproved s; } McEnd;

/* Шаг LCG на 17292 вызовов (consumeCount) — константы скачка: s' = M*s + A (mod 2^48) */
#define MC_END_SKIP 17292

MC_HD MC_INLINE void mc_end_init(McEnd *e, i64 seed) {
    McLcg r = lcg_new(seed);
    lcg_skip(&r, MC_END_SKIP);
    improved_init_lcg(&e->s, &r, 256.0);
}
/* то же, но из готового 48-битного «состояния после setSeed» (для перебора seed'ов): state = (seed ^ 0x5DEECE66D) & mask */
MC_HD MC_INLINE void mc_end_init_from_state(McEnd *e, u64 state48, u64 jm, u64 ja) {
    McLcg r; r.s = (state48 * jm + ja) & MC_LCG_MASK;     /* прыжок на 17292 шага */
    improved_init_lcg(&e->s, &r, 256.0);
}

/* ---------------- SimplexNoise 2D ---------------- */
MC_HD MC_INLINE double mc_simplex_corner(int gi, double x, double y, double z, double base) {
    double t0 = base - x * x - y * y - z * z;
    if (t0 < 0.0) return 0.0;
    t0 *= t0;
    int gx, gy, gz; mc_grad(gi, &gx, &gy, &gz);
    return t0 * t0 * (gx * x + gy * y + gz * z);
}
/* getValue(xin, yin) (26.2, double) и get(xin, yin) (26.3: (float)(70.0*(n0+n1+n2))); ядро одинаково */
MC_HD MC_INLINE double mc_simplex2_core(const McImproved *n, double xin, double yin) {
    const double SQRT_3 = 1.7320508075688772;                 /* Math.sqrt(3.0) */
    const double F2 = 0.5 * (SQRT_3 - 1.0);
    const double G2 = (3.0 - SQRT_3) / 6.0;
    double s = (xin + yin) * F2;
    i32 i = mc_floor_d(xin + s), j = mc_floor_d(yin + s);
    double t = (i + j) * G2;
    double X0 = i - t, Y0 = j - t;
    double x0 = xin - X0, y0 = yin - Y0;
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }
    double x1 = x0 - i1 + G2, y1 = y0 - j1 + G2;
    double x2 = x0 - 1.0 + 2.0 * G2, y2 = y0 - 1.0 + 2.0 * G2;
    int ii = i & 0xFF, jj = j & 0xFF;
    int gi0 = mc_p(n, ii + mc_p(n, jj)) % 12;
    int gi1 = mc_p(n, ii + i1 + mc_p(n, jj + j1)) % 12;
    int gi2 = mc_p(n, ii + 1 + mc_p(n, jj + 1)) % 12;
    double n0 = mc_simplex_corner(gi0, x0, y0, 0.0, 0.5);
    double n1 = mc_simplex_corner(gi1, x1, y1, 0.0, 0.5);
    double n2 = mc_simplex_corner(gi2, x2, y2, 0.0, 0.5);
    return 70.0 * (n0 + n1 + n2);
}

MC_HD MC_INLINE float mc_clampf(float v, float lo, float hi) { float m = v < hi ? v : hi; return v < lo ? lo : m; }

/* getHeightValue: центр (только в 26.2) + внешние острова. Возвращает float doffs.
 * mode DOUBLE (26.1/26.2): начальное doffs = центральный остров; шум сравнивается как double < -0.9F
 * mode FLOAT  (26.3):      начальное doffs = -100.0F (центр считается отдельно); шум приводится к float */
MC_HD static float mc_end_height_value(const McEnd *e, int mode, i32 sx, i32 sz) {
    i32 chunkX = sx / 2, chunkZ = sz / 2;
    i32 subX = sx % 2, subZ = sz % 2;
    float doffs;
    if (mode == MC_NOISE_DOUBLE) {
        float lenv = (float)sqrt((double)(float)(sx * sx + sz * sz));
        doffs = 100.0f - lenv * 8.0f;
        doffs = mc_clampf(doffs, -100.0f, 80.0f);
    } else doffs = -100.0f;
    for (i32 xo = -12; xo <= 12; xo++) {
        for (i32 zo = -12; zo <= 12; zo++) {
            i64 tx = (i64)chunkX + xo, tz = (i64)chunkZ + zo;
            if (tx * tx + tz * tz > 4096LL) {
                double nv = mc_simplex2_core(&e->s, (double)tx, (double)tz);
                int isl = (mode == MC_NOISE_DOUBLE) ? (nv < (double)-0.9f) : ((float)nv < -0.9f);
                if (isl) {
                    float ax = fabsf((float)tx), az = fabsf((float)tz);
                    float islandSize = fmodf(ax * 3439.0f + az * 147.0f, 13.0f) + 9.0f;
                    float xd = (float)(subX - xo * 2), zd = (float)(subZ - zo * 2);
                    float nd = 100.0f - (float)sqrt((double)(xd * xd + zd * zd)) * islandSize;
                    nd = mc_clampf(nd, -100.0f, 80.0f);
                    doffs = doffs > nd ? doffs : nd;      /* Math.max(float,float) */
                }
            }
        }
    }
    return doffs;
}

/* значение функции «erosion» в End (то, что читает TheEndBiomeSource): блоковые координаты (кратные 8 для «weird») */
MC_HD static double mc_end_erosion(const McEnd *e, int mode, i32 bx, i32 bz) {
    float h = mc_end_height_value(e, mode, bx / 8, bz / 8);
    if (mode == MC_NOISE_DOUBLE) return ((double)h - 8.0) / 128.0;
    /* 26.3: max( slice(y=0)((clamp(100 - length(0-bx, 0, 0-bz), -100, 80) - 8) * 0.0078125), (h - 8)/128 ) — float */
    float dx = (float)(0 - bx), dz = (float)(0 - bz);
    float len = (float)sqrt((double)(dx * dx + 0.0f * 0.0f + dz * dz));
    float c = mc_clampf(100.0f - len, -100.0f, 80.0f);
    c = (c - 8.0f) * 0.0078125f;
    float o = (h - 8.0f) / 128.0f;
    return (double)(c > o ? c : o);
}

enum { MC_END_BIOME_END = 0, MC_END_HIGHLANDS, MC_END_MIDLANDS, MC_END_ISLANDS, MC_END_BARRENS };
/* TheEndBiomeSource.getNoiseBiome(quartX, quartY, quartZ) */
MC_HD static int mc_end_biome(const McEnd *e, int mode, i32 qx, i32 qz) {
    i32 bx = qx * 4, bz = qz * 4;
    i32 cx = bx >> 4, cz = bz >> 4;
    if ((i64)cx * cx + (i64)cz * cz <= 4096LL) return MC_END_BIOME_END;
    i32 wbx = (cx * 2 + 1) * 8, wbz = (cz * 2 + 1) * 8;
    double h = mc_end_erosion(e, mode, wbx, wbz);
    if (h > 0.25) return MC_END_HIGHLANDS;
    if (h >= -0.0625) return MC_END_MIDLANDS;
    return h < -0.21875 ? MC_END_ISLANDS : MC_END_BARRENS;
}

#endif /* MC_END_H */

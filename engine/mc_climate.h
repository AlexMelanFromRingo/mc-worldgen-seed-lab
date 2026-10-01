/* mc_climate.h — климат Overworld/Nether/End (MultiNoise), сплайны, поиск биома.
 *
 * Источники (26.2): levelgen/DensityFunctions.java (ShiftedNoise, ShiftA/B, Spline, YClampedGradient),
 *   util/CubicSpline.java, biome/Climate.java, RandomState.java, biome/TheEndBiomeSource.java
 * Источники (26.3): densityfunction/generator/NoiseFunction.java, ShiftNoiseFunction.java, GradientFunction.java,
 *   op/LerpFunction.java, biome/Climate.java, RandomState.java
 *
 * Все данные (параметры шумов, сплайны) читаются из data/climate-<V>.txt (tools/extract_climate.py),
 * таблица биомов — data/params/<V>/overworld.tsv (см. mc_biomes.h). Версия — в McClimateSpec.mode.
 */
#ifndef MC_CLIMATE_H
#define MC_CLIMATE_H
#include "mc_noise.h"

/* слоты координат сплайнов */
enum { MC_C_CONT = 0, MC_C_EROS = 1, MC_C_RIDGE = 2, MC_C_RFOLD = 3 };

/* ---------------------------------------------------------------------------------------
 *                                  Кубические сплайны (float, как CubicSpline)
 * --------------------------------------------------------------------------------------- */
#define MC_MAX_SPL_NODES 512
#define MC_MAX_SPL_PTS   2048

typedef struct {
    int    n_nodes, n_pts;
    /* узел: kind 0=const (val), 1=multipoint (coord, n, off) */
    u8     kind[MC_MAX_SPL_NODES];
    u8     coord[MC_MAX_SPL_NODES];
    u16    npts[MC_MAX_SPL_NODES];
    u16    off[MC_MAX_SPL_NODES];
    float  val[MC_MAX_SPL_NODES];
    /* точки */
    float  loc[MC_MAX_SPL_PTS], der[MC_MAX_SPL_PTS];
    i16    child[MC_MAX_SPL_PTS];     /* индекс узла */
    int    root;
} McSpline;

MC_HD MC_INLINE float mc_lerp_ff(float a, float p0, float p1) { return p0 + a * (p1 - p0); }

MC_HD MC_INLINE float mc_spline_linext(float input, const McSpline *S, int off, float value, int idx) {
    float d = S->der[off + idx];
    return d == 0.0f ? value : value + d * (input - S->loc[off + idx]);
}
/* coords[4] — значения координат (float): continents, erosion, ridges, ridges_folded */
MC_HD static float mc_spline_eval(const McSpline *S, int node, const float *coords) {
    if (S->kind[node] == 0) return S->val[node];
    int off = S->off[node], n = S->npts[node];
    float input = coords[S->coord[node]];
    /* findIntervalStart: binarySearch(0,n, i -> input < loc[i]) - 1 */
    int from = 0, len = n;
    while (len > 0) {
        int half = len / 2, mid = from + half;
        if (input < S->loc[off + mid]) len = half; else { from = mid + 1; len -= half + 1; }
    }
    int start = from - 1, last = n - 1;
    if (start < 0)     return mc_spline_linext(input, S, off, mc_spline_eval(S, S->child[off], coords), 0);
    if (start == last) return mc_spline_linext(input, S, off, mc_spline_eval(S, S->child[off + last], coords), last);
    float x1 = S->loc[off + start], x2 = S->loc[off + start + 1];
    float t = (input - x1) / (x2 - x1);
    float d1 = S->der[off + start], d2 = S->der[off + start + 1];
    float y1 = mc_spline_eval(S, S->child[off + start], coords);
    float y2 = mc_spline_eval(S, S->child[off + start + 1], coords);
    float a = d1 * (x2 - x1) - (y2 - y1);
    float b = -d2 * (x2 - x1) + (y2 - y1);
    return mc_lerp_ff(t, y1, y2) + t * (1.0f - t) * mc_lerp_ff(t, a, b);
}

/* ---------------------------------------------------------------------------------------
 *                     Спецификация климата (не зависит от seed) и экземпляр (от seed)
 * --------------------------------------------------------------------------------------- */
typedef struct {
    int mode;                        /* MC_NOISE_DOUBLE (26.1/26.2) | MC_NOISE_FLOAT (26.3) */
    McNoiseSpec sp_offset, sp_temp, sp_veg, sp_cont, sp_eros, sp_ridge;
    u64 hl[6], hh[6];                /* md5("minecraft:<name>") каждого из 6 шумов; порядок: offset,temp,veg,cont,eros,ridge */
    /* depth = gradient(y) + offset */
    int   grad_from_y, grad_to_y;
    double grad_from_d, grad_to_d;   /* значения градиента (double из JSON) */
    float  grad_from_f, grad_to_f;
    double off_const_d;              /* -0.5037500262260437 (old) */
    float  off_const_f;              /* -0.50375f (new) */
    double rf_c1_d, rf_c2_d, rf_c3_d;/* ridges_folded константы */
    float  rf_c1_f, rf_c2_f, rf_c3_f;
    McSpline offset;
} McClimateSpec;

typedef struct {
    McNormal offset, temp, veg, cont, eros, ridge;
} McClimate;

/* Цель для поиска биома (квантованные значения, Climate.TargetPoint) */
typedef struct { i64 t, h, c, e, d, w; } McTarget;

MC_HD MC_INLINE i64 mc_quantize(float v) { return (i64)(v * 10000.0f); }

/* Инициализация экземпляра под seed (Overworld/Xoroshiro).
 * random = XoroshiroRandomSource(seed).forkPositional(); каждый шум: positional.fromHashOf("minecraft:<name>") */
MC_HD static void mc_climate_init_overworld(McClimate *cl, const McClimateSpec *S, i64 seed) {
    McXoro root = xoro_from_long_seed(seed);
    McXoroPos pf = xoro_fork_positional(&root);
    const McNoiseSpec *specs[6] = {&S->sp_offset, &S->sp_temp, &S->sp_veg, &S->sp_cont, &S->sp_eros, &S->sp_ridge};
    McNormal *dst[6] = {&cl->offset, &cl->temp, &cl->veg, &cl->cont, &cl->eros, &cl->ridge};
    for (int i = 0; i < 6; i++) {
        McXoro r = xoro_from_hash(&pf, S->hl[i], S->hh[i]);
        normal_init_xoro(dst[i], specs[i], &r);
    }
}

/* Значения шумов «сырые» (double для old; float->double для new) */
typedef struct { double t, h, c, e, d, w; } McClimateRaw;

/* Overworld: климат в точке (blockX, blockY, blockZ) — blockX = quartX*4 и т.д.
 * Возвращает «сырые» значения после приведения к float, как в Climate.target(...) */
MC_HD static void mc_climate_overworld_raw(const McClimate *cl, const McClimateSpec *S, int bx, int by, int bz, float out[6]) {
    if (S->mode == MC_NOISE_DOUBLE) {
        /* ShiftA: offsetNoise.getValue(x*0.25, 0*0.25, z*0.25) * 4.0 ; ShiftB: (z*0.25, x*0.25, 0*0.25)*4.0 */
        double sx = normal_get_d(&cl->offset, &S->sp_offset, bx * 0.25, 0.0 * 0.25, bz * 0.25) * 4.0;
        double sz = normal_get_d(&cl->offset, &S->sp_offset, bz * 0.25, bx * 0.25, 0.0 * 0.25) * 4.0;
        /* shifted_noise: x = blockX*0.25 + shiftX ; y = blockY*0.0 + 0.0 ; z = blockZ*0.25 + shiftZ */
        double x = bx * 0.25 + sx, z = bz * 0.25 + sz;
        double y = (double)by * 0.0 + 0.0;
        double temp = normal_get_d(&cl->temp,  &S->sp_temp,  x, y, z);
        double veg  = normal_get_d(&cl->veg,   &S->sp_veg,   x, y, z);
        double cont = normal_get_d(&cl->cont,  &S->sp_cont,  x, y, z);
        double eros = normal_get_d(&cl->eros,  &S->sp_eros,  x, y, z);
        double rdg  = normal_get_d(&cl->ridge, &S->sp_ridge, x, y, z);
        /* ridges_folded = ((|(|r| + c1)| + c2) * c3) в double */
        double rf = (fabs(fabs(rdg) + S->rf_c1_d) + S->rf_c2_d) * S->rf_c3_d;
        float coords[4] = {(float)cont, (float)eros, (float)rdg, (float)rf};
        float sp = mc_spline_eval(&S->offset, S->offset.root, coords);
        /* offset = blend_offset*(1-alpha) + (c + spline)*alpha  при alpha=1, blend_offset=0 */
        double offset = 0.0 * (1.0 + -1.0 * 1.0) + (S->off_const_d + (double)sp) * 1.0;
        /* y_clamped_gradient: Mth.clampedMap(y, fromY, toY, fromV, toV) в double */
        double fac = ((double)by - (double)S->grad_from_y) / ((double)S->grad_to_y - (double)S->grad_from_y);
        double grad = fac < 0.0 ? S->grad_from_d : (fac > 1.0 ? S->grad_to_d : S->grad_from_d + fac * (S->grad_to_d - S->grad_from_d));
        double depth = grad + offset;
        out[0] = (float)temp; out[1] = (float)veg; out[2] = (float)cont; out[3] = (float)eros; out[4] = (float)depth; out[5] = (float)rdg;
    } else {
        /* 26.3: всё во float. shift = NoiseFunction.Sampler(noise, 0.25, 0.25|0.0) * 4.0F */
        float sx = normal_get_f(&cl->offset, &S->sp_offset, bx * 0.25, 0.0 * 0.25, bz * 0.25) * 4.0f;
        float sz = normal_get_f(&cl->offset, &S->sp_offset, bz * 0.25, bx * 0.25, 0.0 * 0.25) * 4.0f;
        /* ShiftedXzSampler: x = blockX*xzScale + shiftX (double + float->double) ; y = blockY*yScale */
        double x = bx * 0.25 + (double)sx, z = bz * 0.25 + (double)sz;
        double y = (double)by * 0.0;
        float temp = normal_get_f(&cl->temp,  &S->sp_temp,  x, y, z);
        float veg  = normal_get_f(&cl->veg,   &S->sp_veg,   x, y, z);
        float cont = normal_get_f(&cl->cont,  &S->sp_cont,  x, y, z);
        float eros = normal_get_f(&cl->eros,  &S->sp_eros,  x, y, z);
        float rdg  = normal_get_f(&cl->ridge, &S->sp_ridge, x, y, z);
        float a = fabsf(rdg) + S->rf_c1_f;
        float rf = (fabsf(a) + S->rf_c2_f) * S->rf_c3_f;
        float coords[4] = {cont, eros, rdg, rf};
        float sp = mc_spline_eval(&S->offset, S->offset.root, coords);
        float offset = S->off_const_f + sp;          /* lerp(alpha=1, blend_offset, x) -> x  (alpha==1 => second) */
        /* GradientFunction.ClampedSampler: from + (clamp(y,min,max) - from) * ((to-from)/range) */
        int lo = S->grad_from_y < S->grad_to_y ? S->grad_from_y : S->grad_to_y;
        int hi = S->grad_from_y < S->grad_to_y ? S->grad_to_y : S->grad_from_y;
        int yy = by < lo ? lo : (by > hi ? hi : by);
        int range = S->grad_to_y - S->grad_from_y;
        float factor = (S->grad_to_f - S->grad_from_f) / (float)range;
        float grad = S->grad_from_f + (float)(yy - S->grad_from_y) * factor;
        float depth = grad + offset;
        out[0] = temp; out[1] = veg; out[2] = cont; out[3] = eros; out[4] = depth; out[5] = rdg;
    }
}

MC_HD MC_INLINE McTarget mc_target_from_raw(const float v[6]) {
    McTarget t; t.t = mc_quantize(v[0]); t.h = mc_quantize(v[1]); t.c = mc_quantize(v[2]);
    t.e = mc_quantize(v[3]); t.d = mc_quantize(v[4]); t.w = mc_quantize(v[5]);
    return t;
}

#endif /* MC_CLIMATE_H */

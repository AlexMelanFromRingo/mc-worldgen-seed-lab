/* mcgpu_dev.cuh — устройственный код libmcgen_cuda: шум, «точечные» вычислители density-функций (новая float-ветка и старая
 * double-ветка), R-дерево биомов, BiomeManager. Всё — перенос функций libmcgen (src/noise.c, df_new.c, df_old.c, biome.c,
 * region.c) с СОХРАНЕНИЕМ порядка операций. Сборка: nvcc --fmad=false, без fast-math (иначе нет побитового совпадения с CPU).
 *
 * Вычисление программы — «жадное»: узлы идут в порядке построения (дети раньше родителей), значения всех узлов считаются
 * подряд в локальный массив. Все функции чистые, поэтому результат узла не зависит от того, считались ли ленивые ветки
 * (min/max/range/lerp на CPU пропускают их только для скорости). Ветвление по виду узла одинаково у всех нитей варпа. */
#pragma once
#include <stdint.h>
#include <math.h>
#include "mcgen_gpu_abi.h"

typedef int32_t i32; typedef int64_t i64; typedef uint32_t u32; typedef uint64_t u64; typedef uint8_t u8;
#define DEV static __device__ __forceinline__
#define DEVN static __device__ __noinline__

/* указатели программы на устройстве */
struct DProg {
    const McgNode *nodes; const McgNoise *noise; const McgLayer *layers; const McgOct *octs;
    const McgSpline *sp; const float *splf; const int *spc; const int *arr; const double *thr;
    int nnodes; int root[MCG_MAX_ROOTS];
};

/* ======================================================================= «Java-математика» */
DEV i32 jm_d2i(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return 2147483647;
    if (v <= -2147483648.0) return (i32)0x80000000u;
    return (i32)v;
}
DEV i64 jm_d2l(double v) {
    if (v != v) return 0;
    if (v >= 9223372036854775807.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return (i64)v;
}
DEV i32 jm_floor_d(double v) { return jm_d2i(floor(v)); }
DEV i32 jm_floor_f(float v) { return jm_d2i(floor((double)v)); }
DEV i64 jm_lfloor(double v) { return jm_d2l(floor(v)); }
DEV i32 jm_floordiv(i32 a, i32 b) { i32 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
DEV i32 jm_floormod(i32 a, i32 b) { i32 m = a % b; if (m != 0 && ((m < 0) != (b < 0))) m += b; return m; }
DEV float jm_lerpf(float a, float p0, float p1) { return p0 + a * (p1 - p0); }
DEV double jm_lerp(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
DEV float jm_maxf(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && signbit(a)) return b;
    return a >= b ? a : b;
}
DEV float jm_minf(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && signbit(b)) return b;
    return a <= b ? a : b;
}
DEV double jm_max(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && signbit(a)) return b;
    return a >= b ? a : b;
}
DEV double jm_min(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && signbit(b)) return b;
    return a <= b ? a : b;
}
DEV float jm_clampf(float v, float lo, float hi) { return v < lo ? lo : jm_minf(v, hi); }
DEV double jm_clamp(double v, double lo, double hi) { return v < lo ? lo : jm_min(v, hi); }
DEV float jm_signumf(float v) { if (v != v || v == 0.0f) return v; return v > 0.0f ? 1.0f : -1.0f; }

/* ======================================================================= градиенты (таблица GRAD[16][3] — 2-битными кодами) */
namespace gradtab {
constexpr int8_t GX[16] = { 1,-1, 1,-1, 1,-1, 1,-1, 0, 0, 0, 0, 1, 0,-1, 0 };
constexpr int8_t GY[16] = { 1, 1,-1,-1, 0, 0, 0, 0, 1,-1, 1,-1, 1,-1, 1,-1 };
constexpr int8_t GZ[16] = { 0, 0, 0, 0, 1, 1,-1,-1, 1, 1,-1,-1, 0, 1, 0,-1 };
constexpr u32 pack(const int8_t (&v)[16]) { u32 r = 0; for (int i = 0; i < 16; i++) r |= (u32)(v[i] + 1) << (2 * i); return r; }
constexpr u32 CX = pack(GX), CY = pack(GY), CZ = pack(GZ);
}
DEV float gcomp(u32 code, int h) { return (float)(int)(((code >> (2 * (h & 15))) & 3u) - 1u); }
DEV double gcompd(u32 code, int h) { return (double)(int)(((code >> (2 * (h & 15))) & 3u) - 1u); }
DEV int P_(const McgOct *n, int i) { return (int)__ldg(&n->p[i & 0xFF]); }

DEV float smooth_f(float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
DEV double smooth_d(double x) { return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
DEV float dot_f(int h, float x, float y, float z) { return gcomp(gradtab::CX, h) * x + gcomp(gradtab::CY, h) * y + gcomp(gradtab::CZ, h) * z; }
DEV double dot_d(int h, double x, double y, double z) { return gcompd(gradtab::CX, h) * x + gcompd(gradtab::CY, h) * y + gcompd(gradtab::CZ, h) * z; }
DEV float lerp3f(float a1, float a2, float a3, float x000, float x100, float x010, float x110, float x001, float x101, float x011, float x111) {
    return jm_lerpf(a3, jm_lerpf(a2, jm_lerpf(a1, x000, x100), jm_lerpf(a1, x010, x110)),
                        jm_lerpf(a2, jm_lerpf(a1, x001, x101), jm_lerpf(a1, x011, x111)));
}
DEV double lerp3d(double a1, double a2, double a3, double x000, double x100, double x010, double x110, double x001, double x101, double x011, double x111) {
    return jm_lerp(a3, jm_lerp(a2, jm_lerp(a1, x000, x100), jm_lerp(a1, x010, x110)),
                       jm_lerp(a2, jm_lerp(a1, x001, x101), jm_lerp(a1, x011, x111)));
}
DEV double wrap_new(double x) {
    const double HALF = 16777215.999999998;
    return (x >= -HALF && x < HALF) ? x : x - floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}
DEV double wrap_old(double x) { return x - (double)jm_lfloor(x / 3.3554432E7 + 0.5) * 3.3554432E7; }

DEV float sample_lerp_f(const McgOct *n, int x, int y, int z, float rx, float ry, float rz, float ry_orig) {
    int x0 = P_(n, x), x1 = P_(n, x + 1);
    int xy00 = P_(n, x0 + y), xy01 = P_(n, x0 + y + 1), xy10 = P_(n, x1 + y), xy11 = P_(n, x1 + y + 1);
    float d000 = dot_f(P_(n, xy00 + z), rx, ry, rz);
    float d100 = dot_f(P_(n, xy10 + z), rx - 1.0f, ry, rz);
    float d010 = dot_f(P_(n, xy01 + z), rx, ry - 1.0f, rz);
    float d110 = dot_f(P_(n, xy11 + z), rx - 1.0f, ry - 1.0f, rz);
    float d001 = dot_f(P_(n, xy00 + z + 1), rx, ry, rz - 1.0f);
    float d101 = dot_f(P_(n, xy10 + z + 1), rx - 1.0f, ry, rz - 1.0f);
    float d011 = dot_f(P_(n, xy01 + z + 1), rx, ry - 1.0f, rz - 1.0f);
    float d111 = dot_f(P_(n, xy11 + z + 1), rx - 1.0f, ry - 1.0f, rz - 1.0f);
    return lerp3f(smooth_f(rx), smooth_f(ry_orig), smooth_f(rz), d000, d100, d010, d110, d001, d101, d011, d111);
}
DEV float perlin_get_f(const McgOct *n, double _x, double _y, double _z) {
    double x = wrap_new(_x) + n->xo, y = wrap_new(_y) + n->yo, z = wrap_new(_z) + n->zo;
    i32 fx = jm_floor_d(x), fy = jm_floor_d(y), fz = jm_floor_d(z);
    float rx = (float)(x - fx), ry = (float)(y - fy), rz = (float)(z - fz);
    return sample_lerp_f(n, fx, fy, fz, rx, ry, rz, ry);
}
DEV double fudge_y(double original_y, double rel_y, double scale) {
    double lim = (original_y >= 0.0 && original_y < rel_y) ? original_y : rel_y;
    return (double)jm_floor_d(lim / scale + (double)1.0E-7f) * scale;
}
DEV float smeared_get_f(const McgOct *n, double fs, double _x, double _y, double _z) {
    double x = wrap_new(_x) + n->xo, y = wrap_new(_y) + n->yo, z = wrap_new(_z) + n->zo;
    i32 fx = jm_floor_d(x), fy = jm_floor_d(y), fz = jm_floor_d(z);
    float rx = (float)(x - fx);
    double ry = y - fy;
    float rz = (float)(z - fz);
    float fry = (float)(ry - fudge_y(_y, ry, fs));
    return sample_lerp_f(n, fx, fy, fz, rx, fry, rz, (float)ry);
}
/* ImprovedNoise.noise(x,y,z,yScale,yFudge) старой ветки (координаты уже обёрнуты) */
DEV double noise_d(const McgOct *n, double _x, double _y, double _z, double y_scale, double y_fudge) {
    double x = _x + n->xo, y = _y + n->yo, z = _z + n->zo;
    i32 xf = jm_floor_d(x), yf = jm_floor_d(y), zf = jm_floor_d(z);
    double xr = x - xf, yr = y - yf, zr = z - zf;
    double yrf = 0.0;
    if (y_scale != 0.0) {
        double lim = (y_fudge >= 0.0 && y_fudge < yr) ? y_fudge : yr;
        yrf = (double)jm_floor_d(lim / y_scale + (double)1.0E-7f) * y_scale;
    }
    double yy = yr - yrf;
    int x0 = P_(n, xf), x1 = P_(n, xf + 1);
    int xy00 = P_(n, x0 + yf), xy01 = P_(n, x0 + yf + 1), xy10 = P_(n, x1 + yf), xy11 = P_(n, x1 + yf + 1);
    double d000 = dot_d(P_(n, xy00 + zf), xr, yy, zr);
    double d100 = dot_d(P_(n, xy10 + zf), xr - 1.0, yy, zr);
    double d010 = dot_d(P_(n, xy01 + zf), xr, yy - 1.0, zr);
    double d110 = dot_d(P_(n, xy11 + zf), xr - 1.0, yy - 1.0, zr);
    double d001 = dot_d(P_(n, xy00 + zf + 1), xr, yy, zr - 1.0);
    double d101 = dot_d(P_(n, xy10 + zf + 1), xr - 1.0, yy, zr - 1.0);
    double d011 = dot_d(P_(n, xy01 + zf + 1), xr, yy - 1.0, zr - 1.0);
    double d111 = dot_d(P_(n, xy11 + zf + 1), xr - 1.0, yy - 1.0, zr - 1.0);
    return lerp3d(smooth_d(xr), smooth_d(yr), smooth_d(zr), d000, d100, d010, d110, d001, d101, d011, d111);
}

/* NStack (26.3+): ns_get */
DEV float ns_get(const DProg &P, int nsi, double x, double y, double z) {
    const McgNoise mn = P.noise[nsi];
    float v = 0.0f;
    for (int i = 0; i < mn.n0; i++) {
        const McgLayer L = P.layers[mn.l0 + i];
        double f = L.a;
        const McgOct *o = P.octs + L.oct;
        float g = L.kind == 0 ? perlin_get_f(o, x * f, y * f, z * f) : smeared_get_f(o, L.b, x * f, y * f, z * f);
        v += L.amp_f * g;
    }
    return v;
}
/* OldPerlin.getValue(x,y,z) при yScale=yFudge=0 для набора слоёв */
DEV double old_perlin_get(const DProg &P, int l0, int n, double x, double y, double z) {
    double value = 0.0;
    for (int i = 0; i < n; i++) {
        const McgLayer L = P.layers[l0 + i];
        if (L.oct >= 0) {
            double factor = L.a;
            double nv = noise_d(P.octs + L.oct, wrap_old(x * factor), wrap_old(y * factor), wrap_old(z * factor), 0.0 * factor, 0.0 * factor);
            value += L.amp_d * nv * L.b;
        }
    }
    return value;
}
DEV double old_normal_get(const DProg &P, int nsi, double x, double y, double z) {
    const McgNoise mn = P.noise[nsi];
    double x2 = x * 1.0181268882175227, y2 = y * 1.0181268882175227, z2 = z * 1.0181268882175227;
    return (old_perlin_get(P, mn.l0, mn.n0, x, y, z) + old_perlin_get(P, mn.l1, mn.n1, x2, y2, z2)) * mn.value_factor;
}

/* ======================================================================= Simplex 2D и острова End */
DEV double simplex_corner(int gi, double x, double y, double z, double base) {
    double t0 = base - x * x - y * y - z * z;
    if (t0 < 0.0) return 0.0;
    t0 *= t0;
    return t0 * t0 * (gcompd(gradtab::CX, gi) * x + gcompd(gradtab::CY, gi) * y + gcompd(gradtab::CZ, gi) * z);
}
DEV double simplex2_core(const McgOct *n, double xin, double yin) {
    const double SQRT_3 = 1.7320508075688772;
    const double F2 = 0.5 * (SQRT_3 - 1.0);
    const double G2 = (3.0 - SQRT_3) / 6.0;
    double s = (xin + yin) * F2;
    i32 i = jm_floor_d(xin + s), j = jm_floor_d(yin + s);
    double t = (i + j) * G2;
    double X0 = i - t, Y0 = j - t;
    double x0 = xin - X0, y0 = yin - Y0;
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }
    double x1 = x0 - i1 + G2, y1 = y0 - j1 + G2;
    double x2 = x0 - 1.0 + 2.0 * G2, y2 = y0 - 1.0 + 2.0 * G2;
    int ii = i & 0xFF, jj = j & 0xFF;
    int gi0 = P_(n, ii + P_(n, jj)) % 12;
    int gi1 = P_(n, ii + i1 + P_(n, jj + j1)) % 12;
    int gi2 = P_(n, ii + 1 + P_(n, jj + 1)) % 12;
    double n0 = simplex_corner(gi0, x0, y0, 0.0, 0.5);
    double n1 = simplex_corner(gi1, x1, y1, 0.0, 0.5);
    double n2 = simplex_corner(gi2, x2, y2, 0.0, 0.5);
    return 70.0 * (n0 + n1 + n2);
}
/* 26.3: EndIslandFunction.getHeightValue (float) */
DEV float end_height_new(const McgOct *n, int sx, int sz) {
    int cx = sx / 2, cz = sz / 2, subx = sx % 2, subz = sz % 2;
    float doffs = -100.0f;
    for (int xo = -12; xo <= 12; xo++) for (int zo = -12; zo <= 12; zo++) {
        i64 tx = (i64)cx + xo, tz = (i64)cz + zo;
        if (tx * tx + tz * tz > 4096LL && (float)simplex2_core(n, (double)tx, (double)tz) < -0.9f) {
            float size = fmodf(fabsf((float)tx) * 3439.0f + fabsf((float)tz) * 147.0f, 13.0f) + 9.0f;
            float xd = (float)(subx - xo * 2), zd = (float)(subz - zo * 2);
            float nd = 100.0f - (float)sqrt((double)(xd * xd + zd * zd)) * size;
            nd = jm_clampf(nd, -100.0f, 80.0f);
            doffs = jm_maxf(doffs, nd);
        }
    }
    return doffs;
}
/* 26.1/26.2: EndIslandDensityFunction.getHeightValue */
DEV float end_height_old(const McgOct *n, int sx, int sz) {
    int cx = sx / 2, cz = sz / 2, subx = sx % 2, subz = sz % 2;
    i32 sq = (i32)((u32)sx * (u32)sx + (u32)sz * (u32)sz);
    float doffs = 100.0f - (float)sqrt((double)(float)sq) * 8.0f;
    doffs = jm_clampf(doffs, -100.0f, 80.0f);
    for (int xo = -12; xo <= 12; xo++) for (int zo = -12; zo <= 12; zo++) {
        i64 tx = (i64)cx + xo, tz = (i64)cz + zo;
        if (tx * tx + tz * tz > 4096LL && simplex2_core(n, (double)tx, (double)tz) < (double)-0.9f) {
            float size = fmodf(fabsf((float)tx) * 3439.0f + fabsf((float)tz) * 147.0f, 13.0f) + 9.0f;
            float xd = (float)(subx - xo * 2), zd = (float)(subz - zo * 2);
            float nd = 100.0f - (float)sqrt((double)(xd * xd + zd * zd)) * size;
            nd = jm_clampf(nd, -100.0f, 80.0f);
            doffs = jm_maxf(doffs, nd);
        }
    }
    return doffs;
}

/* ======================================================================= сплайны (float; координаты берутся из массива значений узлов) */
/* Итеративный обход дерева сплайна (рекурсию GPU не любит: непредсказуемый размер стека). Формулы — spl_eval из df_new.c / spl_eval_o из df_old.c. */
template <typename V>
DEV float spl_eval(const DProg &P, int root_spi, const McgNode &nd, const V *val) {
    int f_spi[16], f_stage[16], f_start[16]; float f_in[16], f_y1[16];
    int sp = 0; f_spi[0] = root_spi; f_stage[0] = 0;
    float ret = 0.0f;
    for (;;) {
        const McgSpline s = P.sp[f_spi[sp]];
        int stage = f_stage[sp];
        if (stage == 0) {
            if (s.is_const) { ret = s.value; if (sp == 0) return ret; sp--; continue; }
            float in = (float)val[P.arr[nd.arr0 + s.coord]];
            const float *loc = P.splf + s.loc0;
            int from = 0, len = s.n;
            while (len > 0) { int half = len / 2, mid = from + half; if (in < loc[mid]) len = half; else { from = mid + 1; len -= half + 1; } }
            int start = from - 1, last = s.n - 1;
            f_in[sp] = in; f_start[sp] = start;
            if (start < 0) { f_stage[sp] = 1; sp++; f_spi[sp] = P.spc[s.val0]; f_stage[sp] = 0; continue; }
            if (start == last) { f_stage[sp] = 1; sp++; f_spi[sp] = P.spc[s.val0 + last]; f_stage[sp] = 0; continue; }
            f_stage[sp] = 2; sp++; f_spi[sp] = P.spc[s.val0 + start]; f_stage[sp] = 0; continue;
        }
        const float *loc = P.splf + s.loc0, *der = P.splf + s.loc0 + s.n;
        float in = f_in[sp]; int start = f_start[sp], last = s.n - 1;
        if (stage == 1) {      /* линейная экстраполяция за краем */
            int e = start < 0 ? 0 : last;
            float d = der[e];
            ret = d == 0.0f ? ret : ret + d * (in - loc[e]);
            if (sp == 0) return ret; sp--; continue;
        }
        if (stage == 2) { f_y1[sp] = ret; f_stage[sp] = 3; sp++; f_spi[sp] = P.spc[s.val0 + start + 1]; f_stage[sp] = 0; continue; }
        /* stage 3: y1 = f_y1[sp], y2 = ret */
        {
            float y1 = f_y1[sp], y2 = ret;
            float x1 = loc[start], x2 = loc[start + 1];
            float t = (in - x1) / (x2 - x1);
            float d1 = der[start], d2 = der[start + 1];
            float a = d1 * (x2 - x1) - (y2 - y1);
            float b = -d2 * (x2 - x1) + (y2 - y1);
            ret = jm_lerpf(t, y1, y2) + t * (1.0f - t) * jm_lerpf(t, a, b);
            if (sp == 0) return ret; sp--; continue;
        }
    }
}

/* ======================================================================= точечный вычислитель, новая ветка (s_value при caches = 0) */
DEV float squeeze_f(float v) { float x = jm_clampf(v, -1.0f, 1.0f); return x / 2.0f - (x * x * x) / 24.0f; }
DEV float grad_compute(const McgNode &s, int coord) {
    switch (s.k) {
    case MCGK_GRAD_CLAMP: { int cc = coord < s.i2 ? s.i2 : (coord > s.i3 ? s.i3 : coord); return s.f0 + (float)(cc - s.i1) * s.f1; }
    case MCGK_GRAD_REPEAT: { int rel = coord - s.i1; return s.f0 + (float)jm_floormod(rel, s.i2) * s.f1; }
    default: {
        int rel = coord - s.i1;
        int tile = jm_floordiv(rel, s.i2);
        int local = rel - tile * s.i2;
        return (tile & 1) == 0 ? s.f0 + (float)local * s.f1 : s.f0 + (float)(s.i2 - local) * s.f1;
    }
    }
}
DEV float dist_metric(int m, float dx, float dy, float dz) {
    switch (m) {
    case 0: return (float)sqrt((double)(dx * dx + dy * dy + dz * dz));
    case 1: return dx * dx + dy * dy + dz * dz;
    case 2: return fabsf(dx) + fabsf(dy) + fabsf(dz);
    default: return jm_maxf(jm_maxf(fabsf(dx), fabsf(dy)), fabsf(dz));
    }
}
#define MCG_MAX_VAL 192
/* Точечные значения узлов программы (новая ветка). Возврат: значение корня root; nodes[0..nnodes) считаются подряд. */
DEVN bool eval_prog_new(const DProg &P, int ox, int oy, int oz, float *val) {
    for (int i = 0; i < P.nnodes; i++) {
        const McgNode s = P.nodes[i];
        const int bx = (s.cm & 1) ? s.cx : ox, by = (s.cm & 2) ? s.cy : oy, bz = (s.cm & 4) ? s.cz : oz;   /* контекст срезов */
        float r;
        switch (s.k) {
        case MCGK_CONST: r = s.f0; break;
        case MCGK_NOISE: r = ns_get(P, s.ns, bx * s.d0, by * s.d1, bz * s.d0); break;
        case MCGK_NOISE_XZ: {
            double nx = bx * s.d0 + (double)val[s.a];
            double ny = by * s.d1;
            double nz = bz * s.d0 + (double)val[s.c];
            r = ns_get(P, s.ns, nx, ny, nz); break;
        }
        case MCGK_NOISE_XYZ: {
            double nx = bx * s.d0 + (double)val[s.a];
            double ny = by * s.d1 + (double)val[s.b];
            double nz = bz * s.d0 + (double)val[s.c];
            r = ns_get(P, s.ns, nx, ny, nz); break;
        }
        case MCGK_SHIFT_B: r = ns_get(P, s.ns, bz * s.d0, bx * s.d0, 0.0) * 4.0f; break;
        case MCGK_END: r = (end_height_new(P.octs + s.oct, bx / 8, bz / 8) - 8.0f) / 128.0f; break;
        case MCGK_DIST: r = dist_metric(s.i3, (float)(s.i0 - bx), (float)(s.i1 - by), (float)(s.i2 - bz)); break;
        case MCGK_GRAD_CLAMP: case MCGK_GRAD_REPEAT: case MCGK_GRAD_MIRROR: r = grad_compute(s, s.i0 == 0 ? bx : s.i0 == 1 ? by : bz); break;
        case MCGK_CTX_ALPHA: case MCGK_CTX_OFFSET: case MCGK_CTX_BEARD: r = s.f0; break;
        case MCGK_ABS: r = fabsf(val[s.a]); break;
        case MCGK_SQUARE: { float v = val[s.a]; r = v * v; break; }
        case MCGK_CUBE: { float v = val[s.a]; r = v * v * v; break; }
        case MCGK_SQRT: r = (float)sqrt((double)val[s.a]); break;
        case MCGK_LEAKY: { float v = val[s.a]; r = v > 0.0f ? v : v * s.f0; break; }
        case MCGK_RECIP: r = 1.0f / val[s.a]; break;
        case MCGK_NEG: r = -val[s.a]; break;
        case MCGK_SQUEEZE: r = squeeze_f(val[s.a]); break;
        case MCGK_SIGN: r = jm_signumf(val[s.a]); break;
        case MCGK_ADD: r = val[s.a] + val[s.b]; break;
        case MCGK_CADD: r = val[s.a] + s.f0; break;
        case MCGK_CSUB: r = s.f0 - val[s.a]; break;
        case MCGK_SUB: r = val[s.a] - val[s.b]; break;
        case MCGK_MUL: { float l = val[s.a]; r = l == 0.0f ? 0.0f : l * val[s.b]; break; }
        case MCGK_CMUL: r = val[s.a] * s.f0; break;
        case MCGK_DIV: { float l = val[s.a]; r = l == 0.0f ? 0.0f : l / val[s.b]; break; }
        case MCGK_CDIV: r = s.f0 / val[s.a]; break;
        case MCGK_MIN: { float l = val[s.a]; r = l <= s.f0 ? l : jm_minf(l, val[s.b]); break; }
        case MCGK_CMIN: r = jm_minf(val[s.a], s.f0); break;
        case MCGK_MAX: { float l = val[s.a]; r = l >= s.f0 ? l : jm_maxf(l, val[s.b]); break; }
        case MCGK_CMAX: r = jm_maxf(val[s.a], s.f0); break;
        case MCGK_SPLINE: r = spl_eval<float>(P, s.sp, s, val); break;
        case MCGK_LERP: {
            float a = val[s.a];
            if (a == 0.0f) r = val[s.b]; else if (a == 1.0f) r = val[s.c]; else r = jm_lerpf(a, val[s.b], val[s.c]);
            break;
        }
        case MCGK_LERP_CF: {
            float a = val[s.a];
            if (a == 0.0f) r = s.f0; else if (a == 1.0f) r = val[s.c]; else r = jm_lerpf(a, s.f0, val[s.c]);
            break;
        }
        case MCGK_LERP_CS: {
            float a = val[s.a];
            if (a == 0.0f) r = val[s.b]; else if (a == 1.0f) r = s.f0; else r = jm_lerpf(a, val[s.b], s.f0);
            break;
        }
        case MCGK_CLAMP: r = jm_clampf(val[s.a], s.f0, s.f1); break;
        case MCGK_RANGE_C: { float v = val[s.a]; r = (v >= s.f0 && v < s.f1) ? s.f2 : s.f3; break; }
        case MCGK_RANGE: { float v = val[s.a]; r = (v >= s.f0 && v < s.f1) ? val[s.b] : val[s.c]; break; }
        case MCGK_ISEL1: { float v = val[s.a]; r = v < s.f0 ? val[s.b] : val[s.c]; break; }
        case MCGK_ISEL: {
            float v = val[s.a];
            int k = 0; for (; k < s.nthr; k++) if (v < (float)P.thr[s.thr0 + k]) break;
            if (k == s.nthr) k = s.narr - 1;
            r = val[P.arr[s.arr0 + k]]; break;
        }
        case MCGK_CACHE: case MCGK_BLEND_DENSITY: case MCGK_SLICE_X: case MCGK_SLICE_Y: case MCGK_SLICE_Z: case MCGK_SLICE_XZ: r = val[s.a]; break;
        default: return false;     /* INTERP/FTS/POW/LOG/ROUND: в точечные программы климата не входят (мост проверяет) */
        }
        val[i] = r;
    }
    return true;
}

/* ======================================================================= точечный вычислитель, старая ветка (oc при NChunk = NULL) */
DEV double clamped_map(double v, double a, double b, double ta, double tb) {
    double f = (v - a) / (b - a);
    return f < 0.0 ? ta : (f > 1.0 ? tb : jm_lerp(f, ta, tb));
}
DEV double weird_rarity(int type, double r) {
    if (type == 0) return r < -0.5 ? 0.75 : (r < 0.0 ? 1.0 : (r < 0.5 ? 1.5 : 2.0));
    return r < -0.75 ? 0.5 : (r < -0.5 ? 0.75 : (r < 0.5 ? 1.0 : (r < 0.75 ? 2.0 : 3.0)));
}
DEVN bool eval_prog_old(const DProg &P, int bx, int by, int bz, double *val) {
    for (int i = 0; i < P.nnodes; i++) {
        const McgNode s = P.nodes[i];
        double r;
        switch (s.k) {
        case MCGO_CONST: r = s.d0; break;
        case MCGO_PASS: r = val[s.a]; break;
        case MCGO_NOISE: r = old_normal_get(P, s.ns, bx * s.d0, by * s.d1, bz * s.d0); break;
        case MCGO_SHIFTED_NOISE: {
            double x = bx * s.d0 + val[s.a];
            double y = by * s.d1 + val[s.b];
            double z = bz * s.d0 + val[s.c];
            r = old_normal_get(P, s.ns, x, y, z); break;
        }
        case MCGO_SHIFT_A: r = old_normal_get(P, s.ns, (double)bx * s.d0, 0.0 * s.d0, (double)bz * s.d0) * 4.0; break;
        case MCGO_SHIFT_B: r = old_normal_get(P, s.ns, (double)bz * s.d0, (double)bx * s.d0, 0.0 * s.d0) * 4.0; break;
        case MCGO_SHIFT: r = old_normal_get(P, s.ns, (double)bx * s.d0, (double)by * s.d1, (double)bz * s.d0) * 4.0; break;
        case MCGO_END_ISLANDS: r = ((double)end_height_old(P.octs + s.oct, bx / 8, bz / 8) - 8.0) / 128.0; break;
        case MCGO_Y_GRADIENT: r = clamped_map((double)by, (double)s.i0, (double)s.i1, s.d0, s.d1); break;
        case MCGO_WEIRD_SCALED: {
            double rr = weird_rarity(s.i0, val[s.a]);
            r = rr * fabs(old_normal_get(P, s.ns, bx / rr, by / rr, bz / rr)); break;
        }
        case MCGO_BLEND_ALPHA: r = 1.0; break;
        case MCGO_ZERO: r = 0.0; break;
        case MCGO_ABS: r = fabs(val[s.a]); break;
        case MCGO_SQUARE: { double v = val[s.a]; r = v * v; break; }
        case MCGO_CUBE: { double v = val[s.a]; r = v * v * v; break; }
        case MCGO_HALF_NEG: { double v = val[s.a]; r = v > 0.0 ? v : v * 0.5; break; }
        case MCGO_QUARTER_NEG: { double v = val[s.a]; r = v > 0.0 ? v : v * 0.25; break; }
        case MCGO_RECIP: r = 1.0 / val[s.a]; break;
        case MCGO_SQUEEZE: { double c = jm_clamp(val[s.a], -1.0, 1.0); r = c / 2.0 - c * c * c / 24.0; break; }
        case MCGO_CLAMP: r = jm_clamp(val[s.a], s.d0, s.d1); break;
        case MCGO_ADD: r = val[s.a] + val[s.b]; break;
        case MCGO_MUL: { double v = val[s.a]; r = v == 0.0 ? 0.0 : v * val[s.b]; break; }
        case MCGO_MIN: { double v = val[s.a]; r = v < s.d1 ? v : jm_min(v, val[s.b]); break; }
        case MCGO_MAX: { double v = val[s.a]; r = v > s.d1 ? v : jm_max(v, val[s.b]); break; }
        case MCGO_CADD: r = val[s.a] + s.d0; break;
        case MCGO_CMUL: r = val[s.a] * s.d0; break;
        case MCGO_RANGE: { double v = val[s.a]; r = (v >= s.d0 && v < s.d1) ? val[s.b] : val[s.c]; break; }
        case MCGO_ISEL: {
            double v = val[s.a];
            int k = 0; for (; k < s.nthr; k++) if (v < P.thr[s.thr0 + k]) break;
            if (k == s.nthr) k = s.narr - 1;
            r = val[P.arr[s.arr0 + k]]; break;
        }
        case MCGO_SPLINE: r = (double)spl_eval<double>(P, s.sp, s, val); break;
        default: return false;
        }
        val[i] = r;
    }
    return true;
}

/* ======================================================================= R-дерево биомов (Climate.RTree) */
struct DTree { const McgTreeNode *n; int root; };
DEV u64 node_dist(const McgTreeNode &n, const i64 *tg) {
    u64 d = 0;
    #pragma unroll
    for (int i = 0; i < 7; i++) {
        i64 above = tg[i] - (i64)n.hi[i], below = (i64)n.lo[i] - tg[i];
        i64 x = above > 0 ? above : (below > 0 ? below : 0);
        d += (u64)(x * x);
    }
    return d;
}
/* точная копия порядка обхода mc_rt_search_node(candidate = −1); возвращает индекс листа */
DEVN int rt_find(const DTree &T, const i64 *tg) {
    int fn[16], fc[16], fcl[16]; u64 fm[16];
    int sp = 0;
    fn[0] = T.root; fc[0] = 0; fm[0] = 0x7fffffffffffffffULL; fcl[0] = -1;
    for (;;) {
        const McgTreeNode n = T.n[fn[sp]];
        if (fc[sp] < n.count) {
            int ci = n.first + fc[sp];
            const McgTreeNode cn = T.n[ci];
            u64 cd = node_dist(cn, tg);
            if (fm[sp] > cd) {
                if (cn.count == 0) { fm[sp] = cd; fcl[sp] = ci; fc[sp]++; }
                else { sp++; fn[sp] = ci; fc[sp] = 0; fm[sp] = fm[sp - 1]; fcl[sp] = fcl[sp - 1]; }
            } else fc[sp]++;
        } else {
            int res = fcl[sp];
            if (sp == 0) return res;
            sp--;
            u64 ld = node_dist(T.n[res], tg);
            if (fm[sp] > ld) { fm[sp] = ld; fcl[sp] = res; }
            fc[sp]++;
        }
    }
}
/* rt_count_min >= 2: есть ли хотя бы два листа с расстоянием d (обход по узлам с расстоянием ≤ d) */
DEVN bool rt_tie(const DTree &T, const i64 *tg, u64 d) {
    int fn[16], fc[16]; int sp = 0; int cnt = 0;
    fn[0] = T.root; fc[0] = 0;
    {
        const McgTreeNode r0 = T.n[T.root];
        if (r0.count == 0) return false;     /* корень-лист: один лист */
    }
    for (;;) {
        const McgTreeNode n = T.n[fn[sp]];
        if (fc[sp] < n.count) {
            int ci = n.first + fc[sp]++;
            const McgTreeNode cn = T.n[ci];
            u64 cd = node_dist(cn, tg);
            if (cd > d) continue;
            if (cn.count == 0) { if (cd == d) { if (++cnt >= 2) return true; } }
            else { sp++; fn[sp] = ci; fc[sp] = 0; }
        } else { if (sp == 0) return false; sp--; }
    }
}

/* ======================================================================= BiomeManager */
DEV i64 zoom_lcg(i64 r, i64 c) { u64 v = (u64)r; v *= v * 6364136223846793005ULL + 1442695040888963407ULL; return (i64)(v + (u64)c); }
DEV double fiddle(i64 r) { i64 m = (r >> 24) % 1024; if (m < 0) m += 1024; return ((double)m / 1024.0 - 0.5) * 0.9; }
DEV double fiddled_distance(i64 seed, int x, int y, int z, double dx, double dy, double dz) {
    i64 r = seed;
    r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z); r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z);
    double fx = fiddle(r); r = zoom_lcg(r, seed);
    double fy = fiddle(r); r = zoom_lcg(r, seed);
    double fz = fiddle(r);
    return (dz + fz) * (dz + fz) + (dy + fy) * (dy + fy) + (dx + fx) * (dx + fx);
}
/* mcgen_biome_at: ячейка шума (qx,qy,qz), выбранная BiomeManager.getBiome */
DEV void zoom_cell(i64 seed, int x, int y, int z, int *oqx, int *oqy, int *oqz) {
    int ax = x - 2, ay = y - 2, az = z - 2;
    int px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
    int mi = 0; double md = INFINITY;
    for (int i = 0; i < 8; i++) {
        int xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        double d = fiddled_distance(seed, xe ? px : px + 1, ye ? py : py + 1, ze ? pz : pz + 1,
                                    xe ? fx : fx - 1.0, ye ? fy : fy - 1.0, ze ? fz : fz - 1.0);
        if (md > d) { mi = i; md = d; }
    }
    *oqx = (mi & 4) == 0 ? px : px + 1; *oqy = (mi & 2) == 0 ? py : py + 1; *oqz = (mi & 1) == 0 ? pz : pz + 1;
}

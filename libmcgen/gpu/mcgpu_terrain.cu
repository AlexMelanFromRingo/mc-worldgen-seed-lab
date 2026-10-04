/* mcgpu_terrain.cu — TERRAIN на GPU (26.3+): объёмный исполнитель density-функций и жилы руд.
 *
 * Исполнитель повторяет рекурсию s_volume из src/df_new.c на уровне «узел × объём → буфер»: для каждого узла запускается
 * ядро поэлементной арифметики с ТЕМИ ЖЕ формулами и порядком операций, что у CPU (объёмный путь: ns_add_volume,
 * interp_fill_cell, spl_eval по буферам …). Значение каждого элемента зависит только от узла и координаты, поэтому
 * результаты узлов можно запоминать (memo по паре «узел, объём») — как кэши игры, но без влияния на числа.
 * Объёмы описаны относительно начала чанка (rel) или абсолютно (срезы slice фиксируют ось), поэтому один «план» годится для
 * любого чанка; в пакет входят B чанков (второе измерение сетки ядер = чанк).
 *
 * Выход: плотность final_density (float, [чанк][iz][ix][iy]) и «заплатки жил» (u16 по раскладке блоков [y][z][x] от nmin;
 * 0xFFFF — нет): решение правил ore_vein для позиции не зависит от блока, CPU применяет заплатку к непустым не-жидким блокам. */
#include "mcgpu_prog.cuh"
#include "../../engine/mc_rng.h"
#include <vector>
#include <algorithm>
#include <map>
#include <string>
#include <chrono>

struct DVol { int sx, sy, sz, dx, dy, dz, y0, xrel, xoff, zrel, zoff; };
static __host__ __device__ __forceinline__ int dv_size(const DVol &v) { return v.sx * v.sy * v.sz; }
static inline bool dv_eq(const DVol &a, const DVol &b) {
    return a.sx == b.sx && a.sy == b.sy && a.sz == b.sz && a.dx == b.dx && a.dy == b.dy && a.dz == b.dz && a.y0 == b.y0 &&
           a.xrel == b.xrel && a.xoff == b.xoff && a.zrel == b.zrel && a.zoff == b.zoff;
}
DEV int vx0(const DVol &v, int cx0) { return v.xrel ? cx0 + v.xoff : v.xoff; }
DEV int vz0(const DVol &v, int cz0) { return v.zrel ? cz0 + v.zoff : v.zoff; }
/* индекс элемента → (ix, iy, iz): раскладка vol_idx = iy + (ix + iz*sx)*sy */
DEV void unpack(int i, int sx, int sy, int &ix, int &iy, int &iz) { iy = i % sy; int t = i / sy; ix = t % sx; iz = t / sx; }

#define THREADS 256
#define ELEM_SETUP(n) \
    long long gi = (long long)blockIdx.x * blockDim.x + threadIdx.x; \
    if (gi >= (long long)(n)) return; \
    const int ci = blockIdx.y; const int i = (int)gi; const size_t off = (size_t)ci * (size_t)(n) + (size_t)i;

/* ======================================================================= ядра: поэлементные операции (формулы объёмного пути s_volume) */
__global__ void k_fill(float *out, int n, float val) { ELEM_SETUP(n); out[off] = val; }

enum { U_ABS, U_SQUARE, U_CUBE, U_SQRT, U_LEAKY, U_RECIP, U_NEG, U_SQUEEZE, U_SIGN, U_CADD, U_CSUB, U_CMUL, U_CDIV, U_CMIN, U_CMAX, U_CLAMP, U_RANGE_C };
__global__ void k_unary(int op, float *out, const float *a, int n, float f0, float f1, float f2, float f3) {
    ELEM_SETUP(n);
    float v = a[off], r;
    switch (op) {
    case U_ABS: r = fabsf(v); break;
    case U_SQUARE: r = v * v; break;
    case U_CUBE: r = v * v * v; break;
    case U_SQRT: r = (float)sqrt((double)v); break;
    case U_LEAKY: r = v > 0.0f ? v : v * f0; break;
    case U_RECIP: r = 1.0f / v; break;
    case U_NEG: r = -v; break;
    case U_SQUEEZE: r = squeeze_f(v); break;
    case U_SIGN: r = jm_signumf(v); break;
    case U_CADD: r = v + f0; break;
    case U_CSUB: r = f0 - v; break;
    case U_CMUL: r = v * f0; break;
    case U_CDIV: r = f0 / v; break;
    case U_CMIN: r = v; if (f0 < r) r = f0; break;
    case U_CMAX: r = v; if (f0 > r) r = f0; break;
    case U_CLAMP: r = jm_clampf(v, f0, f1); break;
    default: r = (v >= f0 && v < f1) ? f2 : f3; break;     /* U_RANGE_C */
    }
    out[off] = r;
}
enum { B_ADD, B_SUB, B_MUL, B_DIV, B_MIN, B_MAX };
__global__ void k_binary(int op, float *out, const float *a, const float *b, int n) {
    ELEM_SETUP(n);
    float o = a[off], r = b[off];
    switch (op) {
    case B_ADD: o = o + r; break;
    case B_SUB: o = o + -r; break;
    case B_MUL: o = o * r; break;
    case B_DIV: o = o / r; break;
    case B_MIN: if (r < o) o = r; break;
    default: if (r > o) o = r; break;
    }
    out[off] = o;
}
/* K_LERP / K_LERP_CF / K_LERP_CS: f или sc может быть nullptr → f0 */
__global__ void k_lerp(float *out, const float *a, const float *f, const float *sc, int n, float f0) {
    ELEM_SETUP(n);
    float av = a[off];
    float fv = f ? f[off] : f0, sv = sc ? sc[off] : f0;
    out[off] = av == 0.0f ? fv : (av == 1.0f ? sv : jm_lerpf(av, fv, sv));
}
__global__ void k_range(float *out, const float *in, const float *b, const float *c, int n, float f0, float f1) {
    ELEM_SETUP(n);
    float iv = in[off];
    out[off] = (!(iv >= f0) || !(iv < f1)) ? c[off] : b[off];
}
__global__ void k_isel1(float *out, const float *a, const float *b, const float *c, int n, float f0) {
    ELEM_SETUP(n);
    out[off] = a[off] < f0 ? b[off] : c[off];
}
struct PtrArr8 { const float *p[8]; };
__global__ void k_isel(DProg P, int thr0, int nthr, int narr, float *out, const float *a, PtrArr8 bs, int n) {
    ELEM_SETUP(n);
    float v = a[off];
    int k = 0; for (; k < nthr; k++) if (v < (float)P.thr[thr0 + k]) break;
    if (k == nthr) k = narr - 1;
    out[off] = bs.p[k][off];
}

/* ---- шум: PerlinNoise/SmearedPerlinNoise.addToVolume для одного элемента (add_volume_impl из noise.c) ---- */
DEV float vnoise_layer(const McgOct *n, int smeared, double fs, int bx, int by, int bz, double xz_scale, double y_scale, float amp) {
    double z = wrap_new(bz * xz_scale) + n->zo;
    i32 fz = jm_floor_d(z);
    float rz = (float)(z - fz);
    float az = smooth_f(rz);
    double x = wrap_new(bx * xz_scale) + n->xo;
    i32 fx = jm_floor_d(x);
    float rx = (float)(x - fx);
    int x0 = P_(n, fx), x1 = P_(n, fx + 1);
    float ax = smooth_f(rx);
    double oy = by * y_scale;
    double y = wrap_new(oy) + n->yo;
    i32 fy = jm_floor_d(y);
    float ry, ay;
    double ryd = y - fy;
    if (!smeared) { ry = (float)ryd; ay = smooth_f(ry); }
    else { ay = smooth_f((float)ryd); ry = 0; }
    int xy00 = P_(n, x0 + fy), xy01 = P_(n, x0 + fy + 1), xy10 = P_(n, x1 + fy), xy11 = P_(n, x1 + fy + 1);
    int h000 = P_(n, xy00 + fz), h100 = P_(n, xy10 + fz), h010 = P_(n, xy01 + fz), h110 = P_(n, xy11 + fz);
    int h001 = P_(n, xy00 + fz + 1), h101 = P_(n, xy10 + fz + 1), h011 = P_(n, xy01 + fz + 1), h111 = P_(n, xy11 + fz + 1);
    #define GX(h) gcomp(gradtab::CX, h)
    #define GY(h) gcomp(gradtab::CY, h)
    #define GZ(h) gcomp(gradtab::CZ, h)
    float d000xz = GX(h000) * rx + GZ(h000) * rz;                     float g000y = GY(h000);
    float d100xz = GX(h100) * (rx - 1.0f) + GZ(h100) * rz;            float g100y = GY(h100);
    float d010xz = GX(h010) * rx + GZ(h010) * rz;                     float g010y = GY(h010);
    float d110xz = GX(h110) * (rx - 1.0f) + GZ(h110) * rz;            float g110y = GY(h110);
    float d001xz = GX(h001) * rx + GZ(h001) * (rz - 1.0f);            float g001y = GY(h001);
    float d101xz = GX(h101) * (rx - 1.0f) + GZ(h101) * (rz - 1.0f);   float g101y = GY(h101);
    float d011xz = GX(h011) * rx + GZ(h011) * (rz - 1.0f);            float g011y = GY(h011);
    float d111xz = GX(h111) * (rx - 1.0f) + GZ(h111) * (rz - 1.0f);   float g111y = GY(h111);
    #undef GX
    #undef GY
    #undef GZ
    if (smeared) ry = (float)(ryd - fudge_y(oy, ryd, fs));
    return amp * lerp3f(ax, ay, az, d000xz + g000y * ry, d100xz + g100y * ry, d010xz + g010y * (ry - 1.0f), d110xz + g110y * (ry - 1.0f),
                        d001xz + g001y * ry, d101xz + g101y * ry, d011xz + g011y * (ry - 1.0f), d111xz + g111y * (ry - 1.0f));
}
/* ns_add_volume для одного элемента: buf = 0; для слоёв buf += amp·lerp … */
DEV float vnoise_elem(const DProg &P, int nsi, int bx, int by, int bz, double xz_scale, double y_scale, float amp) {
    const McgNoise mn = P.noise[nsi];
    float buf = 0.0f;
    for (int l = 0; l < mn.n0; l++) {
        const McgLayer L = P.layers[mn.l0 + l];
        double f = L.a;
        buf += vnoise_layer(P.octs + L.oct, L.kind, L.b, bx, by, bz, xz_scale * f, y_scale * f, amp * L.amp_f);
    }
    return buf;
}
__global__ void k_noise(DProg P, int nsi, DVol v, const int *cx0, const int *cz0, float *out, int n, double xzs, double ys) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    int bx = vx0(v, cx0[ci]) + ix * v.dx, by = v.y0 + iy * v.dy, bz = vz0(v, cz0[ci]) + iz * v.dz;
    out[off] = vnoise_elem(P, nsi, bx, by, bz, xzs, ys, 1.0f);
}
/* K_NOISE_XZ / K_NOISE_XYZ: сдвиги из буферов, точечный ns_get */
__global__ void k_noise_xz(DProg P, int nsi, DVol v, const int *cx0, const int *cz0, float *out, const float *sx_, const float *sy_, const float *sz_, int n, double d0, double d1) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    int bx = vx0(v, cx0[ci]) + ix * v.dx, by = v.y0 + iy * v.dy, bz = vz0(v, cz0[ci]) + iz * v.dz;
    double baseZ = bz * d0, baseX = bx * d0;
    double nx = baseX + (double)sx_[off];
    double ny = by * d1;
    if (sy_) ny += (double)sy_[off];
    double nz = baseZ + (double)sz_[off];
    out[off] = ns_get(P, nsi, nx, ny, nz);
}
/* K_SHIFT_B: шум по транспонированному объёму tv = {sz, sx, 1, z0, x0, 0, dz, dx, 1}, amp 4.0f, затем заполнение по y */
__global__ void k_shift_b(DProg P, int nsi, DVol v, const int *cx0, const int *cz0, float *out, int n, double d0) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    int bxp = vz0(v, cz0[ci]) + iz * v.dz, byp = vx0(v, cx0[ci]) + ix * v.dx, bzp = 0;
    out[off] = vnoise_elem(P, nsi, bxp, byp, bzp, d0, d0, 4.0f);
}
__global__ void k_grad(McgNode s, DVol v, const int *cx0, const int *cz0, float *out, int n) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    int coord = s.i0 == 0 ? vx0(v, cx0[ci]) + ix * v.dx : (s.i0 == 1 ? v.y0 + iy * v.dy : vz0(v, cz0[ci]) + iz * v.dz);
    out[off] = grad_compute(s, coord);
}
__global__ void k_end(DProg P, int oct, DVol v, const int *cx0, const int *cz0, float *out, int n) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    if (iy != 0) return;     /* значение колонки пишется по всем iy ниже */
    int bx = vx0(v, cx0[ci]) + ix * v.dx, bz = vz0(v, cz0[ci]) + iz * v.dz;
    float val = (end_height_new(P.octs + oct, bx / 8, bz / 8) - 8.0f) / 128.0f;
    for (int k = 0; k < v.sy; k++) out[off + k] = val;
}
__global__ void k_dist(McgNode s, DVol v, const int *cx0, const int *cz0, float *out, int n) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    int bx = vx0(v, cx0[ci]) + ix * v.dx, by = v.y0 + iy * v.dy, bz = vz0(v, cz0[ci]) + iz * v.dz;
    out[off] = dist_metric(s.i3, (float)(s.i0 - bx), (float)(s.i1 - by), (float)(s.i2 - bz));
}
/* сплайн по буферам координат (spl_eval с bufs из df_new.c) */
struct SplArgs { const float *c[8]; };
DEV float spl_eval_buf(const DProg &P, int root_spi, const SplArgs &A, size_t idx, unsigned *used) {
    int f_spi[16], f_stage[16], f_start[16]; float f_in[16], f_y1[16];
    int sp = 0; f_spi[0] = root_spi; f_stage[0] = 0;
    float ret = 0.0f;
    for (;;) {
        const McgSpline s = P.sp[f_spi[sp]];
        int stage = f_stage[sp];
        if (stage == 0) {
            if (s.is_const) { ret = s.value; if (sp == 0) return ret; sp--; continue; }
            float in = A.c[s.coord][idx];
            *used |= 1u << s.coord;
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
        if (stage == 1) {
            int e = start < 0 ? 0 : last;
            float d = der[e];
            ret = d == 0.0f ? ret : ret + d * (in - loc[e]);
            if (sp == 0) return ret; sp--; continue;
        }
        if (stage == 2) { f_y1[sp] = ret; f_stage[sp] = 3; sp++; f_spi[sp] = P.spc[s.val0 + start + 1]; f_stage[sp] = 0; continue; }
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
__global__ void k_spline(DProg P, int spi, SplArgs A, float *out, int n, unsigned *flags) {
    long long gi = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    const int ci = blockIdx.y;
    unsigned used = 0;
    if (gi < (long long)n) {
        const size_t off = (size_t)ci * (size_t)n + (size_t)gi;
        out[off] = spl_eval_buf(P, spi, A, off, &used);
    }
    /* какие координаты сплайна тронуты (CPU берёт их лениво): OR по варпу, затем один atomicOr на варп */
    for (int d = 16; d > 0; d >>= 1) used |= __shfl_xor_sync(0xffffffffu, used, d);
    if ((threadIdx.x & 31) == 0 && used) atomicOr(&flags[ci], used);
}
/* K_INTERP, шаг объёмной интерполяции по ячейкам (interp_block_step + interp_fill_cell): выходной объём ov с шагом 1;
 * угловой объём cv лежит в cb. Для элемента находится его ячейка, восемь углов и накопление по y — как в CPU-цикле. */
struct InterpGeom { int cxz, cy; float f0, f1; int cv_sx, cv_sy, cv_sz; int mnx, mny, mnz; };   /* mn* — индекс первой ячейки: floordiv(ov.x0, cxz) и т. п. (в блоках ov.x0 относительно чанка) */
__global__ void k_interp(DVol ov, InterpGeom G, const int *cx0, const int *cz0, float *out, const float *cb, int n, int ncv) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, ov.sx, ov.sy, ix, iy, iz);
    int ox0 = vx0(ov, cx0[ci]), oz0 = vz0(ov, cz0[ci]);
    int bx = ox0 + ix, by = ov.y0 + iy, bz = oz0 + iz;
    int mnx = jm_floordiv(ox0, G.cxz), mny = jm_floordiv(ov.y0, G.cy), mnz = jm_floordiv(oz0, G.cxz);
    int cell_x = jm_floordiv(bx, G.cxz) - mnx, cell_y = jm_floordiv(by, G.cy) - mny, cell_z = jm_floordiv(bz, G.cxz) - mnz;
    int cox = (mnx + cell_x) * G.cxz - ox0, coy = (mny + cell_y) * G.cy - ov.y0, coz = (mnz + cell_z) * G.cxz - oz0;
    int xx = ix - cox, yy = iy - coy, zz = iz - coz;                      /* локальные индексы внутри ячейки */
    int y_start = 0 > -coy ? 0 : -coy;
    int ncx2 = cell_x + 1 < G.cv_sx - 1 ? cell_x + 1 : G.cv_sx - 1;
    int ncz2 = cell_z + 1 < G.cv_sz - 1 ? cell_z + 1 : G.cv_sz - 1;
    int ncy2 = cell_y + 1 < G.cv_sy - 1 ? cell_y + 1 : G.cv_sy - 1;
    const float *cbc = cb + (size_t)ci * (size_t)ncv;
    #define CV(x, y, z) cbc[(y) + ((x) + (z) * G.cv_sx) * G.cv_sy]
    float v000 = CV(cell_x, cell_y, cell_z), v100 = CV(ncx2, cell_y, cell_z);
    float v001 = CV(cell_x, cell_y, ncz2), v101 = CV(ncx2, cell_y, ncz2);
    float v010 = CV(cell_x, ncy2, cell_z), v110 = CV(ncx2, ncy2, cell_z);
    float v011 = CV(cell_x, ncy2, ncz2), v111 = CV(ncx2, ncy2, ncz2);
    #undef CV
    float az = (float)zz * G.f0;
    float v00 = jm_lerpf(az, v000, v001), v01 = jm_lerpf(az, v010, v011), v10 = jm_lerpf(az, v100, v101), v11 = jm_lerpf(az, v110, v111);
    float ax = (float)xx * G.f0;
    float a0 = jm_lerpf(ax, v00, v10), a1 = jm_lerpf(ax, v01, v11);
    float step = (a1 - a0) * G.f1;
    float value = a0 + step * (float)y_start;
    for (int k = y_start; k < yy; k++) value += step;
    out[off] = value;
}
/* срезы: вещание по оси */
__global__ void k_slice(int axis, float *out, const float *in, DVol v, int n, int nin) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    const float *inc = in + (size_t)ci * (size_t)nin;
    int j;
    switch (axis) {
    case 0: j = iy + iz * v.sy; break;                  /* X: iv = {1, sy, sz}: idx(0,iy,iz) = iy + (0 + iz*1)*sy */
    case 1: j = 0 + (ix + iz * v.sx) * 1; break;        /* Y: iv = {sx, 1, sz}: idx(ix,0,iz) = ix + iz*sx */
    case 2: j = iy + ix * v.sy; break;                  /* Z: iv = {sx, sy, 1}: idx(ix,iy,0) = iy + ix*sy */
    default: j = iy; break;                             /* XZ: iv = {1, sy, 1}: idx(0,iy,0) = iy */
    }
    out[off] = inc[j];
}

/* ======================================================================= жилы руд: решение правил ore_vein для позиции */
struct VeinArgs { const float *dens[MCG_MAX_VEINS], *rich[MCG_MAX_VEINS]; int ore[MCG_MAX_VEINS], raw[MCG_MAX_VEINS], filler[MCG_MAX_VEINS]; float raw_chance[MCG_MAX_VEINS]; int gap_root[MCG_MAX_VEINS]; int n; };
__global__ void k_veins(DProg G, VeinArgs A, DVol v, const int *cx0, const int *cz0, unsigned short *out, int n, u64 lo, u64 hi) {
    ELEM_SETUP(n);
    int ix, iy, iz; unpack(i, v.sx, v.sy, ix, iy, iz);
    int bx = vx0(v, cx0[ci]) + ix, by = v.y0 + iy, bz = vz0(v, cz0[ci]) + iz;
    unsigned short result = 0xFFFF;
    McXoroPos pos; pos.lo = lo; pos.hi = hi;
    for (int r = 0; r < A.n; r++) {
        float d = A.dens[r][off];
        if (d <= 0.0f) continue;
        McXoro rr = xoro_at(&pos, bx, by, bz);
        if (xoro_next_float(&rr) > d) continue;
        float rich_v = A.rich[r][off];
        int outst;
        bool pass = xoro_next_float(&rr) < rich_v;
        if (pass) { float val[MCG_MAX_VAL]; eval_prog_new(G, bx, by, bz, val); pass = val[A.gap_root[r]] < 0.0f; }
        if (pass) outst = xoro_next_float(&rr) < A.raw_chance[r] ? A.raw[r] : A.ore[r];
        else outst = A.filler[r];
        result = (unsigned short)outst;
        break;
    }
    out[(size_t)ci * (size_t)n + (size_t)((iy * 16 + iz) * 16 + ix)] = result;
}

/* сбор ячеек кэшей в выходной буфер на устройстве: одна запись на (чанк, кэш) */
struct GatherItem { const float *src; size_t dst; int n; };
__global__ void k_gather(const GatherItem *items, int nitems, float *dst, size_t chunk_stride_unused) {
    int it = blockIdx.y;
    if (it >= nitems) return;
    GatherItem g = items[it];
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < g.n; i += gridDim.x * blockDim.x) dst[g.dst + i] = g.src[i];
}

/* ======================================================================= хост: исполнитель */
static McgProg *prog_clone(const McgProg *p) {
    McgProg *c = (McgProg *)calloc(1, sizeof(McgProg));
    *c = *p;
#define CL(f, nf, T) do { c->f = nullptr; if (p->nf > 0) { c->f = (T *)malloc(sizeof(T) * (size_t)p->nf); memcpy(c->f, p->f, sizeof(T) * (size_t)p->nf); } } while (0)
    CL(nodes, nnodes, McgNode); CL(noise, nnoise, McgNoise); CL(layers, nlayers, McgLayer); CL(octs, noct, McgOct); CL(sp, nsp, McgSpline);
    CL(splf, nsplf, float); CL(spc, nspc, int); CL(arr, narr, int); CL(thr, nthr, double);
#undef CL
    return c;
}
static void prog_free_clone(McgProg *c) {
    if (!c) return;
    free(c->nodes); free(c->noise); free(c->layers); free(c->octs); free(c->sp); free(c->splf); free(c->spc); free(c->arr); free(c->thr); free(c);
}

struct MemoEnt { int node; DVol v; float *p; };
struct SplOp { int node; DVol v; };                  /* экземпляр сплайна в пакете: маски использованных координат — g->flags[op*Bmax + чанк] */
struct CellInfo { int cid; std::vector<DVol> cand; size_t cap, off; };   /* кэш, нужный CPU: кандидаты объёмов, ёмкость (максимум размера), смещение в буфере чанка */

struct GTerrain {
    McgTerrainWorld T;                 /* копия описания; vol/gap заменены клонами */
    McgProg *vol = nullptr, *gap = nullptr;
    GProg gvol, ggap;
    int Bmax = 0;                      /* B: максимум чанков в пакете */
    size_t slab_floats = 0;            /* на пакет */
    float *slab = nullptr;
    int *d_cx0 = nullptr, *d_cz0 = nullptr;
    unsigned short *d_veins = nullptr;
    std::vector<float> h_stage;
    std::vector<int> h_cx0, h_cz0;
    /* состояние исполнения пакета */
    int B = 0; size_t used = 0; bool dry = false; bool fail = false; char fail_msg[200] = "";
    std::vector<MemoEnt> memo;
    McgBeardFn beard_cb = nullptr; void *beard_ud = nullptr;
    size_t per_chunk_floats = 0;       /* из сухого прогона */
    std::vector<CellInfo> cells;       /* кэши, нужные CPU после плотности */
    size_t cell_floats = 0;
    std::vector<SplOp> splops;         /* сплайны текущего пакета (в порядке исполнения) */
    unsigned *d_flags = nullptr; std::vector<unsigned> h_flags; int flags_cap_ops = 0;
    long long launches = 0;
    bool prof = false;                 /* MCGPU_PROFILE=1: время ядер по видам узлов */
    struct ProfEnt { const char *name; int node; int n; cudaEvent_t a, b; };
    std::vector<ProfEnt> pe; std::vector<cudaEvent_t> evpool;
    std::map<std::string, std::pair<double, long long>> pagg;   /* имя → (мс, число запусков) */
    const char *cur_name = "?"; int cur_node = -1;
    double ph[8] = {0,0,0,0,0,0,0,0}; long long nbatches = 0, nchunks_done = 0;
    DVol vfull;                        /* объём плотности: чанк 16 × nh × 16 */
};

static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void fail_(GTerrain *g, const char *msg) { if (!g->fail) { g->fail = true; snprintf(g->fail_msg, sizeof g->fail_msg, "%s", msg); } }

static float *g_alloc(GTerrain *g, int n) {
    size_t need = ((size_t)g->B * (size_t)n + 63) & ~(size_t)63;
    if (g->dry) { g->used += need; return g->slab; }
    if (g->used + need > g->slab_floats) { fail_(g, "не хватило арены для пакета"); return g->slab; }
    float *p = g->slab + g->used; g->used += need; return p;
}
static inline dim3 grid_for(int n, int B) { return dim3((unsigned)((n + THREADS - 1) / THREADS), (unsigned)B, 1); }
static cudaEvent_t prof_ev(GTerrain *g) {
    if (!g->evpool.empty()) { cudaEvent_t e = g->evpool.back(); g->evpool.pop_back(); return e; }
    cudaEvent_t e; cudaEventCreate(&e); return e;
}
#define LAUNCH(kernel, n, ...) do { if (!g->dry && !g->fail) { \
    GTerrain::ProfEnt pe_; pe_.name = #kernel; pe_.node = g->cur_node; pe_.n = (n); if (g->prof) { pe_.a = prof_ev(g); pe_.b = prof_ev(g); cudaEventRecord(pe_.a); } \
    kernel<<<grid_for(n, g->B), THREADS>>>(__VA_ARGS__); g->launches++; \
    if (g->prof) { cudaEventRecord(pe_.b); g->pe.push_back(pe_); } } } while (0)
static void prof_flush(GTerrain *g) {
    if (!g->prof || g->pe.empty()) return;
    cudaDeviceSynchronize();
    for (auto &p : g->pe) { float ms = 0; cudaEventElapsedTime(&ms, p.a, p.b); char key[96]; snprintf(key, sizeof key, "%s n=%d", p.name, p.n); auto &e = g->pagg[key]; e.first += ms; e.second++; g->evpool.push_back(p.a); g->evpool.push_back(p.b); }
    g->pe.clear();
}

static int fl_div(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static int fl_mod(int a, int b) { int m = a % b; if (m != 0 && ((m < 0) != (b < 0))) m += b; return m; }

/* K_INTERP: как поступает s_volume. mode 0 — вход на том же объёме (уже сетка углов), 1 — шаг по ячейкам с угловым объёмом cv, −1 — не поддержано */
static int interp_plan(const McgNode &s, const DVol &v, DVol *cv) {
    int cxz = s.i0, cy = s.i1;
    if (cxz <= 0 || cy <= 0 || 16 % cxz != 0) return -1;
    if ((v.dx == cxz || v.sx == 1) && (v.dy == cy || v.sy == 1) && (v.dz == cxz || v.sz == 1) &&
        fl_mod(v.xoff, cxz) == 0 && fl_mod(v.y0, cy) == 0 && fl_mod(v.zoff, cxz) == 0) return 0;
    if (!(v.dx == 1 && v.dy == 1 && v.dz == 1)) return -1;
    if (v.sx == 1 && v.xrel) return -1;
    int maxx = v.xoff + v.sx - 1, maxy = v.y0 + v.sy - 1, maxz = v.zoff + v.sz - 1;
    int mnx = fl_div(v.xoff, cxz), mny = fl_div(v.y0, cy), mnz = fl_div(v.zoff, cxz);
    int mxx = fl_div(maxx, cxz), mxy = fl_div(maxy, cy), mxz = fl_div(maxz, cxz);
    int ncx = mxx - mnx + 1, ncy = mxy - mny + 1, ncz = mxz - mnz + 1;
    cv->sx = fl_mod(maxx, cxz) == 0 ? ncx : ncx + 1; cv->sy = fl_mod(maxy, cy) == 0 ? ncy : ncy + 1; cv->sz = fl_mod(maxz, cxz) == 0 ? ncz : ncz + 1;
    cv->xrel = v.xrel; cv->xoff = mnx * cxz; cv->y0 = mny * cy; cv->zrel = v.zrel; cv->zoff = mnz * cxz; cv->dx = cxz; cv->dy = cy; cv->dz = cxz;
    return 1;
}
/* срезы: 0 — вход на том же объёме, 1 — вход на объёме iv (axis), −1 — не поддержано */
static int slice_plan(const McgNode &s, const DVol &v, DVol *iv, int *axis) {
    *iv = v;
    if (s.k == MCGK_SLICE_X) {
        if (v.sx == 1) return (!v.xrel && v.xoff == s.i0) ? 0 : -1;
        iv->sx = 1; iv->xrel = 0; iv->xoff = s.i0; *axis = 0; return 1;
    }
    if (s.k == MCGK_SLICE_Y) {
        if (v.sy == 1 && v.y0 == s.i0) return 0;
        iv->sy = 1; iv->y0 = s.i0; *axis = 1; return 1;
    }
    if (s.k == MCGK_SLICE_Z) {
        if (v.sz == 1) return (!v.zrel && v.zoff == s.i0) ? 0 : -1;
        iv->sz = 1; iv->zrel = 0; iv->zoff = s.i0; *axis = 2; return 1;
    }
    if (v.sx == 1 && v.sz == 1 && !v.xrel && !v.zrel && v.xoff == s.i0 && v.zoff == s.i1) return 0;
    if (v.sx == 1 || v.sz == 1) return -1;
    iv->sx = 1; iv->sz = 1; iv->xrel = 0; iv->zrel = 0; iv->xoff = s.i0; iv->zoff = s.i1; *axis = 3; return 1;
}

static float *exec_eval(GTerrain *g, int ni, const DVol &v);

static float *memo_get(GTerrain *g, int ni, const DVol &v) {
    for (const MemoEnt &m : g->memo) if (m.node == ni && dv_eq(m.v, v)) return m.p;
    return nullptr;
}

/* значение узла на объёме v — зеркало s_volume (CPU) */
static float *exec_node(GTerrain *g, int ni, const DVol &v) {
    const McgNode &s = g->vol->nodes[ni];
    const DProg &P = g->gvol.d;
    const int n = dv_size(v);
    float *o = nullptr;
    auto unary = [&](int op) {
        float *a = exec_eval(g, s.a, v); o = g_alloc(g, n);
        LAUNCH(k_unary, n, op, o, a, n, s.f0, s.f1, s.f2, s.f3);
    };
    auto binary = [&](int op) {
        float *a = exec_eval(g, s.a, v); float *b = exec_eval(g, s.b, v); o = g_alloc(g, n);
        LAUNCH(k_binary, n, op, o, a, b, n);
    };
    switch (s.k) {
    case MCGK_CONST: case MCGK_CTX_ALPHA: case MCGK_CTX_OFFSET: o = g_alloc(g, n); LAUNCH(k_fill, n, o, n, s.f0); break;
    case MCGK_CTX_BEARD: {
        o = g_alloc(g, n);
        if (!g->dry && !g->fail) {
            if (s.f0 != 0.0f) { fail_(g, "Beardifier с ненулевым значением по умолчанию"); break; }
            cudaMemset(o, 0, sizeof(float) * (size_t)g->B * (size_t)n);
            if (g->beard_cb) for (int c = 0; c < g->B; c++) {
                int vol[9];
                vol[0] = v.sx; vol[1] = v.sy; vol[2] = v.sz; vol[3] = v.xrel ? g->h_cx0[c] + v.xoff : v.xoff; vol[4] = v.y0;
                vol[5] = v.zrel ? g->h_cz0[c] + v.zoff : v.zoff; vol[6] = v.dx; vol[7] = v.dy; vol[8] = v.dz;
                if (g->h_stage.size() < (size_t)n) g->h_stage.resize((size_t)n);
                if (g->beard_cb(g->beard_ud, c, vol, g->h_stage.data())) cudaMemcpy(o + (size_t)c * n, g->h_stage.data(), sizeof(float) * (size_t)n, cudaMemcpyHostToDevice);
            }
        }
        break;
    }
    case MCGK_NOISE: o = g_alloc(g, n); LAUNCH(k_noise, n, P, s.ns, v, g->d_cx0, g->d_cz0, o, n, s.d0, s.d1); break;
    case MCGK_NOISE_XZ: case MCGK_NOISE_XYZ: {
        float *sx_ = exec_eval(g, s.a, v);
        float *sy_ = s.k == MCGK_NOISE_XYZ ? exec_eval(g, s.b, v) : nullptr;
        float *sz_ = exec_eval(g, s.c, v);
        o = g_alloc(g, n);
        LAUNCH(k_noise_xz, n, P, s.ns, v, g->d_cx0, g->d_cz0, o, sx_, sy_, sz_, n, s.d0, s.d1);
        break;
    }
    case MCGK_SHIFT_B: o = g_alloc(g, n); LAUNCH(k_shift_b, n, P, s.ns, v, g->d_cx0, g->d_cz0, o, n, s.d0); break;
    case MCGK_END: o = g_alloc(g, n); LAUNCH(k_end, n, P, s.oct, v, g->d_cx0, g->d_cz0, o, n); break;
    case MCGK_DIST: o = g_alloc(g, n); LAUNCH(k_dist, n, s, v, g->d_cx0, g->d_cz0, o, n); break;
    case MCGK_GRAD_CLAMP: case MCGK_GRAD_REPEAT: case MCGK_GRAD_MIRROR: o = g_alloc(g, n); LAUNCH(k_grad, n, s, v, g->d_cx0, g->d_cz0, o, n); break;
    case MCGK_ABS: unary(U_ABS); break;
    case MCGK_SQUARE: unary(U_SQUARE); break;
    case MCGK_CUBE: unary(U_CUBE); break;
    case MCGK_SQRT: unary(U_SQRT); break;
    case MCGK_LEAKY: unary(U_LEAKY); break;
    case MCGK_RECIP: unary(U_RECIP); break;
    case MCGK_NEG: unary(U_NEG); break;
    case MCGK_SQUEEZE: unary(U_SQUEEZE); break;
    case MCGK_SIGN: unary(U_SIGN); break;
    case MCGK_CADD: unary(U_CADD); break;
    case MCGK_CSUB: unary(U_CSUB); break;
    case MCGK_CMUL: unary(U_CMUL); break;
    case MCGK_CDIV: unary(U_CDIV); break;
    case MCGK_CMIN: unary(U_CMIN); break;
    case MCGK_CMAX: unary(U_CMAX); break;
    case MCGK_CLAMP: unary(U_CLAMP); break;
    case MCGK_RANGE_C: unary(U_RANGE_C); break;
    case MCGK_ADD: binary(B_ADD); break;
    case MCGK_SUB: binary(B_SUB); break;
    case MCGK_MUL: binary(B_MUL); break;
    case MCGK_DIV: binary(B_DIV); break;
    case MCGK_MIN: binary(B_MIN); break;
    case MCGK_MAX: binary(B_MAX); break;
    case MCGK_LERP: case MCGK_LERP_CF: case MCGK_LERP_CS: {
        float *a = exec_eval(g, s.a, v);
        float *f = s.k != MCGK_LERP_CF ? exec_eval(g, s.b, v) : nullptr;
        float *sc = s.k != MCGK_LERP_CS ? exec_eval(g, s.c, v) : nullptr;
        o = g_alloc(g, n);
        LAUNCH(k_lerp, n, o, a, f, sc, n, s.f0);
        break;
    }
    case MCGK_RANGE: {
        float *b = exec_eval(g, s.b, v); float *in = exec_eval(g, s.a, v); float *c = exec_eval(g, s.c, v);
        o = g_alloc(g, n);
        LAUNCH(k_range, n, o, in, b, c, n, s.f0, s.f1);
        break;
    }
    case MCGK_ISEL1: {
        float *a = exec_eval(g, s.a, v); float *b = exec_eval(g, s.b, v); float *c = exec_eval(g, s.c, v);
        o = g_alloc(g, n);
        LAUNCH(k_isel1, n, o, a, b, c, n, s.f0);
        break;
    }
    case MCGK_ISEL: {
        if (s.narr > 8) { fail_(g, "interval_select с более чем 8 ветками"); break; }
        float *a = exec_eval(g, s.a, v);
        PtrArr8 bs; memset(&bs, 0, sizeof bs);
        for (int k = 0; k < s.narr; k++) bs.p[k] = exec_eval(g, g->vol->arr[s.arr0 + k], v);
        o = g_alloc(g, n);
        LAUNCH(k_isel, n, P, s.thr0, s.nthr, s.narr, o, a, bs, n);
        break;
    }
    case MCGK_SPLINE: {
        if (s.narr > 8) { fail_(g, "сплайн с более чем 8 координатами"); break; }
        SplArgs A; memset(&A, 0, sizeof A);
        for (int k = 0; k < s.narr; k++) A.c[k] = exec_eval(g, g->vol->arr[s.arr0 + k], v);    /* жадно: все координаты (значения чистые) */
        o = g_alloc(g, n);
        int opi = (int)g->splops.size(); g->splops.push_back(SplOp{ ni, v });
        if (!g->dry && opi >= g->flags_cap_ops) { fail_(g, "слишком много сплайнов в пакете"); break; }
        LAUNCH(k_spline, n, P, s.sp, A, o, n, g->d_flags + (size_t)opi * g->Bmax);
        break;
    }
    case MCGK_CACHE: case MCGK_BLEND_DENSITY: o = exec_eval(g, s.a, v); break;
    case MCGK_INTERP: {
        DVol cv; int mode = interp_plan(s, v, &cv);
        if (mode < 0) { fail_(g, "интерполяция: неподдержанная геометрия объёма"); break; }
        if (mode == 0) { o = exec_eval(g, s.a, v); break; }
        float *cb = exec_eval(g, s.a, cv);
        o = g_alloc(g, n);
        InterpGeom G; G.cxz = s.i0; G.cy = s.i1; G.f0 = s.f0; G.f1 = s.f1; G.cv_sx = cv.sx; G.cv_sy = cv.sy; G.cv_sz = cv.sz; G.mnx = 0; G.mny = 0; G.mnz = 0;
        LAUNCH(k_interp, n, v, G, g->d_cx0, g->d_cz0, o, cb, n, dv_size(cv));
        break;
    }
    case MCGK_SLICE_X: case MCGK_SLICE_Y: case MCGK_SLICE_Z: case MCGK_SLICE_XZ: {
        DVol iv; int axis = 0; int mode = slice_plan(s, v, &iv, &axis);
        if (mode < 0) { fail_(g, "slice: неподдержанная геометрия объёма"); break; }
        if (mode == 0) { o = exec_eval(g, s.a, v); break; }
        float *ib = exec_eval(g, s.a, iv);
        o = g_alloc(g, n);
        LAUNCH(k_slice, n, axis, o, ib, v, n, dv_size(iv));
        break;
    }
    default: {
        char m[100]; snprintf(m, sizeof m, "узел вида %d не поддержан объёмным исполнителем", s.k); fail_(g, m); break;
    }
    }
    return o ? o : g->slab;
}
static float *exec_eval(GTerrain *g, int ni, const DVol &v) {
    if (g->fail) return g->slab;
    if (float *m = memo_get(g, ni, v)) return m;
    int saved = g->cur_node; g->cur_node = ni;
    float *o = exec_node(g, ni, v);
    g->cur_node = saved;
    MemoEnt e; e.node = ni; e.v = v; e.p = o; g->memo.push_back(e);
    return o;
}

/* ---- трасса состояния кэшей одного чанка: порядок посещений s_volume у CPU (без запоминания; кэш — ячейка «последний объём»).
 * cells[cid] — последний объём кэша; посещение с тем же объёмом — попадание: потомки не посещаются (как K_CACHE в df_new.c).
 * Координаты сплайна CPU считает лениво: здесь посещаются только те, что реально использовались (маска из ядра k_spline). */
struct TCell { bool set; DVol v; };
static unsigned spline_mask(GTerrain *g, int chunk, int node, const DVol &v, bool *found) {
    for (size_t i = 0; i < g->splops.size(); i++) if (g->splops[i].node == node && dv_eq(g->splops[i].v, v)) { *found = true; return g->h_flags[i * (size_t)g->Bmax + (size_t)chunk]; }
    *found = false; return 0;
}
static void trace(GTerrain *g, int chunk, bool lazy, std::vector<TCell> &cells, int ni, const DVol &v, bool *ok) {
    if (!*ok || ni < 0) return;
    const McgNode &s = g->vol->nodes[ni];
    auto rec = [&](int child, const DVol &vv) { trace(g, chunk, lazy, cells, child, vv, ok); };
    switch (s.k) {
    case MCGK_CONST: case MCGK_CTX_ALPHA: case MCGK_CTX_OFFSET: case MCGK_CTX_BEARD: case MCGK_NOISE: case MCGK_SHIFT_B: case MCGK_END: case MCGK_DIST:
    case MCGK_GRAD_CLAMP: case MCGK_GRAD_REPEAT: case MCGK_GRAD_MIRROR: break;
    case MCGK_NOISE_XZ: case MCGK_NOISE_XYZ: rec(s.a, v); if (s.k == MCGK_NOISE_XYZ) rec(s.b, v); rec(s.c, v); break;
    case MCGK_ABS: case MCGK_SQUARE: case MCGK_CUBE: case MCGK_SQRT: case MCGK_LEAKY: case MCGK_RECIP: case MCGK_NEG: case MCGK_SQUEEZE: case MCGK_SIGN:
    case MCGK_CADD: case MCGK_CSUB: case MCGK_CMUL: case MCGK_CDIV: case MCGK_CMIN: case MCGK_CMAX: case MCGK_CLAMP: case MCGK_RANGE_C: case MCGK_BLEND_DENSITY:
        rec(s.a, v); break;
    case MCGK_ADD: case MCGK_SUB: case MCGK_MUL: case MCGK_DIV: case MCGK_MIN: case MCGK_MAX: rec(s.a, v); rec(s.b, v); break;
    case MCGK_LERP: rec(s.a, v); rec(s.b, v); rec(s.c, v); break;
    case MCGK_LERP_CF: rec(s.a, v); rec(s.c, v); break;
    case MCGK_LERP_CS: rec(s.a, v); rec(s.b, v); break;
    case MCGK_RANGE: rec(s.b, v); rec(s.a, v); rec(s.c, v); break;
    case MCGK_ISEL1: rec(s.a, v); rec(s.b, v); rec(s.c, v); break;
    case MCGK_ISEL: rec(s.a, v); for (int k = 0; k < s.narr; k++) rec(g->vol->arr[s.arr0 + k], v); break;
    case MCGK_SPLINE: {
        unsigned mask = 0xFFFFFFFFu; bool found = false;
        if (lazy) { mask = spline_mask(g, chunk, ni, v, &found); if (!found) { *ok = false; break; } }
        for (int k = 0; k < s.narr; k++) if (mask & (1u << k)) rec(g->vol->arr[s.arr0 + k], v);
        break;
    }
    case MCGK_CACHE: {
        if (s.cid >= (int)cells.size()) cells.resize((size_t)s.cid + 1, TCell{ false, DVol{} });
        TCell &c = cells[s.cid];
        if (c.set && dv_eq(c.v, v)) break;
        c.set = true; c.v = v;
        rec(s.a, v);
        break;
    }
    case MCGK_INTERP: {
        DVol cv; int mode = interp_plan(s, v, &cv);
        if (mode < 0) { *ok = false; break; }
        rec(s.a, mode == 0 ? v : cv);
        break;
    }
    case MCGK_SLICE_X: case MCGK_SLICE_Y: case MCGK_SLICE_Z: case MCGK_SLICE_XZ: {
        DVol iv; int axis = 0; int mode = slice_plan(s, v, &iv, &axis);
        if (mode < 0) { *ok = false; break; }
        rec(s.a, mode == 0 ? v : iv);
        break;
    }
    case MCGK_FTS: rec(s.b, v); break;          /* find_top_surface: объём считает только вход b; a — точечно (кэш-ячейки не меняет) */
    default: *ok = false; break;
    }
}

/* ======================================================================= API */
struct GBatchOut { float *dens; unsigned short *veins; float *cells; };

static void gt_release(GTerrain *g) {
    if (!g) return;
    if (g->prof) {
        prof_flush(g);
        std::vector<std::pair<double, std::string>> v;
        double tot = 0; for (auto &kv : g->pagg) { v.push_back({ kv.second.first, kv.first + " ×" + std::to_string(kv.second.second) }); tot += kv.second.first; }
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
        fprintf(stderr, "[mcgpu] фазы пакетов (%lld пакетов, %lld чанков): план/запуск %.1f, ядра жил %.1f, D2H плотность %.1f, D2H жилы %.1f, трасса+ячейки %.1f, синхр. %.1f мс\n", g->nbatches, g->nchunks_done, g->ph[0], g->ph[1], g->ph[2], g->ph[3], g->ph[4], g->ph[5]);
        fprintf(stderr, "[mcgpu] профиль ядер, всего %.1f мс:\n", tot);
        for (size_t i = 0; i < v.size() && i < 14; i++) fprintf(stderr, "  %8.2f мс  %s\n", v[i].first, v[i].second.c_str());
    }
    g->gvol.release(); g->ggap.release();
    cudaFree(g->slab); cudaFree(g->d_cx0); cudaFree(g->d_cz0); cudaFree(g->d_veins); cudaFree(g->d_flags);
    prog_free_clone(g->vol); prog_free_clone(g->gap);
}

/* один прогон плана для пакета из B чанков; dry — только подсчёт арены */
static bool run_plan(GTerrain *g, int B, bool dry) {
    g->B = B; g->dry = dry; g->fail = false; g->fail_msg[0] = 0; g->used = 0; g->memo.clear(); g->splops.clear();
    if (!dry && g->d_flags && g->flags_cap_ops) cudaMemset(g->d_flags, 0, sizeof(unsigned) * (size_t)g->flags_cap_ops * (size_t)g->Bmax);
    float *dens = exec_eval(g, g->vol->root[g->T.root_density], g->vfull);
    (void)dens;
    for (int r = 0; r < g->T.nveins && g->T.veins_on; r++) {
        exec_eval(g, g->vol->root[g->T.vein[r].density_root], g->vfull);
        exec_eval(g, g->vol->root[g->T.vein[r].richness_root], g->vfull);
    }
    return !g->fail;
}

MCGPU_EXPORT void *mcgpu_terrain_new(const McgTerrainWorld *T, int max_chunks, char *err, size_t errlen) {
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    if (!mcgpu_ensure_init()) { if (err && errlen) snprintf(err, errlen, "%s", mcgpu_get_error()); return nullptr; }
    GTerrain *g = new GTerrain();
    g->T = *T;
    g->prof = getenv("MCGPU_PROFILE") != nullptr;
    g->vol = prog_clone(T->vol); g->gap = T->veins_on && T->gap ? prog_clone(T->gap) : nullptr;
    auto bad = [&](const char *msg) -> void * { if (err && errlen) snprintf(err, errlen, "%s", msg); gt_release(g); delete g; return nullptr; };
    if (T->vol->old) return bad("объёмный исполнитель поддерживает только ветку 26.3+");
    for (int i = 0; i < g->vol->nnodes; i++) if (g->vol->nodes[i].k >= MCGK__COUNT) return bad("неизвестный вид узла");
    if (!g->gvol.upload(g->vol)) return bad(mcgpu_get_error());
    if (g->gap && !g->ggap.upload(g->gap)) return bad(mcgpu_get_error());
    if (g->gap && g->gap->nnodes > MCG_MAX_VAL) return bad("программа «щели» слишком велика");
    g->vfull = DVol{ 16, T->nh, 16, 1, 1, 1, T->nmin, 1, 0, 1, 0 };
    g->T.vol = g->vol; g->T.gap = g->gap;
    /* сухой прогон: арена на один чанк; заодно — кандидаты объёмов для кэшей, нужных CPU, и число сплайнов */
    if (!run_plan(g, 1, true)) { char m[260]; snprintf(m, sizeof m, "план исполнения: %s", g->fail_msg); return bad(m); }
    g->per_chunk_floats = g->used;
    g->flags_cap_ops = (int)g->splops.size() + 4;
    size_t off = 0;
    for (int k = 0; k < T->ncache_ids; k++) {
        CellInfo ci; ci.cid = T->cache_ids[k]; ci.cap = 0; ci.off = 0;
        for (const MemoEnt &m : g->memo) {
            const McgNode &sn = g->vol->nodes[m.node];
            if (sn.k != MCGK_CACHE || sn.cid != ci.cid) continue;
            bool have = false; for (const DVol &c : ci.cand) if (dv_eq(c, m.v)) have = true;
            if (!have) { ci.cand.push_back(m.v); ci.cap = std::max<size_t>(ci.cap, (size_t)dv_size(m.v)); }
        }
        ci.off = off; off += ci.cap;
        g->cells.push_back(ci);
    }
    g->cell_floats = off;
    /* трасса должна проходить на всех сплайнах: проверка геометрии (ленивая трасса требует масок, здесь — только проверка) */
    { std::vector<TCell> tc; bool ok = true;
      if (T->pre_root >= 0) trace(g, 0, false, tc, g->vol->root[T->pre_root], DVol{ T->pre_sx, T->pre_sy, T->pre_sz, T->pre_dx, T->pre_dy, T->pre_dz, T->pre_y0, 1, T->pre_xoff, 1, T->pre_zoff }, &ok);
      trace(g, 0, false, tc, g->vol->root[g->T.root_density], g->vfull, &ok);
      if (!ok) return bad("трасса кэшей: неподдержанная геометрия"); }
    /* размер пакета по свободной памяти */
    size_t fr = 0, tot = 0; cudaMemGetInfo(&fr, &tot);
    size_t budget = fr > ((size_t)1 << 30) ? std::min<size_t>(fr / 2, (size_t)2 << 30) : fr / 3;
    size_t per_chunk_bytes = g->per_chunk_floats * sizeof(float) + (size_t)T->nh * 256 * (sizeof(unsigned short));
    int B = (int)std::min<size_t>((size_t)max_chunks, std::max<size_t>(1, budget / per_chunk_bytes));
    if (B < 1) B = 1;
    g->Bmax = B;
    if (!run_plan(g, B, true)) return bad("сухой прогон пакета");
    g->slab_floats = g->used + 64;
    if (g->prof) fprintf(stderr, "[mcgpu] рельеф: узлов %d, шумов %d, октав %d, сплайнов-операций %d; на чанк %.1f МБ арены, пакет B=%d, арена %.0f МБ; кэши для CPU: %zu (ёмкость %zu float на чанк)\n", g->vol->nnodes, g->vol->nnoise, g->vol->noct, (int)g->splops.size(), g->per_chunk_floats * 4 / 1048576.0, B, g->slab_floats * 4 / 1048576.0, g->cells.size(), g->cell_floats);
    if (cudaMalloc((void **)&g->slab, g->slab_floats * sizeof(float)) != cudaSuccess) { cudaGetLastError(); return bad("нет памяти под арену"); }
    if (cudaMalloc((void **)&g->d_cx0, sizeof(int) * (size_t)B) != cudaSuccess || cudaMalloc((void **)&g->d_cz0, sizeof(int) * (size_t)B) != cudaSuccess) return bad("нет памяти");
    if (cudaMalloc((void **)&g->d_flags, sizeof(unsigned) * (size_t)g->flags_cap_ops * (size_t)B) != cudaSuccess) return bad("нет памяти под флаги сплайнов");
    if (T->veins_on && cudaMalloc((void **)&g->d_veins, sizeof(unsigned short) * (size_t)B * (size_t)T->nh * 256) != cudaSuccess) return bad("нет памяти под заплатки жил");
    return g;
}
MCGPU_EXPORT void mcgpu_terrain_free(void *h) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    mcgpu_ensure_init();
    GTerrain *g = (GTerrain *)h;
    gt_release(g); delete g;
}
MCGPU_EXPORT int mcgpu_terrain_max_chunks(void *h) { return ((GTerrain *)h)->Bmax; }
MCGPU_EXPORT size_t mcgpu_terrain_cell_floats(void *h) { return ((GTerrain *)h)->cell_floats; }
/* кэш k (в порядке McgTerrainWorld.cache_ids): номер, число кандидатов-объёмов, ёмкость и смещение буфера (на чанк) */
MCGPU_EXPORT int mcgpu_terrain_cell_info(void *h, int k, int *cid, int *ncand, size_t *cap, size_t *off) {
    GTerrain *g = (GTerrain *)h;
    if (k < 0 || k >= (int)g->cells.size()) return -1;
    const CellInfo &c = g->cells[(size_t)k];
    *cid = c.cid; *ncand = (int)c.cand.size(); *cap = c.cap; *off = c.off;
    return 0;
}
/* кандидат j кэша k: объём (x, z — относительно чанка при xrel/zrel) */
MCGPU_EXPORT int mcgpu_terrain_cell_cand(void *h, int k, int j, int vol[9], int *xrel, int *zrel, int *n) {
    GTerrain *g = (GTerrain *)h;
    if (k < 0 || k >= (int)g->cells.size() || j < 0 || j >= (int)g->cells[(size_t)k].cand.size()) return -1;
    const DVol &v = g->cells[(size_t)k].cand[(size_t)j];
    vol[0] = v.sx; vol[1] = v.sy; vol[2] = v.sz; vol[3] = v.xoff; vol[4] = v.y0; vol[5] = v.zoff; vol[6] = v.dx; vol[7] = v.dy; vol[8] = v.dz;
    *xrel = v.xrel; *zrel = v.zrel; *n = dv_size(v);
    return 0;
}
MCGPU_EXPORT void *mcgpu_host_alloc(size_t bytes) {
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    if (!mcgpu_ensure_init()) return nullptr;
    void *p = nullptr;
    if (cudaHostAlloc(&p, bytes, cudaHostAllocPortable) != cudaSuccess) { cudaGetLastError(); return nullptr; }
    return p;
}
MCGPU_EXPORT void mcgpu_host_free(void *p) { if (p) { std::lock_guard<std::mutex> lk(mcgpu_mutex()); mcgpu_ensure_init(); cudaFreeHost(p); } }

/* пакет: n ≤ Bmax чанков (cx[i], cz[i] — индексы чанков). out_dens: n × (16·nh·16) float; out_veins: n × (nh·256) u16 (или NULL);
 * out_cells: n × cell_floats float и out_which: n × ncache_ids int8 (или NULL): какой кандидат-объём кэша CPU оставил бы после
 * плотности (−1 — ячейка не выставлена, −2 — состояние неизвестно: чанк считать на CPU). 0 — успех. */
MCGPU_EXPORT int mcgpu_terrain_batch(void *h, int n, const int *cx, const int *cz, McgBeardFn beard_cb, void *beard_ud, float *out_dens, unsigned short *out_veins, float *out_cells, signed char *out_which) {
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    GTerrain *g = (GTerrain *)h;
    if (!g || n < 1 || n > g->Bmax) { mcgpu_set_error("неверный размер пакета"); return 1; }
    if (!mcgpu_ensure_init()) return 1;
    g->h_cx0.assign((size_t)n, 0); g->h_cz0.assign((size_t)n, 0);
    for (int i = 0; i < n; i++) { g->h_cx0[(size_t)i] = cx[i] * 16; g->h_cz0[(size_t)i] = cz[i] * 16; }
    cudaMemcpy(g->d_cx0, g->h_cx0.data(), sizeof(int) * (size_t)n, cudaMemcpyHostToDevice);
    cudaMemcpy(g->d_cz0, g->h_cz0.data(), sizeof(int) * (size_t)n, cudaMemcpyHostToDevice);
    g->beard_cb = beard_cb; g->beard_ud = beard_ud;
    double t0 = now_ms();
    bool ok = run_plan(g, n, false);
    if (g->prof) { cudaDeviceSynchronize(); g->ph[0] += now_ms() - t0; g->nbatches++; g->nchunks_done += n; }
    if (!ok) { mcgpu_set_error("%s", g->fail_msg); g->beard_cb = nullptr; return 2; }
    const int nd = dv_size(g->vfull);
    float *dens = memo_get(g, g->vol->root[g->T.root_density], g->vfull);
    cudaError_t e = cudaSuccess;
    if (g->T.veins_on && out_veins) {
        VeinArgs A; memset(&A, 0, sizeof A); A.n = g->T.nveins;
        for (int r = 0; r < g->T.nveins; r++) {
            A.dens[r] = memo_get(g, g->vol->root[g->T.vein[r].density_root], g->vfull);
            A.rich[r] = memo_get(g, g->vol->root[g->T.vein[r].richness_root], g->vfull);
            A.ore[r] = g->T.vein[r].ore; A.raw[r] = g->T.vein[r].raw; A.filler[r] = g->T.vein[r].filler; A.raw_chance[r] = g->T.vein[r].raw_chance;
            A.gap_root[r] = g->gap->root[g->T.vein[r].gap_root];
        }
        k_veins<<<grid_for(nd, n), THREADS>>>(g->ggap.d, A, g->vfull, g->d_cx0, g->d_cz0, g->d_veins, nd, g->T.ore_lo, g->T.ore_hi);
    }
    double t1 = now_ms();
    e = cudaGetLastError();
    if (g->prof) { cudaDeviceSynchronize(); g->ph[1] += now_ms() - t1; }
    t1 = now_ms();
    if (e == cudaSuccess) e = cudaMemcpy(out_dens, dens, sizeof(float) * (size_t)n * (size_t)nd, cudaMemcpyDeviceToHost);
    if (g->prof) { g->ph[2] += now_ms() - t1; t1 = now_ms(); }
    if (e == cudaSuccess && g->T.veins_on && out_veins) e = cudaMemcpy(out_veins, g->d_veins, sizeof(unsigned short) * (size_t)n * (size_t)nd, cudaMemcpyDeviceToHost);
    if (g->prof) { g->ph[3] += now_ms() - t1; t1 = now_ms(); }
    if (e == cudaSuccess && out_cells && out_which && !g->cells.empty()) {
        size_t nops = g->splops.size();
        g->h_flags.assign(std::max<size_t>(1, nops) * (size_t)g->Bmax, 0u);
        if (nops) e = cudaMemcpy(g->h_flags.data(), g->d_flags, sizeof(unsigned) * nops * (size_t)g->Bmax, cudaMemcpyDeviceToHost);
        std::vector<GatherItem> items;
        for (int c = 0; c < n && e == cudaSuccess; c++) {
            std::vector<TCell> tc; bool tok = true;
            if (g->T.pre_root >= 0) trace(g, c, false, tc, g->vol->root[g->T.pre_root], DVol{ g->T.pre_sx, g->T.pre_sy, g->T.pre_sz, g->T.pre_dx, g->T.pre_dy, g->T.pre_dz, g->T.pre_y0, 1, g->T.pre_xoff, 1, g->T.pre_zoff }, &tok);
            std::vector<TCell> t1 = tc;       /* состояние после aq_init (его CPU делает сам) */
            trace(g, c, true, tc, g->vol->root[g->T.root_density], g->vfull, &tok);
            for (size_t k = 0; k < g->cells.size() && e == cudaSuccess; k++) {
                const CellInfo &ci = g->cells[k];
                signed char which = -1;
                if (!tok) which = -2;
                else if (ci.cid < (int)tc.size() && tc[(size_t)ci.cid].set) {
                    bool before = ci.cid < (int)t1.size() && t1[(size_t)ci.cid].set && dv_eq(t1[(size_t)ci.cid].v, tc[(size_t)ci.cid].v);
                    if (!before) {          /* плотность изменила ячейку: её содержимое должно прийти с GPU */
                        which = -2;
                        for (size_t j = 0; j < ci.cand.size(); j++) if (dv_eq(ci.cand[j], tc[(size_t)ci.cid].v)) which = (signed char)j;
                    }
                }
                out_which[(size_t)c * g->cells.size() + k] = which;
                if (which < 0) continue;
                const DVol &cv = ci.cand[(size_t)which]; int nn = dv_size(cv);
                float *p = nullptr;
                for (const MemoEnt &m : g->memo) { const McgNode &sn = g->vol->nodes[m.node]; if (sn.k == MCGK_CACHE && sn.cid == ci.cid && dv_eq(m.v, cv)) { p = m.p; break; } }
                if (!p) { which = -2; out_which[(size_t)c * g->cells.size() + k] = which; continue; }
                items.push_back(GatherItem{ p + (size_t)c * nn, (size_t)c * g->cell_floats + ci.off, nn });
            }
        }
        if (e == cudaSuccess && !items.empty()) {
            GatherItem *d_items = nullptr; float *d_out = nullptr;
            if (cudaMalloc((void **)&d_items, sizeof(GatherItem) * items.size()) != cudaSuccess || cudaMalloc((void **)&d_out, sizeof(float) * (size_t)n * g->cell_floats) != cudaSuccess) e = cudaErrorMemoryAllocation;
            else {
                cudaMemset(d_out, 0, sizeof(float) * (size_t)n * g->cell_floats);
                cudaMemcpy(d_items, items.data(), sizeof(GatherItem) * items.size(), cudaMemcpyHostToDevice);
                k_gather<<<dim3(2, (unsigned)items.size()), 128>>>(d_items, (int)items.size(), d_out, 0);
                e = cudaGetLastError();
                if (e == cudaSuccess) e = cudaMemcpy(out_cells, d_out, sizeof(float) * (size_t)n * g->cell_floats, cudaMemcpyDeviceToHost);
            }
            cudaFree(d_items); cudaFree(d_out);
        }
    }
    if (g->prof) { g->ph[4] += now_ms() - t1; t1 = now_ms(); }
    if (e == cudaSuccess) e = cudaDeviceSynchronize();
    if (g->prof) g->ph[5] += now_ms() - t1;
    prof_flush(g);
    g->beard_cb = nullptr;
    if (e != cudaSuccess) { mcgpu_set_error("terrain: %s", cudaGetErrorString(e)); cudaGetLastError(); return 3; }
    return 0;
}

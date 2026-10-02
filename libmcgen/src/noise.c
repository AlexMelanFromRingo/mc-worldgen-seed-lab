/* noise.c — шумы и интервалы (см. noise.h). Алгоритмы — как в synth/* игры; арифметика повторяет порядок операций Java. */
#include "noise.h"
#include "json.h"
#include "mc_end.h"
#include <stdlib.h>
#include <stdio.h>

/* ======================================================================= RandomSource */
Rnd rnd_xoro_seed(i64 seed) { Rnd r; memset(&r, 0, sizeof r); r.legacy = 0; r.x = xoro_from_long_seed(seed); return r; }
Rnd rnd_legacy_seed(i64 seed) { Rnd r; memset(&r, 0, sizeof r); r.legacy = 1; r.l = lcg_new(seed); return r; }
i64 rnd_next_long(Rnd *r) { return r->legacy ? lcg_next_long(&r->l) : (i64)xoro_next_long(&r->x); }
i32 rnd_next_int_bound(Rnd *r, i32 bound) { return r->legacy ? lcg_next_int_bound(&r->l, bound) : xoro_next_int_bound(&r->x, bound); }
double rnd_next_double(Rnd *r) { return r->legacy ? lcg_next_double(&r->l) : xoro_next_double(&r->x); }
float rnd_next_float(Rnd *r) { return r->legacy ? lcg_next_float(&r->l) : xoro_next_float(&r->x); }
void rnd_consume(Rnd *r, int n) { if (r->legacy) lcg_skip(&r->l, n); else xoro_skip(&r->x, n); }
PosRnd rnd_fork_positional(Rnd *r) {
    PosRnd p; memset(&p, 0, sizeof p);
    if (r->legacy) { p.legacy = 1; p.seed = lcg_next_long(&r->l); }
    else { p.legacy = 0; p.lo = xoro_next_long(&r->x); p.hi = xoro_next_long(&r->x); }
    return p;
}
Rnd pos_from_hash(const PosRnd *p, const char *name) {
    if (p->legacy) return rnd_legacy_seed((i64)java_str_hash(name) ^ p->seed);
    u64 lo, hi; mc_md5_seed128(name, &lo, &hi);
    Rnd r; memset(&r, 0, sizeof r); r.x = xoro_new(lo ^ p->lo, hi ^ p->hi); return r;
}
Rnd pos_at(const PosRnd *p, i32 x, i32 y, i32 z) {
    if (p->legacy) return rnd_legacy_seed(mc_get_seed_xyz(x, y, z) ^ p->seed);
    Rnd r; memset(&r, 0, sizeof r); r.x = xoro_new((u64)mc_get_seed_xyz(x, y, z) ^ p->lo, p->hi); return r;
}

/* ======================================================================= одна октава */
void gn_init(GNoise *n, Rnd *r, double scale) {
    n->xo = rnd_next_double(r) * scale;
    n->yo = rnd_next_double(r) * scale;
    n->zo = rnd_next_double(r) * scale;
    for (int i = 0; i < 256; i++) n->p[i] = (u8)i;
    for (int i = 0; i < 256; i++) {
        int off = rnd_next_int_bound(r, 256 - i);
        u8 t = n->p[i]; n->p[i] = n->p[i + off]; n->p[i + off] = t;
    }
}

static const signed char GRAD[16][3] = {
    {1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
    {0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1},{1,1,0},{0,-1,1},{-1,1,0},{0,-1,-1}};

static inline int perm(const GNoise *n, int x) { return n->p[x & 0xFF]; }
static inline float smooth_f(float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
static inline double smooth_d(double x) { return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
static inline float dot_f(int h, float x, float y, float z) { const signed char *g = GRAD[h & 15]; return (float)g[0] * x + (float)g[1] * y + (float)g[2] * z; }
static inline double dot_d(int h, double x, double y, double z) { const signed char *g = GRAD[h & 15]; return (double)g[0] * x + (double)g[1] * y + (double)g[2] * z; }
static inline float lerp3f(float a1, float a2, float a3, float x000, float x100, float x010, float x110, float x001, float x101, float x011, float x111) {
    return jm_lerpf(a3, jm_lerpf(a2, jm_lerpf(a1, x000, x100), jm_lerpf(a1, x010, x110)),
                        jm_lerpf(a2, jm_lerpf(a1, x001, x101), jm_lerpf(a1, x011, x111)));
}
static inline double lerp3d(double a1, double a2, double a3, double x000, double x100, double x010, double x110, double x001, double x101, double x011, double x111) {
    return jm_lerp(a3, jm_lerp(a2, jm_lerp(a1, x000, x100), jm_lerp(a1, x010, x110)),
                       jm_lerp(a2, jm_lerp(a1, x001, x101), jm_lerp(a1, x011, x111)));
}
/* GradientNoise.wrap (26.3): быстрый путь для |x| < nextDown(2^24) */
static inline double wrap_new(double x) {
    const double HALF = 16777215.999999998;
    return (x >= -HALF && x < HALF) ? x : x - floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}
/* PerlinNoise.wrap (26.1/26.2) */
static inline double wrap_old(double x) { return x - (double)jm_lfloor(x / 3.3554432E7 + 0.5) * 3.3554432E7; }

static float sample_lerp_f(const GNoise *n, int x, int y, int z, float rx, float ry, float rz, float ry_orig) {
    int x0 = perm(n, x), x1 = perm(n, x + 1);
    int xy00 = perm(n, x0 + y), xy01 = perm(n, x0 + y + 1), xy10 = perm(n, x1 + y), xy11 = perm(n, x1 + y + 1);
    float d000 = dot_f(perm(n, xy00 + z), rx, ry, rz);
    float d100 = dot_f(perm(n, xy10 + z), rx - 1.0f, ry, rz);
    float d010 = dot_f(perm(n, xy01 + z), rx, ry - 1.0f, rz);
    float d110 = dot_f(perm(n, xy11 + z), rx - 1.0f, ry - 1.0f, rz);
    float d001 = dot_f(perm(n, xy00 + z + 1), rx, ry, rz - 1.0f);
    float d101 = dot_f(perm(n, xy10 + z + 1), rx - 1.0f, ry, rz - 1.0f);
    float d011 = dot_f(perm(n, xy01 + z + 1), rx, ry - 1.0f, rz - 1.0f);
    float d111 = dot_f(perm(n, xy11 + z + 1), rx - 1.0f, ry - 1.0f, rz - 1.0f);
    return lerp3f(smooth_f(rx), smooth_f(ry_orig), smooth_f(rz), d000, d100, d010, d110, d001, d101, d011, d111);
}

float gn_perlin_get_f(const GNoise *n, double _x, double _y, double _z) {
    double x = wrap_new(_x) + n->xo, y = wrap_new(_y) + n->yo, z = wrap_new(_z) + n->zo;
    i32 fx = jm_floor_d(x), fy = jm_floor_d(y), fz = jm_floor_d(z);
    float rx = (float)(x - fx), ry = (float)(y - fy), rz = (float)(z - fz);
    return sample_lerp_f(n, fx, fy, fz, rx, ry, rz, ry);
}

static inline double fudge_y(double original_y, double rel_y, double scale) {
    double lim = (original_y >= 0.0 && original_y < rel_y) ? original_y : rel_y;
    return (double)jm_floor_d(lim / scale + (double)1.0E-7f) * scale;
}

float gn_smeared_get_f(const GNoise *n, double fs, double _x, double _y, double _z) {
    double x = wrap_new(_x) + n->xo, y = wrap_new(_y) + n->yo, z = wrap_new(_z) + n->zo;
    i32 fx = jm_floor_d(x), fy = jm_floor_d(y), fz = jm_floor_d(z);
    float rx = (float)(x - fx);
    double ry = y - fy;
    float rz = (float)(z - fz);
    float fry = (float)(ry - fudge_y(_y, ry, fs));
    return sample_lerp_f(n, fx, fy, fz, rx, fry, rz, (float)ry);
}

double gn_noise_d(const GNoise *n, double _x, double _y, double _z, double y_scale, double y_fudge) {
    double x = _x + n->xo, y = _y + n->yo, z = _z + n->zo;
    i32 xf = jm_floor_d(x), yf = jm_floor_d(y), zf = jm_floor_d(z);
    double xr = x - xf, yr = y - yf, zr = z - zf;
    double yrf = 0.0;
    if (y_scale != 0.0) {
        double lim = (y_fudge >= 0.0 && y_fudge < yr) ? y_fudge : yr;
        yrf = (double)jm_floor_d(lim / y_scale + (double)1.0E-7f) * y_scale;
    }
    double yy = yr - yrf;
    int x0 = perm(n, xf), x1 = perm(n, xf + 1);
    int xy00 = perm(n, x0 + yf), xy01 = perm(n, x0 + yf + 1), xy10 = perm(n, x1 + yf), xy11 = perm(n, x1 + yf + 1);
    double d000 = dot_d(perm(n, xy00 + zf), xr, yy, zr);
    double d100 = dot_d(perm(n, xy10 + zf), xr - 1.0, yy, zr);
    double d010 = dot_d(perm(n, xy01 + zf), xr, yy - 1.0, zr);
    double d110 = dot_d(perm(n, xy11 + zf), xr - 1.0, yy - 1.0, zr);
    double d001 = dot_d(perm(n, xy00 + zf + 1), xr, yy, zr - 1.0);
    double d101 = dot_d(perm(n, xy10 + zf + 1), xr - 1.0, yy, zr - 1.0);
    double d011 = dot_d(perm(n, xy01 + zf + 1), xr, yy - 1.0, zr - 1.0);
    double d111 = dot_d(perm(n, xy11 + zf + 1), xr - 1.0, yy - 1.0, zr - 1.0);
    return lerp3d(smooth_d(xr), smooth_d(yr), smooth_d(zr), d000, d100, d010, d110, d001, d101, d011, d111);
}

/* ======================================================================= объёмы */
int vol_index_of_block(const Vol *v, int bx, int by, int bz) {
    int rx = bx - v->x0, ry = by - v->y0, rz = bz - v->z0;
    if (v->dx == 1 && v->dy == 1 && v->dz == 1) {
        if (rx >= 0 && ry >= 0 && rz >= 0 && rx < v->sx && ry < v->sy && rz < v->sz) return vol_idx(v, rx, ry, rz);
        return -1;
    }
    if (rx >= 0 && ry >= 0 && rz >= 0 && rx < v->sx * v->dx && ry < v->sy * v->dy && rz < v->sz * v->dz &&
        jm_floormod(rx, v->dx) == 0 && jm_floormod(ry, v->dy) == 0 && jm_floormod(rz, v->dz) == 0)
        return vol_idx(v, jm_floordiv(rx, v->dx), jm_floordiv(ry, v->dy), jm_floordiv(rz, v->dz));
    return -1;
}

/* общий цикл PerlinNoise/SmearedPerlinNoise.addToVolume; smeared — с поправкой y */
static void add_volume_impl(const GNoise *n, int smeared, double fs, float *buf, const Vol *v, double xz_scale, double y_scale, float amp) {
    float d000xz = 0, d100xz = 0, d010xz = 0, d110xz = 0, d001xz = 0, d101xz = 0, d011xz = 0, d111xz = 0;
    float g000y = 0, g100y = 0, g010y = 0, g110y = 0, g001y = 0, g101y = 0, g011y = 0, g111y = 0;
    int index = 0;
    for (int iz = 0; iz < v->sz; iz++) {
        double z = wrap_new(vol_bz(v, iz) * xz_scale) + n->zo;
        i32 fz = jm_floor_d(z);
        float rz = (float)(z - fz);
        float az = smooth_f(rz);
        for (int ix = 0; ix < v->sx; ix++) {
            double x = wrap_new(vol_bx(v, ix) * xz_scale) + n->xo;
            i32 fx = jm_floor_d(x);
            float rx = (float)(x - fx);
            int x0 = perm(n, fx), x1 = perm(n, fx + 1);
            float ax = smooth_f(rx);
            i32 last_fy = (i32)0x80000000u;
            for (int iy = 0; iy < v->sy; iy++) {
                double oy = vol_by(v, iy) * y_scale;
                double y = wrap_new(oy) + n->yo;
                i32 fy = jm_floor_d(y);
                float ry, ay;
                double ryd = y - fy;
                if (!smeared) { ry = (float)ryd; ay = smooth_f(ry); }
                else { ay = smooth_f((float)ryd); ry = 0; }
                if (last_fy != fy) {
                    int xy00 = perm(n, x0 + fy), xy01 = perm(n, x0 + fy + 1), xy10 = perm(n, x1 + fy), xy11 = perm(n, x1 + fy + 1);
                    const signed char *g;
                    g = GRAD[perm(n, xy00 + fz) & 15];     d000xz = (float)g[0] * rx + (float)g[2] * rz;                   g000y = (float)g[1];
                    g = GRAD[perm(n, xy10 + fz) & 15];     d100xz = (float)g[0] * (rx - 1.0f) + (float)g[2] * rz;          g100y = (float)g[1];
                    g = GRAD[perm(n, xy01 + fz) & 15];     d010xz = (float)g[0] * rx + (float)g[2] * rz;                   g010y = (float)g[1];
                    g = GRAD[perm(n, xy11 + fz) & 15];     d110xz = (float)g[0] * (rx - 1.0f) + (float)g[2] * rz;          g110y = (float)g[1];
                    g = GRAD[perm(n, xy00 + fz + 1) & 15]; d001xz = (float)g[0] * rx + (float)g[2] * (rz - 1.0f);          g001y = (float)g[1];
                    g = GRAD[perm(n, xy10 + fz + 1) & 15]; d101xz = (float)g[0] * (rx - 1.0f) + (float)g[2] * (rz - 1.0f); g101y = (float)g[1];
                    g = GRAD[perm(n, xy01 + fz + 1) & 15]; d011xz = (float)g[0] * rx + (float)g[2] * (rz - 1.0f);          g011y = (float)g[1];
                    g = GRAD[perm(n, xy11 + fz + 1) & 15]; d111xz = (float)g[0] * (rx - 1.0f) + (float)g[2] * (rz - 1.0f); g111y = (float)g[1];
                    last_fy = fy;
                }
                if (smeared) ry = (float)(ryd - fudge_y(oy, ryd, fs));
                buf[index] += amp * lerp3f(ax, ay, az,
                    d000xz + g000y * ry, d100xz + g100y * ry, d010xz + g010y * (ry - 1.0f), d110xz + g110y * (ry - 1.0f),
                    d001xz + g001y * ry, d101xz + g101y * ry, d011xz + g011y * (ry - 1.0f), d111xz + g111y * (ry - 1.0f));
                index++;
            }
        }
    }
}
void gn_perlin_add_volume(const GNoise *n, float *buf, const Vol *v, double xz_scale, double y_scale, float amp) {
    add_volume_impl(n, 0, 0.0, buf, v, xz_scale, y_scale, amp);
}
void gn_smeared_add_volume(const GNoise *n, double fs, float *buf, const Vol *v, double xz_scale, double y_scale, float amp) {
    add_volume_impl(n, 1, fs, buf, v, xz_scale, y_scale, amp);
}

/* ======================================================================= Interval */
Ival iv_nai(void) { Ival r = { NAN, NAN, 1 }; return r; }
Ival iv_inf(void) { Ival r = { -INFINITY, INFINITY, 0 }; return r; }
Ival iv_of(float lo, float hi) {
    if (lo != lo || hi != hi || hi < lo) return iv_nai();   /* в Java — исключение; в данных не встречается */
    Ival r = { lo, hi, 0 }; return r;
}
int iv_contains(Ival a, float v) { return v >= a.lo && v <= a.hi; }
Ival iv_encaps(const Ival *v, int n) {
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < n; i++) if (!v[i].nai) { lo = jm_minf(v[i].lo, lo); hi = jm_maxf(v[i].hi, hi); }
    return hi < lo ? iv_nai() : iv_of(lo, hi);
}
Ival iv_encaps2(float a, float b) {
    if (a != a && b != b) return iv_nai();
    if (a != a) return iv_exact(b);
    if (b != b) return iv_exact(a);
    return iv_of(jm_minf(a, b), jm_maxf(a, b));
}
static Ival iv_encaps_if(Ival a, float b) {
    if (b != b) return a;
    return a.nai ? iv_exact(b) : iv_of(jm_minf(a.lo, b), jm_maxf(a.hi, b));
}
Ival iv_add(Ival a, Ival b) { float lo = a.lo + b.lo, hi = a.hi + b.hi; return (lo == lo && hi == hi) ? iv_of(lo, hi) : iv_nai(); }
Ival iv_sub(Ival a, Ival b) { float lo = a.lo - b.hi, hi = a.hi - b.lo; return (lo == lo && hi == hi) ? iv_of(lo, hi) : iv_nai(); }
static inline float mul_bound(float a, float b) { return (a != 0.0f && b != 0.0f) ? a * b : 0.0f; }
Ival iv_mul(Ival a, Ival b) {
    if (a.nai || b.nai) return iv_nai();
    float mm = mul_bound(a.lo, b.lo), mM = mul_bound(a.lo, b.hi), Mm = mul_bound(a.hi, b.lo), MM = mul_bound(a.hi, b.hi);
    return iv_of(jm_minf(jm_minf(mm, mM), jm_minf(Mm, MM)), jm_maxf(jm_maxf(mm, mM), jm_maxf(Mm, MM)));
}
Ival iv_reciprocal(Ival a) {
    if (a.nai || (a.lo == 0.0f && a.hi == 0.0f)) return iv_nai();
    if (!iv_contains(a, 0.0f)) return iv_of(1.0f / a.hi, 1.0f / a.lo);
    if (a.hi == 0.0f) return iv_of(-INFINITY, 1.0f / a.lo);
    return a.lo == 0.0f ? iv_of(1.0f / a.hi, INFINITY) : iv_inf();
}
Ival iv_div(Ival a, Ival b) { return iv_mul(a, iv_reciprocal(b)); }
Ival iv_min(Ival a, Ival b) { return (!a.nai && !b.nai) ? iv_of(jm_minf(a.lo, b.lo), jm_minf(a.hi, b.hi)) : iv_nai(); }
Ival iv_max(Ival a, Ival b) { return (!a.nai && !b.nai) ? iv_of(jm_maxf(a.lo, b.lo), jm_maxf(a.hi, b.hi)) : iv_nai(); }
Ival iv_clamp(Ival a, float lo, float hi) {
    if (a.nai) return iv_nai();
    if (a.lo >= hi) return iv_of(hi, hi);
    if (a.hi <= lo) return iv_of(lo, lo);
    return iv_of(jm_maxf(a.lo, lo), jm_minf(a.hi, hi));
}
Ival iv_abs(Ival a) {
    if (a.nai) return iv_nai();
    float mx = jm_maxf(fabsf(a.lo), fabsf(a.hi));
    return iv_contains(a, 0.0f) ? iv_of(0.0f, mx) : iv_of(jm_minf(fabsf(a.lo), fabsf(a.hi)), mx);
}
Ival iv_square(Ival a) {
    if (a.nai) return iv_nai();
    float mx = jm_maxf(a.lo * a.lo, a.hi * a.hi);
    return iv_contains(a, 0.0f) ? iv_of(0.0f, mx) : iv_of(jm_minf(a.lo * a.lo, a.hi * a.hi), mx);
}
static Ival pow_zero_base(Ival e) {
    if (iv_contains(e, 0.0f)) {
        if (e.hi == 0.0f) return iv_of(1.0f, INFINITY);
        return e.lo == 0.0f ? iv_of(0.0f, 1.0f) : iv_of(0.0f, INFINITY);
    }
    return e.hi < 0.0f ? iv_exact(INFINITY) : iv_exact(0.0f);
}
static Ival pow_inf_exp(float b, Ival e) {
    if (isinf(e.lo) && isinf(e.hi)) return iv_of(0.0f, INFINITY);
    if (isinf(e.lo)) return b < 1.0f ? iv_of((float)pow(b, e.hi), INFINITY) : iv_of(0.0f, (float)pow(b, e.hi));
    return b < 1.0f ? iv_of(0.0f, (float)pow(b, e.lo)) : iv_of((float)pow(b, e.lo), INFINITY);
}
static Ival pow_pos_base(float b, Ival e) {
    if (isfinite(e.lo) && isfinite(e.hi)) return iv_encaps2((float)pow(b, e.lo), (float)pow(b, e.hi));
    return pow_inf_exp(b, e);
}
static Ival pow_neg_base(float b, Ival e) {
    float emin = (float)ceil(e.lo), emax = (float)floor(e.hi);
    if (emax < emin) return iv_nai();
    float bmin = (float)pow(b, emin), bmax = (float)pow(b, emax);
    Ival r = iv_encaps2(bmin, bmax);
    if (isinf(emin)) r = iv_encaps_if(r, -bmin);
    else if (emin + 1.0f < emax) r = iv_encaps_if(r, (float)pow(b, emin + 1.0f));
    if (isinf(emax)) r = iv_encaps_if(r, -bmax);
    else if (emax - 1.0f > emin) r = iv_encaps_if(r, (float)pow(b, emax - 1.0f));
    return r;
}
static Ival pow_scalar(float b, Ival e) {
    if (b != b || e.nai) return iv_nai();
    if (e.lo == e.hi) { float v = (float)pow(b, e.lo); return v != v ? iv_nai() : iv_exact(v); }
    if (b == 0.0f) return iv_mul(pow_zero_base(e), iv_exact(copysignf(1.0f, b)));
    if (b == 1.0f) return iv_exact(1.0f);
    return b > 0.0f ? pow_pos_base(b, e) : pow_neg_base(b, e);
}
Ival iv_pow(Ival base, Ival e) {
    if (base.nai || e.nai) return iv_nai();
    if (base.lo == base.hi) return pow_scalar(base.lo, e);
    Ival p1 = pow_scalar(base.lo, e), p2 = pow_scalar(base.hi, e);
    Ival tmp[2] = { p1, p2 };
    Ival r = iv_encaps(tmp, 2);
    if (iv_contains(base, 0.0f)) {
        if (base.hi > 0.0f) { Ival t[2] = { r, pow_scalar(0.0f, e) }; r = iv_encaps(t, 2); }
        if (base.lo < 0.0f) { Ival t[2] = { r, pow_scalar(-0.0f, e) }; r = iv_encaps(t, 2); }
    }
    return r;
}
static float logf_j(float x) { return (float)log((double)x); }
Ival iv_map_monotonic(Ival a, FloatOp op) {
    if (a.nai) return iv_nai();
    float m1 = op(a.lo), m2 = op(a.hi);
    if (m1 != m1 || m2 != m2) return iv_nai();
    return iv_of(jm_minf(m1, m2), jm_maxf(m1, m2));
}
Ival iv_log(Ival a) {
    if (a.hi < 0.0f) return iv_nai();
    return iv_map_monotonic(iv_max(a, iv_exact(0.0f)), logf_j);
}
Ival iv_sign(Ival a) {
    if (a.nai) return iv_nai();
    if (a.lo == a.hi) return iv_exact(jm_signumf(a.lo));
    if (iv_contains(a, 0.0f)) {
        if (a.lo == 0.0f) return iv_of(0.0f, 1.0f);
        return a.hi == 0.0f ? iv_of(-1.0f, 0.0f) : iv_of(-1.0f, 1.0f);
    }
    return iv_exact(a.lo > 0.0f ? 1.0f : -1.0f);
}
static float lerp_finite_bound(float alpha, float a, float b) { return a + mul_bound(alpha, b - a); }
static float lerp_inf_bound(float alpha, float a, float b) {
    float fp = mul_bound(1.0f - alpha, a), sp = mul_bound(alpha, b);
    if (!isinf(fp) || !isinf(sp)) return fp + sp;
    if (alpha <= 0.0f) return b > a ? -INFINITY : INFINITY;
    if (alpha >= 1.0f) return b > a ? INFINITY : -INFINITY;
    return NAN;
}
static Ival iv_lerp_s(Ival alpha, float a, float b) {
    if (alpha.nai || a != a || b != b) return iv_nai();
    if (isfinite(a) && isfinite(b)) return iv_encaps2(lerp_finite_bound(alpha.lo, a, b), lerp_finite_bound(alpha.hi, a, b));
    if (a == b) return iv_exact(a);
    float lo = lerp_inf_bound(alpha.lo, a, b), hi = lerp_inf_bound(alpha.hi, a, b);
    return (lo == lo && hi == hi) ? iv_encaps2(lo, hi) : iv_nai();
}
Ival iv_lerp(Ival alpha, Ival f, Ival s) {
    if (alpha.nai || f.nai || s.nai) return iv_nai();
    Ival t[4] = { iv_lerp_s(alpha, f.lo, s.lo), iv_lerp_s(alpha, f.hi, s.lo), iv_lerp_s(alpha, f.lo, s.hi), iv_lerp_s(alpha, f.hi, s.hi) };
    return iv_encaps(t, 4);
}

/* ======================================================================= NoiseStack */
float ns_get(const NStack *s, double x, double y, double z) {
    float v = 0.0f;
    for (int i = 0; i < s->n; i++) {
        const NLayer *L = &s->l[i];
        double f = L->freq;
        float g = L->kind == NL_PERLIN ? gn_perlin_get_f(&L->n, x * f, y * f, z * f) : gn_smeared_get_f(&L->n, L->fudge, x * f, y * f, z * f);
        v += L->amp * g;
    }
    return v;
}
float ns_get2(const NStack *s, double x, double y) {
    float v = 0.0f;
    for (int i = 0; i < s->n; i++) {
        const NLayer *L = &s->l[i];
        double f = L->freq;
        /* PerlinNoise.get(x,y) = get(wrap(x), 0, wrap(y)) */
        v += L->amp * gn_perlin_get_f(&L->n, wrap_new(x * f), 0.0, wrap_new(y * f));
    }
    return v;
}
void ns_add_volume(const NStack *s, float *buf, const Vol *v, double xz_scale, double y_scale, float amp) {
    for (int i = 0; i < s->n; i++) {
        const NLayer *L = &s->l[i];
        double f = L->freq;
        if (L->kind == NL_PERLIN) gn_perlin_add_volume(&L->n, buf, v, xz_scale * f, y_scale * f, amp * L->amp);
        else gn_smeared_add_volume(&L->n, L->fudge, buf, v, xz_scale * f, y_scale * f, amp * L->amp);
    }
}
void ns_free(NStack *s) { free(s->l); s->l = NULL; s->n = 0; }

/* ======================================================================= параметры шума */
int noise_params_parse(const void *jsv, int new_format, NoiseParams *P, char *err, size_t errlen) {
    const Js *js = jsv;
    memset(P, 0, sizeof *P);
    P->new_format = new_format;
    if (!js_is_obj(js)) { set_err(err, errlen, "шум: ожидался объект"); return -1; }
    if (new_format) {
        Js *bo = js_get(js, "base_octave");
        if (!js_is_num(bo)) { set_err(err, errlen, "шум: нет base_octave"); return -1; }
        P->first_octave = js_int(bo, 0);
        P->count = js_int(js_get(js, "octave_count"), 1);
        P->base_amplitude = js_num(js_get(js, "base_amplitude"), 1.0);
        Js *nz = js_get(js, "normalize");
        if (!nz) P->normalize = 1;
        else if (nz->t == JS_BOOL) P->normalize = js_bool(nz, 1);
        else if (js_is_str(nz) && !strcmp(nz->s, "legacy")) P->normalize = 2;
        else { set_err(err, errlen, "шум: плохой normalize"); return -1; }
        Js *am = js_get(js, "amplitude_modifiers");
        if (js_is_arr(am) && am->n > 0) {
            if (am->n != P->count || am->n > 32) { set_err(err, errlen, "шум: amplitude_modifiers"); return -1; }
            P->has_mod = 1;
            for (int i = 0; i < am->n; i++) P->amp[i] = js_num(am->items[i], 0.0);
        }
    } else {
        Js *fo = js_get(js, "firstOctave"), *am = js_get(js, "amplitudes");
        if (!js_is_num(fo) || !js_is_arr(am) || am->n < 1 || am->n > 32) { set_err(err, errlen, "шум: нет firstOctave/amplitudes"); return -1; }
        P->first_octave = js_int(fo, 0);
        P->count = am->n; P->has_mod = 1;
        for (int i = 0; i < am->n; i++) P->amp[i] = js_num(am->items[i], 0.0);
        P->base_amplitude = 1.0; P->normalize = 1;
    }
    return 0;
}

/* --- NormalNoise 26.3 --- */
static double mod_of(const NoiseParams *P, int i) { return P->has_mod ? P->amp[i] : 1.0; }
typedef struct { int idx[32]; double freq[32], amp[32]; int n; } OctInfo;
static void build_octaves(const NoiseParams *P, OctInfo *o) {
    double frequency = pow(2.0, (double)P->first_octave);
    double amplitude = P->base_amplitude;
    if (P->normalize != 0) amplitude *= pow(0.5, (double)(-(P->count - 1))) / (pow(0.5, (double)(-P->count)) - 1.0);
    o->n = 0;
    for (int i = 0; i < P->count; i++) {
        double m = mod_of(P, i);
        if (m != 0.0) { o->idx[o->n] = P->first_octave + i; o->freq[o->n] = frequency; o->amp[o->n] = amplitude * m; o->n++; }
        frequency *= 2.0; amplitude *= 0.5;
    }
}
/* DoubleStream.sum() — компенсированное суммирование (Collectors.sumWithCompensation + computeFinalSum) */
static double stream_sum(const double *v, int n) {
    double sum = 0.0, comp = 0.0, simple = 0.0;
    for (int i = 0; i < n; i++) {
        simple += v[i];
        double tmp = v[i] - comp;
        double velvel = sum + tmp;
        comp = (velvel - sum) - tmp;
        sum = velvel;
    }
    double tmp = sum - comp;
    if (tmp != tmp && simple == simple && isinf(simple)) return simple;
    return tmp;
}
static void norm_factor(const NoiseParams *P, double *nf_out, double *target_out) {
    OctInfo o; build_octaves(P, &o);
    double absamp[32]; for (int i = 0; i < o.n; i++) absamp[i] = fabs(o.amp[i]);
    double target = stream_sum(absamp, o.n);
    double var = 0.0;
    for (int i = 0; i < o.n; i++) { double d = 0.2702247831245211 * absamp[i]; var += d * d; }
    double dev = sqrt(var);
    double nf = 0.0;
    if (dev != 0.0) nf = (target * 0.3333333333333333) / (dev * sqrt(2.0));
    if (P->normalize == 2 && nf != 0.0) {
        int mn = 0x7fffffff, mx = -0x7fffffff - 1;
        for (int i = 0; i < P->count; i++) if (mod_of(P, i) != 0.0) { if (i < mn) mn = i; if (i > mx) mx = i; }
        double parity = P->base_amplitude * 0.5 * 0.3333333333333333 / (0.1 * (1.0 + 1.0 / (double)(mx - mn + 1)));
        target *= parity / nf;
        nf = parity;
    }
    *nf_out = nf; *target_out = target;
}
Ival nn_new_range(const NoiseParams *P) {
    double nf, target; norm_factor(P, &nf, &target);
    return iv_sym((float)(target * 0.3333333333333333 * 6.0));
}
void nn_new_create(NStack *out, const NoiseParams *P, Rnd *random) {
    OctInfo o; build_octaves(P, &o);
    double nf, target; norm_factor(P, &nf, &target);
    PosRnd fr = rnd_fork_positional(random), sr = rnd_fork_positional(random);
    out->n = 2 * o.n; out->l = xcalloc((size_t)out->n, sizeof(NLayer));
    for (int k = 0; k < o.n; k++) {
        char nm[32]; snprintf(nm, sizeof nm, "octave_%d", o.idx[k]);
        Rnd r1 = pos_from_hash(&fr, nm), r2 = pos_from_hash(&sr, nm);
        float vf = (float)(nf * o.amp[k]);
        NLayer *a = &out->l[2 * k], *b = &out->l[2 * k + 1];
        gn_init(&a->n, &r1, 256.0); a->freq = o.freq[k]; a->amp = vf; a->kind = NL_PERLIN;
        gn_init(&b->n, &r2, 256.0); b->freq = o.freq[k] * 1.0181268882175227; b->amp = vf; b->kind = NL_PERLIN;
    }
    out->range = iv_sym((float)(target * 0.3333333333333333 * 6.0));
}
/* LegacyFbmInitializer.createForLegacyNetherBiome → слои (до сложения стеков) */
static int legacy_fbm(Rnd *random, int first_octave, const double *amps, int octaves, NLayer *out) {
    int zero = -first_octave;
    GNoise lv[32]; int has[32] = {0};
    GNoise z0; gn_init(&z0, random, 256.0);
    if (zero >= 0 && zero < octaves && amps[zero] != 0.0) { lv[zero] = z0; has[zero] = 1; }
    for (int i = zero - 1; i >= 0; i--) {
        if (i < octaves) {
            if (amps[i] != 0.0) { gn_init(&lv[i], random, 256.0); has[i] = 1; }
            else rnd_consume(random, 262);
        } else rnd_consume(random, 262);
    }
    double factor = pow(2.0, (double)(-zero));
    double vfac = pow(2.0, (double)(octaves - 1)) / (pow(2.0, (double)octaves) - 1.0);
    int k = 0;
    for (int i = 0; i < octaves; i++) {
        if (has[i]) { out[k].n = lv[i]; out[k].freq = factor; out[k].amp = (float)(vfac * amps[i]); out[k].kind = NL_PERLIN; k++; }
        factor *= 2.0; vfac /= 2.0;
    }
    return k;
}
void nn_new_create_legacy_nether(NStack *out, const NoiseParams *P, Rnd *random) {
    double amps[32]; for (int i = 0; i < P->count; i++) amps[i] = mod_of(P, i);
    NLayer a[32], b[32];
    int na = legacy_fbm(random, P->first_octave, amps, P->count, a);
    int nb = legacy_fbm(random, P->first_octave, amps, P->count, b);
    double nf, target; norm_factor(P, &nf, &target);
    float vf = (float)(nf * P->base_amplitude);
    out->n = na + nb; out->l = xcalloc((size_t)out->n, sizeof(NLayer));
    for (int i = 0; i < na; i++) { out->l[i] = a[i]; out->l[i].freq = a[i].freq * 1.0; out->l[i].amp = a[i].amp * vf; }
    for (int i = 0; i < nb; i++) { out->l[na + i] = b[i]; out->l[na + i].freq = b[i].freq * 1.0181268882175227; out->l[na + i].amp = b[i].amp * vf; }
    out->range = iv_sym((float)(target * 0.3333333333333333 * 6.0));
}

/* --- BlendedNoise 26.3 --- */
static void create_fbm(NStack *s, Rnd *random, int first_octave, double smear_y, double value_factor) {
    int octaves = -first_octave + 1;
    double factor = 1.0;
    value_factor /= pow(2.0, (double)octaves) - 1.0;
    s->n = octaves; s->l = xcalloc((size_t)octaves, sizeof(NLayer));
    int k = 0;
    for (int i = octaves - 1; i >= 0; i--) {
        NLayer *L = &s->l[k++];
        L->fudge = smear_y * factor;
        gn_init(&L->n, random, 256.0);
        L->freq = factor; L->amp = (float)value_factor; L->kind = NL_SMEARED;
        factor /= 2.0; value_factor *= 2.0;
    }
}
static Ival fbm_range(int first_octave, double smear_y, double value_factor) {
    int octaves = -first_octave + 1;
    double factor = 1.0;
    value_factor /= pow(2.0, (double)octaves) - 1.0;
    Ival r = iv_exact(0.0f);
    for (int i = octaves - 1; i >= 0; i--) {
        Ival lr = iv_mul(iv_sym((float)(fabs(smear_y * factor) + 2.0)), iv_exact((float)value_factor));
        r = iv_add(r, lr);
        factor /= 2.0; value_factor *= 2.0;
    }
    return r;
}
void blended_new_create(BlendFbm *out, Rnd *random, double y_scale, double smear_mult, double y_factor) {
    double y_mul = 684.412 * y_scale;
    double limit_smear = y_mul * smear_mult;
    double main_smear = limit_smear / y_factor;
    create_fbm(&out->min_lim, random, -15, limit_smear, (double)0.99998474f);
    create_fbm(&out->max_lim, random, -15, limit_smear, (double)0.99998474f);
    create_fbm(&out->main, random, -7, main_smear, 12.75);
    out->min_lim.range = out->max_lim.range = out->main.range = iv_inf();
}
Ival blended_new_range(double y_scale, double smear_mult) {
    return fbm_range(-15, 684.412 * y_scale * smear_mult, (double)0.99998474f);
}

/* ======================================================================= старая ветка */
void old_perlin_create(OldPerlin *p, Rnd *random, int first_octave, const double *amps, int n, int new_init) {
    memset(p, 0, sizeof *p);
    p->n = n; p->first_octave = first_octave;
    p->lev = xcalloc((size_t)n, sizeof(GNoise));
    for (int i = 0; i < n; i++) p->amp[i] = amps[i];
    int zero = -first_octave;
    if (new_init) {
        PosRnd pos = rnd_fork_positional(random);
        for (int i = 0; i < n; i++) if (amps[i] != 0.0) {
            char nm[32]; snprintf(nm, sizeof nm, "octave_%d", first_octave + i);
            Rnd r = pos_from_hash(&pos, nm);
            gn_init(&p->lev[i], &r, 256.0); p->has[i] = 1;
        }
    } else {
        GNoise z0; gn_init(&z0, random, 256.0);
        if (zero >= 0 && zero < n && amps[zero] != 0.0) { p->lev[zero] = z0; p->has[zero] = 1; }
        for (int i = zero - 1; i >= 0; i--) {
            if (i < n) {
                if (amps[i] != 0.0) { gn_init(&p->lev[i], random, 256.0); p->has[i] = 1; }
                else rnd_consume(random, 262);
            } else rnd_consume(random, 262);
        }
    }
    p->lowest_in = pow(2.0, (double)(-zero));
    p->lowest_val = pow(2.0, (double)(n - 1)) / (pow(2.0, (double)n) - 1.0);
    /* edgeValue(2.0) */
    double v = 0.0, vf = p->lowest_val;
    for (int i = 0; i < n; i++) { if (p->has[i]) v += p->amp[i] * 2.0 * vf; vf /= 2.0; }
    p->max_value = v;
}
double old_perlin_max_broken(const OldPerlin *p, double y_scale) {
    double v = 0.0, vf = p->lowest_val, nv = y_scale + 2.0;
    for (int i = 0; i < p->n; i++) { if (p->has[i]) v += p->amp[i] * nv * vf; vf /= 2.0; }
    return v;
}
double old_perlin_get(const OldPerlin *p, double x, double y, double z, double y_scale, double y_fudge) {
    double value = 0.0, factor = p->lowest_in, vf = p->lowest_val;
    for (int i = 0; i < p->n; i++) {
        if (p->has[i]) {
            double nv = gn_noise_d(&p->lev[i], wrap_old(x * factor), wrap_old(y * factor), wrap_old(z * factor), y_scale * factor, y_fudge * factor);
            value += p->amp[i] * nv * vf;
        }
        factor *= 2.0; vf /= 2.0;
    }
    return value;
}
const GNoise *old_perlin_octave(const OldPerlin *p, int i) { int j = p->n - 1 - i; return p->has[j] ? &p->lev[j] : NULL; }
void old_perlin_free(OldPerlin *p) { free(p->lev); p->lev = NULL; }

void old_normal_create(OldNormal *nn, Rnd *random, const NoiseParams *P, int new_init) {
    old_perlin_create(&nn->first, random, P->first_octave, P->amp, P->count, new_init);
    old_perlin_create(&nn->second, random, P->first_octave, P->amp, P->count, new_init);
    int mn = 0x7fffffff, mx = -0x7fffffff - 1;
    for (int i = 0; i < P->count; i++) if (P->amp[i] != 0.0) { if (i < mn) mn = i; if (i > mx) mx = i; }
    nn->value_factor = 0.16666666666666666 / (0.1 * (1.0 + 1.0 / (double)(mx - mn + 1)));
    nn->max_value = (nn->first.max_value + nn->second.max_value) * nn->value_factor;
}
double old_normal_get(const OldNormal *nn, double x, double y, double z) {
    double x2 = x * 1.0181268882175227, y2 = y * 1.0181268882175227, z2 = z * 1.0181268882175227;
    return (old_perlin_get(&nn->first, x, y, z, 0.0, 0.0) + old_perlin_get(&nn->second, x2, y2, z2, 0.0, 0.0)) * nn->value_factor;
}
void old_normal_free(OldNormal *nn) { old_perlin_free(&nn->first); old_perlin_free(&nn->second); }

void old_blended_create(OldBlended *b, Rnd *random, double xz_scale, double y_scale, double xz_factor, double y_factor, double smear) {
    double ones[16]; for (int i = 0; i < 16; i++) ones[i] = 1.0;
    old_perlin_create(&b->min_lim, random, -15, ones, 16, 0);
    old_perlin_create(&b->max_lim, random, -15, ones, 16, 0);
    old_perlin_create(&b->main, random, -7, ones, 8, 0);
    b->xz_mul = 684.412 * xz_scale; b->y_mul = 684.412 * y_scale;
    b->xz_factor = xz_factor; b->y_factor = y_factor; b->smear = smear;
    b->max_value = old_perlin_max_broken(&b->min_lim, b->y_mul);
}
double old_blended_compute(const OldBlended *b, int bx, int by, int bz) {
    double lx = bx * b->xz_mul, ly = by * b->y_mul, lz = bz * b->xz_mul;
    double mx = lx / b->xz_factor, my = ly / b->y_factor, mz = lz / b->xz_factor;
    double lsmear = b->y_mul * b->smear;
    double msmear = lsmear / b->y_factor;
    double bmin = 0.0, bmax = 0.0, mainv = 0.0, pw = 1.0;
    for (int i = 0; i < 8; i++) {
        const GNoise *n = old_perlin_octave(&b->main, i);
        if (n) mainv += gn_noise_d(n, wrap_old(mx * pw), wrap_old(my * pw), wrap_old(mz * pw), msmear * pw, my * pw) / pw;
        pw /= 2.0;
    }
    double factor = (mainv / 10.0 + 1.0) / 2.0;
    int is_max = factor >= 1.0, is_min = factor <= 0.0;
    pw = 1.0;
    for (int i = 0; i < 16; i++) {
        double wx = wrap_old(lx * pw), wy = wrap_old(ly * pw), wz = wrap_old(lz * pw);
        double ysp = lsmear * pw;
        if (!is_max) { const GNoise *n = old_perlin_octave(&b->min_lim, i); if (n) bmin += gn_noise_d(n, wx, wy, wz, ysp, ly * pw) / pw; }
        if (!is_min) { const GNoise *n = old_perlin_octave(&b->max_lim, i); if (n) bmax += gn_noise_d(n, wx, wy, wz, ysp, ly * pw) / pw; }
        pw /= 2.0;
    }
    double a = bmin / 512.0, c = bmax / 512.0;
    double r = factor < 0.0 ? a : (factor > 1.0 ? c : jm_lerp(factor, a, c));
    return r / 128.0;
}
void old_blended_free(OldBlended *b) { old_perlin_free(&b->min_lim); old_perlin_free(&b->max_lim); old_perlin_free(&b->main); }

/* ======================================================================= Simplex 2D */
double simplex2_d(const GNoise *n, double x, double y) { return mc_simplex2_core(n, x, y); }

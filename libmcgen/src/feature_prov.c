/* feature_prov.c — IntProvider, FloatProvider, HeightProvider, VerticalAnchor, WeightedList, шум информации биома; ГСЧ (гауссиан). */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

double frnd_gauss(FRnd *r) {
    if (r->have_g) { r->have_g = 0; return r->next_g; }
    double x, y, s;
    do {
        x = 2.0 * frnd_double(r) - 1.0;
        y = 2.0 * frnd_double(r) - 1.0;
        s = x * x + y * y;
    } while (s >= 1.0 || s == 0.0);
    double m = sqrt(-2.0 * log(s) / s);
    r->next_g = y * m; r->have_g = 1;
    return x * m;
}

int dir_from_name(const char *s) {
    static const char *N[6] = { "down", "up", "north", "south", "west", "east" };
    if (!s) return -1;
    for (int i = 0; i < 6; i++) if (!strcmp(s, N[i])) return i;
    return -1;
}

/* ====================================================================== WeightedList */
typedef struct WList { int n, total; int *w; void **v; } WList;
static int wlist_pick_index(const WList *l, FRnd *r) {
    int sel = frnd_int_bound(r, l->total);
    for (int i = 0; i < l->n; i++) { sel -= l->w[i]; if (sel < 0) return i; }
    return l->n - 1;
}

/* ====================================================================== IntProvider */
enum { IP_CONST, IP_UNIFORM, IP_BIASED, IP_CLAMPED, IP_CLAMPED_NORMAL, IP_WEIGHTED, IP_TRAPEZOID };
struct IntProv {
    int kind;
    int a, b, c;                 /* const: a; uniform/biased/clamped: min a, max b; trapezoid: min a, max b, plateau c */
    float mean, dev;             /* clamped_normal */
    IntProv *src;                /* clamped */
    WList wl;                    /* weighted_list: v — IntProv* */
    int mn, mx;                  /* weighted_list: границы */
};

static const char *type_name(const Js *v) {
    const char *t = js_str(js_get(v, "type"), NULL);
    if (t && !strncmp(t, "minecraft:", 10)) t += 10;
    return t;
}

IntProv *fp_intprov(FParse *p, const Js *v) {
    IntProv *ip = fp_alloc(p, sizeof *ip);
    if (js_is_num(v)) { ip->kind = IP_CONST; ip->a = (int)v->d; return ip; }
    if (!js_is_obj(v)) { fp_fail(p, "IntProvider: ожидалось число или объект"); return NULL; }
    const char *t = type_name(v);
    if (!t) { fp_fail(p, "IntProvider: нет type"); return NULL; }
    if (!strcmp(t, "constant")) { ip->kind = IP_CONST; ip->a = js_int(js_get(v, "value"), 0); }
    else if (!strcmp(t, "uniform")) { ip->kind = IP_UNIFORM; ip->a = js_int(js_get(v, "min_inclusive"), 0); ip->b = js_int(js_get(v, "max_inclusive"), 0); }
    else if (!strcmp(t, "biased_to_bottom")) { ip->kind = IP_BIASED; ip->a = js_int(js_get(v, "min_inclusive"), 0); ip->b = js_int(js_get(v, "max_inclusive"), 0); }
    else if (!strcmp(t, "clamped")) {
        ip->kind = IP_CLAMPED; ip->a = js_int(js_get(v, "min_inclusive"), 0); ip->b = js_int(js_get(v, "max_inclusive"), 0);
        ip->src = fp_intprov(p, js_get(v, "source")); if (!ip->src) return NULL;
    }
    else if (!strcmp(t, "clamped_normal")) {
        ip->kind = IP_CLAMPED_NORMAL; ip->mean = js_numf(js_get(v, "mean"), 0); ip->dev = js_numf(js_get(v, "deviation"), 0);
        ip->a = js_int(js_get(v, "min_inclusive"), 0); ip->b = js_int(js_get(v, "max_inclusive"), 0);
    }
    else if (!strcmp(t, "trapezoid")) { ip->kind = IP_TRAPEZOID; ip->a = js_int(js_get(v, "min"), 0); ip->b = js_int(js_get(v, "max"), 0); ip->c = js_int(js_get(v, "plateau"), 0); }
    else if (!strcmp(t, "weighted_list")) {
        Js *d = js_get(v, "distribution");
        if (!js_is_arr(d) || d->n == 0) { fp_fail(p, "IntProvider weighted_list: пустое distribution"); return NULL; }
        ip->kind = IP_WEIGHTED; ip->wl.n = d->n;
        ip->wl.w = fp_alloc(p, sizeof(int) * (size_t)d->n); ip->wl.v = fp_alloc(p, sizeof(void *) * (size_t)d->n);
        ip->mn = 0x7fffffff; ip->mx = (int)0x80000000;
        for (int i = 0; i < d->n; i++) {
            ip->wl.w[i] = js_int(js_get(d->items[i], "weight"), 1); ip->wl.total += ip->wl.w[i];
            IntProv *e = fp_intprov(p, js_get(d->items[i], "data")); if (!e) return NULL;
            ip->wl.v[i] = e;
            int lo = intprov_min(e), hi = intprov_max(e);
            if (lo < ip->mn) ip->mn = lo; if (hi > ip->mx) ip->mx = hi;
        }
    }
    else { fp_fail(p, "IntProvider: неизвестный тип %s", t); return NULL; }
    return ip;
}

int intprov_sample(const IntProv *ip, FRnd *r) {
    switch (ip->kind) {
    case IP_CONST: return ip->a;
    case IP_UNIFORM: return frnd_between(r, ip->a, ip->b);
    case IP_BIASED: return ip->a + frnd_int_bound(r, frnd_int_bound(r, ip->b - ip->a + 1) + 1);
    case IP_CLAMPED: { int v = intprov_sample(ip->src, r); return v < ip->a ? ip->a : (v > ip->b ? ip->b : v); }
    case IP_CLAMPED_NORMAL: {
        float n = ip->mean + (float)frnd_gauss(r) * ip->dev;           /* Mth.normal */
        float lo = (float)ip->a, hi = (float)ip->b;
        float cl = n < lo ? lo : jm_minf(n, hi);                       /* Mth.clamp(float) */
        return jm_d2i((double)cl);
    }
    case IP_TRAPEZOID: {
        int mn = ip->a, mx = ip->b, pl = ip->c;
        if (pl == 0 && mx == -mn) return frnd_int_bound(r, mx + 1) - frnd_int_bound(r, mx + 1);
        int range = mx - mn;
        if (pl == range) return frnd_between(r, mn, mx);
        int ps = (range - pl) / 2, pe = range - ps;
        int a = frnd_between(r, 0, pe);
        int b = frnd_between(r, 0, ps);
        return mn + a + b;
    }
    case IP_WEIGHTED: return intprov_sample(ip->wl.v[wlist_pick_index(&ip->wl, r)], r);
    }
    return 0;
}
int intprov_min(const IntProv *ip) {
    switch (ip->kind) {
    case IP_CONST: case IP_UNIFORM: case IP_BIASED: case IP_TRAPEZOID: return ip->a;
    case IP_CLAMPED: { int s = intprov_min(ip->src); return ip->a > s ? ip->a : s; }
    case IP_CLAMPED_NORMAL: return ip->a;
    case IP_WEIGHTED: return ip->mn;
    }
    return 0;
}
int intprov_max(const IntProv *ip) {
    switch (ip->kind) {
    case IP_CONST: return ip->a;
    case IP_UNIFORM: case IP_BIASED: case IP_TRAPEZOID: case IP_CLAMPED_NORMAL: return ip->b;
    case IP_CLAMPED: { int s = intprov_max(ip->src); return ip->b < s ? ip->b : s; }
    case IP_WEIGHTED: return ip->mx;
    }
    return 0;
}

/* ====================================================================== FloatProvider */
enum { FP_CONST, FP_UNIFORM, FP_CLAMPED_NORMAL, FP_TRAPEZOID };
struct FloatProv { int kind; float a, b, c; float mean, dev; };
FloatProv *fp_floatprov(FParse *p, const Js *v) {
    FloatProv *f = fp_alloc(p, sizeof *f);
    if (js_is_num(v)) { f->kind = FP_CONST; f->a = js_numf(v, 0); return f; }
    if (!js_is_obj(v)) { fp_fail(p, "FloatProvider: ожидалось число или объект"); return NULL; }
    const char *t = type_name(v);
    if (!t) { fp_fail(p, "FloatProvider: нет type"); return NULL; }
    if (!strcmp(t, "constant")) { f->kind = FP_CONST; f->a = js_numf(js_get(v, "value"), 0); }
    else if (!strcmp(t, "uniform")) { f->kind = FP_UNIFORM; f->a = js_numf(js_get(v, "min_inclusive"), 0); f->b = js_numf(js_get(v, "max_exclusive"), 0); }
    else if (!strcmp(t, "clamped_normal")) { f->kind = FP_CLAMPED_NORMAL; f->mean = js_numf(js_get(v, "mean"), 0); f->dev = js_numf(js_get(v, "deviation"), 0);
                                             f->a = js_numf(js_get(v, "min"), 0); f->b = js_numf(js_get(v, "max"), 0); }
    else if (!strcmp(t, "trapezoid")) { f->kind = FP_TRAPEZOID; f->a = js_numf(js_get(v, "min"), 0); f->b = js_numf(js_get(v, "max"), 0); f->c = js_numf(js_get(v, "plateau"), 0); }
    else { fp_fail(p, "FloatProvider: неизвестный тип %s", t); return NULL; }
    return f;
}
float floatprov_sample(const FloatProv *f, FRnd *r) {
    switch (f->kind) {
    case FP_CONST: return f->a;
    case FP_UNIFORM: return frnd_float(r) * (f->b - f->a) + f->a;                                    /* Mth.randomBetween */
    case FP_CLAMPED_NORMAL: { float n = f->mean + (float)frnd_gauss(r) * f->dev; return jm_clampf(n, f->a, f->b); }
    case FP_TRAPEZOID: {
        float range = f->b - f->a, plateau_start = (range - f->c) / 2.0f, plateau_end = range - plateau_start;
        return f->a + frnd_float(r) * plateau_end + frnd_float(r) * plateau_start;                  /* TrapezoidFloat.sample */
    }
    }
    return 0;
}
float floatprov_min(const FloatProv *f) { return f->kind == FP_CONST ? f->a : f->a; }
float floatprov_max(const FloatProv *f) { return f->kind == FP_CONST ? f->a : f->b; }

/* ====================================================================== VerticalAnchor и HeightProvider */
int fp_vanchor(FParse *p, const Js *v, VAnchor *out) {
    static const char *K[4] = { "absolute", "above_bottom", "below_top", "relative_to_sea_level" };
    if (js_is_obj(v)) {
        for (int k = 0; k < 4; k++) { Js *e = js_get(v, K[k]); if (e) { out->kind = k; out->off = js_int(e, 0); return 1; } }
    }
    return fp_fail(p, "VerticalAnchor: ожидалось absolute/above_bottom/below_top/relative_to_sea_level");
}
int vanchor_resolve(const VAnchor *a, const FCtx *c) {
    switch (a->kind) {
    case 0: return a->off;
    case 1: return c->gen_min_y + a->off;
    case 2: return c->gen_depth - 1 + c->gen_min_y - a->off;
    default: return c->sea_level + a->off;
    }
}

enum { HP_CONST, HP_UNIFORM, HP_BIASED, HP_VERY_BIASED, HP_TRAPEZOID, HP_WEIGHTED };
struct HeightProv { int kind; VAnchor min, max; int inner, plateau; WList wl; };
HeightProv *fp_heightprov(FParse *p, const Js *v) {
    HeightProv *h = fp_alloc(p, sizeof *h);
    if (js_is_obj(v) && !js_get(v, "type")) { h->kind = HP_CONST; if (!fp_vanchor(p, v, &h->min)) return NULL; return h; }
    if (!js_is_obj(v)) { fp_fail(p, "HeightProvider: ожидался объект"); return NULL; }
    const char *t = type_name(v);
    if (!t) { fp_fail(p, "HeightProvider: нет type"); return NULL; }
    if (!strcmp(t, "constant")) { h->kind = HP_CONST; if (!fp_vanchor(p, js_get(v, "value"), &h->min)) return NULL; return h; }
    if (!strcmp(t, "weighted_list")) {
        Js *d = js_get(v, "distribution");
        if (!js_is_arr(d) || d->n == 0) { fp_fail(p, "HeightProvider weighted_list: пусто"); return NULL; }
        h->kind = HP_WEIGHTED; h->wl.n = d->n;
        h->wl.w = fp_alloc(p, sizeof(int) * (size_t)d->n); h->wl.v = fp_alloc(p, sizeof(void *) * (size_t)d->n);
        for (int i = 0; i < d->n; i++) {
            h->wl.w[i] = js_int(js_get(d->items[i], "weight"), 1); h->wl.total += h->wl.w[i];
            h->wl.v[i] = fp_heightprov(p, js_get(d->items[i], "data")); if (!h->wl.v[i]) return NULL;
        }
        return h;
    }
    if (!strcmp(t, "uniform")) h->kind = HP_UNIFORM;
    else if (!strcmp(t, "biased_to_bottom")) h->kind = HP_BIASED;
    else if (!strcmp(t, "very_biased_to_bottom")) h->kind = HP_VERY_BIASED;
    else if (!strcmp(t, "trapezoid")) h->kind = HP_TRAPEZOID;
    else { fp_fail(p, "HeightProvider: неизвестный тип %s", t); return NULL; }
    if (!fp_vanchor(p, js_get(v, "min_inclusive"), &h->min) || !fp_vanchor(p, js_get(v, "max_inclusive"), &h->max)) return NULL;
    h->inner = js_int(js_get(v, "inner"), 1); h->plateau = js_int(js_get(v, "plateau"), 0);
    return h;
}
int heightprov_sample(const HeightProv *h, FCtx *c) {
    FRnd *r = c->rnd;
    if (h->kind == HP_CONST) return vanchor_resolve(&h->min, c);
    if (h->kind == HP_WEIGHTED) return heightprov_sample(h->wl.v[wlist_pick_index(&h->wl, r)], c);
    int mn = vanchor_resolve(&h->min, c), mx = vanchor_resolve(&h->max, c);
    switch (h->kind) {
    case HP_UNIFORM: return mn > mx ? mn : frnd_between(r, mn, mx);
    case HP_BIASED: {
        if (mx - mn - h->inner + 1 <= 0) return mn;
        int limit = frnd_int_bound(r, mx - mn - h->inner + 1);
        return frnd_int_bound(r, limit + h->inner) + mn;
    }
    case HP_VERY_BIASED: {
        if (mx - mn - h->inner + 1 <= 0) return mn;
        int upper = mth_next_int(r, mn + h->inner, mx);
        int biased = mth_next_int(r, mn, upper - 1);
        return mth_next_int(r, mn, biased - 1 + h->inner);
    }
    case HP_TRAPEZOID: {
        if (mn > mx) return mn;
        int range = mx - mn;
        if (h->plateau >= range) return frnd_between(r, mn, mx);
        int ps = (range - h->plateau) / 2, pe = range - ps;
        int a = frnd_between(r, 0, pe);
        int b = frnd_between(r, 0, ps);
        return mn + a + b;
    }
    }
    return mn;
}

/* ====================================================================== Mth.sin / Mth.cos (таблица игры) */
static float g_sin_tab[65536]; static int g_sin_ready;
void fm_init(void) {
    if (g_sin_ready) return;
    for (int i = 0; i < 65536; i++) g_sin_tab[i] = (float)sin((double)i / 10430.378350470453);
    g_sin_ready = 1;
}
float fm_sin(double v) { return g_sin_tab[(int)(jm_d2l(v * 10430.378350470453) & 65535LL)]; }
float fm_cos(double v) { return g_sin_tab[(int)(jm_d2l(v * 10430.378350470453 + 16384.0) & 65535LL)]; }

/* ====================================================================== Biome.BIOME_INFO_NOISE */
static GNoise g_info_noise; static int g_info_ready;
double biome_info_noise(const McGen *g, double x, double z) {
    if (!g_info_ready) {
        mutex_lock(g->lock);
        if (!g_info_ready) { Rnd r = rnd_legacy_seed(2345); gn_init(&g_info_noise, &r, 0.0); g_info_ready = 1; }
        mutex_unlock(g->lock);
    }
    double v = simplex2_d(&g_info_noise, x, z);
    return g->newf ? (double)(float)v : v;
}

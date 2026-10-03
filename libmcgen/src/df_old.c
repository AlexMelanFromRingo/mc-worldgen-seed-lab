/* df_old.c — density-функции 26.1/26.2: проводка, точечное вычисление (compute) и NoiseChunk (fillArray, обёртки).
 * Классы игры: levelgen/DensityFunctions (Ap2, MulOrAdd, Mapped, Clamp, RangeChoice, IntervalSelect, Spline, …),
 * levelgen/NoiseChunk (NoiseInterpolator, FlatCache, Cache2D, CacheOnce, CacheAllInCell). Числа — double, как в игре. */
#include "df_old.h"
#include "mc_end.h"
#include <stdlib.h>
#include <stdio.h>

enum {
    O_HOLDER = DF__COUNT + 10, O_AP2_ADD, O_AP2_MUL, O_AP2_MIN, O_AP2_MAX, O_MOA_ADD, O_MOA_MUL,
    W_INTERP, W_FLAT, W_CACHE2D, W_ONCE, W_ALLCELL, W_BEARD
};

typedef struct OSp OSp;
struct OSp { int is_const; float value; ON *coord; int n; float *loc, *der; OSp **val; float minv, maxv; };

struct ON {
    int t;
    ON *a, *b, *c;
    ON **list; int nlist;
    double *thr; int nthr;
    double d0, d1;
    int i0, i1;
    const OldNormal *nn; const OldBlended *bl; const McEnd *end;
    OSp *sp;
    double minv, maxv;
    int wid;
    u64 h; ON *hnext;
};

#define HC_SIZE 4096
struct OldWire {
    McWorld *w;
    StrMap memo_dummy;
    const Df **mk; ON **mv; int nm, capm;       /* мемо проводки по указателю AST */
    PtrVec all;                                 /* все узлы (для освобождения) */
    PtrVec splines;
    ON *rf[RF__COUNT];                          /* проведённый роутер */
    /* «обёрнутый» граф NoiseChunk */
    ON *hc[HC_SIZE];
    ON *cw[RF__COUNT];
    ON *full;                                   /* cache_all_in_cell(add(final_density, beardifier)) */
    int nw;                                     /* число обёрток */
    ON **wl; int capw;                          /* обёртки по wid (порядок создания) */
    OldBlended *blended[8]; const Df *blended_key[8]; int nblended;
    McEnd end; int has_end;
    char *err; size_t errlen; int fail;
};

static ON *onew(OldWire *o, int t) { ON *n = xcalloc(1, sizeof(ON)); n->t = t; n->wid = -1; pv_push(&o->all, n); return n; }
static OSp *spnew(OldWire *o) { OSp *s = xcalloc(1, sizeof(OSp)); pv_push(&o->splines, s); return s; }
static void ofail(OldWire *o, const char *fmt, const char *a) { if (!o->fail) set_err(o->err, o->errlen, fmt, a ? a : ""); o->fail = 1; }

/* ---------------- границы (minValue/maxValue) ---------------- */
static double mapped_tr(int t, double v) {
    switch (t) {
    case DF_ABS: return fabs(v);
    case DF_SQUARE: return v * v;
    case DF_CUBE: return v * v * v;
    case DF_HALF_NEGATIVE: return v > 0.0 ? v : v * 0.5;
    case DF_QUARTER_NEGATIVE: return v > 0.0 ? v : v * 0.25;
    case DF_RECIPROCAL: return 1.0 / v;
    default: { double c = jm_clamp(v, -1.0, 1.0); return c / 2.0 - c * c * c / 24.0; }
    }
}
static void mapped_bounds(ON *n) {
    double mn = n->a->minv, mx = n->a->maxv, mi = mapped_tr(n->t, mn), ma = mapped_tr(n->t, mx);
    if (n->t == DF_RECIPROCAL) {
        if (mn < 0.0 && mx > 0.0) { n->minv = -INFINITY; n->maxv = INFINITY; } else { n->minv = ma; n->maxv = mi; }
    } else if (n->t == DF_ABS || n->t == DF_SQUARE) { n->minv = jm_max(0.0, mn); n->maxv = jm_max(mi, ma); }
    else { n->minv = mi; n->maxv = ma; }
}
/* TwoArgumentSimpleFunction.create: Ap2 или MulOrAdd (если один аргумент — Constant) */
static ON *ap2_create(OldWire *o, int type /* DF_ADD/MUL/MIN/MAX */, ON *a, ON *b) {
    double min1 = a->minv, min2 = b->minv, max1 = a->maxv, max2 = b->maxv, mn, mx;
    switch (type) {
    case DF_ADD: mn = min1 + min2; mx = max1 + max2; break;
    case DF_MUL:
        mn = (min1 > 0.0 && min2 > 0.0) ? min1 * min2 : ((max1 < 0.0 && max2 < 0.0) ? max1 * max2 : jm_min(min1 * max2, max1 * min2));
        mx = (min1 > 0.0 && min2 > 0.0) ? max1 * max2 : ((max1 < 0.0 && max2 < 0.0) ? min1 * min2 : jm_max(min1 * min2, max1 * max2));
        break;
    case DF_MIN: mn = jm_min(min1, min2); mx = jm_min(max1, max2); break;
    default: mn = jm_max(min1, min2); mx = jm_max(max1, max2); break;
    }
    ON *n;
    if ((type == DF_ADD || type == DF_MUL) && (a->t == DF_CONST || b->t == DF_CONST)) {
        n = onew(o, type == DF_ADD ? O_MOA_ADD : O_MOA_MUL);
        if (a->t == DF_CONST) { n->a = b; n->d0 = a->d0; } else { n->a = a; n->d0 = b->d0; }
    } else {
        n = onew(o, type == DF_ADD ? O_AP2_ADD : type == DF_MUL ? O_AP2_MUL : type == DF_MIN ? O_AP2_MIN : O_AP2_MAX);
        n->a = a; n->b = b;
    }
    n->minv = mn; n->maxv = mx;
    return n;
}
/* MulOrAdd.mapChildren: свои формулы границ */
static void moa_bounds(ON *n) {
    double mn = n->a->minv, mx = n->a->maxv, g = n->d0;
    if (n->t == O_MOA_ADD) { n->minv = mn + g; n->maxv = mx + g; }
    else if (g >= 0.0) { n->minv = mn * g; n->maxv = mx * g; }
    else { n->minv = mx * g; n->maxv = mn * g; }
}
static float lin_ext(float in, const float *loc, float v, const float *der, int i) { float d = der[i]; return d == 0.0f ? v : v + d * (in - loc[i]); }
static void spline_bounds(OSp *s) {
    if (s->is_const) { s->minv = s->maxv = s->value; return; }
    int last = s->n - 1;
    float mn = INFINITY, mx = -INFINITY;
    float in_min = (float)s->coord->minv, in_max = (float)s->coord->maxv;
    if (in_min < s->loc[0]) {
        float e1 = lin_ext(in_min, s->loc, s->val[0]->minv, s->der, 0), e2 = lin_ext(in_min, s->loc, s->val[0]->maxv, s->der, 0);
        mn = jm_minf(mn, jm_minf(e1, e2)); mx = jm_maxf(mx, jm_maxf(e1, e2));
    }
    if (in_max > s->loc[last]) {
        float e1 = lin_ext(in_max, s->loc, s->val[last]->minv, s->der, last), e2 = lin_ext(in_max, s->loc, s->val[last]->maxv, s->der, last);
        mn = jm_minf(mn, jm_minf(e1, e2)); mx = jm_maxf(mx, jm_maxf(e1, e2));
    }
    for (int i = 0; i < s->n; i++) { mn = jm_minf(mn, s->val[i]->minv); mx = jm_maxf(mx, s->val[i]->maxv); }
    for (int i = 0; i < last; i++) {
        float x1 = s->loc[i], x2 = s->loc[i + 1], xd = x2 - x1;
        float min1 = s->val[i]->minv, max1 = s->val[i]->maxv, min2 = s->val[i + 1]->minv, max2 = s->val[i + 1]->maxv;
        float d1 = s->der[i], d2 = s->der[i + 1];
        if (d1 != 0.0f || d2 != 0.0f) {
            float p1 = d1 * xd, p2 = d2 * xd;
            float minL1 = jm_minf(min1, min2), maxL1 = jm_maxf(max1, max2);
            float minA = p1 - max2 + min1, maxA = p1 - min2 + max1, minB = -p2 + min2 - max1, maxB = -p2 + max2 - min1;
            float minL2 = jm_minf(minA, minB), maxL2 = jm_maxf(maxA, maxB);
            mn = jm_minf(mn, minL1 + 0.25f * minL2); mx = jm_maxf(mx, maxL1 + 0.25f * maxL2);
        }
    }
    s->minv = mn; s->maxv = mx;
}
static void generic_bounds(ON *n) {
    switch (n->t) {
    case DF_CLAMP: n->minv = n->d0; n->maxv = n->d1; break;
    case DF_RANGE_CHOICE: n->minv = jm_min(n->b->minv, n->c->minv); n->maxv = jm_max(n->b->maxv, n->c->maxv); break;
    case DF_INTERVAL_SELECT: {
        double mn = 1.7976931348623157e308, mx = -1.7976931348623157e308;
        for (int i = 0; i < n->nlist; i++) { mn = jm_min(n->list[i]->minv, mn); mx = jm_max(n->list[i]->maxv, mx); }
        n->minv = mn; n->maxv = mx; break;
    }
    case DF_SPLINE: n->minv = n->sp->minv; n->maxv = n->sp->maxv; break;
    case DF_FIND_TOP_SURFACE: n->minv = n->i0; n->maxv = jm_max((double)n->i0, n->b->maxv); break;
    default: break;
    }
}

/* ---------------- проводка AST → граф ---------------- */
static ON *wire(OldWire *o, const Df *f);
static OSp *wire_spline(OldWire *o, const DfSpline *s) {
    OSp *p = spnew(o);
    if (s->is_const) { p->is_const = 1; p->value = s->value.f; spline_bounds(p); return p; }
    p->coord = wire(o, s->coord); p->n = s->n;
    p->loc = xcalloc((size_t)s->n, sizeof(float)); p->der = xcalloc((size_t)s->n, sizeof(float)); p->val = xcalloc((size_t)s->n, sizeof(OSp *));
    for (int i = 0; i < s->n; i++) { p->loc[i] = s->loc[i].f; p->der[i] = s->der[i].f; p->val[i] = wire_spline(o, s->val[i]); }
    spline_bounds(p);
    return p;
}
static const OldBlended *get_blended(OldWire *o, const Df *f) {
    for (int i = 0; i < o->nblended; i++) if (o->blended_key[i] == f) return o->blended[i];
    McWorld *w = o->w;
    OldBlended *b = xcalloc(1, sizeof *b);
    Rnd r = w->ns->legacy_random ? rnd_legacy_seed(w->seeds.terrain + 0) : pos_from_hash(&w->pos_terrain, "minecraft:terrain");
    old_blended_create(b, &r, f->n0.d, f->n1.d, f->n2.d, f->n3.d, f->n4.d);
    if (o->nblended < 8) { o->blended_key[o->nblended] = f; o->blended[o->nblended++] = b; }
    return b;
}
static const McEnd *get_end(OldWire *o) {
    if (!o->has_end) {
        Rnd r = rnd_legacy_seed(o->w->seeds.climate);
        rnd_consume(&r, 17292);
        gn_init(&o->end.s, &r, 256.0);
        o->has_end = 1;
    }
    return &o->end;
}
static const OldNormal *get_noise(OldWire *o, const char *name) {
    const OldNormal *nn = world_noise_old(o->w, name, o->err, o->errlen);
    if (!nn) o->fail = 1;
    return nn;
}
/* тонкие настройки: множители координат шума (только если ≠ 1) */
static void scale_noise(OldWire *o, const char *name, ON *n) {
    double mxz, my; world_noise_scale(o->w, name, &mxz, &my);
    if (mxz != 1.0) n->d0 *= mxz;
    if (my != 1.0) n->d1 *= my;
}
static ON *wire_impl(OldWire *o, const Df *f) {
    ON *n;
    switch (f->t) {
    case DF_CONST: n = onew(o, DF_CONST); n->d0 = f->n0.d; n->minv = n->maxv = n->d0; return n;
    case DF_REF: {
        const Df *t = world_df_ref(o->w, f->name);
        if (!t) { ofail(o, "нет density_function %s", f->name); return NULL; }
        n = onew(o, O_HOLDER); n->a = wire(o, t);
        if (n->a) { n->minv = n->a->minv; n->maxv = n->a->maxv; }
        return n;
    }
    case DF_NOISE: n = onew(o, DF_NOISE); n->nn = get_noise(o, f->name); n->d0 = f->n0.d; n->d1 = f->n1.d; scale_noise(o, f->name, n);
        if (n->nn) { n->maxv = n->nn->max_value; n->minv = -n->maxv; } return n;
    case DF_SHIFTED_NOISE: n = onew(o, DF_SHIFTED_NOISE); n->nn = get_noise(o, f->name); n->d0 = f->n0.d; n->d1 = f->n1.d; scale_noise(o, f->name, n);
        n->a = wire(o, f->a); n->b = wire(o, f->b); n->c = wire(o, f->c);
        if (n->nn) { n->maxv = n->nn->max_value; n->minv = -n->maxv; } return n;
    case DF_SHIFT_A: case DF_SHIFT_B: case DF_SHIFT:
        n = onew(o, f->t); n->nn = get_noise(o, f->name); n->d0 = 0.25; n->d1 = 0.25; scale_noise(o, f->name, n);
        if (n->nn) { n->maxv = n->nn->max_value * 4.0; n->minv = -n->maxv; } return n;
    case DF_END_ISLANDS: n = onew(o, DF_END_ISLANDS); n->end = get_end(o); n->minv = -0.84375; n->maxv = 0.5625; return n;
    case DF_Y_CLAMPED_GRADIENT: n = onew(o, f->t); n->i0 = f->i0; n->i1 = f->i1; n->d0 = f->n0.d; n->d1 = f->n1.d;
        n->minv = jm_min(n->d0, n->d1); n->maxv = jm_max(n->d0, n->d1); return n;
    case DF_OLD_BLENDED_NOISE: n = onew(o, f->t); n->bl = get_blended(o, f); n->maxv = n->bl->max_value; n->minv = -n->maxv; return n;
    case DF_WEIRD_SCALED: n = onew(o, f->t); n->a = wire(o, f->a); n->nn = get_noise(o, f->name); n->i0 = f->i0;
        n->minv = 0.0; if (n->nn) n->maxv = (f->i0 == 0 ? 2.0 : 3.0) * n->nn->max_value; return n;
    case DF_BLEND_ALPHA: n = onew(o, f->t); n->minv = n->maxv = 1.0; return n;
    case DF_BLEND_OFFSET: case DF_BEARDIFIER: n = onew(o, f->t); n->minv = n->maxv = 0.0; return n;
    case DF_ABS: case DF_SQUARE: case DF_CUBE: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_RECIPROCAL: case DF_SQUEEZE:
        n = onew(o, f->t); n->a = wire(o, f->a); if (n->a) mapped_bounds(n); return n;
    case DF_CLAMP: n = onew(o, f->t); n->a = wire(o, f->a); n->d0 = f->n0.d; n->d1 = f->n1.d; generic_bounds(n); return n;
    case DF_ADD: case DF_MUL: case DF_MIN: case DF_MAX: {
        ON *a = wire(o, f->a), *b = wire(o, f->b);
        if (!a || !b) return NULL;
        return ap2_create(o, f->t, a, b);
    }
    case DF_RANGE_CHOICE: n = onew(o, f->t); n->a = wire(o, f->a); n->b = wire(o, f->b); n->c = wire(o, f->c); n->d0 = f->n0.d; n->d1 = f->n1.d;
        if (n->b && n->c) generic_bounds(n); return n;
    case DF_INTERVAL_SELECT:
        n = onew(o, f->t); n->a = wire(o, f->a);
        n->nthr = f->nthr; n->thr = xcalloc((size_t)f->nthr, sizeof(double)); for (int i = 0; i < f->nthr; i++) n->thr[i] = f->thr[i].d;
        n->nlist = f->nlist; n->list = xcalloc((size_t)f->nlist, sizeof(ON *));
        for (int i = 0; i < f->nlist; i++) { n->list[i] = wire(o, f->list[i]); if (!n->list[i]) return NULL; }
        generic_bounds(n); return n;
    case DF_SPLINE: n = onew(o, f->t); n->sp = wire_spline(o, f->spline); generic_bounds(n); return n;
    case DF_FIND_TOP_SURFACE: n = onew(o, f->t); n->a = wire(o, f->a); n->b = wire(o, f->b); n->i0 = f->i0; n->i1 = f->i1;
        if (n->b) generic_bounds(n); return n;
    case DF_INTERPOLATED: case DF_FLAT_CACHE: case DF_CACHE_2D: case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY:
        n = onew(o, f->t); n->a = wire(o, f->a);
        if (n->a) { if (f->t == DF_BLEND_DENSITY) { n->minv = -INFINITY; n->maxv = INFINITY; } else { n->minv = n->a->minv; n->maxv = n->a->maxv; } }
        return n;
    default: ofail(o, "26.1/26.2: тип %s не поддержан", df_type_name(f->t)); return NULL;
    }
}
static ON *wire(OldWire *o, const Df *f) {
    if (o->fail || !f) return NULL;
    for (int i = 0; i < o->nm; i++) if (o->mk[i] == f) return o->mv[i];
    ON *n = wire_impl(o, f);
    if (o->nm == o->capm) { o->capm = o->capm ? o->capm * 2 : 256; o->mk = xrealloc(o->mk, sizeof(Df *) * (size_t)o->capm); o->mv = xrealloc(o->mv, sizeof(ON *) * (size_t)o->capm); }
    o->mk[o->nm] = f; o->mv[o->nm] = n; o->nm++;
    return n;
}

/* ---------------- обёртывание (NoiseChunk.wrap через mapAll), hash-consing ---------------- */
static u64 hmix(u64 h, u64 v) { h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); return h; }
static u64 dbits(double d) { u64 b; memcpy(&b, &d, 8); return b; }
static u64 node_hash(const ON *n) {
    u64 h = (u64)n->t * 1000003ULL;
    h = hmix(h, (u64)(uintptr_t)n->a); h = hmix(h, (u64)(uintptr_t)n->b); h = hmix(h, (u64)(uintptr_t)n->c);
    for (int i = 0; i < n->nlist; i++) h = hmix(h, (u64)(uintptr_t)n->list[i]);
    for (int i = 0; i < n->nthr; i++) h = hmix(h, dbits(n->thr[i]));
    h = hmix(h, dbits(n->d0)); h = hmix(h, dbits(n->d1)); h = hmix(h, (u64)(u32)n->i0); h = hmix(h, (u64)(u32)n->i1);
    h = hmix(h, (u64)(uintptr_t)n->nn); h = hmix(h, (u64)(uintptr_t)n->bl); h = hmix(h, (u64)(uintptr_t)n->end); h = hmix(h, (u64)(uintptr_t)n->sp);
    return h;
}
static int osp_eq(const OSp *a, const OSp *b) {
    if (a == b) return 1;
    if (!a || !b || a->is_const != b->is_const) return 0;
    if (a->is_const) return jm_feq(a->value, b->value);
    if (a->n != b->n || a->coord != b->coord) return 0;
    for (int i = 0; i < a->n; i++) if (!jm_feq(a->loc[i], b->loc[i]) || !jm_feq(a->der[i], b->der[i]) || !osp_eq(a->val[i], b->val[i])) return 0;
    return 1;
}
static int node_eq(const ON *x, const ON *y) {
    if (x->t != y->t || x->a != y->a || x->b != y->b || x->c != y->c || x->nlist != y->nlist || x->nthr != y->nthr) return 0;
    for (int i = 0; i < x->nlist; i++) if (x->list[i] != y->list[i]) return 0;
    for (int i = 0; i < x->nthr; i++) if (!jm_deq(x->thr[i], y->thr[i])) return 0;
    if (!jm_deq(x->d0, y->d0) || !jm_deq(x->d1, y->d1) || x->i0 != y->i0 || x->i1 != y->i1) return 0;
    if (x->nn != y->nn || x->bl != y->bl || x->end != y->end) return 0;
    return osp_eq(x->sp, y->sp);
}
static u64 osp_hash(const OSp *s) {
    if (!s) return 5;
    if (s->is_const) { u32 b; memcpy(&b, &s->value, 4); return 7 + b; }
    u64 h = (u64)(uintptr_t)s->coord;
    for (int i = 0; i < s->n; i++) { u32 b; memcpy(&b, &s->loc[i], 4); h = hmix(h, b); memcpy(&b, &s->der[i], 4); h = hmix(h, b); h = hmix(h, osp_hash(s->val[i])); }
    return h;
}
/* вставить/найти структурно равный узел */
static ON *intern(OldWire *o, ON *n) {
    n->h = hmix(node_hash(n), osp_hash(n->sp));
    int b = (int)(n->h & (HC_SIZE - 1));
    for (ON *q = o->hc[b]; q; q = q->hnext) if (q->h == n->h && node_eq(q, n)) return q;
    n->hnext = o->hc[b]; o->hc[b] = n;
    if (n->t >= W_INTERP && n->t <= W_ALLCELL) {
        n->wid = o->nw++;
        if (o->nw > o->capw) { o->capw = o->capw ? o->capw * 2 : 32; o->wl = xrealloc(o->wl, sizeof(ON *) * (size_t)o->capw); }
        o->wl[n->wid] = n;
    }
    return n;
}
static ON *cwrap(OldWire *o, ON *n);
static OSp *cwrap_spline(OldWire *o, OSp *s) {
    if (s->is_const) return s;
    OSp *p = spnew(o); *p = *s;
    p->coord = cwrap(o, s->coord);
    p->val = xcalloc((size_t)s->n, sizeof(OSp *));
    p->loc = xcalloc((size_t)s->n, sizeof(float)); p->der = xcalloc((size_t)s->n, sizeof(float));
    memcpy(p->loc, s->loc, sizeof(float) * (size_t)s->n); memcpy(p->der, s->der, sizeof(float) * (size_t)s->n);
    for (int i = 0; i < s->n; i++) p->val[i] = cwrap_spline(o, s->val[i]);
    spline_bounds(p);
    return p;
}
static ON *cwrap(OldWire *o, ON *n) {
    if (!n) return NULL;
    if (n->t == O_HOLDER) return cwrap(o, n->a);
    ON *a = n->a ? cwrap(o, n->a) : NULL;
    ON *b = n->b ? cwrap(o, n->b) : NULL;
    ON *c = n->c ? cwrap(o, n->c) : NULL;
    ON *r;
    switch (n->t) {
    case DF_INTERPOLATED: case DF_FLAT_CACHE: case DF_CACHE_2D: case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: {
        r = onew(o, n->t == DF_INTERPOLATED ? W_INTERP : n->t == DF_FLAT_CACHE ? W_FLAT : n->t == DF_CACHE_2D ? W_CACHE2D : n->t == DF_CACHE_ONCE ? W_ONCE : W_ALLCELL);
        r->a = a; r->minv = a->minv; r->maxv = a->maxv;
        return intern(o, r);
    }
    case DF_BLEND_DENSITY: return a;          /* Blender пуст (новый мир) */
    case DF_BEARDIFIER: r = onew(o, W_BEARD); r->minv = -INFINITY; r->maxv = INFINITY; return intern(o, r);
    case O_AP2_ADD: case O_AP2_MUL: case O_AP2_MIN: case O_AP2_MAX: {
        int ty = n->t == O_AP2_ADD ? DF_ADD : n->t == O_AP2_MUL ? DF_MUL : n->t == O_AP2_MIN ? DF_MIN : DF_MAX;
        return intern(o, ap2_create(o, ty, a, b));
    }
    case O_MOA_ADD: case O_MOA_MUL: r = onew(o, n->t); r->a = a; r->d0 = n->d0; moa_bounds(r); return intern(o, r);
    default: break;
    }
    r = onew(o, n->t);
    r->a = a; r->b = b; r->c = c;
    r->d0 = n->d0; r->d1 = n->d1; r->i0 = n->i0; r->i1 = n->i1; r->nn = n->nn; r->bl = n->bl; r->end = n->end;
    r->minv = n->minv; r->maxv = n->maxv;
    if (n->nlist) { r->nlist = n->nlist; r->list = xcalloc((size_t)n->nlist, sizeof(ON *)); for (int i = 0; i < n->nlist; i++) r->list[i] = cwrap(o, n->list[i]); }
    if (n->nthr) { r->nthr = n->nthr; r->thr = xcalloc((size_t)n->nthr, sizeof(double)); memcpy(r->thr, n->thr, sizeof(double) * (size_t)n->nthr); }
    if (n->sp) r->sp = cwrap_spline(o, n->sp);
    switch (r->t) {
    case DF_ABS: case DF_SQUARE: case DF_CUBE: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_RECIPROCAL: case DF_SQUEEZE: mapped_bounds(r); break;
    case DF_CLAMP: case DF_RANGE_CHOICE: case DF_INTERVAL_SELECT: case DF_SPLINE: case DF_FIND_TOP_SURFACE: generic_bounds(r); break;
    default: break;
    }
    return intern(o, r);
}

OldWire *old_wire_new(McWorld *w, char *err, size_t errlen) {
    OldWire *o = xcalloc(1, sizeof *o);
    o->w = w; o->err = err; o->errlen = errlen;
    const NoiseSettings *ns = w->ns;
    /* порядок полей NoiseRouter 26.1/26.2 (важен лишь для порядка создания обёрток) */
    static const int ORDER[] = { RF_BARRIER, RF_FLUID_FLOOD, RF_FLUID_SPREAD, RF_LAVA, RF_TEMPERATURE, RF_VEGETATION, RF_CONTINENTS,
                                 RF_EROSION, RF_DEPTH, RF_RIDGES, RF_PRELIMINARY_SURFACE, RF_FINAL_DENSITY, RF_VEIN_TOGGLE, RF_VEIN_RIDGED, RF_VEIN_GAP };
    for (size_t k = 0; k < sizeof ORDER / sizeof ORDER[0] && !o->fail; k++) {
        int f = ORDER[k];
        if (!ns->rf[f]) { ofail(o, "noise_router: нет поля (%s)", "26.1/26.2"); break; }
        o->rf[f] = wire(o, world_rf(o->w, f));
    }
    if (o->fail) { old_wire_free(o); return NULL; }
    for (size_t k = 0; k < sizeof ORDER / sizeof ORDER[0]; k++) o->cw[ORDER[k]] = cwrap(o, o->rf[ORDER[k]]);
    /* fullNoiseValue = cacheAllInCell(add(finalDensity, beardifier)).mapAll(wrap) */
    ON *beard = onew(o, W_BEARD); beard->minv = -INFINITY; beard->maxv = INFINITY; beard = intern(o, beard);
    ON *add = intern(o, ap2_create(o, DF_ADD, o->cw[RF_FINAL_DENSITY], beard));
    ON *all = onew(o, W_ALLCELL); all->a = add; all->minv = add->minv; all->maxv = add->maxv;
    o->full = intern(o, all);
    return o;
}
void old_wire_free(OldWire *o) {
    if (!o) return;
    for (int i = 0; i < o->all.n; i++) { ON *n = o->all.v[i]; free(n->list); free(n->thr); free(n); }
    for (int i = 0; i < o->splines.n; i++) { OSp *s = o->splines.v[i]; free(s->loc); free(s->der); free(s->val); free(s); }
    pv_free(&o->all); pv_free(&o->splines);
    for (int i = 0; i < o->nblended; i++) { old_blended_free(o->blended[i]); free(o->blended[i]); }
    free(o->mk); free(o->mv); free(o->wl); free(o);
}

/* ======================================================================== NoiseChunk */
typedef struct {
    double *s0, *s1;          /* срезы [cz][cy] (cellCountXZ+1)×(cellCountY+1) */
    double n000, n001, n100, n101, n010, n011, n110, n111;
    double xz00, xz10, xz01, xz11, z0, z1, value;
} WInterp;
typedef struct {
    int kind;
    WInterp ip;
    double *flat;             /* FlatCache: (noiseSizeXZ+1)^2 */
    i64 last_pos; int has_pos; double last_val;     /* Cache2D */
    i64 lc, lac; double lv; double *la; int la_len; /* CacheOnce */
    double *cell;             /* CacheAllInCell */
} WState;

struct NChunk {
    OldWire *o;
    int cw, ch, ccxz, ccy, cell_min_y, first_cx, first_cz, first_nx, first_nz, nsxz;
    int csx, csy, csz, icx, icy, icz;
    i64 icounter, aicounter; int aidx;
    int interpolating, filling;
    WState *ws;
    int *interps; int ninterp;   /* wid интерполяторов в порядке создания */
    int *cells; int ncells;
    const void *beard;           /* Beardifier чанка (structure.c) или NULL */
    /* кэш предварительной поверхности */
    u64 *ps_key; int *ps_val; u8 *ps_used; int ps_cap, ps_n;
};

/* контекст: nc — NoiseChunk-владелец обёрток (NULL — граф без обёрток); chunk = 1 — сам контекст есть NoiseChunk */
typedef struct { NChunk *nc; int chunk; int x, y, z; } OCtx;
enum { PROV_CHUNK = 0, PROV_SLICE = 1 };

static inline int cbx(const OCtx *c) { return c->chunk ? c->nc->csx + c->nc->icx : c->x; }
static inline int cby(const OCtx *c) { return c->chunk ? c->nc->csy + c->nc->icy : c->y; }
static inline int cbz(const OCtx *c) { return c->chunk ? c->nc->csz + c->nc->icz : c->z; }

double beard_value_d(const void *b, int x, int y, int z);   /* structure.c */
static double oc(const ON *f, const OCtx *c);
static void ofill(const ON *f, double *out, int n, NChunk *nc, int prov);

static float spl_eval_o(const OSp *s, const OCtx *c) {
    if (s->is_const) return s->value;
    float in = (float)oc(s->coord, c);
    int from = 0, len = s->n;
    while (len > 0) { int half = len / 2, mid = from + half; if (in < s->loc[mid]) len = half; else { from = mid + 1; len -= half + 1; } }
    int start = from - 1, last = s->n - 1;
    if (start < 0) return lin_ext(in, s->loc, spl_eval_o(s->val[0], c), s->der, 0);
    if (start == last) return lin_ext(in, s->loc, spl_eval_o(s->val[last], c), s->der, last);
    float x1 = s->loc[start], x2 = s->loc[start + 1], t = (in - x1) / (x2 - x1);
    float d1 = s->der[start], d2 = s->der[start + 1];
    float y1 = spl_eval_o(s->val[start], c), y2 = spl_eval_o(s->val[start + 1], c);
    float a = d1 * (x2 - x1) - (y2 - y1), b = -d2 * (x2 - x1) + (y2 - y1);
    return jm_lerpf(t, y1, y2) + t * (1.0f - t) * jm_lerpf(t, a, b);
}
static double clamped_map(double v, double a, double b, double ta, double tb) {
    double f = (v - a) / (b - a);
    return f < 0.0 ? ta : (f > 1.0 ? tb : jm_lerp(f, ta, tb));
}
static double weird_rarity(int type, double r) {
    if (type == 0) return r < -0.5 ? 0.75 : (r < 0.0 ? 1.0 : (r < 0.5 ? 1.5 : 2.0));
    return r < -0.75 ? 0.5 : (r < -0.5 ? 0.75 : (r < 0.5 ? 1.0 : (r < 0.75 ? 2.0 : 3.0)));
}
static double o_find_top(const ON *f, const OCtx *c) {
    int cell = f->i1, lower = f->i0;
    int top = jm_floor_d(oc(f->b, c) / (double)cell) * cell;
    if (top <= lower) return lower;
    int bx = cbx(c), bz = cbz(c);
    for (int y = top; y >= lower; y -= cell) {
        OCtx p = { c->nc, 0, bx, y, bz };
        if (oc(f->a, &p) > 0.0) return y;
    }
    return lower;
}
/* EndIslandDensityFunction.getHeightValue (26.1/26.2): центральный остров считается здесь же; при |секция| > ~32768
 * sectionX*sectionX переполняет int (как в Java) → sqrt(отрицательного) = NaN → NaN доходит до результата. */
static float old_end_height(const GNoise *n, int sx, int sz) {
    int cx = sx / 2, cz = sz / 2, subx = sx % 2, subz = sz % 2;
    i32 sq = (i32)((u32)sx * (u32)sx + (u32)sz * (u32)sz);
    float doffs = 100.0f - (float)sqrt((double)(float)sq) * 8.0f;
    doffs = jm_clampf(doffs, -100.0f, 80.0f);
    for (int xo = -12; xo <= 12; xo++) for (int zo = -12; zo <= 12; zo++) {
        i64 tx = (i64)cx + xo, tz = (i64)cz + zo;
        if (tx * tx + tz * tz > 4096LL && simplex2_d(n, (double)tx, (double)tz) < (double)-0.9f) {
            float size = fmodf(fabsf((float)tx) * 3439.0f + fabsf((float)tz) * 147.0f, 13.0f) + 9.0f;
            float xd = (float)(subx - xo * 2), zd = (float)(subz - zo * 2);
            float nd = 100.0f - (float)sqrt((double)(xd * xd + zd * zd)) * size;
            nd = jm_clampf(nd, -100.0f, 80.0f);
            doffs = jm_maxf(doffs, nd);
        }
    }
    return doffs;
}
static inline i64 chunkpos_pack(int x, int z) { return (i64)((u64)(u32)x | ((u64)(u32)z << 32)); }

static double oc(const ON *f, const OCtx *c) {
    switch (f->t) {
    case DF_CONST: return f->d0;
    case O_HOLDER: return oc(f->a, c);
    case DF_NOISE: return old_normal_get(f->nn, cbx(c) * f->d0, cby(c) * f->d1, cbz(c) * f->d0);
    case DF_SHIFTED_NOISE: {
        double x = cbx(c) * f->d0 + oc(f->a, c);
        double y = cby(c) * f->d1 + oc(f->b, c);
        double z = cbz(c) * f->d0 + oc(f->c, c);
        return old_normal_get(f->nn, x, y, z);
    }
    case DF_SHIFT_A: return old_normal_get(f->nn, (double)cbx(c) * f->d0, 0.0 * f->d0, (double)cbz(c) * f->d0) * 4.0;
    case DF_SHIFT_B: return old_normal_get(f->nn, (double)cbz(c) * f->d0, (double)cbx(c) * f->d0, 0.0 * f->d0) * 4.0;
    case DF_SHIFT: return old_normal_get(f->nn, (double)cbx(c) * f->d0, (double)cby(c) * f->d1, (double)cbz(c) * f->d0) * 4.0;
    case DF_END_ISLANDS: return ((double)old_end_height(&f->end->s, cbx(c) / 8, cbz(c) / 8) - 8.0) / 128.0;
    case DF_Y_CLAMPED_GRADIENT: return clamped_map((double)cby(c), (double)f->i0, (double)f->i1, f->d0, f->d1);
    case DF_OLD_BLENDED_NOISE: return old_blended_compute(f->bl, cbx(c), cby(c), cbz(c));
    case DF_WEIRD_SCALED: {
        double r = weird_rarity(f->i0, oc(f->a, c));
        return r * fabs(old_normal_get(f->nn, cbx(c) / r, cby(c) / r, cbz(c) / r));
    }
    case DF_BLEND_ALPHA: return 1.0;
    case DF_BLEND_OFFSET: case DF_BEARDIFIER: return 0.0;
    case W_BEARD: return (c->chunk && c->nc && c->nc->beard) ? beard_value_d(c->nc->beard, cbx(c), cby(c), cbz(c)) : 0.0;    /* Beardifier.compute (structure.c) */
    case DF_ABS: case DF_SQUARE: case DF_CUBE: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_RECIPROCAL: case DF_SQUEEZE:
        return mapped_tr(f->t, oc(f->a, c));
    case DF_CLAMP: return jm_clamp(oc(f->a, c), f->d0, f->d1);
    case O_AP2_ADD: { double v = oc(f->a, c); return v + oc(f->b, c); }
    case O_AP2_MUL: { double v = oc(f->a, c); return v == 0.0 ? 0.0 : v * oc(f->b, c); }
    case O_AP2_MIN: { double v = oc(f->a, c); return v < f->b->minv ? v : jm_min(v, oc(f->b, c)); }
    case O_AP2_MAX: { double v = oc(f->a, c); return v > f->b->maxv ? v : jm_max(v, oc(f->b, c)); }
    case O_MOA_ADD: return oc(f->a, c) + f->d0;
    case O_MOA_MUL: return oc(f->a, c) * f->d0;
    case DF_RANGE_CHOICE: { double v = oc(f->a, c); return (v >= f->d0 && v < f->d1) ? oc(f->b, c) : oc(f->c, c); }
    case DF_INTERVAL_SELECT: {
        double v = oc(f->a, c);
        for (int i = 0; i < f->nthr; i++) if (v < f->thr[i]) return oc(f->list[i], c);
        return oc(f->list[f->nlist - 1], c);
    }
    case DF_SPLINE: return (double)spl_eval_o(f->sp, c);
    case DF_FIND_TOP_SURFACE: return o_find_top(f, c);
    case DF_INTERPOLATED: case DF_FLAT_CACHE: case DF_CACHE_2D: case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY:
        return oc(f->a, c);
    /* ---- обёртки NoiseChunk ---- */
    case W_INTERP: {
        NChunk *nc = c->nc;
        if (!c->chunk) return oc(f->a, c);
        WInterp *ip = &nc->ws[f->wid].ip;
        if (nc->filling) {
            double fx = (double)nc->icx / nc->cw, fy = (double)nc->icy / nc->ch, fz = (double)nc->icz / nc->cw;
            return jm_lerp(fz, jm_lerp(fy, jm_lerp(fx, ip->n000, ip->n100), jm_lerp(fx, ip->n010, ip->n110)),
                               jm_lerp(fy, jm_lerp(fx, ip->n001, ip->n101), jm_lerp(fx, ip->n011, ip->n111)));
        }
        return ip->value;
    }
    case W_FLAT: {
        /* FlatCache.compute не проверяет тип контекста: кварта в сетке чанка → значение из кэша */
        NChunk *cur = c->nc;
        int x = (cbx(c) >> 2) - cur->first_nx, z = (cbz(c) >> 2) - cur->first_nz, sz = cur->nsxz + 1;
        if (x >= 0 && z >= 0 && x < sz && z < sz && cur->ws[f->wid].flat) return cur->ws[f->wid].flat[x + z * sz];
        return oc(f->a, c);
    }
    case W_CACHE2D: {
        WState *s = &c->nc->ws[f->wid];
        i64 key = chunkpos_pack(cbx(c), cbz(c));
        if (s->has_pos && s->last_pos == key) return s->last_val;
        s->last_pos = key; s->has_pos = 1;
        double v = oc(f->a, c);
        s->last_val = v;
        return v;
    }
    case W_ONCE: {
        NChunk *nc = c->nc;
        if (!c->chunk) return oc(f->a, c);
        WState *s = &nc->ws[f->wid];
        if (s->la && s->lac == nc->aicounter) return s->la[nc->aidx];
        if (s->lc == nc->icounter) return s->lv;
        s->lc = nc->icounter;
        double v = oc(f->a, c);
        s->lv = v;
        return v;
    }
    case W_ALLCELL: {
        NChunk *nc = c->nc;
        if (!c->chunk) return oc(f->a, c);
        int x = nc->icx, y = nc->icy, z = nc->icz;
        if (x >= 0 && y >= 0 && z >= 0 && x < nc->cw && y < nc->ch && z < nc->cw)
            return nc->ws[f->wid].cell[((nc->ch - 1 - y) * nc->cw + x) * nc->cw + z];
        return oc(f->a, c);
    }
    }
    return 0.0;
}

/* поставщик контекста: forIndex */
static void prov_for_index(NChunk *nc, int prov, int i) {
    if (prov == PROV_SLICE) {
        nc->csy = (i + nc->cell_min_y) * nc->ch; nc->icounter++; nc->icy = 0; nc->aidx = i;
    } else {
        int zi = jm_floormod(i, nc->cw), xy = jm_floordiv(i, nc->cw), xi = jm_floormod(xy, nc->cw), yi = nc->ch - 1 - jm_floordiv(xy, nc->cw);
        nc->icx = xi; nc->icy = yi; nc->icz = zi; nc->aidx = i;
    }
}
static void fill_directly(const ON *f, double *out, int n, NChunk *nc, int prov) {
    OCtx c = { nc, 1, 0, 0, 0 };
    if (prov == PROV_SLICE) {
        for (int cy = 0; cy < nc->ccy + 1; cy++) {
            nc->csy = (cy + nc->cell_min_y) * nc->ch; nc->icounter++; nc->icy = 0; nc->aidx = cy;
            out[cy] = oc(f, &c);
        }
    } else {
        nc->aidx = 0;
        for (int yi = nc->ch - 1; yi >= 0; yi--) {
            nc->icy = yi;
            for (int xi = 0; xi < nc->cw; xi++) {
                nc->icx = xi;
                for (int zi = 0; zi < nc->cw; zi++) { nc->icz = zi; out[nc->aidx++] = oc(f, &c); }
            }
        }
    }
    (void)n;
}
static void ofill(const ON *f, double *out, int n, NChunk *nc, int prov) {
    OCtx c = { nc, 1, 0, 0, 0 };
    switch (f->t) {
    case DF_CONST: for (int i = 0; i < n; i++) out[i] = f->d0; return;
    case DF_BLEND_ALPHA: for (int i = 0; i < n; i++) out[i] = 1.0; return;
    case DF_BLEND_OFFSET: case DF_BEARDIFIER: for (int i = 0; i < n; i++) out[i] = 0.0; return;
    case O_HOLDER: ofill(f->a, out, n, nc, prov); return;
    case DF_ABS: case DF_SQUARE: case DF_CUBE: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_RECIPROCAL: case DF_SQUEEZE:
        ofill(f->a, out, n, nc, prov); for (int i = 0; i < n; i++) out[i] = mapped_tr(f->t, out[i]); return;
    case DF_CLAMP: ofill(f->a, out, n, nc, prov); for (int i = 0; i < n; i++) out[i] = jm_clamp(out[i], f->d0, f->d1); return;
    case O_MOA_ADD: ofill(f->a, out, n, nc, prov); for (int i = 0; i < n; i++) out[i] = out[i] + f->d0; return;
    case O_MOA_MUL: ofill(f->a, out, n, nc, prov); for (int i = 0; i < n; i++) out[i] = out[i] * f->d0; return;
    case DF_WEIRD_SCALED:
        ofill(f->a, out, n, nc, prov);
        for (int i = 0; i < n; i++) { prov_for_index(nc, prov, i); double r = weird_rarity(f->i0, out[i]);
            out[i] = r * fabs(old_normal_get(f->nn, cbx(&c) / r, cby(&c) / r, cbz(&c) / r)); }
        return;
    case O_AP2_ADD: {
        ofill(f->a, out, n, nc, prov);
        double *v2 = xmalloc(sizeof(double) * (size_t)n);
        ofill(f->b, v2, n, nc, prov);
        for (int i = 0; i < n; i++) out[i] += v2[i];
        free(v2); return;
    }
    case O_AP2_MUL: ofill(f->a, out, n, nc, prov);
        for (int i = 0; i < n; i++) { double v = out[i]; if (v == 0.0) out[i] = 0.0; else { prov_for_index(nc, prov, i); out[i] = v * oc(f->b, &c); } }
        return;
    case O_AP2_MIN: ofill(f->a, out, n, nc, prov);
        for (int i = 0; i < n; i++) { double v = out[i]; if (v < f->b->minv) out[i] = v; else { prov_for_index(nc, prov, i); out[i] = jm_min(v, oc(f->b, &c)); } }
        return;
    case O_AP2_MAX: ofill(f->a, out, n, nc, prov);
        for (int i = 0; i < n; i++) { double v = out[i]; if (v > f->b->maxv) out[i] = v; else { prov_for_index(nc, prov, i); out[i] = jm_max(v, oc(f->b, &c)); } }
        return;
    case DF_RANGE_CHOICE: ofill(f->a, out, n, nc, prov);
        for (int i = 0; i < n; i++) { double v = out[i]; prov_for_index(nc, prov, i); out[i] = (v >= f->d0 && v < f->d1) ? oc(f->b, &c) : oc(f->c, &c); }
        return;
    case DF_INTERVAL_SELECT: ofill(f->a, out, n, nc, prov);
        for (int i = 0; i < n; i++) {
            double v = out[i]; prov_for_index(nc, prov, i);
            int k = 0; for (; k < f->nthr; k++) if (v < f->thr[k]) break;
            out[i] = oc(k < f->nthr ? f->list[k] : f->list[f->nlist - 1], &c);
        }
        return;
    case DF_INTERPOLATED: case DF_FLAT_CACHE: case DF_CACHE_2D: case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY:
        ofill(f->a, out, n, nc, prov); return;
    case W_INTERP: if (nc->filling) fill_directly(f, out, n, nc, prov); else ofill(f->a, out, n, nc, prov); return;
    case W_CACHE2D: ofill(f->a, out, n, nc, prov); return;
    case W_ONCE: {
        WState *s = &nc->ws[f->wid];
        if (s->la && s->lac == nc->aicounter) { memcpy(out, s->la, sizeof(double) * (size_t)n); return; }
        ofill(f->a, out, n, nc, prov);
        if (!s->la || s->la_len != n) { free(s->la); s->la = xmalloc(sizeof(double) * (size_t)n); s->la_len = n; }
        memcpy(s->la, out, sizeof(double) * (size_t)n);
        s->lac = nc->aicounter;
        return;
    }
    case W_BEARD:
        if (!nc->beard) { for (int i = 0; i < n; i++) out[i] = 0.0; return; }
        for (int i = 0; i < n; i++) { prov_for_index(nc, prov, i); out[i] = beard_value_d(nc->beard, cbx(&c), cby(&c), cbz(&c)); }
        return;
    default: fill_directly(f, out, n, nc, prov); return;   /* SimpleFunction, Noise, Spline, FindTopSurface, FlatCache, CacheAllInCell … */
    }
}

/* ---- точечные функции ---- */
double old_router_point(void *ov, int field, int x, int y, int z) {
    OldWire *o = ov;
    OCtx c = { NULL, 0, x, y, z };
    return oc(o->rf[field], &c);
}
int old_df_point(OldWire *o, const Df *f, int n, const int *xyz, double *out, char *err, size_t errlen) {
    o->err = err; o->errlen = errlen; o->fail = 0;
    ON *r = wire(o, f);
    if (!r || o->fail) return MCGEN_E_DATA;
    for (int i = 0; i < n; i++) { OCtx c = { NULL, 0, xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2] }; out[i] = oc(r, &c); }
    return MCGEN_OK;
}

/* ---- NoiseChunk ---- */
NChunk *nchunk_new(OldWire *o) {
    NChunk *c = xcalloc(1, sizeof *c);
    c->o = o;
    c->ws = xcalloc((size_t)(o->nw ? o->nw : 1), sizeof(WState));
    c->interps = xcalloc((size_t)(o->nw ? o->nw : 1), sizeof(int));
    c->cells = xcalloc((size_t)(o->nw ? o->nw : 1), sizeof(int));
    for (int i = 0; i < o->nw; i++) {
        c->ws[i].kind = o->wl[i]->t;
        if (o->wl[i]->t == W_INTERP) c->interps[c->ninterp++] = i;
        if (o->wl[i]->t == W_ALLCELL) c->cells[c->ncells++] = i;
    }
    return c;
}
void nchunk_free(NChunk *c) {
    if (!c) return;
    for (int i = 0; i < c->o->nw; i++) { WState *s = &c->ws[i]; free(s->ip.s0); free(s->ip.s1); free(s->flat); free(s->la); free(s->cell); }
    free(c->ws); free(c->interps); free(c->cells); free(c->ps_key); free(c->ps_val); free(c->ps_used); free(c);
}
int nchunk_cell_width(const NChunk *c) { return c->cw; }
int nchunk_cell_height(const NChunk *c) { return c->ch; }
int nchunk_block_x(const NChunk *c) { return c->csx + c->icx; }
int nchunk_block_y(const NChunk *c) { return c->csy + c->icy; }
int nchunk_block_z(const NChunk *c) { return c->csz + c->icz; }

void nchunk_begin(NChunk *c, int cminx, int cminz, int min_y, int height) { nchunk_begin_cells(c, cminx, cminz, min_y, height, 0); }
/* ccxz > 0 — число ячеек по XZ (NoiseChunk.forColumn: 1); 0 — на чанк (16 / cellWidth) */
void nchunk_begin_cells(NChunk *c, int cminx, int cminz, int min_y, int height, int ccxz) {
    OldWire *o = c->o;
    const NoiseSettings *ns = o->w->ns;
    c->cw = ns->size_h << 2; c->ch = ns->size_v << 2;
    c->ccxz = ccxz > 0 ? ccxz : 16 / c->cw;
    c->ccy = jm_floordiv(height, c->ch);
    c->cell_min_y = jm_floordiv(min_y, c->ch);
    c->first_cx = jm_floordiv(cminx, c->cw); c->first_cz = jm_floordiv(cminz, c->cw);
    c->first_nx = cminx >> 2; c->first_nz = cminz >> 2;
    c->nsxz = (c->ccxz * c->cw) >> 2;
    c->csx = c->csy = c->csz = c->icx = c->icy = c->icz = 0;
    c->icounter = c->aicounter = 0; c->aidx = 0; c->interpolating = c->filling = 0;
    c->ps_n = 0; if (c->ps_used) memset(c->ps_used, 0, (size_t)c->ps_cap);
    int sxz = c->nsxz + 1;
    for (int i = 0; i < o->nw; i++) {
        WState *s = &c->ws[i];
        s->has_pos = 0; s->lc = 0; s->lac = 0; s->lv = 0;
        if (s->la) { free(s->la); s->la = NULL; s->la_len = 0; }
        if (s->kind == W_INTERP) {
            int sz = (c->ccxz + 1) * (c->ccy + 1);
            if (!s->ip.s0) { s->ip.s0 = xcalloc((size_t)sz, sizeof(double)); s->ip.s1 = xcalloc((size_t)sz, sizeof(double)); }
        } else if (s->kind == W_ALLCELL) {
            if (!s->cell) s->cell = xcalloc((size_t)(c->cw * c->cw * c->ch), sizeof(double));
        }
    }
    /* плоские кэши заполняются при создании (в порядке создания обёрток) */
    for (int i = 0; i < o->nw; i++) {
        WState *s = &c->ws[i];
        if (s->kind != W_FLAT) continue;
        free(s->flat); s->flat = NULL;
        double *v = xcalloc((size_t)(sxz * sxz), sizeof(double));
        for (int x = 0; x <= c->nsxz; x++) for (int z = 0; z <= c->nsxz; z++) {
            OCtx p = { c, 0, (c->first_nx + x) << 2, 0, (c->first_nz + z) << 2 };
            v[x + z * sxz] = oc(o->wl[i]->a, &p);
        }
        s->flat = v;
    }
}
static double *slice_at(WInterp *ip, int which, int cz, int ccy) { return (which ? ip->s1 : ip->s0) + (size_t)cz * (size_t)(ccy + 1); }
static void fill_slice(NChunk *c, int slice0, int cell_x) {
    c->csx = cell_x * c->cw; c->icx = 0;
    for (int cz = 0; cz < c->ccxz + 1; cz++) {
        c->csz = (c->first_cz + cz) * c->cw; c->icz = 0;
        c->aicounter++;
        for (int k = 0; k < c->ninterp; k++) {
            int wid = c->interps[k];
            double *sl = slice_at(&c->ws[wid].ip, !slice0, cz, c->ccy);
            ofill(c->o->wl[wid], sl, c->ccy + 1, c, PROV_SLICE);
        }
    }
    c->aicounter++;
}
void nchunk_init_first_cell_x(NChunk *c) { c->interpolating = 1; c->icounter = 0; fill_slice(c, 1, c->first_cx); }
void nchunk_advance_cell_x(NChunk *c, int cxi) { fill_slice(c, 0, c->first_cx + cxi + 1); c->csx = (c->first_cx + cxi) * c->cw; }
void nchunk_select_cell_yz(NChunk *c, int cyi, int czi) {
    for (int k = 0; k < c->ninterp; k++) {
        WInterp *ip = &c->ws[c->interps[k]].ip;
        double *a0 = slice_at(ip, 0, czi, c->ccy), *a1 = slice_at(ip, 0, czi + 1, c->ccy);
        double *b0 = slice_at(ip, 1, czi, c->ccy), *b1 = slice_at(ip, 1, czi + 1, c->ccy);
        ip->n000 = a0[cyi]; ip->n001 = a1[cyi]; ip->n100 = b0[cyi]; ip->n101 = b1[cyi];
        ip->n010 = a0[cyi + 1]; ip->n011 = a1[cyi + 1]; ip->n110 = b0[cyi + 1]; ip->n111 = b1[cyi + 1];
    }
    c->filling = 1;
    c->csy = (cyi + c->cell_min_y) * c->ch;
    c->csz = (c->first_cz + czi) * c->cw;
    c->aicounter++;
    int n = c->cw * c->cw * c->ch;
    for (int k = 0; k < c->ncells; k++) {
        int wid = c->cells[k];
        ofill(c->o->wl[wid]->a, c->ws[wid].cell, n, c, PROV_CHUNK);
    }
    c->aicounter++;
    c->filling = 0;
}
void nchunk_update_y(NChunk *c, int pos_y, double fy) {
    c->icy = pos_y - c->csy;
    for (int k = 0; k < c->ninterp; k++) {
        WInterp *ip = &c->ws[c->interps[k]].ip;
        ip->xz00 = jm_lerp(fy, ip->n000, ip->n010); ip->xz10 = jm_lerp(fy, ip->n100, ip->n110);
        ip->xz01 = jm_lerp(fy, ip->n001, ip->n011); ip->xz11 = jm_lerp(fy, ip->n101, ip->n111);
    }
}
void nchunk_update_x(NChunk *c, int pos_x, double fx) {
    c->icx = pos_x - c->csx;
    for (int k = 0; k < c->ninterp; k++) {
        WInterp *ip = &c->ws[c->interps[k]].ip;
        ip->z0 = jm_lerp(fx, ip->xz00, ip->xz10); ip->z1 = jm_lerp(fx, ip->xz01, ip->xz11);
    }
}
void nchunk_update_z(NChunk *c, int pos_z, double fz) {
    c->icz = pos_z - c->csz;
    c->icounter++;
    for (int k = 0; k < c->ninterp; k++) { WInterp *ip = &c->ws[c->interps[k]].ip; ip->value = jm_lerp(fz, ip->z0, ip->z1); }
}
void nchunk_swap_slices(NChunk *c) {
    for (int k = 0; k < c->ninterp; k++) { WInterp *ip = &c->ws[c->interps[k]].ip; double *t = ip->s0; ip->s0 = ip->s1; ip->s1 = t; }
}
void nchunk_stop(NChunk *c) { c->interpolating = 0; }
void nchunk_set_beard(NChunk *c, const void *beard) { c->beard = beard; }
double nchunk_full_density(NChunk *c) { OCtx x = { c, 1, 0, 0, 0 }; return oc(c->o->full, &x); }
double nchunk_router_here(NChunk *c, int field) { OCtx x = { c, 1, 0, 0, 0 }; return oc(c->o->cw[field], &x); }
double nchunk_router_point(NChunk *c, int field, int x, int y, int z) { OCtx p = { c, 0, x, y, z }; return oc(c->o->cw[field], &p); }

static int *ps_slot(NChunk *c, u64 key, int *found) {
    if ((c->ps_n + 1) * 2 > c->ps_cap) {
        int oc_ = c->ps_cap; u64 *ok = c->ps_key; int *ov = c->ps_val; u8 *ou = c->ps_used;
        c->ps_cap = oc_ ? oc_ * 2 : 256; c->ps_key = xcalloc((size_t)c->ps_cap, 8); c->ps_val = xcalloc((size_t)c->ps_cap, sizeof(int)); c->ps_used = xcalloc((size_t)c->ps_cap, 1); c->ps_n = 0;
        for (int i = 0; i < oc_; i++) if (ou && ou[i]) { int f; *ps_slot(c, ok[i], &f) = ov[i]; }
        free(ok); free(ov); free(ou);
    }
    int i = (int)((key * 0x9E3779B97F4A7C15ULL) >> 40) & (c->ps_cap - 1);
    while (c->ps_used[i] && c->ps_key[i] != key) i = (i + 1) & (c->ps_cap - 1);
    *found = c->ps_used[i];
    if (!c->ps_used[i]) { c->ps_used[i] = 1; c->ps_key[i] = key; c->ps_n++; }
    return &c->ps_val[i];
}
int nchunk_prelim_surface(NChunk *c, int x, int z) {
    int qx = (x >> 2) << 2, qz = (z >> 2) << 2;
    u64 key = (u64)(u32)qx | ((u64)(u32)qz << 32);    /* ColumnPos.asLong */
    int found; int *v = ps_slot(c, key, &found);
    if (!found) { OCtx p = { c, 0, qx, 0, qz }; *v = jm_floor_d(oc(c->o->cw[RF_PRELIMINARY_SURFACE], &p)); }
    return *v;
}
int nchunk_max_prelim_surface(NChunk *c, int minx, int minz, int maxx, int maxz) {
    int m = (int)0x80000000u;
    for (int z = minz; z <= maxz; z += 4) for (int x = minx; x <= maxx; x += 4) { int s = nchunk_prelim_surface(c, x, z); if (s > m) m = s; }
    return m;
}

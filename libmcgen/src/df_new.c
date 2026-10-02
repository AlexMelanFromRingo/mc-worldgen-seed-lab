/* df_new.c — density-функции 26.3+: оптимизация (DensityFunctionCompiler), компиляция в сэмплеры и вычисление.
 * Каждая ветка ниже соответствует классу сэмплера игры (в комментарии — имя класса); арифметика и порядок
 * операций повторяют Java (float, с double там, где в игре double). */
#include "df_new.h"
#include <stdio.h>
#include <stdlib.h>

/* ============================================================== переписанная функция (после rewrite) */
enum { R_PREPARED = DF__COUNT + 1 };

typedef struct R R;
typedef struct RSpline RSpline;
struct RSpline { const DfSpline *src; R *coord; RSpline **val; };
struct R {
    int t;
    const Df *src;              /* параметры (числа, имя шума, …) — из исходного узла */
    R *a, *b, *c;
    R **list; int nlist;
    RSpline *sp;
    int sl_axis, sl_coord;      /* DF_SLICE */
    /* PreparedCache */
    int cache_id; S *cs; int p_axes; Ival p_range;
    /* мемо */
    int axes_memo; Ival range_memo; int has_axes, has_range;
};

/* ============================================================== скомпилированный сэмплер */
enum {
    K_CONST, K_NOISE, K_NOISE_XZ, K_NOISE_XYZ, K_SHIFT_B, K_END, K_DIST, K_GRAD_CLAMP, K_GRAD_REPEAT, K_GRAD_MIRROR,
    K_CTX_ALPHA, K_CTX_OFFSET, K_CTX_BEARD,
    K_ABS, K_SQUARE, K_CUBE, K_SQRT, K_LEAKY, K_RECIP, K_NEG, K_SQUEEZE, K_LOG, K_SIGN,
    K_ROUND_INT, K_ROUND,
    K_ADD, K_CADD, K_CSUB, K_SUB, K_MUL, K_CMUL, K_DIV, K_CDIV, K_MIN, K_CMIN, K_MAX, K_CMAX,
    K_POW_CB, K_POW_CE, K_POW,
    K_SPLINE, K_LERP, K_LERP_CF, K_LERP_CS, K_CLAMP, K_RANGE_C, K_RANGE, K_ISEL1, K_ISEL,
    K_CACHE, K_BLEND_DENSITY, K_INTERP, K_SLICE_X, K_SLICE_Y, K_SLICE_Z, K_SLICE_XZ, K_FTS
};

typedef struct SSp SSp;
struct SSp { int is_const; float value; int ci; int n; float *loc, *der; SSp **val; };

struct S {
    int k;
    S *a, *b, *c;
    S **arr; int narr;
    float f0, f1, f2, f3;
    double d0, d1;
    int i0, i1, i2, i3;
    const NStack *ns;
    const GNoise *gn;
    float *thr; int nthr;
    SSp *sp; S **coords; int ncoord;
    int cid;
};

/* ============================================================== компилятор */
typedef struct { const Df *key; R *node; } PrepEnt;
typedef struct Mem { struct Mem *next; } Mem;
struct NComp {
    NEnv env;
    PrepEnt *prep; int nprep, capprep;
    int next_cache_id;
    Mem *mem;                       /* все выделения компилятора */
    char *err; size_t errlen; int fail;
    /* мемо nc_get по указателю исходной функции */
    const Df **got_key; const S **got_val; int ngot, capgot;
};

static void *cmem(NComp *c, size_t n) {
    Mem *m = xcalloc(1, sizeof(Mem) + n);
    m->next = c->mem; c->mem = m;
    return (void *)(m + 1);
}
static void cfail(NComp *c, const char *fmt, const char *a) {
    if (!c->fail) set_err(c->err, c->errlen, fmt, a ? a : "");
    c->fail = 1;
}

NComp *nc_new(const NEnv *env) { NComp *c = xcalloc(1, sizeof *c); c->env = *env; return c; }
void nc_free(NComp *c) {
    if (!c) return;
    Mem *m = c->mem; while (m) { Mem *n = m->next; free(m); m = n; }
    free(c->prep); free(c->got_key); free(c->got_val); free(c);
}
int nc_cache_count(const NComp *c) { return c->next_cache_id; }

static R *rnew(NComp *c, int t, const Df *src) { R *r = cmem(c, sizeof(R)); r->t = t; r->src = src; return r; }

/* ---------------- оси (domainAxes) ---------------- */
static int axes_of(NComp *c, R *r);
static void spline_axes(NComp *c, RSpline *s, int *ax) {
    if (!s || s->src->is_const) return;
    *ax |= axes_of(c, s->coord);
    for (int i = 0; i < s->src->n; i++) spline_axes(c, s->val[i], ax);
}
static int axes_of(NComp *c, R *r) {
    if (r->has_axes) return r->axes_memo;
    int ax = 0;
    switch (r->t) {
    case DF_CONST: ax = 0; break;
    case DF_NOISE:
        ax = 7;
        if (r->src->n1.d == 0.0) ax &= ~2;
        if (r->src->n0.d == 0.0) ax &= ~5;
        ax |= axes_of(c, r->a) | axes_of(c, r->b) | axes_of(c, r->c);
        break;
    case DF_END_OUTER_ISLANDS: ax = 5; break;
    case DF_DISTANCE_TO_POINT: ax = 7; break;
    case DF_GRADIENT: ax = 1 << r->src->i0; break;
    case DF_SHIFT_A: case DF_SHIFT_B: ax = 5; break;
    case DF_SHIFT: ax = 7; break;
    case DF_BLEND_ALPHA: case DF_BLEND_OFFSET: ax = 5; break;
    case DF_BEARDIFIER: ax = 7; break;
    case DF_OLD_BLENDED_NOISE: ax = 7; break;
    case DF_ABS: case DF_SQUARE: case DF_CUBE: case DF_SQRT: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_RECIPROCAL:
    case DF_NEGATE: case DF_SQUEEZE: case DF_LOG: case DF_SIGN: case DF_CLAMP: case DF_BLEND_DENSITY: case DF_INTERPOLATED:
        ax = axes_of(c, r->a); break;
    case DF_FLOOR: case DF_ROUND: case DF_CEIL: case DF_TRUNCATE:
    case DF_ADD: case DF_SUB: case DF_MUL: case DF_DIV: case DF_MIN: case DF_MAX: case DF_POW:
        ax = axes_of(c, r->a) | axes_of(c, r->b); break;
    case DF_LERP: case DF_RANGE_CHOICE: ax = axes_of(c, r->a) | axes_of(c, r->b) | axes_of(c, r->c); break;
    case DF_INTERVAL_SELECT: ax = axes_of(c, r->a); for (int i = 0; i < r->nlist; i++) ax |= axes_of(c, r->list[i]); break;
    case DF_SPLINE: spline_axes(c, r->sp, &ax); break;
    case DF_SLICE: ax = axes_of(c, r->a) & ~(1 << r->sl_axis); break;
    case DF_FIND_TOP_SURFACE: ax = (axes_of(c, r->a) | axes_of(c, r->b)) & ~2; break;
    case R_PREPARED: ax = r->p_axes; break;
    default: cfail(c, "axes: тип %s", df_type_name((DfType)r->t)); break;
    }
    r->axes_memo = ax; r->has_axes = 1;
    return ax;
}

/* ---------------- диапазон (range) ---------------- */
static Ival range_of(NComp *c, R *r);
static float lin_ext(float in, const DNum *loc, float value, const DNum *der, int i) {
    float d = der[i].f;
    return d == 0.0f ? value : value + d * (in - loc[i].f);
}
static Ival spline_range(NComp *c, RSpline *s) {
    const DfSpline *d = s->src;
    if (d->is_const) return iv_exact(d->value.f);
    int last = d->n - 1;
    float mn = INFINITY, mx = -INFINITY;
    Ival in = range_of(c, s->coord);
    if (in.nai) return in;
    Ival *vr = xcalloc((size_t)d->n, sizeof(Ival));
    for (int i = 0; i < d->n; i++) vr[i] = spline_range(c, s->val[i]);
    if (in.lo < d->loc[0].f) {
        Ival fr = vr[0];
        float e1 = lin_ext(in.lo, d->loc, fr.lo, d->der, 0), e2 = lin_ext(in.lo, d->loc, fr.hi, d->der, 0);
        mn = jm_minf(mn, jm_minf(e1, e2)); mx = jm_maxf(mx, jm_maxf(e1, e2));
    }
    if (in.hi > d->loc[last].f) {
        Ival lr = vr[last];
        float e1 = lin_ext(in.hi, d->loc, lr.lo, d->der, last), e2 = lin_ext(in.hi, d->loc, lr.hi, d->der, last);
        mn = jm_minf(mn, jm_minf(e1, e2)); mx = jm_maxf(mx, jm_maxf(e1, e2));
    }
    for (int i = 0; i < d->n; i++) { mn = jm_minf(mn, vr[i].lo); mx = jm_maxf(mx, vr[i].hi); }
    for (int i = 0; i < last; i++) {
        float x1 = d->loc[i].f, x2 = d->loc[i + 1].f, xd = x2 - x1;
        float min1 = vr[i].lo, max1 = vr[i].hi, min2 = vr[i + 1].lo, max2 = vr[i + 1].hi;
        float d1 = d->der[i].f, d2 = d->der[i + 1].f;
        if (d1 != 0.0f || d2 != 0.0f) {
            float p1 = d1 * xd, p2 = d2 * xd;
            float minL1 = jm_minf(min1, min2), maxL1 = jm_maxf(max1, max2);
            float minA = p1 - max2 + min1, maxA = p1 - min2 + max1;
            float minB = -p2 + min2 - max1, maxB = -p2 + max2 - min1;
            float minL2 = jm_minf(minA, minB), maxL2 = jm_maxf(maxA, maxB);
            mn = jm_minf(mn, minL1 + 0.25f * minL2);
            mx = jm_maxf(mx, maxL1 + 0.25f * maxL2);
        }
    }
    free(vr);
    return iv_of(mn, mx);
}
static float f_cube(float v) { return v * v * v; }
static float f_leaky05(float v) { return v > 0.0f ? v : v * 0.5f; }
static float f_leaky025(float v) { return v > 0.0f ? v : v * 0.25f; }
static float f_squeeze(float v) { float x = jm_clampf(v, -1.0f, 1.0f); return x / 2.0f - (x * x * x) / 24.0f; }
static float round_to_int(float in, int type) {
    switch (type) {
    case DF_FLOOR: return (float)floor((double)in);
    case DF_ROUND: return (float)jm_roundf(in);
    case DF_CEIL: return (float)ceil((double)in);
    default: return in > 0.0f ? (float)floor((double)in) : (float)ceil((double)in);
    }
}
static int g_round_type;
static float f_round_cur(float v) { return round_to_int(v, g_round_type); }

static Ival range_of(NComp *c, R *r) {
    if (r->has_range) return r->range_memo;
    Ival v = iv_inf();
    const Df *s = r->src;
    switch (r->t) {
    case DF_CONST: v = iv_exact(s->n0.f); break;
    case DF_NOISE: v = c->env.noise_range(c->env.ud, s->name); break;
    case DF_SHIFT_A: case DF_SHIFT_B: case DF_SHIFT: v = iv_mul(c->env.noise_range(c->env.ud, s->name), iv_exact(4.0f)); break;
    case DF_END_OUTER_ISLANDS: v = iv_of(-0.84375f, 0.5625f); break;
    case DF_DISTANCE_TO_POINT: v = iv_of(0.0f, INFINITY); break;
    case DF_GRADIENT: v = iv_encaps2(s->n0.f, s->n1.f); break;
    case DF_BLEND_ALPHA: v = iv_of(0.0f, 1.0f); break;
    case DF_BLEND_OFFSET: case DF_BEARDIFIER: v = iv_inf(); break;
    case DF_OLD_BLENDED_NOISE: v = blended_new_range(s->n1.d, s->n4.d); break;
    case DF_ABS: v = iv_abs(range_of(c, r->a)); break;
    case DF_SQUARE: v = iv_square(range_of(c, r->a)); break;
    case DF_CUBE: v = iv_map_monotonic(range_of(c, r->a), f_cube); break;
    case DF_SQRT: v = iv_pow(range_of(c, r->a), iv_exact(0.5f)); break;
    case DF_HALF_NEGATIVE: v = iv_map_monotonic(range_of(c, r->a), f_leaky05); break;
    case DF_QUARTER_NEGATIVE: v = iv_map_monotonic(range_of(c, r->a), f_leaky025); break;
    case DF_RECIPROCAL: v = iv_reciprocal(range_of(c, r->a)); break;
    case DF_NEGATE: v = iv_sub(iv_exact(0.0f), range_of(c, r->a)); break;
    case DF_SQUEEZE: v = iv_map_monotonic(range_of(c, r->a), f_squeeze); break;
    case DF_LOG: v = iv_log(range_of(c, r->a)); break;
    case DF_SIGN: v = iv_sign(range_of(c, r->a)); break;
    case DF_FLOOR: case DF_ROUND: case DF_CEIL: case DF_TRUNCATE: {
        Ival m = range_of(c, r->b);
        g_round_type = r->t;
        v = iv_mul(iv_map_monotonic(iv_div(range_of(c, r->a), m), f_round_cur), m);
        break;
    }
    case DF_ADD: v = iv_add(range_of(c, r->a), range_of(c, r->b)); break;
    case DF_SUB: v = iv_sub(range_of(c, r->a), range_of(c, r->b)); break;
    case DF_MUL: v = iv_mul(range_of(c, r->a), range_of(c, r->b)); break;
    case DF_DIV: v = iv_div(range_of(c, r->a), range_of(c, r->b)); break;
    case DF_MIN: v = iv_min(range_of(c, r->a), range_of(c, r->b)); break;
    case DF_MAX: v = iv_max(range_of(c, r->a), range_of(c, r->b)); break;
    case DF_POW: v = iv_pow(range_of(c, r->a), range_of(c, r->b)); break;
    case DF_SPLINE: v = spline_range(c, r->sp); break;
    case DF_LERP: v = iv_lerp(range_of(c, r->a), range_of(c, r->b), range_of(c, r->c)); break;
    case DF_CLAMP: v = iv_clamp(range_of(c, r->a), s->n0.f, s->n1.f); break;
    case DF_RANGE_CHOICE: { Ival t[2] = { range_of(c, r->b), range_of(c, r->c) }; v = iv_encaps(t, 2); break; }
    case DF_INTERVAL_SELECT: {
        Ival *t = xcalloc((size_t)r->nlist, sizeof(Ival));
        for (int i = 0; i < r->nlist; i++) t[i] = range_of(c, r->list[i]);
        v = iv_encaps(t, r->nlist); free(t); break;
    }
    case DF_BLEND_DENSITY: case DF_INTERPOLATED: case DF_SLICE: v = range_of(c, r->a); break;
    case DF_FIND_TOP_SURFACE: { float lb = (float)s->i0; v = iv_of(lb, jm_maxf(lb, range_of(c, r->b).hi)); break; }
    case R_PREPARED: v = r->p_range; break;
    default: cfail(c, "range: тип %s", df_type_name((DfType)r->t)); break;
    }
    r->range_memo = v; r->has_range = 1;
    return v;
}

/* ---------------- переписывание ---------------- */
typedef R *(*Rule)(NComp *c, R *r, int arg);
static R *opt_ast(NComp *c, const Df *f);
static R *slice_rule(NComp *c, R *r, int parent);
static S *compile(NComp *c, R *r);

/* AST → R c применением opt к детям (rewriteChildren(optimizer)) */
static RSpline *opt_spline(NComp *c, const DfSpline *s) {
    RSpline *r = cmem(c, sizeof(RSpline)); r->src = s;
    if (s->is_const) return r;
    r->coord = opt_ast(c, s->coord);
    r->val = cmem(c, sizeof(RSpline *) * (size_t)s->n);
    for (int i = 0; i < s->n; i++) r->val[i] = opt_spline(c, s->val[i]);
    return r;
}
static R *prepared(NComp *c, const Df *cache);
static R *opt_ast(NComp *c, const Df *f) {
    if (c->fail || !f) return NULL;
    if (f->t == DF_REF) {                    /* INLINE_REFERENCE (один уровень), далее как у функции-значения */
        const Df *t = c->env.ref(c->env.ud, f->name);
        if (!t) { cfail(c, "нет density_function %s", f->name); return NULL; }
        f = t;
        if (f->t == DF_REF) return opt_ast(c, f);   /* HolderHolder.rewriteChildren(rule) = rule.rewrite(value) */
    }
    if (f->t == DF_CACHE) return prepared(c, f);
    R *r = rnew(c, f->t, f);
    if (f->a) r->a = opt_ast(c, f->a);
    if (f->b) r->b = opt_ast(c, f->b);
    if (f->c) r->c = opt_ast(c, f->c);
    if (f->nlist) { r->nlist = f->nlist; r->list = cmem(c, sizeof(R *) * (size_t)f->nlist); for (int i = 0; i < f->nlist; i++) r->list[i] = opt_ast(c, f->list[i]); }
    if (f->spline) r->sp = opt_spline(c, f->spline);
    if (f->t == DF_SLICE) { r->sl_axis = f->i0; r->sl_coord = f->i1; }
    return r;
}
static R *full_opt(NComp *c, const Df *f) { R *r = opt_ast(c, f); return c->fail ? NULL : slice_rule(c, r, 7); }

static R *prepared(NComp *c, const Df *cache) {
    for (int i = 0; i < c->nprep; i++) if (df_equal(c->prep[i].key, cache->a)) return c->prep[i].node;
    R *node = rnew(c, R_PREPARED, cache);
    node->cache_id = c->next_cache_id++;
    /* регистрируем до рекурсии не нужно (циклов нет); как в игре — после подготовки */
    R *in = full_opt(c, cache->a);
    if (c->fail) return NULL;
    node->p_range = range_of(c, in);
    node->p_axes = axes_of(c, in);
    S *cs = cmem(c, sizeof(S));
    cs->k = K_CACHE; cs->cid = node->cache_id; cs->a = compile(c, in);
    node->cs = cs;
    if (c->nprep == c->capprep) { c->capprep = c->capprep ? c->capprep * 2 : 32; c->prep = xrealloc(c->prep, sizeof(PrepEnt) * (size_t)c->capprep); }
    c->prep[c->nprep].key = cache->a; c->prep[c->nprep].node = node; c->nprep++;
    return node;
}

/* R → R с применением правила к детям */
static RSpline *rw_spline(NComp *c, RSpline *s, int parent) {
    if (s->src->is_const) return s;
    RSpline *n = cmem(c, sizeof(RSpline)); *n = *s;
    n->coord = slice_rule(c, s->coord, parent);
    n->val = cmem(c, sizeof(RSpline *) * (size_t)s->src->n);
    for (int i = 0; i < s->src->n; i++) n->val[i] = rw_spline(c, s->val[i], parent);
    return n;
}
static R *rw_children(NComp *c, R *r, int parent) {
    if (r->t == R_PREPARED) return r;
    R *n = rnew(c, r->t, r->src);
    n->sl_axis = r->sl_axis; n->sl_coord = r->sl_coord;
    if (r->a) n->a = slice_rule(c, r->a, parent);
    if (r->b) n->b = slice_rule(c, r->b, parent);
    if (r->c) n->c = slice_rule(c, r->c, parent);
    if (r->nlist) { n->nlist = r->nlist; n->list = cmem(c, sizeof(R *) * (size_t)r->nlist); for (int i = 0; i < r->nlist; i++) n->list[i] = slice_rule(c, r->list[i], parent); }
    if (r->sp) n->sp = rw_spline(c, r->sp, parent);
    return n;
}
static R *mk_slice(NComp *c, int axis, R *in) { R *s = rnew(c, DF_SLICE, NULL); s->sl_axis = axis; s->sl_coord = 0; s->a = in; return s; }
static R *slice_rule(NComp *c, R *r, int parent) {
    if (!r || c->fail) return r;
    if (r->t == DF_CONST || (r->t == DF_GRADIENT && !c->env.v264)) return r;
    int ax = axes_of(c, r);
    if (parent == ax) return rw_children(c, r, parent);
    R *n = rw_children(c, r, ax);
    int removed = parent & ~ax;
    int existing = 0;
    for (R *q = n; q->t == DF_SLICE; q = q->a) existing |= 1 << q->sl_axis;
    int filt = removed & ~existing;
    if (filt & 1) n = mk_slice(c, 0, n);
    if (filt & 4) n = mk_slice(c, 2, n);
    if (filt & 2) n = mk_slice(c, 1, n);
    return n;
}

/* ---------------- компиляция ---------------- */
static S *snew(NComp *c, int k) { S *s = cmem(c, sizeof(S)); s->k = k; return s; }
static int is_const(const R *r, float *v) { if (r && r->t == DF_CONST) { if (v) *v = r->src->n0.f; return 1; } return 0; }
static int is_zero_const(const R *r) { float v; return is_const(r, &v) && jm_feq(v, 0.0f); }

static int r_params_eq(const R *a, const R *b) {
    if (a->t != b->t) return 0;
    if (a->t == R_PREPARED) return a->cache_id == b->cache_id;
    if (a->t == DF_SLICE) return a->sl_axis == b->sl_axis && a->sl_coord == b->sl_coord;
    const Df *x = a->src, *y = b->src;
    if (x == y) return 1;
    if (!x || !y) return 0;
    #define NE(n) (jm_deq(x->n.d, y->n.d) && jm_feq(x->n.f, y->n.f))
    if (!NE(n0) || !NE(n1) || !NE(n2) || !NE(n3) || !NE(n4)) return 0;
    #undef NE
    if (x->i0 != y->i0 || x->i1 != y->i1 || x->i2 != y->i2 || x->i3 != y->i3 || x->nthr != y->nthr) return 0;
    for (int i = 0; i < x->nthr; i++) if (!jm_feq(x->thr[i].f, y->thr[i].f)) return 0;
    if ((x->name == NULL) != (y->name == NULL) || (x->name && strcmp(x->name, y->name))) return 0;
    if ((x->spline == NULL) != (y->spline == NULL)) return 0;
    return 1;
}
static int r_equal(const R *a, const R *b);
static int rsp_equal(const RSpline *a, const RSpline *b) {
    if (a == b) return 1;
    const DfSpline *x = a->src, *y = b->src;
    if (x->is_const != y->is_const) return 0;
    if (x->is_const) return jm_feq(x->value.f, y->value.f);
    if (x->n != y->n || !r_equal(a->coord, b->coord)) return 0;
    for (int i = 0; i < x->n; i++) if (!jm_feq(x->loc[i].f, y->loc[i].f) || !jm_feq(x->der[i].f, y->der[i].f) || !rsp_equal(a->val[i], b->val[i])) return 0;
    return 1;
}
static int r_equal(const R *a, const R *b) {
    if (a == b) return 1;
    if (!a || !b || !r_params_eq(a, b)) return 0;
    if (!r_equal(a->a, b->a) || !r_equal(a->b, b->b) || !r_equal(a->c, b->c) || a->nlist != b->nlist) return 0;
    for (int i = 0; i < a->nlist; i++) if (!r_equal(a->list[i], b->list[i])) return 0;
    if ((a->sp == NULL) != (b->sp == NULL)) return 0;
    return !a->sp || rsp_equal(a->sp, b->sp);
}

typedef struct { R **fn; S **s; int n, cap; } CoordMap;
static SSp *compile_spline(NComp *c, RSpline *rs, CoordMap *cm) {
    SSp *p = cmem(c, sizeof(SSp));
    const DfSpline *d = rs->src;
    if (d->is_const) { p->is_const = 1; p->value = d->value.f; return p; }
    int ci = -1;
    for (int i = 0; i < cm->n; i++) if (r_equal(cm->fn[i], rs->coord)) { ci = i; break; }
    if (ci < 0) {
        if (cm->n == cm->cap) { cm->cap = cm->cap ? cm->cap * 2 : 8; cm->fn = xrealloc(cm->fn, sizeof(R *) * (size_t)cm->cap); cm->s = xrealloc(cm->s, sizeof(S *) * (size_t)cm->cap); }
        ci = cm->n++;
        cm->fn[ci] = rs->coord; cm->s[ci] = compile(c, rs->coord);
    }
    p->ci = ci; p->n = d->n;
    p->loc = cmem(c, sizeof(float) * (size_t)d->n); p->der = cmem(c, sizeof(float) * (size_t)d->n);
    p->val = cmem(c, sizeof(SSp *) * (size_t)d->n);
    for (int i = 0; i < d->n; i++) { p->loc[i] = d->loc[i].f; p->der[i] = d->der[i].f; }
    for (int i = 0; i < d->n; i++) p->val[i] = compile_spline(c, rs->val[i], cm);
    return p;
}

static S *mk_noise(NComp *c, const char *name, double xz, double y) {
    S *s = snew(c, K_NOISE);
    s->ns = c->env.noise(c->env.ud, name, c->err, c->errlen);
    if (!s->ns) c->fail = 1;
    if (c->env.noise_scale) {   /* тонкие настройки: множитель только если ≠ 1 (ваниль побитово) */
        double mxz, my; c->env.noise_scale(c->env.ud, name, &mxz, &my);
        if (mxz != 1.0) xz *= mxz;
        if (my != 1.0) y *= my;
    }
    s->d0 = xz; s->d1 = y; return s;
}
static S *mk1(NComp *c, int k, S *a) { S *s = snew(c, k); s->a = a; return s; }

static S *compile(NComp *c, R *r) {
    if (c->fail || !r) return NULL;
    const Df *src = r->src;
    S *s;
    switch (r->t) {
    case DF_CONST: s = snew(c, K_CONST); s->f0 = src->n0.f; return s;
    case DF_NOISE: {
        if (is_zero_const(r->a) && is_zero_const(r->b) && is_zero_const(r->c)) return mk_noise(c, src->name, src->n0.d, src->n1.d);
        S *sx = compile(c, r->a), *sz = compile(c, r->c);
        s = mk_noise(c, src->name, src->n0.d, src->n1.d);
        if (is_zero_const(r->b)) { s->k = K_NOISE_XZ; s->a = sx; s->c = sz; }
        else { s->k = K_NOISE_XYZ; s->a = sx; s->b = compile(c, r->b); s->c = sz; }
        return s;
    }
    case DF_SHIFT_A: s = snew(c, K_CMUL); s->a = mk_noise(c, src->name, 0.25, 0.0); s->f0 = 4.0f; return s;
    case DF_SHIFT:   s = snew(c, K_CMUL); s->a = mk_noise(c, src->name, 0.25, 0.25); s->f0 = 4.0f; return s;
    case DF_SHIFT_B: s = mk_noise(c, src->name, 0.25, 0.25); s->k = K_SHIFT_B; return s;
    case DF_END_OUTER_ISLANDS: s = snew(c, K_END); s->gn = c->env.end_islands(c->env.ud); return s;
    case DF_DISTANCE_TO_POINT: s = snew(c, K_DIST); s->i0 = src->i0; s->i1 = src->i1; s->i2 = src->i2; s->i3 = src->i3; return s;
    case DF_GRADIENT: {
        int range = src->i3 - src->i2;
        float factor = (src->n1.f - src->n0.f) / (float)range;
        s = snew(c, src->i1 == 0 ? K_GRAD_CLAMP : src->i1 == 1 ? K_GRAD_REPEAT : K_GRAD_MIRROR);
        s->i0 = src->i0;                     /* ось */
        s->i1 = src->i2;                     /* from */
        s->i2 = src->i2 < src->i3 ? src->i2 : src->i3;   /* min (clamp) */
        s->i3 = src->i2 < src->i3 ? src->i3 : src->i2;   /* max */
        if (src->i1 != 0) s->i2 = range;
        s->f0 = src->n0.f; s->f1 = factor;
        return s;
    }
    case DF_BLEND_ALPHA: s = snew(c, K_CTX_ALPHA); s->f0 = 1.0f; return s;
    case DF_BLEND_OFFSET: s = snew(c, K_CTX_OFFSET); s->f0 = 0.0f; return s;
    case DF_BEARDIFIER: s = snew(c, K_CTX_BEARD); s->f0 = 0.0f; return s;
    case DF_ABS: return mk1(c, K_ABS, compile(c, r->a));
    case DF_SQUARE: return mk1(c, K_SQUARE, compile(c, r->a));
    case DF_CUBE: return mk1(c, K_CUBE, compile(c, r->a));
    case DF_SQRT: return mk1(c, K_SQRT, compile(c, r->a));
    case DF_HALF_NEGATIVE: s = mk1(c, K_LEAKY, compile(c, r->a)); s->f0 = 0.5f; return s;
    case DF_QUARTER_NEGATIVE: s = mk1(c, K_LEAKY, compile(c, r->a)); s->f0 = 0.25f; return s;
    case DF_RECIPROCAL: return mk1(c, K_RECIP, compile(c, r->a));
    case DF_NEGATE: return mk1(c, K_NEG, compile(c, r->a));
    case DF_SQUEEZE: return mk1(c, K_SQUEEZE, compile(c, r->a));
    case DF_LOG: return mk1(c, K_LOG, compile(c, r->a));
    case DF_SIGN: return mk1(c, K_SIGN, compile(c, r->a));
    case DF_FLOOR: case DF_ROUND: case DF_CEIL: case DF_TRUNCATE: {
        S *in = compile(c, r->a);
        float m;
        if (is_const(r->b, &m) && m == 1.0f) { s = mk1(c, K_ROUND_INT, in); s->i0 = r->t; return s; }
        s = mk1(c, K_ROUND, in); s->b = compile(c, r->b); s->i0 = r->t; return s;
    }
    case DF_ADD: case DF_SUB: case DF_MUL: case DF_DIV: case DF_MIN: case DF_MAX: {
        S *L = compile(c, r->a), *Rr = compile(c, r->b);
        float lv, rv; int lc = is_const(r->a, &lv), rc = is_const(r->b, &rv);
        switch (r->t) {
        case DF_ADD:
            if (lc) { s = mk1(c, K_CADD, Rr); s->f0 = lv; return s; }
            if (rc) { s = mk1(c, K_CADD, L); s->f0 = rv; return s; }
            s = mk1(c, K_ADD, L); s->b = Rr; return s;
        case DF_SUB:
            if (lc) { s = mk1(c, K_CSUB, Rr); s->f0 = lv; return s; }
            if (rc) { s = mk1(c, K_CADD, L); s->f0 = -rv; return s; }
            s = mk1(c, K_SUB, L); s->b = Rr; return s;
        case DF_MUL:
            if (lc) { s = mk1(c, K_CMUL, Rr); s->f0 = lv; return s; }
            if (rc) { s = mk1(c, K_CMUL, L); s->f0 = rv; return s; }
            s = mk1(c, K_MUL, L); s->b = Rr; return s;
        case DF_DIV:
            if (lc) { s = mk1(c, K_CDIV, Rr); s->f0 = lv; return s; }
            if (rc) { s = mk1(c, K_CMUL, L); s->f0 = 1.0f / rv; return s; }
            s = mk1(c, K_DIV, L); s->b = Rr; return s;
        case DF_MIN: {
            Ival lr = range_of(c, r->a), rr = range_of(c, r->b);
            if (lr.hi < rr.lo) return L;
            if (rr.hi < lr.lo) return Rr;
            if (lc) { s = mk1(c, K_CMIN, Rr); s->f0 = lv; return s; }
            if (rc) { s = mk1(c, K_CMIN, L); s->f0 = rv; return s; }
            s = mk1(c, K_MIN, L); s->b = Rr; s->f0 = rr.lo; return s;
        }
        default: {
            Ival lr = range_of(c, r->a), rr = range_of(c, r->b);
            if (lr.lo > rr.hi) return L;
            if (rr.lo > lr.hi) return Rr;
            if (lc) { s = mk1(c, K_CMAX, Rr); s->f0 = lv; return s; }
            if (rc) { s = mk1(c, K_CMAX, L); s->f0 = rv; return s; }
            s = mk1(c, K_MAX, L); s->b = Rr; s->f0 = rr.hi; return s;
        }
        }
    }
    case DF_POW: {
        S *B = compile(c, r->a), *E = compile(c, r->b);
        float bv, ev;
        if (is_const(r->a, &bv)) { s = mk1(c, K_POW_CB, E); s->d0 = (double)bv; return s; }
        if (is_const(r->b, &ev)) {
            float ae = fabsf(ev); S *sp;
            if (ae == 0.5f) sp = mk1(c, K_SQRT, B);
            else if (ae == 1.0f) sp = B;
            else if (ae == 2.0f) sp = mk1(c, K_SQUARE, B);
            else if (ae == 3.0f) sp = mk1(c, K_CUBE, B);
            else { s = mk1(c, K_POW_CE, B); s->d0 = (double)ev; return s; }
            return ev >= 0.0f ? sp : mk1(c, K_RECIP, sp);
        }
        s = mk1(c, K_POW, B); s->b = E; return s;
    }
    case DF_SPLINE: {
        CoordMap cm = {0};
        s = snew(c, K_SPLINE);
        s->sp = compile_spline(c, r->sp, &cm);
        s->ncoord = cm.n; s->coords = cmem(c, sizeof(S *) * (size_t)(cm.n ? cm.n : 1));
        for (int i = 0; i < cm.n; i++) s->coords[i] = cm.s[i];
        free(cm.fn); free(cm.s);
        return s;
    }
    case DF_LERP: {
        S *A = compile(c, r->a), *F = compile(c, r->b), *Sc = compile(c, r->c);
        float fv, sv;
        if (is_const(r->b, &fv)) { s = mk1(c, K_LERP_CF, A); s->c = Sc; s->f0 = fv; return s; }
        if (is_const(r->c, &sv)) { s = mk1(c, K_LERP_CS, A); s->b = F; s->f0 = sv; return s; }
        s = mk1(c, K_LERP, A); s->b = F; s->c = Sc; return s;
    }
    case DF_CLAMP: {
        float mn = src->n0.f, mx = src->n1.f;
        if (c->env.v264) {   /* 26.4: clamp по range входа — без операции / ConstMax / ConstMin / Clamp */
            Ival in = range_of(c, r->a);
            S *x = compile(c, r->a);
            if (mn <= in.lo && mx >= in.hi) return x;
            if (mx >= in.hi) { s = mk1(c, K_CMAX, x); s->f0 = mn; return s; }
            if (mn <= in.lo) { s = mk1(c, K_CMIN, x); s->f0 = mx; return s; }
            s = mk1(c, K_CLAMP, x); s->f0 = mn; s->f1 = mx; return s;
        }
        s = mk1(c, K_CLAMP, compile(c, r->a)); s->f0 = mn; s->f1 = mx; return s;
    }
    case DF_RANGE_CHOICE: {
        S *in = compile(c, r->a);
        float iv, ov;
        if (is_const(r->b, &iv) && is_const(r->c, &ov)) { s = mk1(c, K_RANGE_C, in); s->f0 = src->n0.f; s->f1 = src->n1.f; s->f2 = iv; s->f3 = ov; return s; }
        s = mk1(c, K_RANGE, in); s->f0 = src->n0.f; s->f1 = src->n1.f; s->b = compile(c, r->b); s->c = compile(c, r->c); return s;
    }
    case DF_INTERVAL_SELECT: {
        S *in = compile(c, r->a);
        if (src->nthr == 1) {
            s = mk1(c, K_ISEL1, in); s->f0 = src->thr[0].f; s->b = compile(c, r->list[0]); s->c = compile(c, r->list[r->nlist - 1]); return s;
        }
        s = mk1(c, K_ISEL, in);
        s->nthr = src->nthr; s->thr = cmem(c, sizeof(float) * (size_t)src->nthr);
        for (int i = 0; i < src->nthr; i++) s->thr[i] = src->thr[i].f;
        s->narr = r->nlist; s->arr = cmem(c, sizeof(S *) * (size_t)r->nlist);
        for (int i = 0; i < r->nlist; i++) s->arr[i] = compile(c, r->list[i]);
        return s;
    }
    case R_PREPARED: return r->cs;
    case DF_BLEND_DENSITY: return mk1(c, K_BLEND_DENSITY, compile(c, r->a));
    case DF_INTERPOLATED:
        s = mk1(c, K_INTERP, compile(c, r->a));
        s->i0 = src->i0; s->i1 = src->i1; s->f0 = 1.0f / (float)src->i0; s->f1 = 1.0f / (float)src->i1;
        return s;
    case DF_SLICE: {
        R *in = r->a;
        if (in && in->t == DF_SLICE && ((r->sl_axis == 0 && in->sl_axis == 2) || (r->sl_axis == 2 && in->sl_axis == 0))) {
            s = mk1(c, K_SLICE_XZ, compile(c, in->a));
            if (r->sl_axis == 0) { s->i0 = r->sl_coord; s->i1 = in->sl_coord; } else { s->i0 = in->sl_coord; s->i1 = r->sl_coord; }
            return s;
        }
        s = mk1(c, r->sl_axis == 0 ? K_SLICE_X : r->sl_axis == 1 ? K_SLICE_Y : K_SLICE_Z, compile(c, in));
        s->i0 = r->sl_coord;
        return s;
    }
    case DF_FIND_TOP_SURFACE: {
        S *f = snew(c, K_FTS); f->a = compile(c, r->a); f->b = compile(c, r->b); f->i0 = src->i0; f->i1 = src->i1;
        s = mk1(c, K_SLICE_Y, f); s->i0 = 0; return s;
    }
    case DF_OLD_BLENDED_NOISE: {
        const BlendFbm *fb = c->env.blended(c->env.ud, src);
        double xzm = 684.412 * src->n0.d, ym = 684.412 * src->n1.d;
        S *mn = snew(c, K_NOISE); mn->ns = &fb->min_lim; mn->d0 = xzm; mn->d1 = ym;
        S *mx = snew(c, K_NOISE); mx->ns = &fb->max_lim; mx->d0 = xzm; mx->d1 = ym;
        S *mainn = snew(c, K_NOISE); mainn->ns = &fb->main; mainn->d0 = xzm / src->n2.d; mainn->d1 = ym / src->n3.d;
        S *add = mk1(c, K_CADD, mainn); add->f0 = 0.5f;
        S *cl = mk1(c, K_CLAMP, add); cl->f0 = 0.0f; cl->f1 = 1.0f;
        s = mk1(c, K_LERP, cl); s->b = mn; s->c = mx;
        return s;
    }
    default:
        cfail(c, "компиляция: тип %s не поддержан в 26.3+", df_type_name((DfType)r->t));
        return NULL;
    }
}

const S *nc_get(NComp *c, const Df *f, char *err, size_t errlen) {
    for (int i = 0; i < c->ngot; i++) if (c->got_key[i] == f) return c->got_val[i];
    c->err = err; c->errlen = errlen; c->fail = 0;
    R *r = full_opt(c, f);
    S *s = c->fail ? NULL : compile(c, r);
    if (c->fail) return NULL;
    if (c->ngot == c->capgot) { c->capgot = c->capgot ? c->capgot * 2 : 16; c->got_key = xrealloc(c->got_key, sizeof(Df *) * (size_t)c->capgot); c->got_val = xrealloc(c->got_val, sizeof(S *) * (size_t)c->capgot); }
    c->got_key[c->ngot] = f; c->got_val[c->ngot] = s; c->ngot++;
    return s;
}
Ival nc_range_of(NComp *c, const Df *f) { R *r = full_opt(c, f); return c->fail ? iv_nai() : range_of(c, r); }

/* ============================================================== контекст */
typedef struct { Vol vol; float *buf; int cap; int has_buf; i64 vkey; float val; } Cell;
typedef struct { float *p; int cap; int used; } PoolEnt;
struct SCtx {
    int caches;
    Cell *cells; int ncells;
    PoolEnt *pool; int npool, cappool;
    SBeard beard; int has_beard;
};
SCtx *sctx_new(const NComp *c, int caches) {
    SCtx *x = xcalloc(1, sizeof *x);
    x->caches = caches;
    x->ncells = c ? c->next_cache_id : 0;
    x->cells = xcalloc((size_t)(x->ncells ? x->ncells : 1), sizeof(Cell));
    for (int i = 0; i < x->ncells; i++) x->cells[i].val = NAN;
    return x;
}
void sctx_free(SCtx *x) {
    if (!x) return;
    for (int i = 0; i < x->ncells; i++) free(x->cells[i].buf);
    for (int i = 0; i < x->npool; i++) free(x->pool[i].p);
    free(x->cells); free(x->pool); free(x);
}
void sctx_reset_caches(SCtx *x) {
    for (int i = 0; i < x->ncells; i++) { x->cells[i].has_buf = 0; x->cells[i].vkey = 0; x->cells[i].val = NAN; }
}
void sctx_set_beardifier(SCtx *x, const SBeard *b) { if (b) { x->beard = *b; x->has_beard = 1; } else x->has_beard = 0; }
float *sctx_acquire(SCtx *x, int n) {
    int best = -1;
    for (int i = 0; i < x->npool; i++) if (!x->pool[i].used && x->pool[i].cap >= n && (best < 0 || x->pool[i].cap < x->pool[best].cap)) best = i;
    if (best < 0) {
        for (int i = 0; i < x->npool; i++) if (!x->pool[i].used) { best = i; break; }
        if (best < 0) {
            if (x->npool == x->cappool) { x->cappool = x->cappool ? x->cappool * 2 : 16; x->pool = xrealloc(x->pool, sizeof(PoolEnt) * (size_t)x->cappool); }
            best = x->npool++; x->pool[best].p = NULL; x->pool[best].cap = 0;
        }
        free(x->pool[best].p); x->pool[best].p = xmalloc(sizeof(float) * (size_t)(n ? n : 1)); x->pool[best].cap = n;
    }
    x->pool[best].used = 1;
    return x->pool[best].p;
}
void sctx_release(SCtx *x, float *p) {
    for (int i = 0; i < x->npool; i++) if (x->pool[i].p == p) { x->pool[i].used = 0; return; }
}

/* ============================================================== вычисление: точка */
static inline i64 block_pos_long(int x, int y, int z) {
    return (i64)((((u64)(u32)x & 0x3FFFFFFULL) << 38) | (((u64)(u32)y & 0xFFFULL)) | (((u64)(u32)z & 0x3FFFFFFULL) << 12));
}
static float end_height(const GNoise *n, int sx, int sz) {
    int cx = sx / 2, cz = sz / 2, subx = sx % 2, subz = sz % 2;
    float doffs = -100.0f;
    for (int xo = -12; xo <= 12; xo++) for (int zo = -12; zo <= 12; zo++) {
        i64 tx = (i64)cx + xo, tz = (i64)cz + zo;
        if (tx * tx + tz * tz > 4096LL && (float)simplex2_d(n, (double)tx, (double)tz) < -0.9f) {
            float size = fmodf(fabsf((float)tx) * 3439.0f + fabsf((float)tz) * 147.0f, 13.0f) + 9.0f;
            float xd = (float)(subx - xo * 2), zd = (float)(subz - zo * 2);
            float nd = 100.0f - (float)sqrt((double)(xd * xd + zd * zd)) * size;
            nd = jm_clampf(nd, -100.0f, 80.0f);
            doffs = jm_maxf(doffs, nd);
        }
    }
    return doffs;
}
static float grad_compute(const S *s, int coord) {
    switch (s->k) {
    case K_GRAD_CLAMP: {
        int cc = coord < s->i2 ? s->i2 : (coord > s->i3 ? s->i3 : coord);
        return s->f0 + (float)(cc - s->i1) * s->f1;
    }
    case K_GRAD_REPEAT: {
        int rel = coord - s->i1;
        return s->f0 + (float)jm_floormod(rel, s->i2) * s->f1;
    }
    default: {
        int rel = coord - s->i1;
        int tile = jm_floordiv(rel, s->i2);
        int local = rel - tile * s->i2;
        return (tile & 1) == 0 ? s->f0 + (float)local * s->f1 : s->f0 + (float)(s->i2 - local) * s->f1;
    }
    }
}
static float dist_metric(int m, float dx, float dy, float dz) {
    switch (m) {
    case 0: return (float)sqrt((double)(dx * dx + dy * dy + dz * dz));
    case 1: return dx * dx + dy * dy + dz * dz;
    case 2: return fabsf(dx) + fabsf(dy) + fabsf(dz);
    default: return jm_maxf(jm_maxf(fabsf(dx), fabsf(dy)), fabsf(dz));
    }
}
static float leaky(float f, float v) { return v > 0.0f ? v : v * f; }
static float squeeze(float v) { float x = jm_clampf(v, -1.0f, 1.0f); return x / 2.0f - (x * x * x) / 24.0f; }

/* сплайн: вход — точечный (кэш координат) или буферный */
typedef struct { SCtx *x; const S *s; int bx, by, bz; float *cv; float **bufs; const Vol *vol; int idx; } SplIn;
static float spl_coord(SplIn *in, int ci) {
    if (!in->bufs) {
        if (in->cv[ci] != in->cv[ci]) in->cv[ci] = s_value(in->x, in->s->coords[ci], in->bx, in->by, in->bz);
        return in->cv[ci];
    }
    if (!in->bufs[ci]) { in->bufs[ci] = sctx_acquire(in->x, vol_size(in->vol)); s_volume(in->x, in->s->coords[ci], in->bufs[ci], in->vol); }
    return in->bufs[ci][in->idx];
}
static float spl_eval(const SSp *p, SplIn *in) {
    if (p->is_const) return p->value;
    float input = spl_coord(in, p->ci);
    /* Mth.binarySearch(0, n, i -> input < loc[i]) - 1 */
    int from = 0, len = p->n;
    while (len > 0) { int half = len / 2, mid = from + half; if (input < p->loc[mid]) len = half; else { from = mid + 1; len -= half + 1; } }
    int start = from - 1, last = p->n - 1;
    if (start < 0) { float v = spl_eval(p->val[0], in); float d = p->der[0]; return d == 0.0f ? v : v + d * (input - p->loc[0]); }
    if (start == last) { float v = spl_eval(p->val[last], in); float d = p->der[last]; return d == 0.0f ? v : v + d * (input - p->loc[last]); }
    float x1 = p->loc[start], x2 = p->loc[start + 1];
    float t = (input - x1) / (x2 - x1);
    float d1 = p->der[start], d2 = p->der[start + 1];
    float y1 = spl_eval(p->val[start], in), y2 = spl_eval(p->val[start + 1], in);
    float a = d1 * (x2 - x1) - (y2 - y1);
    float b = -d2 * (x2 - x1) + (y2 - y1);
    return jm_lerpf(t, y1, y2) + t * (1.0f - t) * jm_lerpf(t, a, b);
}

static int find_top(SCtx *x, const S *s, int bx, int bz, float upper) {
    int cell = s->i1, lower = s->i0;
    int top = jm_floor_f(upper / (float)cell) * cell;
    if (top <= lower) return lower;
    for (int py = top; py >= lower; py -= cell)
        if (s_value(x, s->a, bx, py, bz) > 0.0f) return py;
    return lower;
}

float s_value(SCtx *x, const S *s, int bx, int by, int bz) {
    switch (s->k) {
    case K_CONST: return s->f0;
    case K_NOISE: return ns_get(s->ns, bx * s->d0, by * s->d1, bz * s->d0);
    case K_NOISE_XZ: {
        double nx = bx * s->d0 + (double)s_value(x, s->a, bx, by, bz);
        double ny = by * s->d1;
        double nz = bz * s->d0 + (double)s_value(x, s->c, bx, by, bz);
        return ns_get(s->ns, nx, ny, nz);
    }
    case K_NOISE_XYZ: {
        double nx = bx * s->d0 + (double)s_value(x, s->a, bx, by, bz);
        double ny = by * s->d1 + (double)s_value(x, s->b, bx, by, bz);
        double nz = bz * s->d0 + (double)s_value(x, s->c, bx, by, bz);
        return ns_get(s->ns, nx, ny, nz);
    }
    case K_SHIFT_B: return ns_get(s->ns, bz * s->d0, bx * s->d0, 0.0) * 4.0f;
    case K_END: return (end_height(s->gn, bx / 8, bz / 8) - 8.0f) / 128.0f;
    case K_DIST: return dist_metric(s->i3, (float)(s->i0 - bx), (float)(s->i1 - by), (float)(s->i2 - bz));
    case K_GRAD_CLAMP: case K_GRAD_REPEAT: case K_GRAD_MIRROR: return grad_compute(s, s->i0 == 0 ? bx : s->i0 == 1 ? by : bz);
    case K_CTX_ALPHA: case K_CTX_OFFSET: return s->f0;
    case K_CTX_BEARD: return x->has_beard ? x->beard.value(x->beard.ud, bx, by, bz) : s->f0;
    case K_ABS: return fabsf(s_value(x, s->a, bx, by, bz));
    case K_SQUARE: { float v = s_value(x, s->a, bx, by, bz); return v * v; }
    case K_CUBE: { float v = s_value(x, s->a, bx, by, bz); return v * v * v; }
    case K_SQRT: return (float)sqrt((double)s_value(x, s->a, bx, by, bz));
    case K_LEAKY: return leaky(s->f0, s_value(x, s->a, bx, by, bz));
    case K_RECIP: return 1.0f / s_value(x, s->a, bx, by, bz);
    case K_NEG: return -s_value(x, s->a, bx, by, bz);
    case K_SQUEEZE: return squeeze(s_value(x, s->a, bx, by, bz));
    case K_LOG: return (float)log((double)s_value(x, s->a, bx, by, bz));
    case K_SIGN: return jm_signumf(s_value(x, s->a, bx, by, bz));
    case K_ROUND_INT: return round_to_int(s_value(x, s->a, bx, by, bz), s->i0);
    case K_ROUND: {
        float in = s_value(x, s->a, bx, by, bz), m = s_value(x, s->b, bx, by, bz);
        return m == 0.0f ? in : round_to_int(in / m, s->i0) * m;
    }
    case K_ADD: { float l = s_value(x, s->a, bx, by, bz); return l + s_value(x, s->b, bx, by, bz); }
    case K_CADD: return s_value(x, s->a, bx, by, bz) + s->f0;
    case K_CSUB: return s->f0 - s_value(x, s->a, bx, by, bz);
    case K_SUB: { float l = s_value(x, s->a, bx, by, bz); return l - s_value(x, s->b, bx, by, bz); }
    case K_MUL: { float l = s_value(x, s->a, bx, by, bz); return l == 0.0f ? 0.0f : l * s_value(x, s->b, bx, by, bz); }
    case K_CMUL: return s_value(x, s->a, bx, by, bz) * s->f0;
    case K_DIV: { float l = s_value(x, s->a, bx, by, bz); return l == 0.0f ? 0.0f : l / s_value(x, s->b, bx, by, bz); }
    case K_CDIV: return s->f0 / s_value(x, s->a, bx, by, bz);
    case K_MIN: { float l = s_value(x, s->a, bx, by, bz); return l <= s->f0 ? l : jm_minf(l, s_value(x, s->b, bx, by, bz)); }
    case K_CMIN: return jm_minf(s_value(x, s->a, bx, by, bz), s->f0);
    case K_MAX: { float l = s_value(x, s->a, bx, by, bz); return l >= s->f0 ? l : jm_maxf(l, s_value(x, s->b, bx, by, bz)); }
    case K_CMAX: return jm_maxf(s_value(x, s->a, bx, by, bz), s->f0);
    case K_POW_CB: return (float)pow(s->d0, (double)s_value(x, s->a, bx, by, bz));
    case K_POW_CE: return (float)pow((double)s_value(x, s->a, bx, by, bz), s->d0);
    case K_POW: { float b = s_value(x, s->a, bx, by, bz); float e = s_value(x, s->b, bx, by, bz); return (float)pow((double)b, (double)e); }
    case K_SPLINE: {
        float cv[64]; float *cvp = s->ncoord <= 64 ? cv : xmalloc(sizeof(float) * (size_t)s->ncoord);
        for (int i = 0; i < s->ncoord; i++) cvp[i] = NAN;
        SplIn in = { x, s, bx, by, bz, cvp, NULL, NULL, 0 };
        float v = spl_eval(s->sp, &in);
        if (cvp != cv) free(cvp);
        return v;
    }
    case K_LERP: {
        float a = s_value(x, s->a, bx, by, bz);
        if (a == 0.0f) return s_value(x, s->b, bx, by, bz);
        if (a == 1.0f) return s_value(x, s->c, bx, by, bz);
        float f = s_value(x, s->b, bx, by, bz);
        return jm_lerpf(a, f, s_value(x, s->c, bx, by, bz));
    }
    case K_LERP_CF: {
        float a = s_value(x, s->a, bx, by, bz);
        if (a == 0.0f) return s->f0;
        if (a == 1.0f) return s_value(x, s->c, bx, by, bz);
        return jm_lerpf(a, s->f0, s_value(x, s->c, bx, by, bz));
    }
    case K_LERP_CS: {
        float a = s_value(x, s->a, bx, by, bz);
        if (a == 0.0f) return s_value(x, s->b, bx, by, bz);
        if (a == 1.0f) return s->f0;
        return jm_lerpf(a, s_value(x, s->b, bx, by, bz), s->f0);
    }
    case K_CLAMP: return jm_clampf(s_value(x, s->a, bx, by, bz), s->f0, s->f1);
    case K_RANGE_C: { float v = s_value(x, s->a, bx, by, bz); return (v >= s->f0 && v < s->f1) ? s->f2 : s->f3; }
    case K_RANGE: { float v = s_value(x, s->a, bx, by, bz); return (v >= s->f0 && v < s->f1) ? s_value(x, s->b, bx, by, bz) : s_value(x, s->c, bx, by, bz); }
    case K_ISEL1: { float v = s_value(x, s->a, bx, by, bz); return v < s->f0 ? s_value(x, s->b, bx, by, bz) : s_value(x, s->c, bx, by, bz); }
    case K_ISEL: {
        float v = s_value(x, s->a, bx, by, bz);
        int i = 0; for (; i < s->nthr; i++) if (v < s->thr[i]) break;
        if (i == s->nthr) i = s->narr - 1;
        return s_value(x, s->arr[i], bx, by, bz);
    }
    case K_CACHE: {
        if (!x->caches) return s_value(x, s->a, bx, by, bz);
        Cell *cl = &x->cells[s->cid];
        i64 key = block_pos_long(bx, by, bz);
        if (cl->vkey == key && cl->val == cl->val) return cl->val;
        if (cl->has_buf) { int idx = vol_index_of_block(&cl->vol, bx, by, bz); if (idx != -1) return cl->buf[idx]; }
        float v = s_value(x, s->a, bx, by, bz);
        cl->vkey = key; cl->val = v;
        return v;
    }
    case K_BLEND_DENSITY: return s_value(x, s->a, bx, by, bz);   /* Blender пуст (новые миры) */
    case K_INTERP: {
        int cxz = s->i0, cy = s->i1;
        int xi = jm_floormod(bx, cxz), yi = jm_floormod(by, cy), zi = jm_floormod(bz, cxz);
        if (xi == 0 && yi == 0 && zi == 0) return s_value(x, s->a, bx, by, bz);
        Vol v = { 2, 2, 2, bx - xi, by - yi, bz - zi, cxz, cy, cxz };
        float *b = sctx_acquire(x, 8);
        s_volume(x, s->a, b, &v);
        float r = jm_lerpf((float)zi / (float)cxz,
            jm_lerpf((float)yi / (float)cy, jm_lerpf((float)xi / (float)cxz, b[vol_idx(&v,0,0,0)], b[vol_idx(&v,1,0,0)]),
                                            jm_lerpf((float)xi / (float)cxz, b[vol_idx(&v,0,1,0)], b[vol_idx(&v,1,1,0)])),
            jm_lerpf((float)yi / (float)cy, jm_lerpf((float)xi / (float)cxz, b[vol_idx(&v,0,0,1)], b[vol_idx(&v,1,0,1)]),
                                            jm_lerpf((float)xi / (float)cxz, b[vol_idx(&v,0,1,1)], b[vol_idx(&v,1,1,1)])));
        sctx_release(x, b);
        return r;
    }
    case K_SLICE_X: return s_value(x, s->a, s->i0, by, bz);
    case K_SLICE_Y: return s_value(x, s->a, bx, s->i0, bz);
    case K_SLICE_Z: return s_value(x, s->a, bx, by, s->i0);
    case K_SLICE_XZ: return s_value(x, s->a, s->i0, by, s->i1);
    case K_FTS: { float up = s_value(x, s->b, bx, by, bz); return (float)find_top(x, s, bx, bz, up); }
    }
    return 0.0f;
}

/* ============================================================== вычисление: объём */
static void fill(float *o, int n, float v) { for (int i = 0; i < n; i++) o[i] = v; }

static void interp_fill_cell(const S *s, float *out, const Vol *ov, const Vol *cv, int cx, int cy, int cz,
                             float v000, float v100, float v010, float v110, float v001, float v101, float v011, float v111) {
    int cox = vol_bx(cv, cx) - ov->x0, coy = vol_by(cv, cy) - ov->y0, coz = vol_bz(cv, cz) - ov->z0;
    int x0 = 0 > -cox ? 0 : -cox, y0 = 0 > -coy ? 0 : -coy, z0 = 0 > -coz ? 0 : -coz;
    int x1 = (s->i0 < ov->sx - cox ? s->i0 : ov->sx - cox) - 1;
    int y1 = (s->i1 < ov->sy - coy ? s->i1 : ov->sy - coy) - 1;
    int z1 = (s->i0 < ov->sz - coz ? s->i0 : ov->sz - coz) - 1;
    for (int z = z0; z <= z1; z++) {
        int oz = coz + z;
        float az = (float)z * s->f0;
        float v00 = jm_lerpf(az, v000, v001), v01 = jm_lerpf(az, v010, v011), v10 = jm_lerpf(az, v100, v101), v11 = jm_lerpf(az, v110, v111);
        for (int xx = x0; xx <= x1; xx++) {
            int ox = cox + xx;
            float ax = (float)xx * s->f0;
            float a0 = jm_lerpf(ax, v00, v10), a1 = jm_lerpf(ax, v01, v11);
            float step = (a1 - a0) * s->f1;
            float value = a0 + step * (float)y0;
            int oi = vol_idx(ov, ox, coy + y0, oz);
            for (int y = y0; y <= y1; y++) { out[oi++] = value; value += step; }
        }
    }
}
static void interp_block_step(SCtx *x, const S *s, float *out, const Vol *v) {
    int cxz = s->i0, cy = s->i1;
    int mnx = jm_floordiv(v->x0, cxz), mny = jm_floordiv(v->y0, cy), mnz = jm_floordiv(v->z0, cxz);
    int mxx = jm_floordiv(vol_max_x(v), cxz), mxy = jm_floordiv(vol_max_y(v), cy), mxz = jm_floordiv(vol_max_z(v), cxz);
    int ncx = mxx - mnx + 1, ncy = mxy - mny + 1, ncz = mxz - mnz + 1;
    Vol cv = { jm_floormod(vol_max_x(v), cxz) == 0 ? ncx : ncx + 1, jm_floormod(vol_max_y(v), cy) == 0 ? ncy : ncy + 1,
               jm_floormod(vol_max_z(v), cxz) == 0 ? ncz : ncz + 1, mnx * cxz, mny * cy, mnz * cxz, cxz, cy, cxz };
    float *cb = sctx_acquire(x, vol_size(&cv));
    s_volume(x, s->a, cb, &cv);
    for (int cz = 0; cz < ncz; cz++) {
        int ncz2 = cz + 1 < cv.sz - 1 ? cz + 1 : cv.sz - 1;
        for (int cx = 0; cx < ncx; cx++) {
            int ncx2 = cx + 1 < cv.sx - 1 ? cx + 1 : cv.sx - 1;
            float v000 = cb[vol_idx(&cv, cx, 0, cz)], v100 = cb[vol_idx(&cv, ncx2, 0, cz)];
            float v001 = cb[vol_idx(&cv, cx, 0, ncz2)], v101 = cb[vol_idx(&cv, ncx2, 0, ncz2)];
            for (int cyy = 0; cyy < ncy; cyy++) {
                int ncy2 = cyy + 1 < cv.sy - 1 ? cyy + 1 : cv.sy - 1;
                float v010 = cb[vol_idx(&cv, cx, ncy2, cz)], v110 = cb[vol_idx(&cv, ncx2, ncy2, cz)];
                float v011 = cb[vol_idx(&cv, cx, ncy2, ncz2)], v111 = cb[vol_idx(&cv, ncx2, ncy2, ncz2)];
                interp_fill_cell(s, out, v, &cv, cx, cyy, cz, v000, v100, v010, v110, v001, v101, v011, v111);
                v000 = v010; v100 = v110; v001 = v011; v101 = v111;
            }
        }
    }
    sctx_release(x, cb);
}

/* общий пробег по объёму с точечной функцией (DistanceToPoint, FindTopSurface и т. п.) */
void s_volume(SCtx *x, const S *s, float *o, const Vol *v) {
    int n = vol_size(v);
    switch (s->k) {
    case K_CONST: case K_CTX_ALPHA: case K_CTX_OFFSET: fill(o, n, s->f0); return;
    case K_CTX_BEARD: if (x->has_beard) x->beard.volume(x->beard.ud, o, v); else fill(o, n, s->f0); return;
    case K_NOISE: fill(o, n, 0.0f); ns_add_volume(s->ns, o, v, s->d0, s->d1, 1.0f); return;
    case K_NOISE_XZ: case K_NOISE_XYZ: {
        s_volume(x, s->a, o, v);
        float *by_ = NULL;
        if (s->k == K_NOISE_XYZ) { by_ = sctx_acquire(x, n); s_volume(x, s->b, by_, v); }
        float *bz = sctx_acquire(x, n); s_volume(x, s->c, bz, v);
        int idx = 0;
        for (int iz = 0; iz < v->sz; iz++) {
            double baseZ = vol_bz(v, iz) * s->d0;
            for (int ix = 0; ix < v->sx; ix++) {
                double baseX = vol_bx(v, ix) * s->d0;
                for (int iy = 0; iy < v->sy; iy++) {
                    double nx = baseX + (double)o[idx];
                    double ny = vol_by(v, iy) * s->d1;
                    if (by_) ny += (double)by_[idx];
                    double nz = baseZ + (double)bz[idx];
                    o[idx] = ns_get(s->ns, nx, ny, nz);
                    idx++;
                }
            }
        }
        sctx_release(x, bz); if (by_) sctx_release(x, by_);
        return;
    }
    case K_SHIFT_B: {
        Vol tv = { v->sz, v->sx, 1, v->z0, v->x0, 0, v->dz, v->dx, 1 };
        float *tb = sctx_acquire(x, vol_size(&tv));
        fill(tb, vol_size(&tv), 0.0f);
        ns_add_volume(s->ns, tb, &tv, s->d0, s->d0, 4.0f);
        for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) {
            float val = tb[vol_idx(&tv, iz, ix, 0)];
            fill(o + vol_idx(v, ix, 0, iz), v->sy, val);
        }
        sctx_release(x, tb);
        return;
    }
    case K_END:
        for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) {
            float val = s_value(x, s, vol_bx(v, ix), 0, vol_bz(v, iz));
            fill(o + vol_idx(v, ix, 0, iz), v->sy, val);
        }
        return;
    case K_DIST: {
        int idx = 0;
        for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) for (int iy = 0; iy < v->sy; iy++)
            o[idx++] = s_value(x, s, vol_bx(v, ix), vol_by(v, iy), vol_bz(v, iz));
        return;
    }
    case K_GRAD_CLAMP: case K_GRAD_REPEAT: case K_GRAD_MIRROR:
        if (s->i0 == 0) {
            for (int ix = 0; ix < v->sx; ix++) { float val = grad_compute(s, vol_bx(v, ix)); for (int iz = 0; iz < v->sz; iz++) fill(o + vol_idx(v, ix, 0, iz), v->sy, val); }
        } else if (s->i0 == 1) {
            for (int iy = 0; iy < v->sy; iy++) { float val = grad_compute(s, vol_by(v, iy)); for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) o[vol_idx(v, ix, iy, iz)] = val; }
        } else {
            for (int iz = 0; iz < v->sz; iz++) { float val = grad_compute(s, vol_bz(v, iz)); fill(o + vol_idx(v, 0, 0, iz), v->sx * v->sy, val); }
        }
        return;
    case K_ABS: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = fabsf(o[i]); return;
    case K_SQUARE: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = o[i] * o[i]; return;
    case K_CUBE: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = o[i] * o[i] * o[i]; return;
    case K_SQRT: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = (float)sqrt((double)o[i]); return;
    case K_LEAKY: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = leaky(s->f0, o[i]); return;
    case K_RECIP: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = 1.0f / o[i]; return;
    case K_NEG: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = -o[i]; return;
    case K_SQUEEZE: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = squeeze(o[i]); return;
    case K_LOG: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = (float)log((double)o[i]); return;
    case K_SIGN: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = jm_signumf(o[i]); return;
    case K_ROUND_INT: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = round_to_int(o[i], s->i0); return;
    case K_ROUND: {
        s_volume(x, s->a, o, v);
        float *m = sctx_acquire(x, n); s_volume(x, s->b, m, v);
        for (int i = 0; i < n; i++) o[i] = m[i] == 0.0f ? o[i] : round_to_int(o[i] / m[i], s->i0) * m[i];
        sctx_release(x, m); return;
    }
    case K_ADD: case K_SUB: case K_MUL: case K_DIV: case K_MIN: case K_MAX: {
        s_volume(x, s->a, o, v);
        float *r = sctx_acquire(x, n); s_volume(x, s->b, r, v);
        switch (s->k) {
        case K_ADD: for (int i = 0; i < n; i++) o[i] = o[i] + r[i]; break;
        case K_SUB: for (int i = 0; i < n; i++) o[i] = o[i] + -r[i]; break;
        case K_MUL: for (int i = 0; i < n; i++) o[i] = o[i] * r[i]; break;
        case K_DIV: for (int i = 0; i < n; i++) o[i] = o[i] / r[i]; break;
        case K_MIN: for (int i = 0; i < n; i++) if (r[i] < o[i]) o[i] = r[i]; break;
        default:    for (int i = 0; i < n; i++) if (r[i] > o[i]) o[i] = r[i]; break;
        }
        sctx_release(x, r); return;
    }
    case K_CADD: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = o[i] + s->f0; return;
    case K_CSUB: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = s->f0 - o[i]; return;
    case K_CMUL: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = o[i] * s->f0; return;
    case K_CDIV: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = s->f0 / o[i]; return;
    case K_CMIN: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) if (s->f0 < o[i]) o[i] = s->f0; return;
    case K_CMAX: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) if (s->f0 > o[i]) o[i] = s->f0; return;
    case K_POW_CB: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = (float)pow(s->d0, (double)o[i]); return;
    case K_POW_CE: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = (float)pow((double)o[i], s->d0); return;
    case K_POW: {
        s_volume(x, s->a, o, v);
        float *e = sctx_acquire(x, n); s_volume(x, s->b, e, v);
        for (int i = 0; i < n; i++) o[i] = (float)pow((double)o[i], (double)e[i]);
        sctx_release(x, e); return;
    }
    case K_SPLINE: {
        float **bufs = xcalloc((size_t)(s->ncoord ? s->ncoord : 1), sizeof(float *));
        SplIn in = { x, s, 0, 0, 0, NULL, bufs, v, 0 };
        for (int i = 0; i < n; i++) { in.idx = i; o[i] = spl_eval(s->sp, &in); }
        for (int i = 0; i < s->ncoord; i++) if (bufs[i]) sctx_release(x, bufs[i]);
        free(bufs);
        return;
    }
    case K_LERP: case K_LERP_CF: case K_LERP_CS: {
        s_volume(x, s->a, o, v);
        float *f = NULL, *sc = NULL;
        if (s->k != K_LERP_CF) { f = sctx_acquire(x, n); s_volume(x, s->b, f, v); }
        if (s->k != K_LERP_CS) { sc = sctx_acquire(x, n); s_volume(x, s->c, sc, v); }
        for (int i = 0; i < n; i++) {
            float a = o[i];
            float fv = f ? f[i] : s->f0, sv = sc ? sc[i] : s->f0;
            o[i] = a == 0.0f ? fv : (a == 1.0f ? sv : jm_lerpf(a, fv, sv));
        }
        if (sc) sctx_release(x, sc);
        if (f) sctx_release(x, f);
        return;
    }
    case K_CLAMP: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = jm_clampf(o[i], s->f0, s->f1); return;
    case K_RANGE_C: s_volume(x, s->a, o, v); for (int i = 0; i < n; i++) o[i] = (o[i] >= s->f0 && o[i] < s->f1) ? s->f2 : s->f3; return;
    case K_RANGE: {
        s_volume(x, s->b, o, v);
        float *in = sctx_acquire(x, n); s_volume(x, s->a, in, v);
        float *out = sctx_acquire(x, n); s_volume(x, s->c, out, v);
        for (int i = 0; i < n; i++) if (!(in[i] >= s->f0) || !(in[i] < s->f1)) o[i] = out[i];
        sctx_release(x, out); sctx_release(x, in); return;
    }
    case K_ISEL1: {
        s_volume(x, s->a, o, v);
        float *bl = sctx_acquire(x, n); s_volume(x, s->b, bl, v);
        float *ab = sctx_acquire(x, n); s_volume(x, s->c, ab, v);
        for (int i = 0; i < n; i++) o[i] = o[i] < s->f0 ? bl[i] : ab[i];
        sctx_release(x, ab); sctx_release(x, bl); return;
    }
    case K_ISEL: {
        s_volume(x, s->a, o, v);
        float **bs = xcalloc((size_t)s->narr, sizeof(float *));
        for (int k = 0; k < s->narr; k++) { bs[k] = sctx_acquire(x, n); s_volume(x, s->arr[k], bs[k], v); }
        for (int i = 0; i < n; i++) {
            int k = 0; for (; k < s->nthr; k++) if (o[i] < s->thr[k]) break;
            if (k == s->nthr) k = s->narr - 1;
            o[i] = bs[k][i];
        }
        for (int k = s->narr - 1; k >= 0; k--) sctx_release(x, bs[k]);
        free(bs); return;
    }
    case K_CACHE: {
        if (!x->caches) { s_volume(x, s->a, o, v); return; }
        Cell *cl = &x->cells[s->cid];
        if (!cl->has_buf || !vol_eq(v, &cl->vol)) {
            if (cl->cap < n) { free(cl->buf); cl->buf = xmalloc(sizeof(float) * (size_t)n); cl->cap = n; }
            cl->vol = *v; cl->has_buf = 0;
            s_volume(x, s->a, cl->buf, v);
            cl->has_buf = 1;
        }
        memcpy(o, cl->buf, sizeof(float) * (size_t)n);
        return;
    }
    case K_BLEND_DENSITY: s_volume(x, s->a, o, v); return;
    case K_INTERP: {
        int cxz = s->i0, cy = s->i1;
        if ((v->dx == cxz || v->sx == 1) && (v->dy == cy || v->sy == 1) && (v->dz == cxz || v->sz == 1) &&
            jm_floormod(v->x0, cxz) == 0 && jm_floormod(v->y0, cy) == 0 && jm_floormod(v->z0, cxz) == 0) {
            s_volume(x, s->a, o, v); return;
        }
        if (v->dx == 1 && v->dy == 1 && v->dz == 1) { interp_block_step(x, s, o, v); return; }
        Vol bv = { v->sx * v->dx, v->sy * v->dy, v->sz * v->dz, v->x0, v->y0, v->z0, 1, 1, 1 };
        float *bb = sctx_acquire(x, vol_size(&bv));
        interp_block_step(x, s, bb, &bv);
        for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) for (int iy = 0; iy < v->sy; iy++)
            o[vol_idx(v, ix, iy, iz)] = bb[vol_idx(&bv, ix * v->dx, iy * v->dy, iz * v->dz)];
        sctx_release(x, bb);
        return;
    }
    case K_SLICE_X: {
        if (v->sx == 1 && v->x0 == s->i0) { s_volume(x, s->a, o, v); return; }
        Vol iv = { 1, v->sy, v->sz, s->i0, v->y0, v->z0, v->dx, v->dy, v->dz };
        float *ib = sctx_acquire(x, vol_size(&iv)); s_volume(x, s->a, ib, &iv);
        int idx = 0;
        for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) for (int iy = 0; iy < v->sy; iy++) o[idx++] = ib[vol_idx(&iv, 0, iy, iz)];
        sctx_release(x, ib); return;
    }
    case K_SLICE_Y: {
        if (v->sy == 1 && v->y0 == s->i0) { s_volume(x, s->a, o, v); return; }
        Vol iv = { v->sx, 1, v->sz, v->x0, s->i0, v->z0, v->dx, v->dy, v->dz };
        float *ib = sctx_acquire(x, vol_size(&iv)); s_volume(x, s->a, ib, &iv);
        for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) fill(o + vol_idx(v, ix, 0, iz), v->sy, ib[vol_idx(&iv, ix, 0, iz)]);
        sctx_release(x, ib); return;
    }
    case K_SLICE_Z: {
        if (v->sz == 1 && v->z0 == s->i0) { s_volume(x, s->a, o, v); return; }
        Vol iv = { v->sx, v->sy, 1, v->x0, v->y0, s->i0, v->dx, v->dy, v->dz };
        float *ib = sctx_acquire(x, vol_size(&iv)); s_volume(x, s->a, ib, &iv);
        int idx = 0;
        for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) for (int iy = 0; iy < v->sy; iy++) o[idx++] = ib[vol_idx(&iv, ix, iy, 0)];
        sctx_release(x, ib); return;
    }
    case K_SLICE_XZ: {
        if (v->sx == 1 && v->sz == 1 && v->x0 == s->i0 && v->z0 == s->i1) { s_volume(x, s->a, o, v); return; }
        Vol iv = { 1, v->sy, 1, s->i0, v->y0, s->i1, v->dx, v->dy, v->dz };
        float *ib = sctx_acquire(x, vol_size(&iv)); s_volume(x, s->a, ib, &iv);
        for (int iy = 0; iy < v->sy; iy++) {
            float val = ib[vol_idx(&iv, 0, iy, 0)];
            int idx = vol_idx(v, 0, iy, 0);
            for (int iz = 0; iz < v->sz; iz++) for (int ix = 0; ix < v->sx; ix++) { o[idx] = val; idx += v->sy; }
        }
        sctx_release(x, ib); return;
    }
    case K_FTS: {
        /* размер по y обязан быть 1 (обёртка slice y=0) */
        s_volume(x, s->b, o, v);
        int idx = 0;
        for (int iz = 0; iz < v->sz; iz++) {
            int bz = vol_bz(v, iz);
            for (int ix = 0; ix < v->sx; ix++) {
                int bx = vol_bx(v, ix);
                o[idx] = (float)find_top(x, s, bx, bz, o[idx]);
                idx++;
            }
        }
        return;
    }
    }
}

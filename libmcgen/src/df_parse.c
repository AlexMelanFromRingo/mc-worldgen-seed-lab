/* df_parse.c — разбор JSON density-функций в дерево Df (оба семейства схем). */
#include "df.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TYPE_NAMES[DF__COUNT] = {
    "constant", "<ref>", "noise", "shifted_noise", "shift_a", "shift_b", "shift", "end_islands", "end_outer_islands",
    "distance_to_point", "gradient", "y_clamped_gradient", "old_blended_noise", "weird_scaled_sampler",
    "blend_alpha", "blend_offset", "beardifier",
    "abs", "square", "cube", "sqrt", "half_negative", "quarter_negative", "reciprocal", "negate", "squeeze", "log", "sign",
    "floor", "round", "ceil", "truncate",
    "add", "sub", "mul", "div", "min", "max", "pow",
    "spline", "lerp", "clamp", "range_choice", "interval_select",
    "cache", "interpolated", "flat_cache", "cache_2d", "cache_once", "cache_all_in_cell", "blend_density",
    "slice", "find_top_surface"
};
const char *df_type_name(DfType t) { return (t >= 0 && t < DF__COUNT) ? TYPE_NAMES[t] : "?"; }

char *df_full_id(const char *s) { return strchr(s, ':') ? xstrdup(s) : xsprintf("minecraft:%s", s); }

typedef struct { int newf; char *err; size_t errlen; int fail; } Ctx;

static Df *mk(DfType t) { Df *f = xcalloc(1, sizeof(Df)); f->t = t; return f; }
static DNum num_of(const Js *v) { DNum n; n.d = js_num(v, 0.0); n.f = js_numf(v, 0.0f); return n; }
static DNum num_const(double d, float f) { DNum n; n.d = d; n.f = f; return n; }
static Df *mk_const(DNum v) { Df *f = mk(DF_CONST); f->n0 = v; return f; }

static Df *parse(Ctx *c, const Js *js);
static DfSpline *parse_spline(Ctx *c, const Js *js);

static void fail(Ctx *c, const char *fmt, const char *arg) {
    if (!c->fail) set_err(c->err, c->errlen, fmt, arg ? arg : "");
    c->fail = 1;
}
static Df *field(Ctx *c, const Js *o, const char *key, int required) {
    Js *v = js_get(o, key);
    if (!v) { if (required) fail(c, "density_function: нет поля %s", key); return NULL; }
    return parse(c, v);
}
static int need_num(Ctx *c, const Js *o, const char *key, DNum *out) {
    Js *v = js_get(o, key);
    if (!js_is_num(v)) { fail(c, "density_function: нет числа %s", key); return 0; }
    *out = num_of(v); return 1;
}
static int axis_of(const char *s) { return !s ? -1 : !strcmp(s, "x") ? 0 : !strcmp(s, "y") ? 1 : !strcmp(s, "z") ? 2 : -1; }

static DfType type_from(const char *tn, int newf) {
    if (!strncmp(tn, "minecraft:", 10)) tn += 10;
    static const struct { const char *n; DfType t; int fam; } T[] = {   /* fam: 0 обе, 1 new, 2 old */
        {"constant", DF_CONST, 0}, {"noise", DF_NOISE, 0}, {"shifted_noise", DF_SHIFTED_NOISE, 2},
        {"shift_a", DF_SHIFT_A, 0}, {"shift_b", DF_SHIFT_B, 0}, {"shift", DF_SHIFT, 0},
        {"end_islands", DF_END_ISLANDS, 2}, {"end_outer_islands", DF_END_OUTER_ISLANDS, 1},
        {"distance_to_point", DF_DISTANCE_TO_POINT, 1}, {"gradient", DF_GRADIENT, 1}, {"y_clamped_gradient", DF_Y_CLAMPED_GRADIENT, 2},
        {"old_blended_noise", DF_OLD_BLENDED_NOISE, 0}, {"weird_scaled_sampler", DF_WEIRD_SCALED, 2},
        {"blend_alpha", DF_BLEND_ALPHA, 0}, {"blend_offset", DF_BLEND_OFFSET, 0}, {"beardifier", DF_BEARDIFIER, 0},
        {"abs", DF_ABS, 0}, {"square", DF_SQUARE, 0}, {"cube", DF_CUBE, 0}, {"sqrt", DF_SQRT, 1},
        {"half_negative", DF_HALF_NEGATIVE, 0}, {"quarter_negative", DF_QUARTER_NEGATIVE, 0},
        {"reciprocal", DF_RECIPROCAL, 1}, {"invert", DF_RECIPROCAL, 2}, {"negate", DF_NEGATE, 1}, {"squeeze", DF_SQUEEZE, 0},
        {"log", DF_LOG, 1}, {"sign", DF_SIGN, 1},
        {"floor", DF_FLOOR, 1}, {"round", DF_ROUND, 1}, {"ceil", DF_CEIL, 1}, {"truncate", DF_TRUNCATE, 1},
        {"add", DF_ADD, 0}, {"sub", DF_SUB, 1}, {"mul", DF_MUL, 0}, {"div", DF_DIV, 1}, {"min", DF_MIN, 0}, {"max", DF_MAX, 0},
        {"pow", DF_POW, 1}, {"spline", DF_SPLINE, 0}, {"lerp", DF_LERP, 1}, {"clamp", DF_CLAMP, 0},
        {"range_choice", DF_RANGE_CHOICE, 0}, {"interval_select", DF_INTERVAL_SELECT, 0},
        {"cache", DF_CACHE, 1}, {"interpolated", DF_INTERPOLATED, 0}, {"flat_cache", DF_FLAT_CACHE, 2}, {"cache_2d", DF_CACHE_2D, 2},
        {"cache_once", DF_CACHE_ONCE, 2}, {"cache_all_in_cell", DF_CACHE_ALL_IN_CELL, 2}, {"blend_density", DF_BLEND_DENSITY, 0},
        {"slice", DF_SLICE, 1}, {"find_top_surface", DF_FIND_TOP_SURFACE, 0},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(T[i].n, tn) && (T[i].fam == 0 || T[i].fam == (newf ? 1 : 2))) return T[i].t;
    return (DfType)-1;
}

static Df *parse(Ctx *c, const Js *js) {
    if (c->fail) return NULL;
    if (js_is_num(js)) return mk_const(num_of(js));
    if (js_is_str(js)) { Df *f = mk(DF_REF); f->name = df_full_id(js->s); return f; }
    if (!js_is_obj(js)) { fail(c, "density_function: ожидался объект/число/строка%s", ""); return NULL; }
    const char *tn = js_str(js_get(js, "type"), NULL);
    if (!tn) { fail(c, "density_function: нет type%s", ""); return NULL; }
    DfType t = type_from(tn, c->newf);
    if ((int)t < 0) { fail(c, "density_function: неизвестный тип %s", tn); return NULL; }
    Df *f = mk(t);
    const char *A1 = c->newf ? "left" : "argument1", *A2 = c->newf ? "right" : "argument2", *ARG = c->newf ? "input" : "argument";
    switch (t) {
    case DF_CONST: { Js *v = js_get(js, c->newf ? "value" : "argument"); if (!js_is_num(v)) fail(c, "constant: нет значения%s", ""); else f->n0 = num_of(v); break; }
    case DF_NOISE:
        f->name = df_full_id(js_str(js_get(js, "noise"), "?"));
        need_num(c, js, "xz_scale", &f->n0); need_num(c, js, "y_scale", &f->n1);
        if (c->newf) {
            f->a = js_get(js, "shift_x") ? field(c, js, "shift_x", 1) : mk_const(num_const(0.0, 0.0f));
            f->b = js_get(js, "shift_y") ? field(c, js, "shift_y", 1) : mk_const(num_const(0.0, 0.0f));
            f->c = js_get(js, "shift_z") ? field(c, js, "shift_z", 1) : mk_const(num_const(0.0, 0.0f));
        }
        break;
    case DF_SHIFTED_NOISE:
        f->name = df_full_id(js_str(js_get(js, "noise"), "?"));
        need_num(c, js, "xz_scale", &f->n0); need_num(c, js, "y_scale", &f->n1);
        f->a = field(c, js, "shift_x", 1); f->b = field(c, js, "shift_y", 1); f->c = field(c, js, "shift_z", 1);
        break;
    case DF_SHIFT_A: case DF_SHIFT_B: case DF_SHIFT:
        f->name = df_full_id(js_str(js_get(js, c->newf ? "noise" : "argument"), "?"));
        break;
    case DF_END_ISLANDS: case DF_END_OUTER_ISLANDS: case DF_BLEND_ALPHA: case DF_BLEND_OFFSET: case DF_BEARDIFIER: break;
    case DF_DISTANCE_TO_POINT: {
        Js *p = js_get(js, "point");
        if (!js_is_arr(p) || p->n != 3) { fail(c, "distance_to_point: point%s", ""); break; }
        f->i0 = js_int(p->items[0], 0); f->i1 = js_int(p->items[1], 0); f->i2 = js_int(p->items[2], 0);
        const char *m = js_str(js_get(js, "metric"), "");
        f->i3 = !strcmp(m, "euclidean") ? 0 : !strcmp(m, "euclidean_squared") ? 1 : !strcmp(m, "manhattan") ? 2 : !strcmp(m, "chebyshev") ? 3 : -1;
        if (f->i3 < 0) fail(c, "distance_to_point: metric %s", m);
        break;
    }
    case DF_GRADIENT: {
        f->i0 = axis_of(js_str(js_get(js, "axis"), NULL));
        if (f->i0 < 0) fail(c, "gradient: axis%s", "");
        const char *tl = js_str(js_get(js, "tiling"), "clamp_to_edge");
        f->i1 = !strcmp(tl, "clamp_to_edge") ? 0 : !strcmp(tl, "repeat") ? 1 : !strcmp(tl, "mirrored_repeat") ? 2 : -1;
        if (f->i1 < 0) fail(c, "gradient: tiling %s", tl);
        f->i2 = js_int(js_get(js, "from_coordinate"), 0); f->i3 = js_int(js_get(js, "to_coordinate"), 0);
        need_num(c, js, "from_value", &f->n0); need_num(c, js, "to_value", &f->n1);
        if (f->i2 == f->i3) fail(c, "gradient: from_coordinate == to_coordinate%s", "");
        break;
    }
    case DF_Y_CLAMPED_GRADIENT:
        f->i0 = js_int(js_get(js, "from_y"), 0); f->i1 = js_int(js_get(js, "to_y"), 0);
        need_num(c, js, "from_value", &f->n0); need_num(c, js, "to_value", &f->n1);
        break;
    case DF_OLD_BLENDED_NOISE:
        need_num(c, js, "xz_scale", &f->n0); need_num(c, js, "y_scale", &f->n1); need_num(c, js, "xz_factor", &f->n2);
        need_num(c, js, "y_factor", &f->n3); need_num(c, js, "smear_scale_multiplier", &f->n4);
        break;
    case DF_WEIRD_SCALED: {
        f->a = field(c, js, "input", 1);
        f->name = df_full_id(js_str(js_get(js, "noise"), "?"));
        const char *m = js_str(js_get(js, "rarity_value_mapper"), "");
        f->i0 = !strcmp(m, "type_1") ? 0 : !strcmp(m, "type_2") ? 1 : -1;
        if (f->i0 < 0) fail(c, "weird_scaled_sampler: %s", m);
        break;
    }
    case DF_ABS: case DF_SQUARE: case DF_CUBE: case DF_SQRT: case DF_HALF_NEGATIVE: case DF_QUARTER_NEGATIVE: case DF_RECIPROCAL:
    case DF_NEGATE: case DF_SQUEEZE: case DF_LOG: case DF_SIGN:
    case DF_INTERPOLATED: case DF_FLAT_CACHE: case DF_CACHE_2D: case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY: case DF_CACHE:
        f->a = field(c, js, ARG, 1);
        if (t == DF_INTERPOLATED && c->newf) { f->i0 = js_int(js_get(js, "cell_size_xz"), 0); f->i1 = js_int(js_get(js, "cell_size_y"), 0);
            if (f->i0 <= 0 || f->i1 <= 0) fail(c, "interpolated: cell_size%s", ""); }
        break;
    case DF_FLOOR: case DF_ROUND: case DF_CEIL: case DF_TRUNCATE:
        f->a = field(c, js, "input", 1);
        f->b = js_get(js, "multiple") ? field(c, js, "multiple", 1) : mk_const(num_const(1.0, 1.0f));
        break;
    case DF_ADD: case DF_SUB: case DF_MUL: case DF_DIV: case DF_MIN: case DF_MAX:
        f->a = field(c, js, A1, 1); f->b = field(c, js, A2, 1);
        break;
    case DF_POW: f->a = field(c, js, "base", 1); f->b = field(c, js, "exponent", 1); break;
    case DF_SPLINE: { Js *s = js_get(js, "spline"); if (!s) fail(c, "spline: нет spline%s", ""); else f->spline = parse_spline(c, s); break; }
    case DF_LERP: f->a = field(c, js, "alpha", 1); f->b = field(c, js, "first", 1); f->c = field(c, js, "second", 1); break;
    case DF_CLAMP: f->a = field(c, js, "input", 1); need_num(c, js, "min", &f->n0); need_num(c, js, "max", &f->n1); break;
    case DF_RANGE_CHOICE:
        f->a = field(c, js, "input", 1); need_num(c, js, "min_inclusive", &f->n0); need_num(c, js, "max_exclusive", &f->n1);
        f->b = field(c, js, "when_in_range", 1); f->c = field(c, js, "when_out_of_range", 1);
        break;
    case DF_INTERVAL_SELECT: {
        f->a = field(c, js, "input", 1);
        Js *th = js_get(js, "thresholds"), *fn = js_get(js, "functions");
        if (!js_is_arr(th) || !js_is_arr(fn) || fn->n < 2 || th->n != fn->n - 1) { fail(c, "interval_select: thresholds/functions%s", ""); break; }
        f->nthr = th->n; f->thr = xcalloc((size_t)th->n, sizeof(DNum));
        for (int i = 0; i < th->n; i++) f->thr[i] = num_of(th->items[i]);
        f->nlist = fn->n; f->list = xcalloc((size_t)fn->n, sizeof(Df *));
        for (int i = 0; i < fn->n; i++) f->list[i] = parse(c, fn->items[i]);
        break;
    }
    case DF_SLICE:
        f->i0 = axis_of(js_str(js_get(js, "axis"), NULL)); if (f->i0 < 0) fail(c, "slice: axis%s", "");
        f->i1 = js_int(js_get(js, "coordinate"), 0); f->a = field(c, js, "input", 1);
        break;
    case DF_FIND_TOP_SURFACE:
        f->a = field(c, js, "density", 1); f->b = field(c, js, "upper_bound", 1);
        f->i0 = js_int(js_get(js, "lower_bound"), 0); f->i1 = js_int(js_get(js, "cell_height"), 0);
        if (f->i1 <= 0) fail(c, "find_top_surface: cell_height%s", "");
        break;
    default: fail(c, "density_function: тип %s не поддержан", tn);
    }
    return f;
}

static DfSpline *parse_spline(Ctx *c, const Js *js) {
    DfSpline *s = xcalloc(1, sizeof *s);
    if (js_is_num(js)) { s->is_const = 1; s->value = num_of(js); return s; }
    if (!js_is_obj(js)) { fail(c, "spline: ожидался объект%s", ""); return s; }
    Js *co = js_get(js, "coordinate"), *pts = js_get(js, "points");
    if (!co || !js_is_arr(pts) || pts->n < 1) { fail(c, "spline: coordinate/points%s", ""); return s; }
    s->coord = parse(c, co);
    s->n = pts->n;
    s->loc = xcalloc((size_t)s->n, sizeof(DNum)); s->der = xcalloc((size_t)s->n, sizeof(DNum)); s->val = xcalloc((size_t)s->n, sizeof(DfSpline *));
    for (int i = 0; i < s->n; i++) {
        Js *p = pts->items[i];
        if (!need_num(c, p, "location", &s->loc[i]) || !need_num(c, p, "derivative", &s->der[i])) return s;
        Js *v = js_get(p, "value"); if (!v) { fail(c, "spline: нет value%s", ""); return s; }
        s->val[i] = parse_spline(c, v);
        if (i > 0 && !(s->loc[i].f > s->loc[i - 1].f)) fail(c, "spline: точки не по возрастанию%s", "");
    }
    return s;
}

Df *df_parse(const void *js, int new_family, char *err, size_t errlen) {
    Ctx c = { new_family, err, errlen, 0 };
    Df *f = parse(&c, (const Js *)js);
    if (c.fail) { df_free(f); return NULL; }
    df_hash(f);
    return f;
}

static void spline_free(DfSpline *s) {
    if (!s) return;
    df_free(s->coord);
    for (int i = 0; i < s->n; i++) spline_free(s->val[i]);
    free(s->loc); free(s->der); free(s->val); free(s);
}
void df_free(Df *f) {
    if (!f) return;
    df_free(f->a); df_free(f->b); df_free(f->c); df_free(f->d);
    for (int i = 0; i < f->nlist; i++) df_free(f->list[i]);
    free(f->list); free(f->thr); free(f->name);
    spline_free(f->spline);
    free(f);
}

/* ---- структурный хэш/равенство ---- */
static u64 mixh(u64 h, u64 v) { h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); return h; }
static u64 num_bits(DNum n) { u64 b; memcpy(&b, &n.d, 8); u32 fb; memcpy(&fb, &n.f, 4); return b ^ ((u64)fb << 17); }
static u64 spline_hash(const DfSpline *s) {
    if (!s) return 7;
    u64 h = (u64)s->is_const * 31 + 11;
    if (s->is_const) return mixh(h, num_bits(s->value));
    h = mixh(h, df_hash(s->coord));
    for (int i = 0; i < s->n; i++) { h = mixh(h, num_bits(s->loc[i])); h = mixh(h, num_bits(s->der[i])); h = mixh(h, spline_hash(s->val[i])); }
    return h;
}
u64 df_hash(const Df *f) {
    if (!f) return 3;
    if (f->hash) return f->hash;
    u64 h = (u64)f->t * 1000003ULL + 17;
    h = mixh(h, df_hash(f->a)); h = mixh(h, df_hash(f->b)); h = mixh(h, df_hash(f->c)); h = mixh(h, df_hash(f->d));
    for (int i = 0; i < f->nlist; i++) h = mixh(h, df_hash(f->list[i]));
    for (int i = 0; i < f->nthr; i++) h = mixh(h, num_bits(f->thr[i]));
    h = mixh(h, num_bits(f->n0)); h = mixh(h, num_bits(f->n1)); h = mixh(h, num_bits(f->n2)); h = mixh(h, num_bits(f->n3)); h = mixh(h, num_bits(f->n4));
    h = mixh(h, (u64)(u32)f->i0); h = mixh(h, (u64)(u32)f->i1); h = mixh(h, (u64)(u32)f->i2); h = mixh(h, (u64)(u32)f->i3);
    if (f->name) h = mixh(h, str_hash(f->name));
    if (f->spline) h = mixh(h, spline_hash(f->spline));
    ((Df *)f)->hash = h ? h : 1;
    return f->hash;
}
static int num_eq(DNum a, DNum b) { return jm_deq(a.d, b.d) && jm_feq(a.f, b.f); }
static int spline_eq(const DfSpline *a, const DfSpline *b) {
    if (a == b) return 1;
    if (!a || !b || a->is_const != b->is_const) return 0;
    if (a->is_const) return num_eq(a->value, b->value);
    if (a->n != b->n || !df_equal(a->coord, b->coord)) return 0;
    for (int i = 0; i < a->n; i++) if (!num_eq(a->loc[i], b->loc[i]) || !num_eq(a->der[i], b->der[i]) || !spline_eq(a->val[i], b->val[i])) return 0;
    return 1;
}
int df_equal(const Df *a, const Df *b) {
    if (a == b) return 1;
    if (!a || !b || a->t != b->t || df_hash(a) != df_hash(b)) return 0;
    if (!df_equal(a->a, b->a) || !df_equal(a->b, b->b) || !df_equal(a->c, b->c) || !df_equal(a->d, b->d)) return 0;
    if (a->nlist != b->nlist || a->nthr != b->nthr) return 0;
    for (int i = 0; i < a->nlist; i++) if (!df_equal(a->list[i], b->list[i])) return 0;
    for (int i = 0; i < a->nthr; i++) if (!num_eq(a->thr[i], b->thr[i])) return 0;
    if (!num_eq(a->n0, b->n0) || !num_eq(a->n1, b->n1) || !num_eq(a->n2, b->n2) || !num_eq(a->n3, b->n3) || !num_eq(a->n4, b->n4)) return 0;
    if (a->i0 != b->i0 || a->i1 != b->i1 || a->i2 != b->i2 || a->i3 != b->i3) return 0;
    if ((a->name == NULL) != (b->name == NULL) || (a->name && strcmp(a->name, b->name))) return 0;
    return spline_eq(a->spline, b->spline);
}

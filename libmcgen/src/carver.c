/* carver.c — стадия CARVERS для 26.3+ (карверы описаны данными: worldgen/carver/*.json; тип cave / canyon).
 *
 * Как устроено в игре (NoiseBasedChunkGenerator.generateCarvers):
 *   1. маска CarvingMask чанка (биты по (x, z, y), y ∈ [minGenY+1, minGenY+genDepth−1−7]);
 *   2. для каждого исходного чанка (cx+dx, cz+dz), dx, dz ∈ [−8, 8] (порядок: dx — внешний цикл), берётся список карверов биома
 *      исходного чанка (шумовой биом клетки (x_min/4, qy=0, z_min/4)), и для каждого карвера i списка:
 *      random.setLargeFeatureSeed(worldSeed + i, sx, sz); если isStartChunk (nextFloat() <= probability) — carve(): пути пещеры/каньона
 *      из ГСЧ Legacy (java.util.Random), эллипсоиды отсекаются по чанку и пишутся в маску;
 *   3. applyCarvingMask: по столбцам x, z (x — внешний), внутри столбца по непрерывным отрезкам маски снизу вверх, внутри отрезка сверху вниз:
 *      блок не из тега uncarvable → Aquifer.computeSubstance(x, y, z, 0.0): null — блок остаётся; иначе записывается состояние
 *      (воздух/вода/лава по уровням водоносного слоя); если встречена трава/мицелий (hasGrass, сбрасывается на каждом отрезке),
 *      а под вырезанным блоком — dirt, то dirt заменяется материалом поверхности (topMaterial).
 * Домен seed — McSeeds.terrain (RandomState.seed() = seed мира).
 * Float-арифметика повторяет Java: float/double там же, где в игре; порядок вызовов ГСЧ в выражениях задан явными временными.
 */
#include "carver.h"
#include "mcgen_tweaks_table.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>


/* статистика (тесты): сколько раз вызывался topMaterial и сколько блоков он изменил */
static long g_top_calls, g_top_changed;
void carvers_x_stats(long *calls, long *changed) { *calls = __atomic_load_n(&g_top_calls, __ATOMIC_RELAXED); *changed = __atomic_load_n(&g_top_changed, __ATOMIC_RELAXED); }

/* ---------------- java.util.Random (LegacyRandomSource / SingleThreadedRandomSource) ---------------- */
typedef struct { u64 s; } CRnd;
#define CR_MASK ((1ULL << 48) - 1)
static inline void cr_seed(CRnd *r, i64 seed) { r->s = ((u64)seed ^ 0x5DEECE66DULL) & CR_MASK; }
static inline i32 cr_next(CRnd *r, int bits) { r->s = (r->s * 0x5DEECE66DULL + 0xBULL) & CR_MASK; return (i32)(u32)(r->s >> (48 - bits)); }
static inline i32 cr_int(CRnd *r, i32 bound) {
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * (i64)cr_next(r, 31)) >> 31);
    i32 sample, modulo;
    do { sample = cr_next(r, 31); modulo = sample % bound; } while ((i32)((u32)sample - (u32)modulo + (u32)(bound - 1)) < 0);
    return modulo;
}
static inline float cr_float(CRnd *r) { return (float)cr_next(r, 24) * 5.9604645E-8f; }
static inline i64 cr_long(CRnd *r) { i32 hi = cr_next(r, 32); i32 lo = cr_next(r, 32); return (i64)(((u64)(i64)hi << 32) + (u64)(i64)lo); }
/* WorldgenRandom.setLargeFeatureSeed */
static inline void cr_large_feature_seed(CRnd *r, i64 seed, i32 cx, i32 cz) {
    cr_seed(r, seed);
    i64 xs = cr_long(r), zs = cr_long(r);
    i64 res = (i64)((u64)(i64)cx * (u64)xs) ^ (i64)((u64)(i64)cz * (u64)zs) ^ seed;
    cr_seed(r, res);
}
/* Mth.nextInt(random, min, max) */
static inline i32 cr_range(CRnd *r, i32 lo, i32 hi) { return lo >= hi ? lo : cr_int(r, hi - lo + 1) + lo; }

/* ---------------- Mth.sin / Mth.cos: таблица из 65536 float ---------------- */
static float g_sin[65536];
static int g_sin_state;   /* 0 — нет, 1 — строится, 2 — готова */
static void sin_ensure(void) {
    if (__atomic_load_n(&g_sin_state, __ATOMIC_ACQUIRE) == 2) return;
    int exp = 0;
    if (__atomic_compare_exchange_n(&g_sin_state, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < 65536; i++) g_sin[i] = (float)sin((double)i / 10430.378350470453);
        __atomic_store_n(&g_sin_state, 2, __ATOMIC_RELEASE);
    } else while (__atomic_load_n(&g_sin_state, __ATOMIC_ACQUIRE) != 2) { }
}
static inline float mth_sin(double v) { return g_sin[(int)(jm_d2l(v * 10430.378350470453) & 65535LL)]; }
static inline float mth_cos(double v) { return g_sin[(int)(jm_d2l(v * 10430.378350470453 + 16384.0) & 65535LL)]; }

/* ---------------- поставщики значений ---------------- */
typedef struct { int kind; int a, b; } IProv;               /* 0 константа a; 1 uniform [a,b]; 2 biased_to_bottom; 3 very_biased_to_bottom */
typedef struct { int kind; float a, b, c; } FProv;          /* 0 константа a; 1 uniform [a,b); 2 trapezoid (min a, max b, plateau c) */
typedef struct { int kind; int inner; int ak[2], ao[2]; } HProv;   /* 0 константа (якорь 0); 1 uniform; 2 biased_to_bottom; 3 very_biased_to_bottom */

static inline i32 ip_sample(const IProv *p, CRnd *r) {
    switch (p->kind) {
    case 0: return p->a;
    case 1: return cr_int(r, p->b - p->a + 1) + p->a;                                   /* UniformInt: randomBetweenInclusive */
    case 2: { i32 q = cr_int(r, p->b - p->a + 1); return p->a + cr_int(r, q + 1); }    /* BiasedToBottomInt */
    default: { i32 q = cr_int(r, p->b - p->a + 1); q = cr_int(r, q + 1); return p->a + cr_int(r, q + 1); }   /* VeryBiasedToBottomInt */
    }
}
static inline float fp_sample(const FProv *p, CRnd *r) {
    switch (p->kind) {
    case 0: return p->a;
    case 1: { float f = cr_float(r); return f * (p->b - p->a) + p->a; }                /* Mth.randomBetween */
    default: {                                                                          /* TrapezoidFloat */
        float range = p->b - p->a;
        float ps = (range - p->c) / 2.0f;
        float pe = range - ps;
        float f1 = cr_float(r), f2 = cr_float(r);
        return p->a + f1 * pe + f2 * ps;
    }
    }
}
static int anchor_resolve(int kind, int off, int min_gen_y, int gen_depth, int sea) {
    switch (kind) {
    case 0: return off;
    case 1: return min_gen_y + off;
    case 2: return gen_depth - 1 + min_gen_y - off;
    default: return sea + off;
    }
}
static int hp_sample(const HProv *p, CRnd *r, int min_gen_y, int gen_depth, int sea) {
    int mn = anchor_resolve(p->ak[0], p->ao[0], min_gen_y, gen_depth, sea);
    if (p->kind == 0) return mn;
    int mx = anchor_resolve(p->ak[1], p->ao[1], min_gen_y, gen_depth, sea);
    switch (p->kind) {
    case 1: if (mn > mx) return mn; return cr_int(r, mx - mn + 1) + mn;                /* UniformHeight: randomBetweenInclusive */
    case 2: { if (mx - mn - p->inner + 1 <= 0) return mn; i32 lim = cr_int(r, mx - mn - p->inner + 1); return cr_int(r, lim + p->inner) + mn; }
    default: {
        if (mx - mn - p->inner + 1 <= 0) return mn;
        i32 up = cr_range(r, mn + p->inner, mx);
        i32 bu = cr_range(r, mn, up - 1);
        return cr_range(r, mn, bu - 1 + p->inner);
    }
    }
}

/* ---------------- определения карверов ---------------- */
typedef struct {
    char *id;
    int type;                          /* 0 cave, 1 canyon */
    float probability;
    HProv y;
    /* cave */
    IProv count; FProv thickness; int weird_bias;
    FProv room_v, hmul, vmul, start_v, floor_level;
    /* canyon */
    FProv vrot, dist_factor, c_thick, hrad_factor, yscale; int smooth; float vdef, vcenter;
    /* 26.1/26.2 (ConfiguredWorldCarver): nether_cave, lava_level, replaceable */
    int nether, lava_k, lava_off; u8 *repl;
    float size_mul;                    /* тонкая настройка cave_size: множитель радиусов/толщины пещер (1.0 — ваниль) */
} CarverDef;

typedef struct Carvers {
    int ndef; CarverDef *def; StrMap ids;
    int nlists; int *list_off, *list_n, *list_items;   /* списки определений (индексы def) */
    int *biome_list;                                    /* биом → индекс списка (−1: нет) */
    int uniform_list;                                   /* список, общий для всех возможных биомов измерения; −2 — списки различаются */
    int has_canyon_mult;
    u8 *uncarv; int b_grass, b_myc, b_dirt;
    char err[256];
} Carvers;

static int jtype_is(const Js *v, const char *name) {   /* "minecraft:name" или "name" */
    const char *t = js_str(js_get(v, "type"), "");
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    return !strcmp(t, name);
}
static int parse_iprov(const Js *v, IProv *o, char *err, size_t el) {
    memset(o, 0, sizeof *o);
    if (js_is_num(v)) { o->kind = 0; o->a = js_int(v, 0); return 0; }
    if (!js_is_obj(v)) { set_err(err, el, "IntProvider: не число и не объект"); return -1; }
    if (jtype_is(v, "constant")) { o->kind = 0; o->a = js_int(js_get(v, "value"), 0); return 0; }
    int k = jtype_is(v, "uniform") ? 1 : jtype_is(v, "biased_to_bottom") ? 2 : jtype_is(v, "very_biased_to_bottom") ? 3 : -1;
    if (k < 0) { set_err(err, el, "IntProvider: тип %s не поддержан", js_str(js_get(v, "type"), "?")); return -1; }
    o->kind = k; o->a = js_int(js_get(v, "min_inclusive"), 0); o->b = js_int(js_get(v, "max_inclusive"), 0);
    return 0;
}
static int parse_fprov(const Js *v, FProv *o, char *err, size_t el) {
    memset(o, 0, sizeof *o);
    if (js_is_num(v)) { o->kind = 0; o->a = js_numf(v, 0.0f); return 0; }
    if (!js_is_obj(v)) { set_err(err, el, "FloatProvider: не число и не объект"); return -1; }
    if (jtype_is(v, "constant")) { o->kind = 0; o->a = js_numf(js_get(v, "value"), 0.0f); return 0; }
    if (jtype_is(v, "uniform")) { o->kind = 1; o->a = js_numf(js_get(v, "min_inclusive"), 0.0f); o->b = js_numf(js_get(v, "max_exclusive"), 0.0f); return 0; }
    if (jtype_is(v, "trapezoid")) {
        o->kind = 2; o->a = js_numf(js_get(v, "min"), 0.0f); o->b = js_numf(js_get(v, "max"), 0.0f); o->c = js_numf(js_get(v, "plateau"), 0.0f); return 0;
    }
    set_err(err, el, "FloatProvider: тип %s не поддержан", js_str(js_get(v, "type"), "?"));
    return -1;
}
static int parse_anchor(const Js *v, int *kind, int *off, char *err, size_t el) {
    static const char *K[4] = { "absolute", "above_bottom", "below_top", "relative_to_sea_level" };
    for (int k = 0; k < 4; k++) { const Js *a = js_get(v, K[k]); if (a) { *kind = k; *off = js_int(a, 0); return 0; } }
    set_err(err, el, "VerticalAnchor: нет absolute/above_bottom/below_top/relative_to_sea_level");
    return -1;
}
static int parse_hprov(const Js *v, HProv *o, char *err, size_t el) {
    memset(o, 0, sizeof *o); o->inner = 1;
    if (!js_is_obj(v)) { set_err(err, el, "HeightProvider: не объект"); return -1; }
    const Js *t = js_get(v, "type");
    if (!t) return parse_anchor(v, &o->ak[0], &o->ao[0], err, el);   /* голый якорь = константа */
    if (jtype_is(v, "constant")) { o->kind = 0; return parse_anchor(js_get(v, "value"), &o->ak[0], &o->ao[0], err, el); }
    int k = jtype_is(v, "uniform") ? 1 : jtype_is(v, "biased_to_bottom") ? 2 : jtype_is(v, "very_biased_to_bottom") ? 3 : -1;
    if (k < 0) { set_err(err, el, "HeightProvider: тип %s не поддержан", js_str(t, "?")); return -1; }
    o->kind = k;
    if (parse_anchor(js_get(v, "min_inclusive"), &o->ak[0], &o->ao[0], err, el) || parse_anchor(js_get(v, "max_inclusive"), &o->ak[1], &o->ao[1], err, el)) return -1;
    if (k >= 2) o->inner = js_int(js_get(v, "inner"), 1);
    return 0;
}

#define NEED(field, expr) do { const Js *_f = js_get(v, field); if (!_f) { set_err(err, el, "карвер %s: нет поля %s", d->id, field); return -1; } if (expr) return -1; } while (0)
static int parse_carver(CarverDef *d, const Js *v, char *err, size_t el) {
    if (!js_is_obj(v)) { set_err(err, el, "карвер %s: не объект", d->id); return -1; }
    int cave = jtype_is(v, "cave");
    if (!cave && !jtype_is(v, "canyon")) { set_err(err, el, "карвер %s: тип %s не поддержан", d->id, js_str(js_get(v, "type"), "?")); return -1; }
    d->type = cave ? 0 : 1;
    d->lava_k = 0; d->lava_off = -100000;      /* без lava_level (лава — только от aquifer); replaceable — всё, кроме uncarvable (для тестового режима eager) */
    d->probability = js_numf(js_get(v, "probability"), 0.0f);
    NEED("y", parse_hprov(_f, &d->y, err, el));
    if (cave) {
        NEED("count", parse_iprov(_f, &d->count, err, el));
        NEED("thickness", parse_fprov(_f, &d->thickness, err, el));
        d->weird_bias = js_bool(js_get(v, "weird_thickness_bias"), 0);
        NEED("room_vertical_radius_multiplier", parse_fprov(_f, &d->room_v, err, el));
        NEED("horizontal_radius_multiplier", parse_fprov(_f, &d->hmul, err, el));
        NEED("vertical_radius_multiplier", parse_fprov(_f, &d->vmul, err, el));
        const Js *sv = js_get(v, "start_vertical_radius_multiplier");
        if (sv) { if (parse_fprov(sv, &d->start_v, err, el)) return -1; } else { d->start_v.kind = 0; d->start_v.a = 1.0f; }
        NEED("floor_level", parse_fprov(_f, &d->floor_level, err, el));
    } else {
        NEED("vertical_rotation", parse_fprov(_f, &d->vrot, err, el));
        const Js *sh = js_get(v, "shape");
        if (!sh) { set_err(err, el, "карвер %s: нет shape", d->id); return -1; }
        const Js *v2 = v; (void)v2;
        {
            const Js *f;
            if (!(f = js_get(sh, "distance_factor")) || parse_fprov(f, &d->dist_factor, err, el)) { set_err(err, el, "карвер %s: shape.distance_factor", d->id); return -1; }
            if (!(f = js_get(sh, "thickness")) || parse_fprov(f, &d->c_thick, err, el)) { set_err(err, el, "карвер %s: shape.thickness", d->id); return -1; }
            if (!(f = js_get(sh, "horizontal_radius_factor")) || parse_fprov(f, &d->hrad_factor, err, el)) { set_err(err, el, "карвер %s: shape.horizontal_radius_factor", d->id); return -1; }
            if (!(f = js_get(sh, "y_scale")) || parse_fprov(f, &d->yscale, err, el)) { set_err(err, el, "карвер %s: shape.y_scale", d->id); return -1; }
            d->smooth = js_int(js_get(sh, "width_smoothness"), 1);
            d->vdef = js_numf(js_get(sh, "vertical_radius_default_factor"), 0.0f);
            d->vcenter = js_numf(js_get(sh, "vertical_radius_center_factor"), 0.0f);
        }
    }
    return 0;
}

/* ---- формат 26.1/26.2: {"type": "...", "config": {...}}; параметры классов Java (getCaveBound, getThickness, getYScale) → поля определения ---- */
static int parse_replaceable(const McGen *g, const Js *v, u8 **out, char *err, size_t el) {
    u8 *m = xcalloc((size_t)(g->nblocks ? g->nblocks : 1), 1);
    const Js *items[1]; int n = 0; const Js **arr = items;
    if (js_is_str(v)) { arr = items; items[0] = v; n = 1; } else if (js_is_arr(v)) { arr = (const Js **)v->items; n = v->n; } else { free(m); set_err(err, el, "replaceable: не строка и не список"); return -1; }
    for (int i = 0; i < n; i++) {
        const char *id = js_str(arr[i], NULL);
        if (!id) { free(m); set_err(err, el, "replaceable: элемент не строка"); return -1; }
        if (id[0] == '#') { const u8 *t = gen_block_tag(g, id + 1); for (int b = 0; b < g->nblocks; b++) if (t[b]) m[b] = 1; }
        else { int st = gen_state_id(g, id); if (st < 0) { free(m); set_err(err, el, "replaceable: нет блока %s", id); return -1; } m[g->state_block[st]] = 1; }
    }
    *out = m;
    return 0;
}
static int parse_carver_old(const McGen *g, CarverDef *d, const Js *root, char *err, size_t el) {
    if (!js_is_obj(root)) { set_err(err, el, "карвер %s: не объект", d->id); return -1; }
    const Js *v = js_get(root, "config");
    if (!js_is_obj(v)) { set_err(err, el, "карвер %s: нет config", d->id); return -1; }
    int cave = jtype_is(root, "cave"), nether = jtype_is(root, "nether_cave");
    if (!cave && !nether && !jtype_is(root, "canyon")) { set_err(err, el, "карвер %s: тип %s не поддержан", d->id, js_str(js_get(root, "type"), "?")); return -1; }
    d->type = (cave || nether) ? 0 : 1; d->nether = nether;
    d->probability = js_numf(js_get(v, "probability"), 0.0f);
    NEED("y", parse_hprov(_f, &d->y, err, el));
    { const Js *lv = js_get(v, "lava_level"); if (!lv || parse_anchor(lv, &d->lava_k, &d->lava_off, err, el)) { set_err(err, el, "карвер %s: lava_level", d->id); return -1; } }
    NEED("replaceable", parse_replaceable(g, _f, &d->repl, err, el));
    if (d->type == 0) {
        d->count.kind = 3; d->count.a = 0; d->count.b = nether ? 9 : 14;                       /* getCaveBound() = 15 | 10 */
        d->thickness.kind = 2; d->thickness.a = 0.0f; d->thickness.b = nether ? 6.0f : 3.0f; d->thickness.c = nether ? 2.0f : 1.0f;   /* getThickness */
        d->weird_bias = !nether;
        NEED("yScale", parse_fprov(_f, &d->room_v, err, el));
        NEED("horizontal_radius_multiplier", parse_fprov(_f, &d->hmul, err, el));
        NEED("vertical_radius_multiplier", parse_fprov(_f, &d->vmul, err, el));
        d->start_v.kind = 0; d->start_v.a = nether ? 5.0f : 1.0f;                               /* getYScale() */
        NEED("floor_level", parse_fprov(_f, &d->floor_level, err, el));
    } else {
        NEED("vertical_rotation", parse_fprov(_f, &d->vrot, err, el));
        NEED("yScale", parse_fprov(_f, &d->yscale, err, el));
        const Js *sh = js_get(v, "shape");
        if (!sh) { set_err(err, el, "карвер %s: нет shape", d->id); return -1; }
        const Js *f;
        if (!(f = js_get(sh, "distance_factor")) || parse_fprov(f, &d->dist_factor, err, el)) { set_err(err, el, "карвер %s: shape.distance_factor", d->id); return -1; }
        if (!(f = js_get(sh, "thickness")) || parse_fprov(f, &d->c_thick, err, el)) { set_err(err, el, "карвер %s: shape.thickness", d->id); return -1; }
        if (!(f = js_get(sh, "horizontal_radius_factor")) || parse_fprov(f, &d->hrad_factor, err, el)) { set_err(err, el, "карвер %s: shape.horizontal_radius_factor", d->id); return -1; }
        d->smooth = js_int(js_get(sh, "width_smoothness"), 1);
        d->vdef = js_numf(js_get(sh, "vertical_radius_default_factor"), 0.0f);
        d->vcenter = js_numf(js_get(sh, "vertical_radius_center_factor"), 0.0f);
    }
    return 0;
}

#undef NEED

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* possible biomes for the world's biome source; returns count in *n (ids into g->biome_names) */
static int *possible_biomes(const McWorld *w, int *n) {
    const McGen *g = w->g;
    int *r = xmalloc(sizeof(int) * (size_t)(g->nbiomes + 8)); int k = 0;
    u8 *seen = xcalloc((size_t)g->nbiomes + 1, 1);
    #define ADD(b) do { int _b = (b); if (_b >= 0 && _b < g->nbiomes && !seen[_b]) { seen[_b] = 1; r[k++] = _b; } } while (0)
    switch (w->preset->biome_source) {
    case BS_MULTI_OVERWORLD: for (int i = 0; i < g->n_ow; i++) ADD(g->ow_points[i].biome); break;
    case BS_MULTI_NETHER: for (int i = 0; i < g->n_nether; i++) ADD(g->nether_points[i].biome); break;
    case BS_THE_END: {
        static const char *E[5] = { "minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands", "minecraft:small_end_islands", "minecraft:end_barrens" };
        for (int i = 0; i < 5; i++) ADD(gen_biome_id(g, E[i]));
        break;
    }
    default: ADD(w->preset->fixed_biome); break;
    }
    #undef ADD
    free(seen);
    *n = k;
    return r;
}

static void carvers_free_data(Carvers *C) {
    if (!C) return;
    for (int i = 0; i < C->ndef; i++) { free(C->def[i].id); free(C->def[i].repl); }
    free(C->def); sm_free(&C->ids, NULL);
    free(C->list_off); free(C->list_n); free(C->list_items); free(C->biome_list);
    free(C);
}

static Carvers *carvers_build(McWorld *w) {
    const McGen *g = w->g;
    Carvers *C = xcalloc(1, sizeof *C);
    char err[256] = {0};
    C->uniform_list = -2;
    char dir[1024]; snprintf(dir, sizeof dir, "%s/data/minecraft/worldgen/%s", g->pack, g->newf ? "carver" : "configured_carver");
    char **files = NULL; int nf = list_dir_recursive(dir, ".json", &files);
    if (nf < 0) nf = 0;
    if (nf > 1) qsort(files, (size_t)nf, sizeof(char *), cmp_str);
    C->def = xcalloc((size_t)(nf ? nf : 1), sizeof(CarverDef));
    for (int i = 0; i < nf && !C->err[0]; i++) {
        char path[1100]; snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        JsDoc *doc = js_parse_file(path, err, sizeof err);
        if (!doc) { snprintf(C->err, sizeof C->err, "карвер %s: %s", files[i], err); break; }
        CarverDef *d = &C->def[C->ndef];
        size_t L = strlen(files[i]); d->id = xsprintf("minecraft:%.*s", (int)(L - 5), files[i]);
        if (g->newf ? parse_carver(d, js_root(doc), err, sizeof err) : parse_carver_old(g, d, js_root(doc), err, sizeof err)) { snprintf(C->err, sizeof C->err, "%s", err); free(d->id); js_free(doc); break; }
        if (g->newf && !d->repl) {
            const u8 *unc = gen_block_tag(g, "minecraft:uncarvable");
            d->repl = xmalloc((size_t)(g->nblocks ? g->nblocks : 1));
            for (int b = 0; b < g->nblocks; b++) d->repl[b] = !unc[b];
        }
        sm_put(&C->ids, d->id, (void *)(intptr_t)(C->ndef + 1));
        C->ndef++;
        js_free(doc);
    }
    free_str_list(files, nf);
    /* тонкие настройки: canyon_frequency (вероятность каньонов), cave_density (вероятность пещер) — по умолчанию 1.0 без эффекта */
    double cm = w->tweak[MCGEN_TWEAK_CANYON_FREQUENCY], cd = w->tweak[MCGEN_TWEAK_CAVE_DENSITY], cs = w->tweak[MCGEN_TWEAK_CAVE_SIZE];
    for (int i = 0; i < C->ndef; i++) {
        CarverDef *d = &C->def[i];
        double k = d->type == 1 ? cm : cd;                   /* каньоны — canyon_frequency, пещеры — cave_density */
        d->size_mul = 1.0f;
        if (k != 1.0) {
            double p = (double)d->probability * k;
            d->probability = p > 1.0 ? 1.0f : (float)p;
            if (k == 0.0) d->probability = -1.0f;            /* никогда: nextFloat() <= −1 ложно */
        }
        if (d->type == 0 && cs != 1.0) d->size_mul = (float)cs;
    }
    /* списки карверов по биомам */
    C->biome_list = xmalloc(sizeof(int) * (size_t)(g->nbiomes ? g->nbiomes : 1));
    C->list_off = xcalloc((size_t)g->nbiomes + 1, sizeof(int)); C->list_n = xcalloc((size_t)g->nbiomes + 1, sizeof(int));
    int cap = 64; C->list_items = xmalloc(sizeof(int) * (size_t)cap); int nitems = 0;
    for (int b = 0; b < g->nbiomes && !C->err[0]; b++) {
        C->biome_list[b] = -1;
        const char *name = g->biome_names[b];
        const char *colon = strchr(name, ':');
        char path[1100]; snprintf(path, sizeof path, "%s/data/%.*s/worldgen/biome/%s.json", g->pack, colon ? (int)(colon - name) : 9, colon ? name : "minecraft", colon ? colon + 1 : name);
        JsDoc *doc = js_parse_file(path, err, sizeof err);
        if (!doc) { snprintf(C->err, sizeof C->err, "биом %s: %s", name, err); break; }
        const Js *cv = js_get(js_root(doc), "carvers");
        int start = nitems, ok = 1;
        if (js_is_str(cv)) {
            const char *s = js_str(cv, "");
            if (s[0] == '#') ok = 0;
            else { intptr_t id = (intptr_t)sm_get(&C->ids, s); if (!id) ok = 0; else { if (nitems == cap) { cap *= 2; C->list_items = xrealloc(C->list_items, sizeof(int) * (size_t)cap); } C->list_items[nitems++] = (int)id - 1; } }
        } else if (js_is_arr(cv)) {
            for (int i = 0; i < cv->n && ok; i++) {
                const char *s = js_str(cv->items[i], NULL);
                intptr_t id = s ? (intptr_t)sm_get(&C->ids, s) : 0;
                if (!id) { ok = 0; break; }
                if (nitems == cap) { cap *= 2; C->list_items = xrealloc(C->list_items, sizeof(int) * (size_t)cap); }
                C->list_items[nitems++] = (int)id - 1;
            }
        } else if (cv) ok = 0;
        if (!ok) snprintf(C->err, sizeof C->err, "биом %s: список carvers не поддержан (теги/встроенные карверы)", name);
        else { C->list_off[C->nlists] = start; C->list_n[C->nlists] = nitems - start; C->biome_list[b] = C->nlists; C->nlists++; }
        js_free(doc);
    }
    /* общий список для всех возможных биомов измерения → поиск биома исходного чанка не нужен */
    if (!C->err[0]) {
        int np; int *pb = possible_biomes(w, &np);
        int first = -1, same = np > 0;
        for (int i = 0; i < np && same; i++) {
            int li = C->biome_list[pb[i]];
            if (first < 0) first = li;
            else if (li != first) {
                /* списки считаются равными, если совпадают по содержимому */
                if (C->list_n[li] != C->list_n[first] || memcmp(C->list_items + C->list_off[li], C->list_items + C->list_off[first], sizeof(int) * (size_t)C->list_n[li])) same = 0;
            }
        }
        C->uniform_list = same ? first : -2;
        free(pb);
    }
    C->uncarv = (u8 *)gen_block_tag(g, "minecraft:uncarvable");
    int s;
    s = gen_state_id(g, "minecraft:grass_block"); C->b_grass = s >= 0 ? g->state_block[s] : -1;
    s = gen_state_id(g, "minecraft:mycelium"); C->b_myc = s >= 0 ? g->state_block[s] : -1;
    s = gen_state_id(g, "minecraft:dirt"); C->b_dirt = s >= 0 ? g->state_block[s] : -1;
    return C;
}

void carvers_world_free(McWorld *w) {
    if (!w || !w->carvers) return;
    carvers_free_data((Carvers *)w->carvers);
    w->carvers = NULL;
}

/* ---------------- маска и эллипсоиды ---------------- */
typedef struct {
    int cx, cz;
    int min_gen_y, gen_depth, sea;
    int mmin, mmax, mh;                 /* геометрия маски: y ∈ [mmin, mmax] */
    u8 *mask;                           /* [(x*16+z)*mh + (y−mmin)] */
    u8 colany[256];
    /* 26.1/26.2: карвер сразу меняет чанк (WorldCarver.carveBlock), маска — только отметка «блок уже вырезан» */
    int eager;
    McWorld *w; const Carvers *C; TerrainCtx *t; uint16_t *blocks; PPMarks *marks;
    const CarverDef *cur;               /* карвер, чей carve() выполняется */
    int lava_y;                         /* lava_level текущего карвера, разрешённый по контексту */
} CCtx;

typedef struct { int kind; double floor_level; const float *wf; int min_gen_y; } Skip;   /* kind 0 — пещера, 1 — каньон */
static inline int skip_check(const Skip *s, double xd, double yd, double zd, int y) {
    if (s->kind == 0) return yd <= s->floor_level ? 1 : (xd * xd + yd * yd + zd * zd >= 1.0);
    int yi = y - s->min_gen_y;
    return (xd * xd + zd * zd) * (double)s->wf[yi - 1] + yd * yd / 6.0 >= 1.0;
}

/* 26.1/26.2: WorldCarver.carveBlock (+ NetherWorldCarver.carveBlock) */
static void old_carve_block(CCtx *c, int xi, int wy, int zi, int *has_grass) {
    McWorld *w = c->w; const McGen *g = w->g; const CarverDef *d = c->cur; const Carvers *C = c->C;
    size_t bi = ((size_t)(wy - w->min_y) * 16 + zi) * 16 + xi;
    int blk = g->state_block[c->blocks[bi]];
    if (d->nether) {                                                         /* NetherWorldCarver: без aquifer */
        if (!d->repl[blk]) return;
        c->blocks[bi] = (uint16_t)(wy <= c->min_gen_y + 31 ? g->st_lava : g->st_cave_air);
        return;
    }
    if (blk == C->b_grass || blk == C->b_myc) *has_grass = 1;
    if (!d->repl[blk]) return;                                               /* canReplaceBlock */
    int wx = c->cx * 16 + xi, wz = c->cz * 16 + zi;
    int sub, sched;
    if (wy <= c->lava_y) sub = g->st_lava;                                   /* getCarveState: lava_level */
    else { int s0 = 0; sub = terrain_carve_substance(c->t, wx, wy, wz, &s0); if (sub < 0) return; }
    sched = terrain_carve_sched(c->t);                                       /* shouldScheduleFluidUpdate() — значение ПОСЛЕДНЕГО вызова aquifer (в ветке лавы — «старое») */
    c->blocks[bi] = (uint16_t)sub;
    int sub_fluid = (g->state_cls[sub] & 4) != 0;
    if (sched && sub_fluid) ppmarks_add(c->marks, (wy - w->min_y) >> 4, xi, wy & 15, zi);
    if (*has_grass && mcgen_surface_top_material != NULL) {
        size_t bj = bi - 256;
        if (g->state_block[c->blocks[bj]] == C->b_dirt) {
            int top = mcgen_surface_top_material(w, c->t, c->cx, c->cz, c->blocks, wx, wy - 1, wz, sub_fluid);
            __atomic_add_fetch(&g_top_calls, 1, __ATOMIC_RELAXED);
            if (top >= 0) {
                if (top != c->blocks[bj]) __atomic_add_fetch(&g_top_changed, 1, __ATOMIC_RELAXED);
                c->blocks[bj] = (uint16_t)top;
                if (g->state_cls[top] & 4) ppmarks_add(c->marks, (wy - 1 - w->min_y) >> 4, xi, (wy - 1) & 15, zi);
            }
        }
    }
}

/* WorldCarver.carveEllipsoid */
static void carve_ellipsoid(CCtx *c, double x, double y, double z, double hr, double vr, const Skip *sk) {
    double center_x = (double)(c->cx * 16 + 8), center_z = (double)(c->cz * 16 + 8);
    double max_delta = 16.0 + hr * 2.0;
    if (!(fabs(x - center_x) > max_delta) && !(fabs(z - center_z) > max_delta)) {
        int cmx = c->cx * 16, cmz = c->cz * 16;
        int min_xi = jm_floor_d(x - hr) - cmx - 1; if (min_xi < 0) min_xi = 0;
        int max_xi = jm_floor_d(x + hr) - cmx; if (max_xi > 15) max_xi = 15;
        int min_y = jm_floor_d(y - vr) - 1; if (min_y < c->mmin) min_y = c->mmin;
        int max_y = jm_floor_d(y + vr) + 1; if (max_y > c->mmax) max_y = c->mmax;
        int min_zi = jm_floor_d(z - hr) - cmz - 1; if (min_zi < 0) min_zi = 0;
        int max_zi = jm_floor_d(z + hr) - cmz; if (max_zi > 15) max_zi = 15;
        for (int xi = min_xi; xi <= max_xi; xi++) {
            int wx = cmx + xi;
            double xd = ((double)wx + 0.5 - x) / hr;
            for (int zi = min_zi; zi <= max_zi; zi++) {
                int wz = cmz + zi;
                double zd = ((double)wz + 0.5 - z) / hr;
                if (!(xd * xd + zd * zd >= 1.0)) {
                    u8 *col = c->mask + (size_t)(xi * 16 + zi) * (size_t)c->mh;
                    int has_grass = 0;
                    for (int wy = max_y; wy > min_y; wy--) {
                        double yd = ((double)wy - 0.5 - y) / vr;
                        if (skip_check(sk, xd, yd, zd, wy)) continue;
                        if (c->eager) {
                            if (col[wy - c->mmin]) continue;                    /* !mask.get(...) */
                            col[wy - c->mmin] = 1;
                            old_carve_block(c, xi, wy, zi, &has_grass);
                        } else { col[wy - c->mmin] = 1; c->colany[xi * 16 + zi] = 1; }
                    }
                }
            }
        }
    }
}
/* WorldCarver.canReach */
static inline int can_reach(const CCtx *c, double x, double z, int cur, int total, float thickness) {
    double xd = x - (double)(c->cx * 16 + 8), zd = z - (double)(c->cz * 16 + 8);
    double remaining = (double)(total - cur);
    double rr = (double)(thickness + 2.0f + 16.0f);
    return xd * xd + zd * zd - remaining * remaining <= rr * rr;
}

#define PI_F ((float)3.141592653589793)
#define TWO_PI_F ((float)(3.141592653589793 * 2))
#define HALF_PI_F ((float)(3.141592653589793 / 2))

/* ---- пещеры (CaveWorldCarver) ---- */
static void cave_tunnel(CCtx *c, i64 tunnel_seed, double x, double y, double z, double hmul, double vmul, float thickness,
                        float hrot, float vrot, int step, int dist, double yscale, const Skip *sk) {
    CRnd r; cr_seed(&r, tunnel_seed);
    int split = cr_int(&r, dist / 2) + dist / 4;
    int steep = cr_int(&r, 6) == 0;
    float y_rota = 0.0f, x_rota = 0.0f;
    for (int cur = step; cur < dist; cur++) {
        float sv = mth_sin((double)((PI_F * (float)cur) / (float)dist));
        double hr = 1.5 + (double)(sv * thickness);
        double vr = hr * yscale;
        float cos_x = mth_cos((double)vrot);
        x += (double)(mth_cos((double)hrot) * cos_x);
        y += (double)mth_sin((double)vrot);
        z += (double)(mth_sin((double)hrot) * cos_x);
        vrot *= steep ? 0.92f : 0.7f;
        vrot += x_rota * 0.1f;
        hrot += y_rota * 0.1f;
        x_rota *= 0.9f;
        y_rota *= 0.75f;
        { float a = cr_float(&r), b = cr_float(&r), cc = cr_float(&r); x_rota += (a - b) * cc * 2.0f; }
        { float a = cr_float(&r), b = cr_float(&r), cc = cr_float(&r); y_rota += (a - b) * cc * 4.0f; }
        if (cur == split && thickness > 1.0f) {
            i64 s1 = cr_long(&r); float t1 = cr_float(&r) * 0.5f + 0.5f;
            cave_tunnel(c, s1, x, y, z, hmul, vmul, t1, hrot - HALF_PI_F, vrot / 3.0f, cur, dist, 1.0, sk);
            i64 s2 = cr_long(&r); float t2 = cr_float(&r) * 0.5f + 0.5f;
            cave_tunnel(c, s2, x, y, z, hmul, vmul, t2, hrot + HALF_PI_F, vrot / 3.0f, cur, dist, 1.0, sk);
            return;
        }
        if (cr_int(&r, 4) != 0) {
            if (!can_reach(c, x, z, cur, dist, thickness)) return;
            carve_ellipsoid(c, x, y, z, hr * hmul, vr * vmul, sk);
        }
    }
}
static void cave_carve(CCtx *c, const CarverDef *d, CRnd *r, int sx, int sz) {
    int max_distance = ((4 * 2 - 1) << 4);     /* SectionPos.sectionToBlockCoord(getRange()*2 − 1), getRange() = 4 */
    int cave_count = ip_sample(&d->count, r);
    for (int cave = 0; cave < cave_count; cave++) {
        double x = (double)(sx * 16 + cr_int(r, 16));
        double y = (double)hp_sample(&d->y, r, c->min_gen_y, c->gen_depth, c->sea);
        double z = (double)(sz * 16 + cr_int(r, 16));
        double hmul = (double)fp_sample(&d->hmul, r);
        double vmul = (double)fp_sample(&d->vmul, r);
        if (d->size_mul != 1.0f) { hmul *= (double)d->size_mul; vmul *= (double)d->size_mul; }
        double svmul = (double)fp_sample(&d->start_v, r);
        double floor_level = (double)fp_sample(&d->floor_level, r);
        Skip sk = { 0, floor_level, NULL, c->min_gen_y };
        int tunnels = 1;
        if (cr_int(r, 4) == 0) {
            double yscale = (double)fp_sample(&d->room_v, r);
            float thickness = 1.0f + cr_float(r) * 6.0f;
            if (d->size_mul != 1.0f) thickness *= d->size_mul;
            double hr = 1.5 + (double)(mth_sin((double)HALF_PI_F) * thickness);
            double vr = hr * yscale;
            carve_ellipsoid(c, x + 1.0, y, z, hr, vr, &sk);
            tunnels += cr_int(r, 4);
        }
        for (int i = 0; i < tunnels; i++) {
            float hrot = cr_float(r) * TWO_PI_F;
            float vrot = (cr_float(r) - 0.5f) / 4.0f;
            float thickness = fp_sample(&d->thickness, r);
            if (d->weird_bias && cr_int(r, 10) == 0) { float a = cr_float(r), b = cr_float(r); thickness *= a * b * 3.0f + 1.0f; }
            if (d->size_mul != 1.0f) thickness *= d->size_mul;
            int distance = max_distance - cr_int(r, max_distance / 4);
            i64 ts = cr_long(r);
            cave_tunnel(c, ts, x, y, z, hmul, vmul, thickness, hrot, vrot, 0, distance, svmul, &sk);
        }
    }
}

/* ---- каньоны (CanyonWorldCarver) ---- */
static void canyon_do_carve(CCtx *c, const CarverDef *d, i64 tunnel_seed, double x, double y, double z, float thickness,
                            float hrot, float vrot, int step, int distance, double yscale) {
    CRnd r; cr_seed(&r, tunnel_seed);
    float *wf = xmalloc(sizeof(float) * (size_t)c->gen_depth);
    {   /* initWidthFactors */
        float width_factor = 1.0f;
        for (int yi = 0; yi < c->gen_depth; yi++) {
            if (yi == 0 || cr_int(&r, d->smooth) == 0) { float a = cr_float(&r), b = cr_float(&r); width_factor = 1.0f + a * b; }
            wf[yi] = width_factor * width_factor;
        }
    }
    Skip sk = { 1, 0.0, wf, c->min_gen_y };
    float y_rota = 0.0f, x_rota = 0.0f;
    for (int cur = step; cur < distance; cur++) {
        float sv = mth_sin((double)(((float)cur * PI_F) / (float)distance));
        double hr = 1.5 + (double)(sv * thickness);
        double vr = hr * yscale;
        hr *= (double)fp_sample(&d->hrad_factor, &r);
        {   /* updateVerticalRadius */
            float vm = 1.0f - fabsf(0.5f - (float)cur / (float)distance) * 2.0f;
            float factor = d->vdef + d->vcenter * vm;
            float rb = cr_float(&r) * (1.0f - 0.75f) + 0.75f;
            vr = (double)factor * vr * (double)rb;
        }
        float xc = mth_cos((double)vrot), xs = mth_sin((double)vrot);
        x += (double)(mth_cos((double)hrot) * xc);
        y += (double)xs;
        z += (double)(mth_sin((double)hrot) * xc);
        vrot *= 0.7f;
        vrot += x_rota * 0.05f;
        hrot += y_rota * 0.05f;
        x_rota *= 0.8f;
        y_rota *= 0.5f;
        { float a = cr_float(&r), b = cr_float(&r), cc = cr_float(&r); x_rota += (a - b) * cc * 2.0f; }
        { float a = cr_float(&r), b = cr_float(&r), cc = cr_float(&r); y_rota += (a - b) * cc * 4.0f; }
        if (cr_int(&r, 4) != 0) {
            if (!can_reach(c, x, z, cur, distance, thickness)) { free(wf); return; }
            carve_ellipsoid(c, x, y, z, hr, vr, &sk);
        }
    }
    free(wf);
}
static void canyon_carve(CCtx *c, const CarverDef *d, CRnd *r, int sx, int sz) {
    int max_distance = (4 * 2 - 1) * 16;
    double x = (double)(sx * 16 + cr_int(r, 16));
    int y = hp_sample(&d->y, r, c->min_gen_y, c->gen_depth, c->sea);
    double z = (double)(sz * 16 + cr_int(r, 16));
    float hrot = cr_float(r) * TWO_PI_F;
    float vrot = fp_sample(&d->vrot, r);
    double yscale = (double)fp_sample(&d->yscale, r);
    float thickness = fp_sample(&d->c_thick, r);
    float df = fp_sample(&d->dist_factor, r);
    int distance = jm_d2i((double)((float)max_distance * df));
    i64 ts = cr_long(r);
    canyon_do_carve(c, d, ts, x, (double)y, z, thickness, hrot, vrot, 0, distance, yscale);
}

/* ---------------- список карверов исходного чанка ---------------- */
static int source_list(McWorld *w, const Carvers *C, int sx, int sz) {
    if (C->uniform_list != -2) return C->uniform_list;
    int b = world_biome_noise(w, sx * 4, 0, sz * 4);
    return b >= 0 && b < w->g->nbiomes ? C->biome_list[b] : -1;
}

static Carvers *carvers_get(McWorld *w, char *err, size_t errlen) {
    Carvers *C = __atomic_load_n((Carvers **)&w->carvers, __ATOMIC_ACQUIRE);
    if (!C) {
        mutex_lock(w->lock);
        C = (Carvers *)w->carvers;
        if (!C) { C = carvers_build(w); __atomic_store_n((Carvers **)&w->carvers, C, __ATOMIC_RELEASE); }
        mutex_unlock(w->lock);
    }
    if (C->err[0]) { set_err(err, errlen, "carvers: %s", C->err); return NULL; }
    return C;
}

/* строит маску в c->mask; возвращает 1, если что-то отмечено */
static void build_mask(McWorld *w, const Carvers *C, CCtx *c) {
    CRnd r;
    i64 seed = w->seeds.terrain;
    for (int dx = -8; dx <= 8; dx++) for (int dz = -8; dz <= 8; dz++) {
        int sx = c->cx + dx, sz = c->cz + dz;
        int li = source_list(w, C, sx, sz);
        if (li < 0) continue;
        const int *items = C->list_items + C->list_off[li];
        for (int index = 0; index < C->list_n[li]; index++) {
            const CarverDef *d = &C->def[items[index]];
            cr_large_feature_seed(&r, seed + index, sx, sz);
            float f = cr_float(&r);
            if (f <= d->probability) {
                c->cur = d;
                if (c->eager) c->lava_y = anchor_resolve(d->lava_k, d->lava_off, c->min_gen_y, c->gen_depth, c->sea);
                if (d->type == 0) cave_carve(c, d, &r, sx, sz); else canyon_carve(c, d, &r, sx, sz);
            }
        }
    }
}

static int ctx_init(McWorld *w, CCtx *c, int cx, int cz) {
    const NoiseSettings *ns = w->ns;
    memset(c, 0, sizeof *c);
    c->cx = cx; c->cz = cz;
    c->min_gen_y = w->min_y > ns->min_y ? w->min_y : ns->min_y;                    /* WorldGenerationContext */
    c->gen_depth = w->height < ns->height ? w->height : ns->height;
    c->sea = ns->sea_level;
    c->mmin = c->min_gen_y + 1;
    c->mmax = c->min_gen_y + c->gen_depth - 1 - 7;                                 /* protectedBlocksOnTop = 7 */
    c->mh = c->mmax - c->mmin + 1;
    if (c->mh <= 0 || c->gen_depth <= 0) return 0;
    c->mask = xcalloc((size_t)c->mh * 256, 1);
    return 1;
}
static void ctx_free(CCtx *c) { free(c->mask); c->mask = NULL; }

/* ---------------- applyCarvingMask ---------------- */
static void apply_mask(McWorld *w, const Carvers *C, TerrainCtx *t, CCtx *c, uint16_t *blocks, PPMarks *marks) {
    const McGen *g = w->g;
    /* 26.4: карвинг встроен в ChunkTerrainBuilder.fillColumn — вырезаются только «твёрдые» блоки шума (воздух и жидкости шума не трогаются),
     * вместо hasGrass/topMaterial — carvedTopBlock (стадия SURFACE, поток W2) */
    int v264 = g->version >= V26_4;
    int has_top = mcgen_surface_top_material != NULL && !v264;
    for (int x = 0; x < 16; x++) for (int z = 0; z < 16; z++) {
        if (!c->colany[x * 16 + z]) continue;
        const u8 *col = c->mask + (size_t)(x * 16 + z) * (size_t)c->mh;
        int wx = c->cx * 16 + x, wz = c->cz * 16 + z;
        int i = 0;
        while (i < c->mh) {
            if (!col[i]) { i++; continue; }
            int j = i; while (j + 1 < c->mh && col[j + 1]) j++;
            int has_grass = 0;
            for (int yy = j; yy >= i; yy--) {
                int y = c->mmin + yy;
                size_t bi = ((size_t)(y - w->min_y) * 16 + z) * 16 + x;
                int st = blocks[bi];
                int blk = g->state_block[st];
                if (C->uncarv[blk]) continue;
                if (v264 && (g->state_cls[st] & (1 | 4))) continue;        /* CL_AIR | CL_FLUID */
                if (blk == C->b_grass || blk == C->b_myc) has_grass = 1;
                int sched = 0;
                int sub = terrain_carve_substance(t, wx, y, wz, &sched);
                if (sub < 0) continue;
                blocks[bi] = (uint16_t)sub;
                int sub_fluid = (g->state_cls[sub] & 4) != 0;      /* CL_FLUID: непустое FluidState */
                if (sched && sub_fluid) ppmarks_add(marks, (y - w->min_y) >> 4, x, y & 15, z);
                if (has_grass) {
                    size_t bj = bi - 256;                          /* y − 1 */
                    if (g->state_block[blocks[bj]] == C->b_dirt && has_top) {
                        int top = mcgen_surface_top_material(w, t, c->cx, c->cz, blocks, wx, y - 1, wz, sub_fluid);
                        __atomic_add_fetch(&g_top_calls, 1, __ATOMIC_RELAXED);
                        if (top >= 0) {
                            if (top != blocks[bj]) __atomic_add_fetch(&g_top_changed, 1, __ATOMIC_RELAXED);
                            blocks[bj] = (uint16_t)top;
                            if (g->state_cls[top] & 4) ppmarks_add(marks, (y - 1 - w->min_y) >> 4, x, (y - 1) & 15, z);
                        }
                    }
                }
            }
            i = j + 1;
        }
    }
}

static int g_force_eager;
void carvers_x_set_eager(int on) { g_force_eager = on; }   /* тест: 26.3+ в «жадном» режиме 26.1/26.2 (сравнение двух путей применения маски) */

int carvers_apply_chunk(McWorld *w, TerrainCtx *t, int cx, int cz, uint16_t *blocks, PPMarks *marks, char *err, size_t errlen) {
    sin_ensure();
    Carvers *C = carvers_get(w, err, errlen);
    if (!C) return MCGEN_E_DATA;
    if (C->uniform_list >= 0 && C->list_n[C->uniform_list] == 0) return MCGEN_OK;   /* у всех возможных биомов карверов нет (End) */
    CCtx c;
    if (!ctx_init(w, &c, cx, cz)) return MCGEN_OK;
    c.w = w; c.C = C; c.t = t; c.blocks = blocks; c.marks = marks; c.eager = !w->g->newf || g_force_eager;
    build_mask(w, C, &c);                                           /* 26.1/26.2: блоки меняются по ходу (old_carve_block) */
    if (!c.eager) {
        int any = 0; for (int i = 0; i < 256; i++) if (c.colany[i]) { any = 1; break; }
        if (any) apply_mask(w, C, t, &c, blocks, marks);
    }
    ctx_free(&c);
    return MCGEN_OK;
}

int carvers_x_mask(McWorld *w, int cx, int cz, uint8_t *mask, size_t cap, int *miny, int *h) {
    if (!w->g->newf) return -1;
    sin_ensure();
    char e[256];
    Carvers *C = carvers_get(w, e, sizeof e);
    if (!C) return -1;
    CCtx c;
    if (!ctx_init(w, &c, cx, cz)) return -1;
    if ((size_t)c.mh * 256 > cap) { ctx_free(&c); return -1; }
    build_mask(w, C, &c);
    memcpy(mask, c.mask, (size_t)c.mh * 256);
    *miny = c.mmin; *h = c.mh;
    ctx_free(&c);
    int n = 0; for (size_t i = 0; i < (size_t)*h * 256; i++) n += mask[i];
    return n;
}

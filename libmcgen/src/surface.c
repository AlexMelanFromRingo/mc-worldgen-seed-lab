/* surface.c — стадия SURFACE: правила материала/поверхности и buildSurface (поток W2).
 *
 * Что здесь:
 *  1) разбор дерева правил (rule: block | sequence | condition | bandlands | ore_vein; condition: biome | noise_threshold |
 *     vertical_gradient | y_above | water | temperature | steep | not | hole | above_preliminary_surface | stone_depth) из
 *     noise_settings (26.1/26.2: surface_rule, 26.3+: ссылка на worldgen/material_rule + material_condition) в компактное
 *     неизменяемое дерево узлов; ссылки на реестры подставляются при разборе;
 *  2) логика SurfaceSystem/MaterialSystem.buildSurface: поиск поверхности и глубин камня по столбцу, `surface`/`surface_secondary`,
 *     расширения eroded_badlands и замёрзшего океана (айсберги), глинистые полосы, карта высот WORLD_SURFACE_WG по ходу работы;
 *  3) биом для правил — BiomeManager.getBiome (зум с «размытием»): клетки собственного чанка — из биомов стадии BIOMES, соседние —
 *     world_biome_cell (то, что игра берёт из соседних чанков).
 * Ветки по версиям (поведение, закодированное в Java): 26.1/26.2 — double, правило применяется только к блоку default_block,
 * preliminary surface — билинейно по ячейкам 16×16; 26.3+ — float (NoiseStack), правило ко всем «камням», preliminary surface
 * — объёмом 16×1×16; 26.4 — см. surface_264 (ChunkTerrainBuilder).
 * Все приведения/округления — как в Java (jm_*), порядок обхода (x снаружи, z внутри) и обновление карты высот — как у игры.
 */
#include "surface.h"
#include "df_old.h"
#include "carver.h"
#include "mcgen_tweaks_table.h"
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#define SC_AIR 1
#define SC_FLUID 4
#define WAY_BELOW_MIN_Y (-2032 * 16)

/* ======================================================================= дерево правил */
enum { RN_BLOCK, RN_SEQ, RN_COND, RN_BANDS, RN_VEIN };
enum { CN_BIOME, CN_NOISE, CN_VGRAD, CN_Y, CN_WATER, CN_TEMP, CN_STEEP, CN_NOT, CN_HOLE, CN_ABOVE, CN_STONE };
typedef struct CNode CNode;
typedef struct RNode RNode;
struct CNode {
    int type, id;
    int lazy;                   /* 1 — кэш по колонке (updateXZ), 0 — по шагу y (updateY), -1 — без кэша */
    CNode *inner;               /* not */
    u8 mask[32];                /* biome: множество id биомов */
    int slot;                   /* noise_threshold: слот шума */
    double lo, hi;
    PosRnd rf; int t_below, f_above;     /* vertical_gradient (границы уже в блоках) */
    int anchor, mult, add_stone, offset; /* y_above / water */
    int add_surface, range, ceiling;     /* stone_depth */
};
struct RNode {
    int type, state;
    RNode **kid; int nkid;
    CNode *cond; RNode *then;
    int vein[3];
};
typedef struct { const NStack *ns; const OldNormal *on; int is3d; } NSlot;
typedef struct { const NStack *ns; const OldNormal *on; } SNoise;

typedef struct SurfWorld {
    McWorld *w; int newf, v264;
    RNode *root; int ncond;
    NSlot *slot; int nslot, cap_slot; StrMap slotmap;
    SNoise surface, secondary, band_off, bad_pillar, bad_roof, bad_surface, ice_pillar, ice_roof, ice_surface;
    int bands[192];
    int sea, defb, defblock_blk;
    int st_packed_ice, st_snow;
    int ctx_min, ctx_h;                 /* WorldGenerationContext: minY = max(...), height = min(...) */
    int b_eroded, b_frozen, b_deep_frozen;
    float *btemp; u8 *bfrozen;          /* температура и модификатор по id биома */
    int has_temp;
    GNoise t_noise, info_noise, f_noise[3];   /* Biome: TEMPERATURE_NOISE, BIOME_INFO_NOISE, FROZEN_TEMPERATURE_NOISE */
    PtrVec allocs;
    const S *prelim;                    /* 26.3+: router.chunk_surface_level */
    u64 uid;
    PtrVec tl;                          /* контексты потоков для mcgen_surface_top_material: принадлежат миру (освобождаются вместе с ним) */
} SurfWorld;

typedef struct Comp {
    McWorld *w; const McGen *g; SurfWorld *S;
    char *err; size_t errlen; int bad;
} Comp;

static u64 g_surface_uid;
static void *nalloc(Comp *k, size_t sz) { void *p = xcalloc(1, sz); pv_push(&k->S->allocs, p); return p; }
static void cerr(Comp *k, const char *fmt, const char *a) {
    if (!k->bad) { k->bad = 1; char buf[400]; snprintf(buf, sizeof buf, fmt, a ? a : ""); set_err(k->err, k->errlen, "surface: %s", buf); }
}
static const char *strip_ns(const char *t) { const char *c = strchr(t, ':'); return c ? c + 1 : t; }

/* состояние блока из JSON: строка «minecraft:x[...]» или {"Name","Properties"}; недостающие свойства — из состояния по умолчанию */
static int surf_state(const McGen *g, const Js *v) {
    if (js_is_str(v)) {
        return gen_state_id(g, v->s);
    }
    if (!js_is_obj(v)) return -1;
    const char *nm = js_str(js_get(v, "Name"), NULL);
    if (!nm) return -1;
    Js *pr = js_get(v, "Properties");
    int def = gen_state_id(g, nm);
    if (def < 0 || !js_is_obj(pr) || pr->n == 0) return def;
    const char *dn = g->state_names[def];
    const char *br = strchr(dn, '[');
    if (!br) return def;
    StrBuf b = {0};
    sb_putc(&b, '[');
    /* свойства по умолчанию в порядке blocks.json, поверх — значения из JSON */
    const char *p = br + 1;
    int first = 1;
    while (*p && *p != ']') {
        const char *e = p; while (*e && *e != ',' && *e != ']') e++;
        const char *eq = memchr(p, '=', (size_t)(e - p));
        size_t kl = eq ? (size_t)(eq - p) : 0;
        char *key = xstrndup(p, kl);
        const char *val = eq ? eq + 1 : "";
        size_t vl = (size_t)(e - val);
        char *vs = xstrndup(val, vl);
        const Js *ov = js_get(pr, key);
        if (!first) sb_putc(&b, ',');
        sb_printf(&b, "%s=%s", key, ov ? js_str(ov, vs) : vs);
        first = 0;
        free(key); free(vs);
        p = *e == ',' ? e + 1 : e;
    }
    sb_putc(&b, ']');
    char *full = xsprintf("%s%s", nm, b.s);
    free(b.s);
    int id = gen_state_id(g, full);
    free(full);
    return id >= 0 ? id : def;
}

static JsDoc *load_ref(const McGen *g, const char *dir, const char *id) {
    const char *c = strchr(id, ':');
    char ns[64] = "minecraft";
    const char *path = id;
    if (c) { size_t n = (size_t)(c - id); if (n >= sizeof ns) n = sizeof ns - 1; memcpy(ns, id, n); ns[n] = 0; path = c + 1; }
    char *f = xsprintf("%s/data/%s/worldgen/%s/%s.json", g->pack, ns, dir, path);
    JsDoc *d = js_parse_file(f, NULL, 0);
    free(f);
    return d;
}

static int resolve_anchor(Comp *k, const Js *a) {
    const SurfWorld *S = k->S;
    if (js_is_num(a)) return (int)a->d;
    if (js_get(a, "absolute")) return js_int(js_get(a, "absolute"), 0);
    if (js_get(a, "above_bottom")) return S->ctx_min + js_int(js_get(a, "above_bottom"), 0);
    if (js_get(a, "below_top")) return S->ctx_h - 1 + S->ctx_min - js_int(js_get(a, "below_top"), 0);
    cerr(k, "неизвестный vertical anchor", NULL);
    return 0;
}

static int slot_for(Comp *k, const char *name, int is3d) {
    SurfWorld *S = k->S;
    char *full = df_full_id(name);
    char *key = xsprintf("%s|%d", full, is3d);
    intptr_t v = (intptr_t)sm_get(&S->slotmap, key);
    if (v) { free(full); free(key); return (int)v - 1; }
    pv_push(&S->allocs, full);          /* world_noise_* хранит указатель на имя */
    if (S->nslot == S->cap_slot) { S->cap_slot = S->cap_slot ? S->cap_slot * 2 : 16; S->slot = xrealloc(S->slot, sizeof(NSlot) * (size_t)S->cap_slot); }
    NSlot *n = &S->slot[S->nslot]; memset(n, 0, sizeof *n); n->is3d = is3d;
    char e[300] = {0};
    if (S->newf) n->ns = world_noise_new(S->w, full, e, sizeof e); else n->on = world_noise_old(S->w, full, e, sizeof e);
    if (!n->ns && !n->on) cerr(k, "нет шума %s", full);
    sm_put(&S->slotmap, key, (void *)(intptr_t)(S->nslot + 1));
    free(key);
    return S->nslot++;
}

static CNode *compile_cond(Comp *k, const Js *j, int depth);
static RNode *compile_rule(Comp *k, const Js *j, int depth);

static CNode *compile_cond(Comp *k, const Js *j, int depth) {
    if (k->bad) return NULL;
    if (depth > 64) { cerr(k, "слишком глубокая вложенность", NULL); return NULL; }
    if (js_is_str(j)) {
        JsDoc *d = load_ref(k->g, "material_condition", j->s);
        if (!d) { cerr(k, "нет material_condition %s", j->s); return NULL; }
        CNode *r = compile_cond(k, js_root(d), depth + 1);
        js_free(d);
        return r;
    }
    const char *t = strip_ns(js_str(js_get(j, "type"), ""));
    CNode *n = nalloc(k, sizeof *n);
    n->id = k->S->ncond++;
    n->lazy = 0;
    if (!strcmp(t, "biome")) {
        const Js *bi = js_get(j, "biome_is");
        int cnt = js_is_arr(bi) ? bi->n : 1;
        for (int i = 0; i < cnt; i++) {
            const char *nm = js_is_arr(bi) ? js_str(bi->items[i], "") : js_str(bi, "");
            int id = gen_biome_id(k->g, nm);
            if (id >= 0 && id < 256) n->mask[id >> 3] |= (u8)(1u << (id & 7));
        }
        n->type = CN_BIOME; n->lazy = 0;
    } else if (!strcmp(t, "noise_threshold")) {
        n->type = CN_NOISE;
        int is3d = js_bool(js_get(j, "is_3d"), 0);
        n->slot = slot_for(k, js_str(js_get(j, "noise"), ""), is3d);
        n->lo = js_num(js_get(j, "min_threshold"), 0.0); n->hi = js_num(js_get(j, "max_threshold"), 0.0);
        n->lazy = -1;
    } else if (!strcmp(t, "vertical_gradient")) {
        n->type = CN_VGRAD;
        char *nm = df_full_id(js_str(js_get(j, "random_name"), ""));
        Rnd r = pos_from_hash(&k->w->pos_terrain, nm);
        n->rf = rnd_fork_positional(&r);
        free(nm);
        n->t_below = resolve_anchor(k, js_get(j, "true_at_and_below")); n->f_above = resolve_anchor(k, js_get(j, "false_at_and_above"));
        n->lazy = 0;
    } else if (!strcmp(t, "y_above")) {
        n->type = CN_Y; n->anchor = resolve_anchor(k, js_get(j, "anchor"));
        n->mult = js_int(js_get(j, "surface_depth_multiplier"), 0); n->add_stone = js_bool(js_get(j, "add_stone_depth"), 0);
    } else if (!strcmp(t, "water")) {
        n->type = CN_WATER; n->offset = js_int(js_get(j, "offset"), 0);
        n->mult = js_int(js_get(j, "surface_depth_multiplier"), 0); n->add_stone = js_bool(js_get(j, "add_stone_depth"), 0);
    } else if (!strcmp(t, "temperature")) { n->type = CN_TEMP; k->S->has_temp = 1; }
    else if (!strcmp(t, "steep")) { n->type = CN_STEEP; n->lazy = 1; }
    else if (!strcmp(t, "hole")) { n->type = CN_HOLE; n->lazy = 1; }
    else if (!strcmp(t, "above_preliminary_surface")) { n->type = CN_ABOVE; n->lazy = -1; }
    else if (!strcmp(t, "not")) { n->type = CN_NOT; n->lazy = -1; n->inner = compile_cond(k, js_get(j, "invert"), depth + 1); }
    else if (!strcmp(t, "stone_depth")) {
        n->type = CN_STONE; n->offset = js_int(js_get(j, "offset"), 0); n->add_surface = js_bool(js_get(j, "add_surface_depth"), 0);
        n->range = js_int(js_get(j, "secondary_depth_range"), 0);
        n->ceiling = !strcmp(strip_ns(js_str(js_get(j, "surface_type"), "floor")), "ceiling");
    } else { cerr(k, "неизвестное условие %s", t); return NULL; }
    return n;
}

static RNode *compile_rule(Comp *k, const Js *j, int depth) {
    if (k->bad) return NULL;
    if (depth > 64) { cerr(k, "слишком глубокая вложенность", NULL); return NULL; }
    if (js_is_str(j)) {
        JsDoc *d = load_ref(k->g, "material_rule", j->s);
        if (!d) { cerr(k, "нет material_rule %s", j->s); return NULL; }
        RNode *r = compile_rule(k, js_root(d), depth + 1);
        js_free(d);
        return r;
    }
    const char *t = strip_ns(js_str(js_get(j, "type"), ""));
    if (!strcmp(t, "sequence")) {
        const Js *sq = js_get(j, "sequence");
        if (!js_is_arr(sq) || sq->n == 0) { cerr(k, "пустая sequence", NULL); return NULL; }
        if (sq->n == 1) return compile_rule(k, sq->items[0], depth + 1);
        RNode *n = nalloc(k, sizeof *n);
        n->type = RN_SEQ; n->nkid = sq->n; n->kid = nalloc(k, sizeof(RNode *) * (size_t)sq->n);
        for (int i = 0; i < sq->n; i++) { n->kid[i] = compile_rule(k, sq->items[i], depth + 1); if (!n->kid[i]) return NULL; }
        return n;
    }
    RNode *n = nalloc(k, sizeof *n);
    if (!strcmp(t, "condition")) {
        n->type = RN_COND;
        n->cond = compile_cond(k, js_get(j, "if_true"), depth + 1);
        n->then = compile_rule(k, js_get(j, "then_run"), depth + 1);
        if (!n->cond || !n->then) return NULL;
    } else if (!strcmp(t, "block")) {
        n->type = RN_BLOCK; n->state = surf_state(k->g, js_get(j, "result_state"));
        if (n->state < 0) { cerr(k, "неизвестный блок в result_state", NULL); return NULL; }
    } else if (!strcmp(t, "bandlands")) n->type = RN_BANDS;
    else if (!strcmp(t, "ore_vein")) {
        n->type = RN_VEIN;
        n->vein[0] = surf_state(k->g, js_get(j, "ore_block")); n->vein[1] = surf_state(k->g, js_get(j, "raw_ore_block"));
        n->vein[2] = surf_state(k->g, js_get(j, "filler_block"));
    } else { cerr(k, "неизвестное правило %s", t); return NULL; }
    return n;
}

/* ======================================================================= система (шумы, полосы, температура) */
static int rnd_bool(Rnd *r) { return r->legacy ? lcg_next(&r->l, 1) != 0 : (xoro_next_long(&r->x) & 1ULL) != 0; }
static void make_bands(Rnd *r, int *bands, int base_width, int state) {
    int count = rnd_next_int_bound(r, 15 - 6 + 1) + 6;
    for (int i = 0; i < count; i++) {
        int width = base_width + rnd_next_int_bound(r, 3);
        int start = rnd_next_int_bound(r, 192);
        for (int p = 0; start + p < 192 && p < width; p++) bands[start + p] = state;
    }
}
static void gen_bands(const McGen *g, Rnd *r, int *bands) {
    int terra = gen_state_id(g, "minecraft:terracotta"), orange = gen_state_id(g, "minecraft:orange_terracotta");
    int yellow = gen_state_id(g, "minecraft:yellow_terracotta"), brown = gen_state_id(g, "minecraft:brown_terracotta");
    int red = gen_state_id(g, "minecraft:red_terracotta"), white = gen_state_id(g, "minecraft:white_terracotta");
    int gray = gen_state_id(g, "minecraft:light_gray_terracotta");
    for (int i = 0; i < 192; i++) bands[i] = terra;
    for (int i = 0; i < 192; i++) { i += rnd_next_int_bound(r, 5) + 1; if (i < 192) bands[i] = orange; }
    make_bands(r, bands, 1, yellow); make_bands(r, bands, 2, brown); make_bands(r, bands, 1, red);
    int white_count = rnd_next_int_bound(r, 15 - 9 + 1) + 9;
    int i = 0;
    for (int start = 0; i < white_count && start < 192; start += rnd_next_int_bound(r, 16) + 4) {
        bands[start] = white;
        if (start - 1 > 0 && rnd_bool(r)) bands[start - 1] = gray;
        if (start + 1 < 192 && rnd_bool(r)) bands[start + 1] = gray;
        i++;
    }
}

static int snoise_init(Comp *k, SNoise *n, const char *name) {
    char e[300] = {0};
    n->ns = NULL; n->on = NULL;
    if (k->S->newf) n->ns = world_noise_new(k->w, name, e, sizeof e); else n->on = world_noise_old(k->w, name, e, sizeof e);
    if (!n->ns && !n->on) { cerr(k, "нет шума %s", name); return -1; }
    return 0;
}

static void load_biome_temps(Comp *k) {
    SurfWorld *S = k->S; const McGen *g = k->g;
    S->btemp = xcalloc((size_t)(g->nbiomes ? g->nbiomes : 1), sizeof(float));
    S->bfrozen = xcalloc((size_t)(g->nbiomes ? g->nbiomes : 1), 1);
    for (int i = 0; i < g->nbiomes; i++) {
        /* температуры нужны и стадии FEATURES (Biome.shouldFreeze/shouldSnow — surface_biome_temperature): грузим для всех биомов (W12) */
        const char *nm = g->biome_names[i];
        JsDoc *d = load_ref(g, "biome", nm);
        if (!d) continue;
        const Js *r = js_root(d);
        S->btemp[i] = js_numf(js_get(r, "temperature"), 0.0f);
        S->bfrozen[i] = !strcmp(strip_ns(js_str(js_get(r, "temperature_modifier"), "none")), "frozen");
        js_free(d);
    }
}

int surface_world_init(McWorld *w, char *err, size_t errlen) {
    const McGen *g = w->g;
    SurfWorld *S = xcalloc(1, sizeof *S);
    w->surface = S;
    S->w = w; S->newf = g->newf; S->v264 = g->version >= V26_4;
    /* уровень моря системы (айсберги, температура) — как у заполнения: настройки шума + твик sea_level_offset (числа в правилах — абсолютные y) */
    S->sea = w->ns->sea_level + (int)w->tweak[MCGEN_TWEAK_SEA_LEVEL_OFFSET];
    S->uid = __atomic_add_fetch(&g_surface_uid, 1, __ATOMIC_RELAXED);
    S->defb = w->def_block;
    S->ctx_min = w->min_y > w->ns->min_y ? w->min_y : w->ns->min_y;
    S->ctx_h = w->height < w->ns->height ? w->height : w->ns->height;
    S->st_packed_ice = gen_state_id(g, "minecraft:packed_ice"); S->st_snow = gen_state_id(g, "minecraft:snow_block");
    S->b_eroded = gen_biome_id(g, "minecraft:eroded_badlands");
    S->b_frozen = gen_biome_id(g, "minecraft:frozen_ocean"); S->b_deep_frozen = gen_biome_id(g, "minecraft:deep_frozen_ocean");
    Comp k = { w, g, S, err, errlen, 0 };
    /* правило: 26.3+ — ссылка material_rule; 26.1/26.2 — surface_rule */
    const Js *root = js_root(w->ns->doc);
    const Js *rj = js_get(root, "material_rule");
    if (!rj) rj = js_get(root, "surface_rule");
    if (!rj) { S->root = NULL; return 0; }   /* нет правила — стадия ничего не меняет (расширения всё равно работают) */
    /* шумы системы */
    int rc = 0;
    rc |= snoise_init(&k, &S->surface, "minecraft:surface");
    rc |= snoise_init(&k, &S->secondary, "minecraft:surface_secondary");
    rc |= snoise_init(&k, &S->band_off, "minecraft:clay_bands_offset");
    rc |= snoise_init(&k, &S->bad_pillar, "minecraft:badlands_pillar");
    rc |= snoise_init(&k, &S->bad_roof, "minecraft:badlands_pillar_roof");
    rc |= snoise_init(&k, &S->bad_surface, "minecraft:badlands_surface");
    rc |= snoise_init(&k, &S->ice_pillar, "minecraft:iceberg_pillar");
    rc |= snoise_init(&k, &S->ice_roof, "minecraft:iceberg_pillar_roof");
    rc |= snoise_init(&k, &S->ice_surface, "minecraft:iceberg_surface");
    if (rc) return MCGEN_E_DATA;
    { Rnd r = pos_from_hash(&w->pos_terrain, "minecraft:clay_bands"); gen_bands(g, &r, S->bands); }
    S->root = compile_rule(&k, rj, 0);
    if (k.bad || !S->root) return MCGEN_E_DATA;
    /* температура биомов (условие temperature и расширение айсбергов) */
    load_biome_temps(&k);
    {   /* Biome.TEMPERATURE_NOISE / FROZEN_TEMPERATURE_NOISE / BIOME_INFO_NOISE: фиксированные seed, не зависят от мира */
        double off = g->newf ? 0.0 : 256.0;
        Rnd r1 = rnd_legacy_seed(1234); gn_init(&S->t_noise, &r1, off);
        Rnd r3 = rnd_legacy_seed(3456); for (int i = 0; i < 3; i++) gn_init(&S->f_noise[i], &r3, off);
        Rnd r2 = rnd_legacy_seed(2345); gn_init(&S->info_noise, &r2, off);
    }
    if (g->newf) {
        S->prelim = w->s_rf[RF_CHUNK_SURFACE_LEVEL];
        if (!S->prelim) { set_err(err, errlen, "surface: в noise_router нет chunk_surface_level"); return MCGEN_E_DATA; }
    }
    return 0;
}

void surface_ctx_free(SurfCtx *c);
void surface_world_free(McWorld *w) {
    SurfWorld *S = w->surface;
    if (!S) return;
    for (int i = 0; i < S->tl.n; i++) surface_ctx_free(S->tl.v[i]);
    pv_free(&S->tl);
    for (int i = 0; i < S->allocs.n; i++) free(S->allocs.v[i]);
    pv_free(&S->allocs);
    sm_free(&S->slotmap, NULL);
    free(S->slot); free(S->btemp); free(S->bfrozen); free(S);
    w->surface = NULL;
}

/* ======================================================================= контекст потока */
#define CELLC 4096
#define FIDC 1024
struct SurfCtx {
    McWorld *w; SurfWorld *S;
    SCtx *x; NChunk *nch; int nch_cx, nch_cz, nch_ok;
    u64 xz_stamp, y_stamp;
    u64 *cst; u8 *cval; u64 *nst; double *nval;
    /* чанк */
    int cx, cz, minY, maxY; uint16_t *blk; const u8 *bio; PPMarks *marks;
    int first[256];                    /* Heightmap WORLD_SURFACE_WG: firstAvailable (highestTaken + 1) */
    int first0[256];                   /* то же до обработки (26.4: NoiseColumn.columnMaxYs — градиенты по ним) */
    float prelim[256]; int prelim_ok;
    /* позиция */
    int bx, by, bz, gradx, gradz, sdepth, above, below, wh, minsl, minsl_ok, biome, biome_ok, cur;
    double sec; int sec_ok;
    int top_mode;                      /* topMaterial: preliminary surface — объём 1×1×1 */
    int top_last, top_cx, top_cz, top_init;      /* topMaterial: лист R-дерева предыдущего запроса биома (ThreadLocal lastResult) в пределах карва чанка (top_cx, top_cz) */
    const u8 *cmask; int cmmin, cmh;   /* 26.4: маска карверов чанка (NULL — нет) */
    /* кэш биомов клеток */
    u64 ck[CELLC]; u8 cv[CELLC];
    /* кэш «размытия» BiomeManager по угловым клеткам (fiddle зависит только от клетки и seed) */
    struct { i32 x, y, z; u8 used; double f[3]; } fid[FIDC];
    /* множества биомов собственного чанка по кварте y (быстрый отказ для условий biome без зума) */
    u8 *qset; int qset_ok, qn; int interior;
};

SurfCtx *surface_ctx_new(McWorld *w) {
    SurfWorld *S = w->surface;
    SurfCtx *c = xcalloc(1, sizeof *c);
    c->w = w; c->S = S;
    c->cst = xcalloc((size_t)(S->ncond + 1), sizeof(u64)); c->cval = xcalloc((size_t)(S->ncond + 1), 1);
    c->nst = xcalloc((size_t)(S->nslot + 1), sizeof(u64)); c->nval = xcalloc((size_t)(S->nslot + 1), sizeof(double));
    c->xz_stamp = c->y_stamp = 1;
    c->qn = w->height >> 2; c->qset = xcalloc((size_t)c->qn, 32);
    if (S->newf) c->x = sctx_new(w->nc, 1);
    else c->nch = nchunk_new(w->old);
    return c;
}
void surface_ctx_free(SurfCtx *c) {
    if (!c) return;
    if (c->x) sctx_free(c->x);
    if (c->nch) nchunk_free(c->nch);
    free(c->cst); free(c->cval); free(c->nst); free(c->nval); free(c->qset); free(c);
}

/* ---- биомы: BiomeManager.getBiome ---- */
static inline i64 zoom_lcg(i64 r, i64 cc) { u64 v = (u64)r; v *= v * 6364136223846793005ULL + 1442695040888963407ULL; return (i64)(v + (u64)cc); }
static inline double fiddle(i64 r) { i64 m = (r >> 24) % 1024; if (m < 0) m += 1024; return ((double)m / 1024.0 - 0.5) * 0.9; }
static void fiddle3(SurfCtx *c, i64 seed, int x, int y, int z, double f[3]) {
    u32 h = ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & (FIDC - 1);
    if (c->fid[h].used && c->fid[h].x == x && c->fid[h].y == y && c->fid[h].z == z) { f[0] = c->fid[h].f[0]; f[1] = c->fid[h].f[1]; f[2] = c->fid[h].f[2]; return; }
    i64 r = seed;
    r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z); r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z);
    f[0] = fiddle(r); r = zoom_lcg(r, seed);
    f[1] = fiddle(r); r = zoom_lcg(r, seed);
    f[2] = fiddle(r);
    c->fid[h].used = 1; c->fid[h].x = x; c->fid[h].y = y; c->fid[h].z = z; c->fid[h].f[0] = f[0]; c->fid[h].f[1] = f[1]; c->fid[h].f[2] = f[2];
}
static int cell_biome(SurfCtx *c, int qx, int qy, int qz) {
    const McWorld *w = c->w;
    int qmin = w->min_y >> 2, qh = w->height >> 2;
    int cy = qy < qmin ? qmin : (qy > qmin + qh - 1 ? qmin + qh - 1 : qy);
    /* topMaterial карвера: biomeGetter — BiomeManager над несохранённым резолвером (точечный путь климата, без записи в чанк); поиск R-дерева
     * наследует лист предыдущего запроса потока, поэтому в «ничьих» результат зависит от порядка запросов — кэш клеток здесь не используем */
    if (c->top_mode && !c->bio) return world_biome_noise_hist(w, qx, qy, qz, &c->top_last);
    if (c->bio && (qx >> 2) == c->cx && (qz >> 2) == c->cz) return c->bio[((cy - qmin) * 4 + (qz & 3)) * 4 + (qx & 3)];
    u64 key = ((u64)((u32)qx & 0xFFFFFFFu) << 36) | ((u64)((u32)qz & 0xFFFFFFFu) << 8) | (u64)(cy - qmin + 1);
    u64 h = (key * 0x9E3779B97F4A7C15ULL) >> 52;      /* 12 бит */
    if (c->ck[h] == key) return c->cv[h];
    int b = world_biome_cell(w, qx, cy, qz);
    c->ck[h] = key; c->cv[h] = (u8)b;
    return b;
}
static int biome_at(SurfCtx *c, int x, int y, int z) {
    int ax = x - 2, ay = y - 2, az = z - 2;
    int px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
    int mi = 0; double md = INFINITY;
    for (int i = 0; i < 8; i++) {
        int xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        double f3[3];
        fiddle3(c, c->w->biome_zoom_seed, xe ? px : px + 1, ye ? py : py + 1, ze ? pz : pz + 1, f3);
        double dx = xe ? fx : fx - 1.0, dy = ye ? fy : fy - 1.0, dz = ze ? fz : fz - 1.0;
        double d = (dz + f3[2]) * (dz + f3[2]) + (dy + f3[1]) * (dy + f3[1]) + (dx + f3[0]) * (dx + f3[0]);
        if (md > d) { mi = i; md = d; }
    }
    return cell_biome(c, (mi & 4) == 0 ? px : px + 1, (mi & 2) == 0 ? py : py + 1, (mi & 1) == 0 ? pz : pz + 1);
}

/* ---- температура биома (Biome.getTemperature без кэша: чистая функция) ---- */
static float frozen_modify(const SurfWorld *S, int x, int z, float base) {
    if (S->newf) {
        float f0 = (float)simplex2_d(&S->f_noise[0], x * 0.05 * 1.0, z * 0.05 * 1.0);
        float f1 = (float)simplex2_d(&S->f_noise[1], x * 0.05 * 0.5, z * 0.05 * 0.5);
        float f2 = (float)simplex2_d(&S->f_noise[2], x * 0.05 * 0.25, z * 0.05 * 0.25);
        float v = 0.0f;
        v += 0.14285715f * f0; v += 0.2857143f * f1; v += 0.5714286f * f2;
        double large = (double)(v * 7.0f);
        double edge = (double)(float)simplex2_d(&S->info_noise, x * 0.2, z * 0.2);
        double ice = large + edge;
        if (ice < 0.3) {
            double small = (double)(float)simplex2_d(&S->info_noise, x * 0.09, z * 0.09);
            if (small < 0.8) return 0.2f;
        }
        return base;
    }
    /* PerlinSimplexNoise: октавы 0 (info) и -2,-1,0 (frozen): factor = 1, 1/2, 1/4; valueFactor = 1/7, 2/7, 4/7 */
    {
        double vf = 1.0 / (pow(2.0, 3) - 1.0), factor = 1.0, value = 0.0;
        double fx = x * 0.05, fz = z * 0.05;
        for (int i = 0; i < 3; i++) { value += simplex2_d(&S->f_noise[i], fx * factor + 0.0, fz * factor + 0.0) * vf; factor /= 2.0; vf *= 2.0; }
        double large = value * 7.0;
        double edge = simplex2_d(&S->info_noise, x * 0.2 + 0.0, z * 0.2 + 0.0) * 1.0;
        double ice = large + edge;
        if (ice < 0.3) {
            double small = simplex2_d(&S->info_noise, x * 0.09 + 0.0, z * 0.09 + 0.0) * 1.0;
            if (small < 0.8) return 0.2f;
        }
        return base;
    }
}
static float biome_temperature(const SurfWorld *S, int b, int x, int y, int z) {
    float t = S->btemp[b];
    if (S->bfrozen[b]) t = frozen_modify(S, x, z, t);
    int snow = S->sea + 17;
    if (y > snow) {
        float v;
        if (S->newf) v = (float)simplex2_d(&S->t_noise, (double)((float)x / 8.0f), (double)((float)z / 8.0f)) * 8.0f;
        else v = (float)(simplex2_d(&S->t_noise, (double)((float)x / 8.0f) + 0.0, (double)((float)z / 8.0f) + 0.0) * 1.0 * 8.0);
        return t - (v + (float)y - (float)snow) * 0.05f / 40.0f;
    }
    return t;
}
/* Biome.getTemperature(pos, seaLevel) по id биома libmcgen — для стадии FEATURES (freeze_top_layer и др., поток W12) */
float surface_biome_temperature(const McWorld *w, int b, int x, int y, int z) { return biome_temperature((const SurfWorld *)w->surface, b, x, y, z); }

/* ---- состояние блока ---- */
static inline int is_air(const SurfCtx *c, int st) { return c->w->g->state_cls[st] & SC_AIR; }
static inline int is_fluid(const SurfCtx *c, int st) { return c->w->g->state_cls[st] & SC_FLUID; }
static inline int get_block(const SurfCtx *c, int col, int y) {
    if (y < c->minY || y > c->maxY) return c->w->g->st_air;
    return c->blk[((size_t)(y - c->minY) * 16 + (col >> 4)) * 16 + (col & 15)];
}

/* Heightmap.update(WORLD_SURFACE_WG): поставили состояние st в y */
static inline int is_carved(const SurfCtx *c, int col, int y);
static void hm_update(SurfCtx *c, int col, int y, int st) {
    int first = c->first[col];
    if (y <= first - 2) return;
    if (!is_air(c, st)) { if (y >= first) c->first[col] = y + 1; }
    else if (first - 1 == y) {
        for (int yy = y - 1; yy >= c->minY; yy--) if (!is_air(c, get_block(c, col, yy))) { c->first[col] = yy + 1; return; }
        c->first[col] = c->minY;
    }
}
/* BlockColumn.setBlock */
static void set_block(SurfCtx *c, int col, int y, int st) {
    if (y < c->minY || y > c->maxY) return;
    c->blk[((size_t)(y - c->minY) * 16 + (col >> 4)) * 16 + (col & 15)] = (uint16_t)st;
    hm_update(c, col, y, st);
    if (is_fluid(c, st) && c->marks) ppmarks_add(c->marks, (y - c->minY) >> 4, col & 15, y & 15, col >> 4);
}

/* ---- контекст правил ---- */
static void update_xz(SurfCtx *c, int bx, int bz, int gx, int gz) {
    const SurfWorld *S = c->S;
    c->xz_stamp++; c->y_stamp++;
    c->bx = bx; c->bz = bz; c->gradx = gx; c->gradz = gz;
    c->minsl_ok = 0; c->sec_ok = 0;
    c->interior = ((bx & 15) >= 2 && (bx & 15) <= 13 && (bz & 15) >= 2 && (bz & 15) <= 13);   /* все 8 угловых клеток зума — в этом чанке */
    double nv = S->newf ? (double)ns_get(S->surface.ns, bx, 0.0, bz) : old_normal_get(S->surface.on, bx, 0.0, bz);
    Rnd r = pos_at(&c->w->pos_terrain, bx, 0, bz);
    c->sdepth = jm_d2i(nv * 2.75 + 3.0 + rnd_next_double(&r) * 0.25);
}
static void update_y(SurfCtx *c, int above, int below, int wh, int y) {
    c->y_stamp++; c->biome_ok = 0; c->by = y; c->wh = wh; c->below = below; c->above = above;
}
static double surface_secondary(SurfCtx *c) {
    if (!c->sec_ok) {
        const SurfWorld *S = c->S;
        c->sec = S->newf ? (double)ns_get(S->secondary.ns, c->bx, 0.0, c->bz) : old_normal_get(S->secondary.on, c->bx, 0.0, c->bz);
        c->sec_ok = 1;
    }
    return c->sec;
}
static int ctx_biome(SurfCtx *c) {
    if (!c->biome_ok) { c->biome = biome_at(c, c->bx, c->by, c->bz); c->biome_ok = 1; }
    return c->biome;
}
static inline double lerp_d(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
static int min_surface_level(SurfCtx *c) {
    if (c->minsl_ok) return c->minsl;
    const SurfWorld *S = c->S; const McWorld *w = c->w;
    int pl;
    if (S->newf) {
        if (c->top_mode) {
            Vol v = { 1, 1, 1, c->bx, 0, c->bz, 1, 1, 1 };
            float out[1]; s_volume(c->x, S->prelim, out, &v);
            pl = jm_floor_f(out[0]);
        } else {
            if (!c->prelim_ok) {
                Vol v = { 16, 1, 16, c->cx * 16, 0, c->cz * 16, 1, 1, 1 };
                s_volume(c->x, S->prelim, c->prelim, &v);
                c->prelim_ok = 1;
            }
            pl = jm_floor_f(c->prelim[(c->bz & 15) * 16 + (c->bx & 15)]);
        }
    } else {
        if (!c->nch_ok || c->nch_cx != c->cx || c->nch_cz != c->cz) {
            /* NoiseChunk чанка: плоские кэши; preliminarySurfaceLevel сам вычисляет точки соседних ячеек */
            nchunk_begin(c->nch, c->cx * 16, c->cz * 16, S->ctx_min, S->ctx_h);
            c->nch_cx = c->cx; c->nch_cz = c->cz; c->nch_ok = 1;
        }
        int cellx = c->bx >> 4, cellz = c->bz >> 4;
        int p0 = nchunk_prelim_surface(c->nch, cellx << 4, cellz << 4), p1 = nchunk_prelim_surface(c->nch, (cellx + 1) << 4, cellz << 4);
        int p2 = nchunk_prelim_surface(c->nch, cellx << 4, (cellz + 1) << 4), p3 = nchunk_prelim_surface(c->nch, (cellx + 1) << 4, (cellz + 1) << 4);
        double a1 = (double)((c->bx & 15) / 16.0f), a2 = (double)((c->bz & 15) / 16.0f);
        pl = jm_floor_d(lerp_d(a2, lerp_d(a1, p0, p1), lerp_d(a1, p2, p3)));
    }
    (void)w;
    c->minsl = pl + c->sdepth - 8; c->minsl_ok = 1;
    return c->minsl;
}

static int band(SurfCtx *c, int x, int y, int z) {
    const SurfWorld *S = c->S;
    int off;
    if (S->newf) off = jm_roundf(ns_get(S->band_off.ns, x, 0.0, z) * 4.0f);
    else { double a = old_normal_get(S->band_off.on, x, 0.0, z) * 4.0; double f = floor(a); off = jm_d2i((a - f >= 0.5) ? f + 1.0 : f); }
    return S->bands[(y + off + 192) % 192];
}
static double noise_slot_value(SurfCtx *c, int s) {
    const SurfWorld *S = c->S; const NSlot *n = &S->slot[s];
    u64 stamp = n->is3d ? c->y_stamp : c->xz_stamp;
    if (c->nst[s] == stamp) return c->nval[s];
    double v;
    double y = n->is3d ? (double)c->by : 0.0;
    v = S->newf ? (double)ns_get(n->ns, c->bx, y, c->bz) : old_normal_get(n->on, c->bx, y, c->bz);
    c->nst[s] = stamp; c->nval[s] = v;
    return v;
}

static int cond_compute(SurfCtx *c, const CNode *n) {
    const SurfWorld *S = c->S;
    switch (n->type) {
    case CN_BIOME: {
        if (c->qset_ok && c->interior && !c->top_mode) {
            /* блок внутри чанка: результат зума — одна из угловых клеток py, py+1 (y зажат) собственного чанка */
            int qmin = c->minY >> 2, py = ((c->by - 2) >> 2) - qmin;
            int q0 = py < 0 ? 0 : (py >= c->qn ? c->qn - 1 : py), q1 = py + 1 < 0 ? 0 : (py + 1 >= c->qn ? c->qn - 1 : py + 1);
            const u8 *a = &c->qset[q0 * 32], *b = &c->qset[q1 * 32];
            int hit = 0;
            for (int i = 0; i < 32 && !hit; i++) hit = n->mask[i] & (a[i] | b[i]);
            if (!hit) return 0;
        }
        int b = ctx_biome(c); return (n->mask[b >> 3] >> (b & 7)) & 1;
    }
    case CN_NOISE: { double v = noise_slot_value(c, n->slot); return v >= n->lo && v <= n->hi; }
    case CN_VGRAD: {
        int y = c->by;
        if (y <= n->t_below) return 1;
        if (y >= n->f_above) return 0;
        double t = ((double)y - (double)n->t_below) / ((double)n->f_above - (double)n->t_below);   /* Mth.map(y, a, b, 1.0, 0.0) */
        double prob = lerp_d(t, 1.0, 0.0);
        Rnd r = pos_at(&n->rf, c->bx, y, c->bz);
        return (double)rnd_next_float(&r) < prob;
    }
    case CN_Y: return c->by + (n->add_stone ? c->above : 0) >= n->anchor + c->sdepth * n->mult;
    case CN_WATER:
        return c->wh == INT_MIN || c->by + (n->add_stone ? c->above : 0) >= c->wh + n->offset + c->sdepth * n->mult;
    case CN_TEMP: {
        float t = biome_temperature(S, ctx_biome(c), c->bx, c->by, c->bz);
        return !(t >= 0.15f);
    }
    case CN_STEEP: return c->gradx <= -4 || c->gradz >= 4;
    case CN_HOLE: return c->sdepth <= 0;
    case CN_STONE: {
        int depth = n->ceiling ? c->below : c->above;
        int sd = n->add_surface ? c->sdepth : 0;
        int sec = 0;
        if (n->range != 0) {
            double t = (surface_secondary(c) - (-1.0)) / (1.0 - (-1.0));
            sec = jm_d2i(lerp_d(t, 0.0, (double)n->range));
        }
        return depth <= 1 + n->offset + sd + sec;
    }
    }
    return 0;
}
static int cond_test(SurfCtx *c, const CNode *n) {
    if (n->type == CN_NOT) return !cond_test(c, n->inner);
    if (n->type == CN_ABOVE) return c->by >= min_surface_level(c);
    if (n->type == CN_NOISE) return cond_compute(c, n);      /* кэш — в слоте шума */
    u64 st = n->lazy == 1 ? c->xz_stamp : c->y_stamp;
    if (c->cst[n->id] == st) return c->cval[n->id];
    int v = cond_compute(c, n);
    c->cst[n->id] = st; c->cval[n->id] = (u8)v;
    return v;
}
static int eval_rule(SurfCtx *c, const RNode *r) {
    switch (r->type) {
    case RN_BLOCK: return r->state;
    case RN_SEQ:
        for (int i = 0; i < r->nkid; i++) { int s = eval_rule(c, r->kid[i]); if (s >= 0) return s; }
        return -1;
    case RN_COND: return cond_test(c, r->cond) ? eval_rule(c, r->then) : -1;
    case RN_BANDS: return band(c, c->bx, c->by, c->bz);
    case RN_VEIN: return (c->cur == r->vein[0] || c->cur == r->vein[1] || c->cur == r->vein[2]) ? c->cur : -1;
    }
    return -1;
}

/* ======================================================================= расширения */
static inline int block_of(const McGen *g, int st) { return g->state_block[st]; }

static void eroded_extension(SurfCtx *c, int col, int bx, int bz, int height) {
    const SurfWorld *S = c->S; const McGen *g = c->w->g;
    double pb;
    if (S->newf) pb = jm_min(fabs((double)ns_get(S->bad_surface.ns, bx, 0.0, bz) * 8.25),
                              (double)(ns_get(S->bad_pillar.ns, bx * 0.2, 0.0, bz * 0.2) * 15.0f));
    else pb = jm_min(fabs(old_normal_get(S->bad_surface.on, bx, 0.0, bz) * 8.25), old_normal_get(S->bad_pillar.on, bx * 0.2, 0.0, bz * 0.2) * 15.0);
    if (!(pb <= 0.0)) {
        double pf = S->newf ? fabs((double)ns_get(S->bad_roof.ns, bx * 0.75, 0.0, bz * 0.75) * 1.5)
                            : fabs(old_normal_get(S->bad_roof.on, bx * 0.75, 0.0, bz * 0.75) * 1.5);
        double top = 64.0 + jm_min(pb * pb * 2.5, ceil(pf * 50.0) + 24.0);
        int start = jm_floor_d(top);
        if (height <= start) {
            for (int y = start; y >= c->minY; y--) {
                int st = get_block(c, col, y);
                if (block_of(g, st) == g->state_block[S->defb]) break;
                if (gen_is_block(g, st, g->blk_water)) return;
            }
            for (int y = start; y >= c->minY && is_air(c, get_block(c, col, y)); y--) set_block(c, col, y, S->defb);
        }
    }
}

static void frozen_extension(SurfCtx *c, int col, int bx, int bz, int height, int sbio) {
    const SurfWorld *S = c->S; const McGen *g = c->w->g;
    int sea = S->sea;
    double iceberg;
    if (S->newf) iceberg = jm_min(fabs((double)ns_get(S->ice_surface.ns, bx, 0.0, bz) * 8.25),
                                  (double)(ns_get(S->ice_pillar.ns, bx * 1.28, 0.0, bz * 1.28) * 15.0f));
    else iceberg = jm_min(fabs(old_normal_get(S->ice_surface.on, bx, 0.0, bz) * 8.25), old_normal_get(S->ice_pillar.on, bx * 1.28, 0.0, bz * 1.28) * 15.0);
    if (iceberg <= 1.8) return;
    double roof = S->newf ? fabs((double)ns_get(S->ice_roof.ns, bx * 1.17, 0.0, bz * 1.17) * 1.5)
                          : fabs(old_normal_get(S->ice_roof.on, bx * 1.17, 0.0, bz * 1.17) * 1.5);
    double top = jm_min(iceberg * iceberg * 1.2, ceil(roof * 40.0) + 14.0);
    if (biome_temperature(S, sbio, bx, sea, bz) > 0.1f) top -= 2.0;
    double ext_bottom, ext_top;
    if (S->newf) {
        if (top <= 2.0) return;
        ext_bottom = sea - top - 7.0; top += sea; ext_top = top;
    } else {
        if (top > 2.0) { ext_bottom = sea - top - 7.0; top += sea; } else { top = 0.0; ext_bottom = 0.0; }
        ext_top = top;
    }
    Rnd r = pos_at(&c->w->pos_terrain, bx, 0, bz);
    int max_snow = 2 + rnd_next_int_bound(&r, 4);
    int min_snow_h = sea + 18 + rnd_next_int_bound(&r, 10);
    int snow_depth = 0;
    int ytop = jm_d2i(ext_top) + 1; if (height > ytop) ytop = height;
    int msl = min_surface_level(c);
    for (int y = ytop; y >= msl; y--) {
        int st = get_block(c, col, y);
        int cond;
        cond = (is_air(c, st) && y < jm_d2i(ext_top) && rnd_next_double(&r) > 0.01) ||
               (gen_is_block(g, st, g->blk_water) && y > jm_d2i(ext_bottom) && y < sea && (S->newf || ext_bottom != 0.0) && rnd_next_double(&r) > 0.15);
        if (cond) {
            /* 26.4: BlockColumn.setBlock не пишет в вырезанные карверами блоки; счётчик снега растёт в любом случае */
            int skip = c->cmask && is_carved(c, col, y);
            if (snow_depth <= max_snow && y > min_snow_h) { if (!skip) set_block(c, col, y, S->st_snow); snow_depth++; }
            else if (!skip) set_block(c, col, y, S->st_packed_ice);
        }
    }
}

/* ======================================================================= столбцы чанка */
static int load_chunk(SurfCtx *c, int cx, int cz, uint16_t *blocks, const uint8_t *bio, PPMarks *marks) {
    const McWorld *w = c->w;
    c->cx = cx; c->cz = cz; c->blk = blocks; c->bio = bio; c->marks = marks;
    c->minY = w->min_y; c->maxY = w->min_y + w->height - 1;
    c->prelim_ok = 0; c->top_mode = 0;
    c->qset_ok = 0;
    if (bio) {   /* множества биомов собственного чанка по квартам y */
        memset(c->qset, 0, (size_t)c->qn * 32);
        for (int qy = 0; qy < c->qn; qy++) for (int i = 0; i < 16; i++) { int b = bio[qy * 16 + i]; c->qset[qy * 32 + (b >> 3)] |= (u8)(1u << (b & 7)); }
        c->qset_ok = 1;
    }
    for (int col = 0; col < 256; col++) {
        int f = c->minY;
        for (int y = w->height - 1; y >= 0; y--) if (!is_air(c, blocks[(size_t)y * 256 + col])) { f = c->minY + y + 1; break; }
        c->first[col] = c->first0[col] = f;
    }
    return 0;
}

/* маска карверов чанка (26.4): [(x*16+z)*mh + (y - mmin)] */
static inline int is_carved(const SurfCtx *c, int col, int y) {
    if (!c->cmask || y < c->cmmin || y >= c->cmmin + c->cmh) return 0;
    return c->cmask[(size_t)((col & 15) * 16 + (col >> 4)) * (size_t)c->cmh + (size_t)(y - c->cmmin)];
}
/* запись блока мимо BlockColumn.setBlock (ChunkTerrainBuilder.setBlock): пометка пост-обработки — по явному признаку */
static void put_block(SurfCtx *c, int col, int y, int st, int mark) {
    if (y < c->minY || y > c->maxY) return;
    c->blk[((size_t)(y - c->minY) * 16 + (col >> 4)) * 16 + (col & 15)] = (uint16_t)st;
    hm_update(c, col, y, st);
    if (mark && c->marks) ppmarks_add(c->marks, (y - c->minY) >> 4, col & 15, y & 15, col >> 4);
}

int surface_apply_chunk(McWorld *w, SurfCtx *c, int cx, int cz, uint16_t *blocks, const uint8_t *chunk_biomes, PPMarks *marks,
                        char *err, size_t errlen) {
    return surface_apply_chunk_ex(w, c, cx, cz, blocks, chunk_biomes, marks, NULL, 0, err, errlen);
}

int surface_apply_chunk_ex(McWorld *w, SurfCtx *c, int cx, int cz, uint16_t *blocks, const uint8_t *chunk_biomes, PPMarks *marks,
                           TerrainCtx *t, int carve, char *err, size_t errlen) {
    SurfWorld *S = w->surface;
    if (!S || !S->root) return MCGEN_OK;
    const McGen *g = w->g;
    int old = !S->newf;
    load_chunk(c, cx, cz, blocks, chunk_biomes, marks);
    if (c->x) sctx_reset_caches(c->x);
    int legacy = w->ns->legacy_random;
    int minY = c->minY, maxY = c->maxY;
    /* 26.4: карвинг внутри прохода (ChunkTerrainBuilder.fillColumn) */
    uint8_t *cmask = NULL;
    c->cmask = NULL;
    if (carve && S->v264 && t) {
        int mmin = 0, mh = 0;
        char e[256] = {0};
        cmask = carvers_mask_alloc(w, cx, cz, &mmin, &mh, e, sizeof e);
        if (cmask) { c->cmask = cmask; c->cmmin = mmin; c->cmh = mh; }
    }
    const u8 *uncarv = cmask ? gen_block_tag(g, "minecraft:uncarvable") : NULL;
    int blk_dirt = cmask ? g->state_block[gen_state_id(g, "minecraft:dirt")] : -1;
    for (int a = 0; a < 256; a++) {
        /* 26.3: x снаружи, z внутри; 26.4 (ChunkTerrainBuilder.fillChunk): z снаружи, x внутри — порядок важен только для 26.3 (карта высот) */
        int x = S->v264 ? a & 15 : a >> 4, z = S->v264 ? a >> 4 : a & 15;
        int bx = cx * 16 + x, bz = cz * 16 + z, col = z * 16 + x;
        int start = c->first[col];
        int sbio = biome_at(c, bx, (old && legacy) ? 0 : start, bz);
        if (sbio == S->b_eroded) eroded_extension(c, col, bx, bz, start);
        int height = c->first[col];
        const int *hf = S->v264 ? c->first0 : c->first;    /* 26.4: градиенты по высотам шума до расширений */
        int gx = hf[z * 16 + (x + 1 < 15 ? x + 1 : 15)] - hf[z * 16 + (x - 1 > 0 ? x - 1 : 0)];
        int gz = hf[(z + 1 < 15 ? z + 1 : 15) * 16 + x] - hf[(z - 1 > 0 ? z - 1 : 0) * 16 + x];
        update_xz(c, bx, bz, gx, gz);
        if (S->root) {
            int stone_above = 0, water_h = INT_MIN, next_ceil = INT_MAX, carved_top = 0;
            for (int y = height; y >= minY; y--) {
                int cur = get_block(c, col, y);
                int carved = cmask ? is_carved(c, col, y) : 0;
                if (is_air(c, cur)) { stone_above = 0; water_h = INT_MIN; carved_top &= carved; }
                else if (is_fluid(c, cur)) { if (water_h == INT_MIN) water_h = y + 1; carved_top &= carved; }
                else {
                    if (next_ceil >= y) {
                        /* 26.3: просмотр до minY − 1 (void_air) => потолок minY; 26.4: NoiseColumn.getCeilingBelowIndex — без «пустоты» под миром */
                        next_ceil = S->v264 ? minY + WAY_BELOW_MIN_Y : WAY_BELOW_MIN_Y;
                        for (int ly = y - 1; ly >= (S->v264 ? minY : minY - 1); ly--) {
                            int ns = get_block(c, col, ly);
                            if (is_air(c, ns) || is_fluid(c, ns)) { next_ceil = ly + 1; break; }
                        }
                    }
                    stone_above++;
                    int stone_below = y - next_ceil + 1;
                    update_y(c, stone_above, stone_below, water_h, y);
                    if (old ? cur == S->defb : (y >= minY && y <= maxY)) {
                        c->cur = cur;
                        int st = eval_rule(c, S->root);
                        if (carved && (st < 0 || !uncarv[g->state_block[st]])) {
                            int sched = 0;
                            int sub = terrain_carve_substance(t, bx, y, bz, &sched);
                            if (sub >= 0) {      /* AIR — блок вырезан (в игре не записывается), жидкость — записывается; −1 (SOLID) — обычный путь */
                                carved_top |= stone_above == 1;
                                put_block(c, col, y, sub, sched && is_fluid(c, sub));
                                continue;
                            }
                        }
                        if (carved_top) {
                            if (st >= 0 && g->state_block[st] == blk_dirt) { update_y(c, 1, stone_below, water_h, y); st = eval_rule(c, S->root); }
                            carved_top = 0;
                        }
                        if (st >= 0) set_block(c, col, y, st);
                    }
                }
            }
        }
        if (sbio == S->b_frozen || sbio == S->b_deep_frozen) frozen_extension(c, col, bx, bz, start, sbio);
    }
    free(cmask); c->cmask = NULL;
    (void)err; (void)errlen;
    return MCGEN_OK;
}

/* ======================================================================= topMaterial для карверов (SurfaceSystem/MaterialSystem.topMaterial) */
static _Thread_local SurfCtx *tl_ctx;
static _Thread_local u64 tl_uid;

/* firstAvailable столбца (карта WORLD_SURFACE_WG по текущим блокам) */
static int col_first(const SurfCtx *c, int col) {
    const McWorld *w = c->w;
    for (int y = w->height - 1; y >= 0; y--) if (!is_air(c, c->blk[(size_t)y * 256 + col])) return c->minY + y + 1;
    return c->minY;
}

/* Материал верхнего слоя в позиции (x, y, z) — АБСОЛЮТНЫЕ координаты блока; blocks — [y][z][x] чанка (cx, cz), в котором лежит позиция.
 * Возвращает id состояния или −1 (Optional.empty). Градиенты поверхности — по карте WORLD_SURFACE_WG чанка в момент вызова. */
int mcgen_surface_top_material(McWorld *w, TerrainCtx *t, int cx, int cz, const uint16_t *blocks, int x, int y, int z, int under_fluid) {
    (void)t;
    SurfWorld *S = w->surface;
    if (!S || !S->root) return -1;
    if (tl_uid != S->uid || !tl_ctx) {   /* контекст принадлежит миру: при смене мира старый не трогаем (его мир мог быть уже освобождён) */
        tl_ctx = surface_ctx_new(w); tl_uid = S->uid;
        mutex_lock(w->lock); pv_push(&S->tl, tl_ctx); mutex_unlock(w->lock);
    }
    SurfCtx *c = tl_ctx;
    c->cx = cx; c->cz = cz; c->blk = (uint16_t *)blocks; c->bio = NULL; c->marks = NULL;
    c->minY = w->min_y; c->maxY = w->min_y + w->height - 1;
    c->prelim_ok = 0; c->top_mode = 1;
    if (c->top_cx != cx || c->top_cz != cz || !c->top_init) { c->top_cx = cx; c->top_cz = cz; c->top_init = 1; c->top_last = -1; }
    int lx = x & 15, lz = z & 15;
    int fe = col_first(c, lz * 16 + (lx + 1 < 15 ? lx + 1 : 15)), fw = col_first(c, lz * 16 + (lx - 1 > 0 ? lx - 1 : 0));
    int fs = col_first(c, (lz + 1 < 15 ? lz + 1 : 15) * 16 + lx), fn = col_first(c, (lz - 1 > 0 ? lz - 1 : 0) * 16 + lx);
    if (c->x) sctx_reset_caches(c->x);
    update_xz(c, x, z, fe - fw, fs - fn);
    update_y(c, 1, 1, under_fluid ? y + 1 : INT_MIN, y);
    c->cur = get_block(c, lz * 16 + lx, y);
    int res = eval_rule(c, S->root);
    return res;
}

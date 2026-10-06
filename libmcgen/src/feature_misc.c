/* feature_misc.c — простые фичи и селекторы: simple_block, no_op, sequence, random_selector, simple_random_selector, random_boolean_selector,
 * weighted_random_selector. Остальные типы растительности (random_patch-цепочки, block_column, деревья…) — группы фич растительности/деревьев. */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== simple_block */
typedef struct SimpleCfg { BSProv *prov; int schedule_tick; } SimpleCfg;
static void *simple_parse(FParse *p, const Js *cfg) {
    SimpleCfg *s = fp_alloc(p, sizeof *s);
    s->prov = fp_bsprov(p, js_get(cfg, "to_place")); if (!s->prov) return NULL;
    s->schedule_tick = js_bool(js_get(cfg, "schedule_tick"), 0);
    return s;
}
static int simple_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SimpleCfg *s = cfg; const BsTab *bs = c->bs;
    int st = bsprov_optional(c, s->prov, x, y, z);
    if (st < 0) return 0;
    if (!block_can_survive(c, st, x, y, z)) return 0;
    if (bs_is_a(bs, st, "DoublePlantBlock")) {
        int above = fc_get(c, x, y + 1, z);
        if (!fc_is_air(c, above)) {
            /* 26.1/26.2: level.isEmptyBlock(origin.above()) — только воздух; 26.3+: и воздух, и «та же жидкость + canBeReplaced» (SimpleBlockFeature) */
            if (!c->g->newf) return 0;
            /* !Objects.equals(stateToPlace.getFluidState(), aboveState.getFluidState()) || !aboveState.canBeReplaced() */
            if (bs->fluid[st] != bs->fluid[above] || !(bs->flags[above] & BSF_REPLACEABLE)) return 0;
        }
        /* DoublePlantBlock.placeAt: нижняя и верхняя половины (waterlogged копируется из воды позиции) */
        int lo = bs_with(bs, st, "half", "lower"), hi = bs_with(bs, st, "half", "upper");
        if (lo < 0 || hi < 0) return 0;
        if (bs_has_prop(bs, lo, "waterlogged")) {
            int wl_lo = BS_FL_TYPE(bs->fluid[fc_get(c, x, y, z)]) == FL_WATER || BS_FL_TYPE(bs->fluid[fc_get(c, x, y, z)]) == FL_FLOWING_WATER;
            lo = bs_with(bs, lo, "waterlogged", wl_lo ? "true" : "false");
            int wl_hi = BS_FL_TYPE(bs->fluid[above]) == FL_WATER || BS_FL_TYPE(bs->fluid[above]) == FL_FLOWING_WATER;
            hi = bs_with(bs, hi, "waterlogged", wl_hi ? "true" : "false");
        }
        fc_set(c, x, y, z, lo, 2); fc_set(c, x, y + 1, z, hi, 2);
    } else if (bs_is_a(bs, st, "MossyCarpetBlock")) {
        extern int veg_mossy_carpet_place(FCtx *c, int x, int y, int z);      /* feature_veg.c (W10) */
        veg_mossy_carpet_place(c, x, y, z);
    } else fc_set(c, x, y, z, st, 2);
    return 1;
}

/* ====================================================================== селекторы */
typedef struct SelCfg {
    int n; Placed **pf; float *chance; int *w; int total;     /* списки */
    Placed *deflt;                                            /* random_selector: default */
    Placed *t, *f;                                            /* random_boolean_selector */
} SelCfg;

static Placed **parse_placed_list(FParse *p, const Js *v, int *n) {
    const Js *items[1]; const Js *const *arr; int cnt;
    if (js_is_arr(v)) { arr = (const Js *const *)v->items; cnt = v->n; } else { items[0] = v; arr = items; cnt = 1; }
    Placed **l = fp_alloc(p, sizeof(Placed *) * (size_t)(cnt ? cnt : 1));
    for (int i = 0; i < cnt; i++) { l[i] = fp_placed(p, arr[i]); if (!l[i]) return NULL; }
    *n = cnt; return l;
}
static void *seq_parse(FParse *p, const Js *cfg) {
    SelCfg *s = fp_alloc(p, sizeof *s);
    s->pf = parse_placed_list(p, js_get(cfg, "features"), &s->n); return s->pf ? s : NULL;
}
static int seq_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SelCfg *s = cfg;
    for (int i = 0; i < s->n; i++) if (!placed_place(c, s->pf[i], x, y, z, 0)) return 0;
    return 1;
}
static int simple_random_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SelCfg *s = cfg;
    int i = frnd_int_bound(c->rnd, s->n);
    return placed_place(c, s->pf[i], x, y, z, 0);
}
static void *random_sel_parse(FParse *p, const Js *cfg) {
    SelCfg *s = fp_alloc(p, sizeof *s);
    Js *l = js_get(cfg, "features");
    if (!js_is_arr(l)) { fp_fail(p, "random_selector: нет features"); return NULL; }
    s->n = l->n; s->pf = fp_alloc(p, sizeof(Placed *) * (size_t)(l->n ? l->n : 1)); s->chance = fp_alloc(p, sizeof(float) * (size_t)(l->n ? l->n : 1));
    for (int i = 0; i < l->n; i++) {
        s->pf[i] = fp_placed(p, js_get(l->items[i], "feature")); if (!s->pf[i]) return NULL;
        s->chance[i] = js_numf(js_get(l->items[i], "chance"), 0);
    }
    s->deflt = fp_placed(p, js_get(cfg, "default")); return s->deflt ? s : NULL;
}
static int random_sel_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SelCfg *s = cfg;
    for (int i = 0; i < s->n; i++) if (frnd_float(c->rnd) < s->chance[i]) return placed_place(c, s->pf[i], x, y, z, 0);
    return placed_place(c, s->deflt, x, y, z, 0);
}
static void *bool_sel_parse(FParse *p, const Js *cfg) {
    SelCfg *s = fp_alloc(p, sizeof *s);
    s->t = fp_placed(p, js_get(cfg, "feature_true")); s->f = fp_placed(p, js_get(cfg, "feature_false"));
    return s->t && s->f ? s : NULL;
}
static int bool_sel_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SelCfg *s = cfg;
    int r = frnd_bool(c->rnd);
    return placed_place(c, r ? s->t : s->f, x, y, z, 0);
}
static void *weighted_sel_parse(FParse *p, const Js *cfg) {
    SelCfg *s = fp_alloc(p, sizeof *s);
    Js *l = js_get(cfg, "features");
    if (!js_is_arr(l)) { fp_fail(p, "weighted_random_selector: нет features"); return NULL; }
    s->n = l->n; s->pf = fp_alloc(p, sizeof(Placed *) * (size_t)(l->n ? l->n : 1)); s->w = fp_alloc(p, sizeof(int) * (size_t)(l->n ? l->n : 1));
    for (int i = 0; i < l->n; i++) {
        s->pf[i] = fp_placed(p, js_get(l->items[i], "data")); if (!s->pf[i]) return NULL;
        s->w[i] = js_int(js_get(l->items[i], "weight"), 1); s->total += s->w[i];
    }
    return s;
}
static int weighted_sel_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SelCfg *s = cfg;
    if (s->total == 0) return 0;
    int sel = frnd_int_bound(c->rnd, s->total);
    for (int i = 0; i < s->n; i++) { sel -= s->w[i]; if (sel < 0) return placed_place(c, s->pf[i], x, y, z, 0); }
    return 0;
}
static void *noop_parse(FParse *p, const Js *cfg) { return fp_alloc(p, 16); }
static int noop_place(FCtx *c, const void *cfg, int x, int y, int z) { return 1; }

static const FeatType T_SIMPLE = { "minecraft:simple_block", simple_parse, simple_place };
static const FeatType T_SEQ = { "minecraft:sequence", seq_parse, seq_place };
static const FeatType T_SIMPLE_RANDOM = { "minecraft:simple_random_selector", seq_parse, simple_random_place };
static const FeatType T_RANDOM = { "minecraft:random_selector", random_sel_parse, random_sel_place };
static const FeatType T_BOOL = { "minecraft:random_boolean_selector", bool_sel_parse, bool_sel_place };
static const FeatType T_WEIGHTED = { "minecraft:weighted_random_selector", weighted_sel_parse, weighted_sel_place };
static const FeatType T_NOOP = { "minecraft:no_op", noop_parse, noop_place };
void feature_register_simple(void) {
    feature_register_type(&T_SIMPLE); feature_register_type(&T_SEQ); feature_register_type(&T_SIMPLE_RANDOM); feature_register_type(&T_RANDOM);
    feature_register_type(&T_BOOL); feature_register_type(&T_WEIGHTED); feature_register_type(&T_NOOP);
}

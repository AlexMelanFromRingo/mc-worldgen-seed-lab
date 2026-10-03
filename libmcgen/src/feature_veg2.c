/* feature_veg2.c — растительность версий 26.1/26.2 (старые типы фич, в 26.3+ заменены block_column/simple_block…):
 * kelp, seagrass, sea_pickle, nether_forest_vegetation, twisting_vines, weeping_vines. Подробно — docs/blender/features-veg.md. */
#include "feature_veg.h"
#include <stdio.h>
#include <stdlib.h>

static int is_block(const FCtx *c, int st, int blk) { return c->g->state_block[st] == blk; }

/* ====================================================================== kelp */
typedef struct KelpCfg { int water, kelp, plant; } KelpCfg;
static void *kelp_parse(FParse *p, const Js *cfg) {
    KelpCfg *k = fp_alloc(p, sizeof *k);
    k->water = bs_block_index(p->bs, "minecraft:water");
    int a = bs_block_index(p->bs, "minecraft:kelp"), b = bs_block_index(p->bs, "minecraft:kelp_plant");
    if (k->water < 0 || a < 0 || b < 0) { fp_fail(p, "kelp: нет блоков"); return NULL; }
    k->kelp = bs_default(p->bs, a); k->plant = bs_default(p->bs, b);
    return k;
}
static int kelp_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const KelpCfg *k = cfg; FRnd *r = c->rnd;
    int placed = 0;
    int y = fc_height(c, HM_OCEAN_FLOOR, ox, oz);
    if (is_block(c, fc_get(c, ox, y, oz), k->water)) {
        int height = 1 + frnd_int_bound(r, 10);
        for (int h = 0; h <= height; h++) {
            if (is_block(c, fc_get(c, ox, y, oz), k->water) && is_block(c, fc_get(c, ox, y + 1, oz), k->water) && block_can_survive(c, k->plant, ox, y, oz)) {
                if (h == height) {
                    char age[8]; snprintf(age, sizeof age, "%d", frnd_int_bound(r, 4) + 20);
                    fc_set(c, ox, y, oz, bs_with(c->bs, k->kelp, "age", age), 2); placed++;
                } else fc_set(c, ox, y, oz, k->plant, 2);
            } else if (h > 0) {
                int by = y - 1;
                if (block_can_survive(c, k->kelp, ox, by, oz) && !is_block(c, fc_get(c, ox, by - 1, oz), c->g->state_block[k->kelp])) {
                    char age[8]; snprintf(age, sizeof age, "%d", frnd_int_bound(r, 4) + 20);
                    fc_set(c, ox, by, oz, bs_with(c->bs, k->kelp, "age", age), 2); placed++;
                }
                break;
            }
            y++;
        }
    }
    return placed > 0;
}

/* ====================================================================== seagrass */
typedef struct SeagrassCfg { float prob; int water, seagrass, tall; } SeagrassCfg;
static void *seagrass_parse(FParse *p, const Js *cfg) {
    SeagrassCfg *s = fp_alloc(p, sizeof *s);
    s->prob = js_numf(js_get(cfg, "probability"), 0);
    s->water = bs_block_index(p->bs, "minecraft:water");
    int a = bs_block_index(p->bs, "minecraft:seagrass"), b = bs_block_index(p->bs, "minecraft:tall_seagrass");
    if (s->water < 0 || a < 0 || b < 0) { fp_fail(p, "seagrass: нет блоков"); return NULL; }
    s->seagrass = bs_default(p->bs, a); s->tall = bs_default(p->bs, b);
    return s;
}
static int seagrass_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const SeagrassCfg *s = cfg; FRnd *r = c->rnd;
    int x = frnd_int_bound(r, 8) - frnd_int_bound(r, 8);
    int z = frnd_int_bound(r, 8) - frnd_int_bound(r, 8);
    int px = ox + x, pz = oz + z;
    int y = fc_height(c, HM_OCEAN_FLOOR, px, pz);
    if (!is_block(c, fc_get(c, px, y, pz), s->water)) return 0;
    int tall = frnd_double(r) < (double)s->prob;
    int st = tall ? bs_with(c->bs, s->tall, "half", "lower") : s->seagrass;
    if (!block_can_survive(c, st, px, y, pz)) return 0;
    if (tall) {
        if (is_block(c, fc_get(c, px, y + 1, pz), s->water)) {
            fc_set(c, px, y, pz, st, 2); fc_set(c, px, y + 1, pz, bs_with(c->bs, st, "half", "upper"), 2);
        }
    } else fc_set(c, px, y, pz, st, 2);
    return 1;
}

/* ====================================================================== sea_pickle */
typedef struct PickleCfg { IntProv *count; int water, pickle; } PickleCfg;
static void *pickle_parse(FParse *p, const Js *cfg) {
    PickleCfg *s = fp_alloc(p, sizeof *s);
    s->count = fp_intprov(p, js_get(cfg, "count")); if (!s->count) return NULL;
    s->water = bs_block_index(p->bs, "minecraft:water");
    int a = bs_block_index(p->bs, "minecraft:sea_pickle");
    if (s->water < 0 || a < 0) { fp_fail(p, "sea_pickle: нет блоков"); return NULL; }
    s->pickle = bs_default(p->bs, a);
    return s;
}
static int pickle_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const PickleCfg *s = cfg; FRnd *r = c->rnd;
    int placed = 0, count = intprov_sample(s->count, r);
    for (int i = 0; i < count; i++) {
        int x = frnd_int_bound(r, 8) - frnd_int_bound(r, 8);
        int z = frnd_int_bound(r, 8) - frnd_int_bound(r, 8);
        int px = ox + x, pz = oz + z;
        int y = fc_height(c, HM_OCEAN_FLOOR, px, pz);
        char n[4]; snprintf(n, sizeof n, "%d", frnd_int_bound(r, 4) + 1);
        int st = bs_with(c->bs, s->pickle, "pickles", n);
        if (st >= 0 && is_block(c, fc_get(c, px, y, pz), s->water) && block_can_survive(c, st, px, y, pz)) { fc_set(c, px, y, pz, st, 2); placed++; }
    }
    return placed > 0;
}

/* ====================================================================== nether_forest_vegetation */
typedef struct NetherVegCfg { BSProv *prov; int sw, sh; const u8 *nylium; } NetherVegCfg;
static void *nvf_parse(FParse *p, const Js *cfg) {
    NetherVegCfg *s = fp_alloc(p, sizeof *s);
    s->prov = fp_bsprov(p, js_get(cfg, "state_provider")); if (!s->prov) return NULL;
    s->sw = js_int(js_get(cfg, "spread_width"), 0); s->sh = js_int(js_get(cfg, "spread_height"), 0);
    s->nylium = gen_block_tag(p->g, "minecraft:nylium");
    return s;
}
static int nvf_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const NetherVegCfg *s = cfg; FRnd *r = c->rnd;
    if (!veg_in_tag(c, s->nylium, fc_get(c, ox, oy - 1, oz))) return 0;
    if (!(oy >= c->min_y + 1 && oy + 1 <= c->min_y + c->height - 1)) return 0;
    int placed = 0;
    for (int i = 0; i < s->sw * s->sw; i++) {
        int dx = frnd_int_bound(r, s->sw) - frnd_int_bound(r, s->sw);
        int dy = frnd_int_bound(r, s->sh) - frnd_int_bound(r, s->sh);
        int dz = frnd_int_bound(r, s->sw) - frnd_int_bound(r, s->sw);
        int x = ox + dx, y = oy + dy, z = oz + dz;
        int st = bsprov_state(c, s->prov, x, y, z);
        if (fc_is_empty_block(c, x, y, z) && y > c->min_y && block_can_survive(c, st, x, y, z)) { fc_set(c, x, y, z, st, 2); placed++; }
    }
    return placed > 0;
}

/* ====================================================================== twisting_vines */
typedef struct VinesCfg2 { int sw, sh, maxh; int head, plant, netherrack, nylium, wart; } VinesCfg2;
static void *tv_parse(FParse *p, const Js *cfg) {
    VinesCfg2 *s = fp_alloc(p, sizeof *s);
    s->sw = js_int(js_get(cfg, "spread_width"), 0); s->sh = js_int(js_get(cfg, "spread_height"), 0); s->maxh = js_int(js_get(cfg, "max_height"), 1);
    int a = bs_block_index(p->bs, "minecraft:twisting_vines"), b = bs_block_index(p->bs, "minecraft:twisting_vines_plant");
    s->netherrack = bs_block_index(p->bs, "minecraft:netherrack"); s->nylium = bs_block_index(p->bs, "minecraft:warped_nylium"); s->wart = bs_block_index(p->bs, "minecraft:warped_wart_block");
    if (a < 0 || b < 0) { fp_fail(p, "twisting_vines: нет блоков"); return NULL; }
    s->head = bs_default(p->bs, a); s->plant = bs_default(p->bs, b);
    return s;
}
static int tv_invalid(FCtx *c, const VinesCfg2 *s, int x, int y, int z) {
    if (!fc_is_empty_block(c, x, y, z)) return 1;
    int below = c->g->state_block[fc_get(c, x, y - 1, z)];
    return !(below == s->netherrack || below == s->nylium || below == s->wart);
}
static int tv_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const VinesCfg2 *s = cfg; FRnd *r = c->rnd;
    if (tv_invalid(c, s, ox, oy, oz)) return 0;
    for (int i = 0; i < s->sw * s->sw; i++) {
        int x = ox + mth_next_int(r, -s->sw, s->sw), y = oy + mth_next_int(r, -s->sh, s->sh), z = oz + mth_next_int(r, -s->sw, s->sw);
        /* findFirstAirBlockAboveGround */
        int ok = 1;
        do { y--; if (fc_outside(c, y)) { ok = 0; break; } } while (fc_is_air(c, fc_get(c, x, y, z)));
        if (!ok) continue;
        y++;
        if (tv_invalid(c, s, x, y, z)) continue;
        int vh = mth_next_int(r, 1, s->maxh);
        if (frnd_int_bound(r, 6) == 0) vh *= 2;
        if (frnd_int_bound(r, 5) == 0) vh = 1;
        for (int h = 1; h <= vh; h++) {
            if (fc_is_empty_block(c, x, y, z)) {
                if (h == vh || !fc_is_empty_block(c, x, y + 1, z)) {
                    char age[8]; snprintf(age, sizeof age, "%d", mth_next_int(r, 17, 25));
                    fc_set(c, x, y, z, bs_with(c->bs, s->head, "age", age), 2);
                    break;
                }
                fc_set(c, x, y, z, s->plant, 2);
            }
            y++;
        }
    }
    return 1;
}

/* ====================================================================== weeping_vines */
typedef struct WeepCfg { int head, plant, netherrack, wart; } WeepCfg;
static void *wv_parse(FParse *p, const Js *cfg) {
    WeepCfg *s = fp_alloc(p, sizeof *s);
    int a = bs_block_index(p->bs, "minecraft:weeping_vines"), b = bs_block_index(p->bs, "minecraft:weeping_vines_plant");
    s->netherrack = bs_block_index(p->bs, "minecraft:netherrack"); s->wart = bs_block_index(p->bs, "minecraft:nether_wart_block");
    if (a < 0 || b < 0 || s->wart < 0) { fp_fail(p, "weeping_vines: нет блоков"); return NULL; }
    s->head = bs_default(p->bs, a); s->plant = bs_default(p->bs, b);
    return s;
}
static int wv_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const WeepCfg *s = cfg; FRnd *r = c->rnd;
    if (!fc_is_empty_block(c, ox, oy, oz)) return 0;
    int above = c->g->state_block[fc_get(c, ox, oy + 1, oz)];
    if (above != s->netherrack && above != s->wart) return 0;
    /* placeRoofNetherWart */
    int wart_st = bs_default(c->bs, s->wart);
    fc_set(c, ox, oy, oz, wart_st, 2);
    for (int i = 0; i < 200; i++) {
        int dx = frnd_int_bound(r, 6) - frnd_int_bound(r, 6);
        int dy = frnd_int_bound(r, 2) - frnd_int_bound(r, 5);
        int dz = frnd_int_bound(r, 6) - frnd_int_bound(r, 6);
        int x = ox + dx, y = oy + dy, z = oz + dz;
        if (fc_is_empty_block(c, x, y, z)) {
            int nb = 0;
            for (int d = 0; d < 6; d++) {
                int b = c->g->state_block[fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d])];
                if (b == s->netherrack || b == s->wart) nb++;
                if (nb > 1) break;
            }
            if (nb == 1) fc_set(c, x, y, z, wart_st, 2);
        }
    }
    /* placeRoofWeepingVines */
    for (int i = 0; i < 100; i++) {
        int dx = frnd_int_bound(r, 8) - frnd_int_bound(r, 8);
        int dy = frnd_int_bound(r, 2) - frnd_int_bound(r, 7);
        int dz = frnd_int_bound(r, 8) - frnd_int_bound(r, 8);
        int x = ox + dx, y = oy + dy, z = oz + dz;
        if (!fc_is_empty_block(c, x, y, z)) continue;
        int ab = c->g->state_block[fc_get(c, x, y + 1, z)];
        if (ab != s->netherrack && ab != s->wart) continue;
        int vh = mth_next_int(r, 1, 8);
        if (frnd_int_bound(r, 6) == 0) vh *= 2;
        if (frnd_int_bound(r, 5) == 0) vh = 1;
        for (int h = 0; h <= vh; h++) {
            if (fc_is_empty_block(c, x, y, z)) {
                if (h == vh || !fc_is_empty_block(c, x, y - 1, z)) {
                    char age[8]; snprintf(age, sizeof age, "%d", mth_next_int(r, 17, 25));
                    fc_set(c, x, y, z, bs_with(c->bs, s->head, "age", age), 2);
                    break;
                }
                fc_set(c, x, y, z, s->plant, 2);
            }
            y--;
        }
    }
    return 1;
}

static const FeatType T_KELP = { "minecraft:kelp", kelp_parse, kelp_place };
static const FeatType T_SEAGRASS = { "minecraft:seagrass", seagrass_parse, seagrass_place };
static const FeatType T_PICKLE = { "minecraft:sea_pickle", pickle_parse, pickle_place };
static const FeatType T_NVF = { "minecraft:nether_forest_vegetation", nvf_parse, nvf_place };
static const FeatType T_TV = { "minecraft:twisting_vines", tv_parse, tv_place };
static const FeatType T_WV = { "minecraft:weeping_vines", wv_parse, wv_place };
void feature_register_veg2(void) {
    feature_register_type(&T_KELP); feature_register_type(&T_SEAGRASS); feature_register_type(&T_PICKLE);
    feature_register_type(&T_NVF); feature_register_type(&T_TV); feature_register_type(&T_WV);
}

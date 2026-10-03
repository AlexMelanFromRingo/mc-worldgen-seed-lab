/* feature_drip.c — сталактиты/сталагмиты: speleothem («pointed_dripstone», «sulfur_spike»), speleothem_cluster («dripstone_cluster»), large_dripstone.
 * 26.1 — прежние классы PointedDripstoneFeature/DripstoneClusterFeature (имена типов pointed_dripstone/dripstone_cluster, другие ключи config: jk2) и
 * LargeDripstoneFeature без replaceable_blocks и без ограничения смещения ветра; 26.2 — уже speleothem*, как в 26.3 (ключи — в обёртке config). */
#include "feature_misc.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------- утилиты (SpeleothemUtils) */
typedef struct SpelEnv { int base_blk, pointed_blk, water_blk, lava_blk; const u8 *replaceable; } SpelEnv;

static inline int sp_is_empty_or_water(const FCtx *c, int st, const SpelEnv *e) { return fc_is_air(c, st) || fc_is_block(c, st, e->water_blk); }
static inline int sp_is_empty_water_lava(const FCtx *c, int st, const SpelEnv *e) { return fc_is_air(c, st) || fc_is_block(c, st, e->water_blk) || fc_is_block(c, st, e->lava_blk); }
static inline int sp_is_base(const FCtx *c, int st, const SpelEnv *e) { return fc_is_block(c, st, e->base_blk) || fc_in_set(c, st, e->replaceable); }
static inline int sp_is_water_at(const FCtx *c, int x, int y, int z) { int f = c->bs->fluid[fc_get(c, x, y, z)]; int t = BS_FL_TYPE(f); return t == FL_WATER || t == FL_FLOWING_WATER; }

/* Column.scan: результат — границы столбца */
typedef struct Col { int has_floor, floor, has_ceil, ceil; } Col;
typedef int (*StPred)(const FCtx *, int, const SpelEnv *);
static int col_scan_dir(const FCtx *c, int x, int y0, int z, int range, StPred inside, StPred edge, const SpelEnv *e, int dir, int *out) {
    int y = y0;
    for (int i = 1; i < range && inside(c, fc_get(c, x, y, z), e); i++) y += dir;
    if (edge(c, fc_get(c, x, y, z), e)) { *out = y; return 1; }
    return 0;
}
static int col_scan(const FCtx *c, int x, int y, int z, int range, StPred inside, StPred edge, const SpelEnv *e, Col *col) {
    if (!inside(c, fc_get(c, x, y, z), e)) return 0;
    col->has_ceil = col_scan_dir(c, x, y, z, range, inside, edge, e, 1, &col->ceil);
    col->has_floor = col_scan_dir(c, x, y, z, range, inside, edge, e, -1, &col->floor);
    return 1;
}
static int p_neither_empty_nor_water(const FCtx *c, int st, const SpelEnv *e) { return !fc_is_air(c, st) && !fc_is_block(c, st, e->water_blk); }
static int p_base_or_lava(const FCtx *c, int st, const SpelEnv *e) { return sp_is_base(c, st, e) || fc_is_block(c, st, e->lava_blk); }

static double speleothem_height(double xz_dist, double radius, double scale, double bluntness) {
    if (xz_dist < bluntness) xz_dist = bluntness;
    double r = xz_dist / radius * 0.384;
    double part1 = 0.75 * pow(r, 1.3333333333333333);
    double part2 = pow(r, 0.6666666666666666);
    double part3 = 0.3333333333333333 * log(r);
    double h = scale * (part1 - part2 - part3);
    h = h > 0.0 ? h : 0.0;      /* Math.max(h, 0.0) */
    return h / 0.384 * radius;
}

static int sp_place_base_if_possible(FCtx *c, const SpelEnv *e, int base_state, int x, int y, int z) {
    if (fc_in_set(c, fc_get(c, x, y, z), e->replaceable)) { fc_set(c, x, y, z, base_state, 2); return 1; }
    return 0;
}

typedef struct SpelStates { int base_state, pointed_state; } SpelStates;
/* SpeleothemUtils.growSpeleothem + buildBaseToTipColumn */
static void sp_grow(FCtx *c, const SpelEnv *e, const SpelStates *ss, int x, int y, int z, int tip_dir /* DIR_UP/DIR_DOWN */, int length, int merged_tip) {
    int ox = x - DIR_DX[tip_dir], oy = y - DIR_DY[tip_dir], oz = z - DIR_DZ[tip_dir];
    if (!sp_is_base(c, fc_get(c, ox, oy, oz), e)) return;
    const BsTab *bs = c->bs;
    const char *dirv = tip_dir == DIR_UP ? "up" : "down";
    int base = bs_with(bs, ss->pointed_state, "vertical_direction", dirv);
    static const char *TH[5] = { "base", "middle", "frustum", "tip", "tip_merge" };
    int n = 0;
    int seq[512];
    if (length > 400) length = 400;
    if (length >= 3) { seq[n++] = 0; for (int i = 0; i < length - 3; i++) seq[n++] = 1; }
    if (length >= 2) seq[n++] = 2;
    if (length >= 1) seq[n++] = merged_tip ? 4 : 3;
    for (int i = 0; i < n; i++) {
        int st = bs_with(bs, base, "thickness", TH[seq[i]]);
        if (bs_has_prop(bs, st, "waterlogged")) st = bs_with(bs, st, "waterlogged", sp_is_water_at(c, x, y, z) ? "true" : "false");
        fc_set(c, x, y, z, st, 2);
        x += DIR_DX[tip_dir]; y += DIR_DY[tip_dir]; z += DIR_DZ[tip_dir];
    }
}

/* 26.1/26.2 (DripstoneUtils): блоки закодированы — dripstone_block / pointed_dripstone, замена по тегу dripstone_replaceable_blocks */
static int sp_parse_env(FParse *p, const Js *cfg, SpelEnv *e, SpelStates *ss) {
    if (js_get(cfg, "base_block")) {
        ss->base_state = bs_from_json(p->bs, js_get(cfg, "base_block")); ss->pointed_state = bs_from_json(p->bs, js_get(cfg, "pointed_block"));
    } else {
        int b = bs_block_index(p->bs, "minecraft:dripstone_block"), q = bs_block_index(p->bs, "minecraft:pointed_dripstone");
        ss->base_state = b < 0 ? -1 : bs_default(p->bs, b); ss->pointed_state = q < 0 ? -1 : bs_default(p->bs, q);
    }
    if (ss->base_state < 0 || ss->pointed_state < 0) { fp_fail(p, "speleothem: плохие base_block/pointed_block"); return 0; }
    e->base_blk = p->g->state_block[ss->base_state]; e->pointed_blk = p->g->state_block[ss->pointed_state];
    e->water_blk = bs_block_index(p->bs, "minecraft:water"); e->lava_blk = bs_block_index(p->bs, "minecraft:lava");
    if (js_get(cfg, "replaceable_blocks")) e->replaceable = fp_blockset(p, js_get(cfg, "replaceable_blocks"));
    else e->replaceable = gen_block_tag(p->g, "minecraft:dripstone_replaceable_blocks");
    if (!e->replaceable) { fp_fail(p, "speleothem: replaceable_blocks"); return 0; }
    return 1;
}
/* ключ нового формата или старого (26.1/26.2) */
static const Js *jk2(const Js *o, const char *a, const char *b) { const Js *v = js_get(o, a); return v ? v : js_get(o, b); }
static float cfgf(const Js *o, const char *k, float d) { const Js *v = js_get(o, k); return v ? js_numf(v, d) : d; }

/* ====================================================================== speleothem */
typedef struct SpelCfg { SpelEnv e; SpelStates ss; float taller, spread_dir, spread_r2, spread_r3; } SpelCfg;
static void *spel_parse(FParse *p, const Js *cfg) {
    SpelCfg *s = fp_alloc(p, sizeof *s);
    if (!sp_parse_env(p, cfg, &s->e, &s->ss)) return NULL;
    { const Js *t = jk2(cfg, "chance_of_taller_generation", "chance_of_taller_dripstone"); s->taller = t ? js_numf(t, 0.2F) : 0.2F; }
    s->spread_dir = cfgf(cfg, "chance_of_directional_spread", 0.7F);
    s->spread_r2 = cfgf(cfg, "chance_of_spread_radius2", 0.5F); s->spread_r3 = cfgf(cfg, "chance_of_spread_radius3", 0.5F);
    return s;
}
static int spel_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SpelCfg *s = cfg; FRnd *r = c->rnd; const SpelEnv *e = &s->e;
    int above = sp_is_base(c, fc_get(c, x, y + 1, z), e), below = sp_is_base(c, fc_get(c, x, y - 1, z), e);
    int tip;
    if (above && below) tip = frnd_bool(r) ? DIR_DOWN : DIR_UP;
    else if (above) tip = DIR_DOWN;
    else if (below) tip = DIR_UP;
    else return 0;
    int rx = x - DIR_DX[tip], ry = y - DIR_DY[tip], rz = z - DIR_DZ[tip];
    /* createPatchOfBaseBlocks */
    sp_place_base_if_possible(c, e, s->ss.base_state, rx, ry, rz);
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    for (int i = 0; i < 4; i++) {
        int d = HD[i];
        if (!(frnd_float(r) > s->spread_dir)) {
            int x1 = rx + DIR_DX[d], y1 = ry, z1 = rz + DIR_DZ[d];
            sp_place_base_if_possible(c, e, s->ss.base_state, x1, y1, z1);
            if (!(frnd_float(r) > s->spread_r2)) {
                int d2 = frnd_int_bound(r, 6);
                int x2 = x1 + DIR_DX[d2], y2 = y1 + DIR_DY[d2], z2 = z1 + DIR_DZ[d2];
                sp_place_base_if_possible(c, e, s->ss.base_state, x2, y2, z2);
                if (!(frnd_float(r) > s->spread_r3)) {
                    int d3 = frnd_int_bound(r, 6);
                    sp_place_base_if_possible(c, e, s->ss.base_state, x2 + DIR_DX[d3], y2 + DIR_DY[d3], z2 + DIR_DZ[d3]);
                }
            }
        }
    }
    int height = (frnd_float(r) < s->taller && sp_is_empty_or_water(c, fc_get(c, x + DIR_DX[tip], y + DIR_DY[tip], z + DIR_DZ[tip]), e)) ? 2 : 1;
    sp_grow(c, e, &s->ss, x, y, z, tip, height, 0);
    return 1;
}

/* ====================================================================== speleothem_cluster */
typedef struct ClusterCfg {
    SpelEnv e; SpelStates ss; int search_range; IntProv *height, *radius; int max_diff, height_dev; IntProv *layer; FloatProv *density, *wetness;
    float chance_max_dist; int max_dist_edge, max_dist_center;
} ClusterCfg;
static void *cluster_parse(FParse *p, const Js *cfg) {
    ClusterCfg *s = fp_alloc(p, sizeof *s);
    if (!sp_parse_env(p, cfg, &s->e, &s->ss)) return NULL;
    s->search_range = js_int(js_get(cfg, "floor_to_ceiling_search_range"), 12);
    s->height = fp_intprov(p, js_get(cfg, "height")); s->radius = fp_intprov(p, js_get(cfg, "radius"));
    s->max_diff = js_int(js_get(cfg, "max_stalagmite_stalactite_height_diff"), 0); s->height_dev = js_int(js_get(cfg, "height_deviation"), 1);
    s->layer = fp_intprov(p, jk2(cfg, "speleothem_block_layer_thickness", "dripstone_block_layer_thickness"));
    s->density = fp_floatprov(p, js_get(cfg, "density")); s->wetness = fp_floatprov(p, js_get(cfg, "wetness"));
    { const Js *v = jk2(cfg, "chance_of_speleothem_at_max_distance_from_center", "chance_of_dripstone_column_at_max_distance_from_center"); s->chance_max_dist = v ? js_numf(v, 0.0F) : 0.0F; }
    { const Js *v = jk2(cfg, "max_distance_from_edge_affecting_chance_of_speleothem", "max_distance_from_edge_affecting_chance_of_dripstone_column"); s->max_dist_edge = js_int(v, 1); }
    s->max_dist_center = js_int(js_get(cfg, "max_distance_from_center_affecting_height_bias"), 1);
    return s->height && s->radius && s->layer && s->density && s->wetness ? s : NULL;
}
static int cl_is_lava(const FCtx *c, const SpelEnv *e, int x, int y, int z) { return fc_is_block(c, fc_get(c, x, y, z), e->lava_blk); }
static int cl_adjacent_to_water_ok(const FCtx *c, int x, int y, int z, const u8 *base_stone) {
    int st = fc_get(c, x, y, z);
    if (fc_in_set(c, st, base_stone)) return 1;
    int t = BS_FL_TYPE(c->bs->fluid[st]);
    return t == FL_WATER || t == FL_FLOWING_WATER;
}
static int cl_can_place_pool(const FCtx *c, const ClusterCfg *s, int x, int y, int z, const u8 *base_stone) {
    const SpelEnv *e = &s->e;
    int st = fc_get(c, x, y, z);
    if (fc_is_block(c, st, e->water_blk) || fc_is_block(c, st, e->base_blk) || fc_is_block(c, st, e->pointed_blk)) return 0;
    int ft = BS_FL_TYPE(c->bs->fluid[fc_get(c, x, y + 1, z)]);
    if (ft == FL_WATER || ft == FL_FLOWING_WATER) return 0;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    for (int i = 0; i < 4; i++) if (!cl_adjacent_to_water_ok(c, x + DIR_DX[HD[i]], y, z + DIR_DZ[HD[i]], base_stone)) return 0;
    return cl_adjacent_to_water_ok(c, x, y - 1, z, base_stone);
}
static void cl_replace_with_base(FCtx *c, const ClusterCfg *s, int x, int y, int z, int max_count, int dir) {
    for (int i = 0; i < max_count; i++) {
        if (!sp_place_base_if_possible(c, &s->e, s->ss.base_state, x, y, z)) return;
        y += DIR_DY[dir];
    }
}
static int cl_height(const ClusterCfg *s, FRnd *r, int dx, int dz, float density, int max_height) {
    if (frnd_float(r) > density) return 0;
    int dist = iabs_(dx) + iabs_(dz);
    /* Mth.clampedMap(double): (dist − 0.0)/(maxDist − 0.0) → clampedLerp(t, maxHeight/2.0, 0.0) */
    double t = ((double)dist - 0.0) / ((double)s->max_dist_center - 0.0);
    double lo = (double)max_height / 2.0, hi = 0.0;
    double v = t < 0.0 ? lo : (t > 1.0 ? hi : lo + t * (hi - lo));
    float mean = (float)v;
    /* randomBetweenBiased → ClampedNormalFloat.sample(random, mean, deviation, 0, maxHeight) */
    float n = mean + (float)frnd_gauss(r) * (float)s->height_dev;
    float mn = 0.0F, mx = (float)max_height;
    float cl = n < mn ? mn : (n < mx ? n : mx);
    return (int)cl;
}
static void cl_column(FCtx *c, const ClusterCfg *s, int x, int y, int z, int dx, int dz, float chance_water, double chance_spel, int cluster_height, float density,
                      const u8 *base_stone) {
    FRnd *r = c->rnd; const SpelEnv *e = &s->e;
    Col base;
    if (!col_scan(c, x, y, z, s->search_range, sp_is_empty_or_water, p_neither_empty_nor_water, e, &base)) return;
    if (!base.has_ceil && !base.has_floor) return;
    int want_pool = frnd_float(r) < chance_water;
    Col col = base;
    if (want_pool && base.has_floor && cl_can_place_pool(c, s, x, base.floor, z, base_stone)) {
        col.floor = base.floor - 1; col.has_floor = 1;
        fc_set(c, x, base.floor, z, bs_default(c->bs, e->water_blk), 2);
    }
    int stalactite_h;
    int want_stalactite = frnd_double(r) < chance_spel;
    if (base.has_ceil && want_stalactite && !cl_is_lava(c, e, x, base.ceil, z)) {
        int thick = intprov_sample(s->layer, r);
        cl_replace_with_base(c, s, x, base.ceil, z, thick, DIR_UP);
        int maxh = col.has_floor ? imin_(cluster_height, base.ceil - col.floor) : cluster_height;
        stalactite_h = cl_height(s, r, dx, dz, density, maxh);
    } else stalactite_h = 0;
    int stalagmite_h;
    int want_stalagmite = frnd_double(r) < chance_spel;
    if (col.has_floor && want_stalagmite && !cl_is_lava(c, e, x, col.floor, z)) {
        int thick = intprov_sample(s->layer, r);
        cl_replace_with_base(c, s, x, col.floor, z, thick, DIR_DOWN);
        if (base.has_ceil) stalagmite_h = imax_(0, stalactite_h + (frnd_int_bound(r, 2 * s->max_diff + 1) - s->max_diff));
        else stalagmite_h = cl_height(s, r, dx, dz, density, cluster_height);
    } else stalagmite_h = 0;
    int act_stalactite, act_stalagmite;
    if (base.has_ceil && col.has_floor && base.ceil - stalactite_h <= col.floor + stalagmite_h) {
        int fy = col.floor, cy = base.ceil;
        int lowest_bottom = imax_(cy - stalactite_h, fy + 1);
        int highest_top = imin_(fy + stalagmite_h, cy - 1);
        int bottom = frnd_int_bound(r, highest_top + 1 - lowest_bottom + 1) + lowest_bottom;
        act_stalactite = cy - bottom;
        act_stalagmite = (bottom - 1) - fy;
    } else { act_stalactite = stalactite_h; act_stalagmite = stalagmite_h; }
    int merge = frnd_bool(r) && act_stalactite > 0 && act_stalagmite > 0 && col.has_floor && base.has_ceil && act_stalactite + act_stalagmite == base.ceil - col.floor - 1;
    if (base.has_ceil) sp_grow(c, e, &s->ss, x, base.ceil - 1, z, DIR_DOWN, act_stalactite, merge);
    if (col.has_floor) sp_grow(c, e, &s->ss, x, col.floor + 1, z, DIR_UP, act_stalagmite, merge);
}
static int cluster_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const ClusterCfg *s = cfg; FRnd *r = c->rnd; const SpelEnv *e = &s->e;
    if (!sp_is_empty_or_water(c, fc_get(c, ox, oy, oz), e)) return 0;
    const u8 *base_stone = gen_block_tag(c->g, "minecraft:base_stone_overworld");
    int height = intprov_sample(s->height, r);
    float wetness = floatprov_sample(s->wetness, r);
    float density = floatprov_sample(s->density, r);
    int xr = intprov_sample(s->radius, r), zr = intprov_sample(s->radius, r);
    for (int dx = -xr; dx <= xr; dx++) for (int dz = -zr; dz <= zr; dz++) {
        int xd = xr - iabs_(dx), zd = zr - iabs_(dz), dist = imin_(xd, zd);
        /* Mth.clampedMap(float): (dist − 0)/(maxEdge − 0) → clampedLerp(t, chanceMax, 1.0) */
        float t = ((float)dist - 0.0F) / ((float)s->max_dist_edge - 0.0F);
        float cm = s->chance_max_dist;
        float ch = t < 0.0F ? cm : (t > 1.0F ? 1.0F : cm + t * (1.0F - cm));
        cl_column(c, s, ox + dx, oy, oz + dz, dx, dz, wetness, (double)ch, height, density, base_stone);
    }
    return 1;
}

/* ====================================================================== large_dripstone */
typedef struct LargeCfg {
    SpelEnv e; int search_range; IntProv *radius; FloatProv *height_scale, *stalactite_blunt, *stalagmite_blunt, *wind_speed; float max_ratio; int min_r_wind; float min_b_wind;
    int dripstone_state; int old;
} LargeCfg;
static void *large_parse(FParse *p, const Js *cfg) {
    LargeCfg *s = fp_alloc(p, sizeof *s);
    s->e.replaceable = js_get(cfg, "replaceable_blocks") ? fp_blockset(p, js_get(cfg, "replaceable_blocks")) : gen_block_tag(p->g, "minecraft:dripstone_replaceable_blocks");
    if (!s->e.replaceable) { fp_fail(p, "large_dripstone: replaceable_blocks"); return NULL; }
    s->old = p->version == V26_1;         /* смещение ветра без ограничения — только 26.1 (26.2 уже с maxOffset) */
    s->e.base_blk = bs_block_index(p->bs, "minecraft:dripstone_block"); s->e.water_blk = bs_block_index(p->bs, "minecraft:water"); s->e.lava_blk = bs_block_index(p->bs, "minecraft:lava");
    s->dripstone_state = bs_default(p->bs, s->e.base_blk);
    s->search_range = js_get(cfg, "floor_to_ceiling_search_range") ? js_int(js_get(cfg, "floor_to_ceiling_search_range"), 30) : 30;
    s->radius = fp_intprov(p, js_get(cfg, "column_radius")); s->height_scale = fp_floatprov(p, js_get(cfg, "height_scale"));
    s->max_ratio = cfgf(cfg, "max_column_radius_to_cave_height_ratio", 0.33F);
    s->stalactite_blunt = fp_floatprov(p, js_get(cfg, "stalactite_bluntness")); s->stalagmite_blunt = fp_floatprov(p, js_get(cfg, "stalagmite_bluntness"));
    s->wind_speed = fp_floatprov(p, js_get(cfg, "wind_speed")); s->min_r_wind = js_int(js_get(cfg, "min_radius_for_wind"), 0); s->min_b_wind = cfgf(cfg, "min_bluntness_for_wind", 0.0F);
    return s->radius && s->height_scale && s->stalactite_blunt && s->stalagmite_blunt && s->wind_speed ? s : NULL;
}
typedef struct LDrip { int rx, ry, rz, up, radius; double blunt, scale; } LDrip;
typedef struct Wind { int has, origin_y, max_off; double wx, wz; } Wind;
static void wind_offset(const Wind *w, int *x, int *y, int *z) {
    if (!w->has) return;
    int dy = w->origin_y - *y;
    double tx = w->wx * (double)dy, tz = w->wz * (double)dy;
    int dx = (int)floor(tx), dz = (int)floor(tz);
    dx = dx < -w->max_off ? -w->max_off : (dx > w->max_off ? w->max_off : dx);
    dz = dz < -w->max_off ? -w->max_off : (dz > w->max_off ? w->max_off : dz);
    *x += dx; *z += dz;
}
static int ld_height_at(const LDrip *d, float cr) { return (int)speleothem_height((double)cr, (double)d->radius, d->scale, d->blunt); }
static int ld_empty_water_lava(const FCtx *c, const SpelEnv *e, int x, int y, int z) { return sp_is_empty_water_lava(c, fc_get(c, x, y, z), e); }
static int ld_circle_embedded(const FCtx *c, const SpelEnv *e, int cx, int cy, int cz, int r) {
    if (ld_empty_water_lava(c, e, cx, cy, cz)) return 0;
    float inc = 6.0F / (float)r;
    for (float a = 0.0F; a < (float)(3.141592653589793 * 2); a += inc) {
        int dx = (int)(fm_cos((double)a) * (float)r), dz = (int)(fm_sin((double)a) * (float)r);
        if (ld_empty_water_lava(c, e, cx + dx, cy, cz + dz)) return 0;
    }
    return 1;
}
static int ld_move_back(FCtx *c, const SpelEnv *e, LDrip *d, const Wind *w) {
    while (d->radius > 1) {
        int ny = d->ry;
        int tries = imin_(10, ld_height_at(d, 0.0F));
        for (int i = 0; i < tries; i++) {
            if (fc_is_block(c, fc_get(c, d->rx, ny, d->rz), e->lava_blk)) return 0;
            int wx = d->rx, wy = ny, wz = d->rz; wind_offset(w, &wx, &wy, &wz);
            if (ld_circle_embedded(c, e, wx, wy, wz, d->radius)) { d->ry = ny; return 1; }
            ny += d->up ? -1 : 1;
        }
        d->radius /= 2;
    }
    return 0;
}
static void ld_place_blocks(FCtx *c, const LargeCfg *s, const LDrip *d, const Wind *w) {
    FRnd *r = c->rnd; const SpelEnv *e = &s->e;
    const u8 *base_stone = gen_block_tag(c->g, "minecraft:base_stone_overworld");
    for (int dx = -d->radius; dx <= d->radius; dx++) for (int dz = -d->radius; dz <= d->radius; dz++) {
        float cr = (float)sqrt((double)(float)(dx * dx + dz * dz));
        if (cr > (float)d->radius) continue;
        int height = ld_height_at(d, cr);
        if (height <= 0) continue;
        if ((double)frnd_float(r) < 0.2) height = (int)((float)height * (frnd_float(r) * (1.0F - 0.8F) + 0.8F));
        int px = d->rx + dx, py = d->ry, pz = d->rz + dz, out = 0;
        int max_y = d->up ? fc_height(c, HM_WORLD_SURFACE_WG, px, pz) : 0x7fffffff;
        for (int i = 0; i < height && py < max_y; i++) {
            int wx = px, wy = py, wz = pz; wind_offset(w, &wx, &wy, &wz);
            if (ld_empty_water_lava(c, e, wx, wy, wz)) { out = 1; fc_set(c, wx, wy, wz, s->dripstone_state, 2); }
            else if (out && fc_in_set(c, fc_get(c, wx, wy, wz), base_stone)) break;
            py += d->up ? 1 : -1;
        }
    }
}
static int large_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const LargeCfg *s = cfg; FRnd *r = c->rnd; const SpelEnv *e = &s->e;
    if (!sp_is_empty_or_water(c, fc_get(c, ox, oy, oz), e)) return 0;
    Col col;
    if (!col_scan(c, ox, oy, oz, s->search_range, sp_is_empty_or_water, p_base_or_lava, e, &col)) return 0;
    if (!col.has_floor || !col.has_ceil) return 0;
    int h = col.ceil - col.floor - 1;
    if (h < 4) return 0;
    int max_by_h = (int)((float)h * s->max_ratio);
    int rmin = intprov_min(s->radius), rmax = intprov_max(s->radius);
    int max_r = imin_(imax_(max_by_h, rmin), rmax);          /* Mth.clamp(int) */
    int radius = frnd_int_bound(r, max_r - rmin + 1) + rmin;
    LDrip st, sg;
    st = (LDrip){ ox, col.ceil - 1, oz, 0, radius, 0, 0 };
    st.blunt = (double)floatprov_sample(s->stalactite_blunt, r); st.scale = (double)floatprov_sample(s->height_scale, r);
    sg = (LDrip){ ox, col.floor + 1, oz, 1, radius, 0, 0 };
    sg.blunt = (double)floatprov_sample(s->stalagmite_blunt, r); sg.scale = (double)floatprov_sample(s->height_scale, r);
    Wind w; memset(&w, 0, sizeof w);
    if (st.radius >= s->min_r_wind && st.blunt >= (double)s->min_b_wind && sg.radius >= s->min_r_wind && sg.blunt >= (double)s->min_b_wind) {
        w.has = 1; w.origin_y = oy; w.max_off = s->old ? 0x3fffffff : 16 - radius;      /* 26.1: смещение ветром не ограничено */
        float speed = floatprov_sample(s->wind_speed, r);
        float dir = frnd_float(r) * (3.1415927F - 0.0F) + 0.0F;      /* Mth.randomBetween(random, 0, (float)Math.PI) */
        w.wx = (double)(fm_cos((double)dir) * speed); w.wz = (double)(fm_sin((double)dir) * speed);
    }
    int st_ok = ld_move_back(c, e, &st, &w);
    int sg_ok = ld_move_back(c, e, &sg, &w);
    if (st_ok) ld_place_blocks(c, s, &st, &w);
    if (sg_ok) ld_place_blocks(c, s, &sg, &w);
    return 1;
}

static const FeatType T_SPEL = { "minecraft:speleothem", spel_parse, spel_place };
static const FeatType T_CLUSTER = { "minecraft:speleothem_cluster", cluster_parse, cluster_place };
static const FeatType T_LARGE = { "minecraft:large_dripstone", large_parse, large_place };
/* 26.1/26.2: те же алгоритмы под прежними именами типов */
static const FeatType T_POINTED_OLD = { "minecraft:pointed_dripstone", spel_parse, spel_place };
static const FeatType T_CLUSTER_OLD = { "minecraft:dripstone_cluster", cluster_parse, cluster_place };
void feature_register_drip(void) {
    feature_register_type(&T_SPEL); feature_register_type(&T_CLUSTER); feature_register_type(&T_LARGE);
    feature_register_type(&T_POINTED_OLD); feature_register_type(&T_CLUSTER_OLD);
}

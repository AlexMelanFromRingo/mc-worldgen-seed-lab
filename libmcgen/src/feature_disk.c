/* feature_disk.c — «блоб-подобные» фичи: disk (DiskFeature), block_blob (BlockBlobFeature), spring_feature (SpringFeature), lake (LakeFeature). */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== disk */
typedef struct DiskCfg { BSProv *prov; BPred *target; IntProv *radius; int half_height; } DiskCfg;
static void *disk_parse(FParse *p, const Js *cfg) {
    DiskCfg *d = fp_alloc(p, sizeof *d);
    d->prov = fp_bsprov(p, js_get(cfg, "state_provider")); if (!d->prov) return NULL;
    d->target = fp_bpred(p, js_get(cfg, "target")); if (!d->target) return NULL;
    d->radius = fp_intprov(p, js_get(cfg, "radius")); if (!d->radius) return NULL;
    d->half_height = js_int(js_get(cfg, "half_height"), 0);
    return d;
}
static int disk_column(FCtx *c, const DiskCfg *d, int top, int bottom, int x, int z) {
    int placed_any = 0, placed_above = 0;
    for (int y = top; y > bottom; y--) {
        if (bpred_test(c, d->target, x, y, z)) {
            int st = bsprov_optional(c, d->prov, x, y, z);
            if (st >= 0) {
                fc_set(c, x, y, z, st, 2);
                if (!placed_above) fc_mark_above(c, x, y, z);
                placed_any = 1; placed_above = 1;
            }
        } else placed_above = 0;
    }
    return placed_any;
}
static int disk_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const DiskCfg *d = cfg;
    int placed = 0, top = oy + d->half_height, bottom = oy - d->half_height - 1;
    int r = intprov_sample(d->radius, c->rnd);
    /* BlockPos.betweenClosed(origin.offset(-r,0,-r), origin.offset(r,0,r)): x быстрее всего, затем z (y — одна позиция) */
    for (int z = oz - r; z <= oz + r; z++) for (int x = ox - r; x <= ox + r; x++) {
        int xd = x - ox, zd = z - oz;
        if (xd * xd + zd * zd <= r * r) placed |= disk_column(c, d, top, bottom, x, z);
    }
    return placed;
}

/* ====================================================================== block_blob */
typedef struct BlobCfg { int state; BPred *can_place_on; } BlobCfg;
static void *blob_parse(FParse *p, const Js *cfg) {
    BlobCfg *b = fp_alloc(p, sizeof *b);
    b->state = bs_from_json(p->bs, js_get(cfg, "state")); if (b->state < 0) { fp_fail(p, "block_blob: плохое state"); return NULL; }
    b->can_place_on = fp_bpred(p, js_get(cfg, "can_place_on")); if (!b->can_place_on) return NULL;
    return b;
}
static int blob_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const BlobCfg *b = cfg; FRnd *r = c->rnd;
    while (oy > c->min_y + 3 && !bpred_test(c, b->can_place_on, ox, oy - 1, oz)) oy--;
    if (oy <= c->min_y + 3) return 0;
    for (int k = 0; k < 3; k++) {
        int xr = frnd_int_bound(r, 2), yr = frnd_int_bound(r, 2), zr = frnd_int_bound(r, 2);
        float tr = (float)(xr + yr + zr) * 0.333f + 0.5f;
        float tr2 = tr * tr;
        /* betweenClosed: x быстрее всего, затем y, затем z */
        for (int z = oz - zr; z <= oz + zr; z++) for (int y = oy - yr; y <= oy + yr; y++) for (int x = ox - xr; x <= ox + xr; x++) {
            double dx = x - ox, dy = y - oy, dz = z - oz;
            if (dx * dx + dy * dy + dz * dz <= (double)tr2) fc_set(c, x, y, z, b->state, 3);
        }
        int dx = -1 + frnd_int_bound(r, 2); int dy = -frnd_int_bound(r, 2); int dz = -1 + frnd_int_bound(r, 2);
        ox += dx; oy += dy; oz += dz;
    }
    return 1;
}

/* ====================================================================== spring_feature */
typedef struct SpringCfg { int state; int requires_below, rock_count, hole_count; u8 *valid; } SpringCfg;
static void *spring_parse(FParse *p, const Js *cfg) {
    SpringCfg *s = fp_alloc(p, sizeof *s);
    s->state = bs_fluid_block_from_json(p->bs, js_get(cfg, "state")); if (s->state < 0) { fp_fail(p, "spring_feature: плохое state"); return NULL; }
    s->requires_below = js_bool(js_get(cfg, "requires_block_below"), 1);
    s->rock_count = js_int(js_get(cfg, "rock_count"), 4); s->hole_count = js_int(js_get(cfg, "hole_count"), 1);
    s->valid = fp_blockset(p, js_get(cfg, "valid_blocks")); if (!s->valid) { fp_fail(p, "spring_feature: плохие valid_blocks"); return NULL; }
    return s;
}
static int spring_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const SpringCfg *s = cfg; const McGen *g = c->g;
    #define VALID(st) (s->valid[g->state_block[(st)]])
    if (!VALID(fc_get(c, x, y + 1, z))) return 0;
    if (s->requires_below && !VALID(fc_get(c, x, y - 1, z))) return 0;
    int cur = fc_get(c, x, y, z);
    if (!fc_is_air(c, cur) && !VALID(cur)) return 0;
    int rock = 0, hole = 0;
    static const int SIDE[5][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 } };   /* west, east, north, south, below */
    for (int i = 0; i < 5; i++) if (VALID(fc_get(c, x + SIDE[i][0], y + SIDE[i][1], z + SIDE[i][2]))) rock++;
    for (int i = 0; i < 5; i++) if (fc_is_air(c, fc_get(c, x + SIDE[i][0], y + SIDE[i][1], z + SIDE[i][2]))) hole++;
    if (rock == s->rock_count && hole == s->hole_count) { fc_set(c, x, y, z, s->state, 2); return 1; }
    #undef VALID
    return 0;
}

/* ====================================================================== lake */
typedef struct LakeCfg { BSProv *fluid, *barrier; BPred *can_place, *can_replace_fluid, *can_replace_barrier; } LakeCfg;
static void *lake_parse(FParse *p, const Js *cfg) {
    LakeCfg *l = fp_alloc(p, sizeof *l);
    l->fluid = fp_bsprov(p, js_get(cfg, "fluid")); l->barrier = fp_bsprov(p, js_get(cfg, "barrier"));
    if (!l->fluid || !l->barrier) return NULL;
    if (js_get(cfg, "can_place_feature")) {      /* 26.3+ */
        l->can_place = fp_bpred(p, js_get(cfg, "can_place_feature"));
        l->can_replace_fluid = fp_bpred(p, js_get(cfg, "can_replace_with_air_or_fluid"));
        l->can_replace_barrier = fp_bpred(p, js_get(cfg, "can_replace_with_barrier"));
        if (!l->can_place || !l->can_replace_fluid || !l->can_replace_barrier) return NULL;
    } else {                                     /* 26.1/26.2: условия закодированы */
        l->can_place = bpred_true(p);
        l->can_replace_fluid = NULL; l->can_replace_barrier = NULL;
    }
    return l;
}
static const u8 *tag_cached(FCtx *c, const char *name) { return gen_block_tag(c->g, name); }
static int lake_can_replace_fluid(FCtx *c, const LakeCfg *l, int x, int y, int z) {
    if (l->can_replace_fluid) return bpred_test(c, l->can_replace_fluid, x, y, z);
    return !tag_cached(c, "minecraft:features_cannot_replace")[c->g->state_block[fc_get(c, x, y, z)]];
}
static int lake_can_replace_barrier(FCtx *c, const LakeCfg *l, int x, int y, int z) {
    if (l->can_replace_barrier) return bpred_test(c, l->can_replace_barrier, x, y, z);
    return !tag_cached(c, "minecraft:lava_pool_stone_cannot_replace")[c->g->state_block[fc_get(c, x, y, z)]];
}
static int lake_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const LakeCfg *l = cfg; FRnd *r = c->rnd;
    if (oy <= c->min_y + 4) return 0;
    ox -= 8; oy -= 4; oz -= 8;
    u8 grid[2048]; memset(grid, 0, sizeof grid);
    int spots = frnd_int_bound(r, 4) + 4;
    for (int i = 0; i < spots; i++) {
        double xr = frnd_double(r) * 6.0 + 3.0, yr = frnd_double(r) * 4.0 + 2.0, zr = frnd_double(r) * 6.0 + 3.0;
        double xp = frnd_double(r) * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
        double yp = frnd_double(r) * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
        double zp = frnd_double(r) * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
        for (int xx = 1; xx < 15; xx++) for (int zz = 1; zz < 15; zz++) for (int yy = 1; yy < 7; yy++) {
            double xd = ((double)xx - xp) / (xr / 2.0), yd = ((double)yy - yp) / (yr / 2.0), zd = ((double)zz - zp) / (zr / 2.0);
            if (xd * xd + yd * yd + zd * zd < 1.0) grid[(xx * 16 + zz) * 8 + yy] = 1;
        }
    }
    int fluid = bsprov_state(c, l->fluid, ox, oy, oz);
    #define G(x, z, y) grid[(((x)) * 16 + (z)) * 8 + (y)]
    #define EDGE(xx, zz, yy) (!G(xx, zz, yy) && ((xx < 15 && G(xx + 1, zz, yy)) || (xx > 0 && G(xx - 1, zz, yy)) || (zz < 15 && G(xx, zz + 1, yy)) || \
                              (zz > 0 && G(xx, zz - 1, yy)) || (yy < 7 && G(xx, zz, yy + 1)) || (yy > 0 && G(xx, zz, yy - 1))))
    for (int xx = 0; xx < 16; xx++) for (int zz = 0; zz < 16; zz++) for (int yy = 0; yy < 8; yy++) {
        if (EDGE(xx, zz, yy)) {
            int st = fc_get(c, ox + xx, oy + yy, oz + zz);
            if (yy >= 4 && (c->bs->flags[st] & BSF_LIQUID)) return 0;
            if (yy < 4 && !(c->bs->flags[st] & BSF_SOLID) && st != fluid) return 0;
            if (!bpred_test(c, l->can_place, ox + xx, oy + yy, oz + zz)) return 0;
        }
    }
    for (int xx = 0; xx < 16; xx++) for (int zz = 0; zz < 16; zz++) for (int yy = 0; yy < 8; yy++) {
        if (G(xx, zz, yy)) {
            int px = ox + xx, py = oy + yy, pz = oz + zz;
            if (lake_can_replace_fluid(c, l, px, py, pz)) {
                int air = yy >= 4;
                fc_set(c, px, py, pz, air ? c->st_cave_air : fluid, 2);
                if (air) fc_mark_above(c, px, py, pz);
            }
        }
    }
    int barrier = bsprov_state(c, l->barrier, ox, oy, oz);
    if (!fc_is_air(c, barrier)) {
        for (int xx = 0; xx < 16; xx++) for (int zz = 0; zz < 16; zz++) for (int yy = 0; yy < 8; yy++) {
            if (EDGE(xx, zz, yy) && (yy < 4 || frnd_int_bound(r, 2) != 0)) {
                int px = ox + xx, py = oy + yy, pz = oz + zz;
                int st = fc_get(c, px, py, pz);
                if ((c->bs->flags[st] & BSF_SOLID) && lake_can_replace_barrier(c, l, px, py, pz)) { fc_set(c, px, py, pz, barrier, 2); fc_mark_above(c, px, py, pz); }
            }
        }
    }
    /* вода: лёд на поверхности (Biome.shouldFreeze) — в данных 26.x озёр воды нет (lake_water отсутствует); при появлении — дописать */
    #undef G
    #undef EDGE
    return 1;
}

static const FeatType T_DISK = { "minecraft:disk", disk_parse, disk_place };
static const FeatType T_BLOB = { "minecraft:block_blob", blob_parse, blob_place };
static const FeatType T_SPRING = { "minecraft:spring_feature", spring_parse, spring_place };
static const FeatType T_LAKE = { "minecraft:lake", lake_parse, lake_place };
void feature_register_blobs(void) {
    feature_register_type(&T_DISK); feature_register_type(&T_BLOB); feature_register_type(&T_SPRING); feature_register_type(&T_LAKE);
}

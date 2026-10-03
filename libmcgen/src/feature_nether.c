/* feature_nether.c — фичи Nether и «составные» фичи 26.3: netherrack_replace_blobs, random_neighbor_spread, single_block_pillar,
 * projected_random_patchy_square, overlay, delta_feature, stepped_column_cluster. Версии 26.1/26.2 хранят эти постройки в старых классах
 * (GlowstoneFeature, BasaltPillarFeature, BasaltColumnsFeature, DeltaFeature с config) — ветки по p->newf. */
#include "feature_misc.h"
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== общие: списки placed_feature */
Placed **fpm_placed_list(FParse *p, const Js *v, int *n) {
    const Js *one[1]; const Js *const *arr; int cnt;
    if (js_is_arr(v)) { arr = (const Js *const *)v->items; cnt = v->n; } else { one[0] = v; arr = one; cnt = 1; }
    Placed **l = fp_alloc(p, sizeof(Placed *) * (size_t)(cnt ? cnt : 1));
    for (int i = 0; i < cnt; i++) { l[i] = fp_placed(p, arr[i]); if (!l[i]) return NULL; }
    *n = cnt; return l;
}

/* ====================================================================== overlay */
typedef struct OverlayCfg { int n; Placed **pf; } OverlayCfg;
static void *overlay_parse(FParse *p, const Js *cfg) {
    OverlayCfg *o = fp_alloc(p, sizeof *o);
    o->pf = fpm_placed_list(p, js_get(cfg, "features"), &o->n);
    return o->pf ? o : NULL;
}
static int overlay_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const OverlayCfg *o = cfg; int any = 0;
    for (int i = 0; i < o->n; i++) any |= placed_place(c, o->pf[i], x, y, z, 0);
    return any;
}

/* ====================================================================== netherrack_replace_blobs (ReplaceBlobsFeature) */
typedef struct ReplBlobCfg { int target_blk, target, state; IntProv *radius; } ReplBlobCfg;
static void *replblob_parse(FParse *p, const Js *cfg) {
    ReplBlobCfg *r = fp_alloc(p, sizeof *r);
    r->target = bs_from_json(p->bs, js_get(cfg, "target")); r->state = bs_from_json(p->bs, js_get(cfg, "state"));
    if (r->target < 0 || r->state < 0) { fp_fail(p, "replace_blobs: плохие target/state"); return NULL; }
    r->target_blk = p->g->state_block[r->target];
    r->radius = fp_intprov(p, js_get(cfg, "radius")); return r->radius ? r : NULL;
}
static int replblob_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const ReplBlobCfg *r = cfg; FRnd *rnd = c->rnd;
    int y = oy < c->min_y + 1 ? c->min_y + 1 : (oy > fc_max_y(c) ? fc_max_y(c) : oy);
    int found = 0;
    while (y > c->min_y + 1) { if (fc_is_block(c, fc_get(c, ox, y, oz), r->target_blk)) { found = 1; break; } y--; }
    if (!found) return 0;
    oy = y;
    int rx = intprov_sample(r->radius, rnd), ry = intprov_sample(r->radius, rnd), rz = intprov_sample(r->radius, rnd);
    int mx = imax_(rx, imax_(ry, rz)), any = 0;
    ManIt it; man_init(&it, ox, oy, oz, rx, ry, rz, rx + ry + rz);
    int x, yy, z;
    while (man_next(&it, &x, &yy, &z)) {
        if (dist_manhattan(x, yy, z, ox, oy, oz) > mx) break;
        if (fc_is_block(c, fc_get(c, x, yy, z), r->target_blk)) { fc_set(c, x, yy, z, r->state, 3); any = 1; }
    }
    return any;
}

/* ====================================================================== random_neighbor_spread (26.3: «glowstone»/«weeping_vines» через обобщённую фичу) */
typedef struct RnsCfg { BSProv *block; u8 *accepted; BPred *can_replace; IntProv *attempts, *xz, *yoff; } RnsCfg;
static void *rns_parse(FParse *p, const Js *cfg) {
    RnsCfg *r = fp_alloc(p, sizeof *r);
    r->block = fp_bsprov(p, js_get(cfg, "block")); if (!r->block) return NULL;
    r->accepted = fp_blockset(p, js_get(cfg, "accepted_neighbors")); if (!r->accepted) { fp_fail(p, "random_neighbor_spread: плохие accepted_neighbors"); return NULL; }
    r->can_replace = fp_bpred(p, js_get(cfg, "can_replace")); if (!r->can_replace) return NULL;
    r->attempts = fp_intprov(p, js_get(cfg, "attempts")); r->xz = fp_intprov(p, js_get(cfg, "xz_offset")); r->yoff = fp_intprov(p, js_get(cfg, "y_offset"));
    return r->attempts && r->xz && r->yoff ? r : NULL;
}
static int rns_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const RnsCfg *r = cfg; FRnd *rnd = c->rnd;
    fc_set(c, ox, oy, oz, bsprov_state(c, r->block, ox, oy, oz), 2);
    int attempts = intprov_sample(r->attempts, rnd);
    for (int i = 0; i < attempts; i++) {
        int dx = intprov_sample(r->xz, rnd), dy = intprov_sample(r->yoff, rnd), dz = intprov_sample(r->xz, rnd);
        int px = ox + dx, py = oy + dy, pz = oz + dz;
        if (!bpred_test(c, r->can_replace, px, py, pz)) continue;
        int nb = 0;
        for (int d = 0; d < 6; d++) {
            if (fc_in_set(c, fc_get(c, px + DIR_DX[d], py + DIR_DY[d], pz + DIR_DZ[d]), r->accepted)) nb++;
            if (nb > 1) break;
        }
        if (nb == 1) fc_set(c, px, py, pz, bsprov_state(c, r->block, px, py, pz), 2);
    }
    return 1;
}

/* ====================================================================== single_block_pillar */
typedef struct PillarCfg { BSProv *block; BPred *can_replace; int dir; float chance; Placed *cap; } PillarCfg;
static void *pillar_parse(FParse *p, const Js *cfg) {
    PillarCfg *s = fp_alloc(p, sizeof *s);
    s->block = fp_bsprov(p, js_get(cfg, "block")); if (!s->block) return NULL;
    s->can_replace = js_get(cfg, "can_replace") ? fp_bpred(p, js_get(cfg, "can_replace")) : bpred_true(p); if (!s->can_replace) return NULL;
    s->dir = dir_from_name(js_str(js_get(cfg, "direction"), NULL)); if (s->dir < 0) { fp_fail(p, "single_block_pillar: плохое direction"); return NULL; }
    s->chance = js_get(cfg, "chance_to_continue") ? js_numf(js_get(cfg, "chance_to_continue"), 1.0f) : 1.0f;
    if (js_get(cfg, "cap_feature")) { s->cap = fp_placed(p, js_get(cfg, "cap_feature")); if (!s->cap) return NULL; }
    return s;
}
static int pillar_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const PillarCfg *s = cfg; FRnd *rnd = c->rnd;
    while (bpred_test(c, s->can_replace, x, y, z) && frnd_float(rnd) < s->chance && !fc_outside(c, y)) {
        fc_set(c, x, y, z, bsprov_state(c, s->block, x, y, z), 2);
        x += DIR_DX[s->dir]; y += DIR_DY[s->dir]; z += DIR_DZ[s->dir];
    }
    int opp = s->dir ^ 1;
    x += DIR_DX[opp]; y += DIR_DY[opp]; z += DIR_DZ[opp];
    if (s->cap) placed_place(c, s->cap, x, y, z, 0);
    return 1;
}

/* ====================================================================== projected_random_patchy_square */
typedef struct PatchySqCfg { BSProv *block; BPred *through; IntProv *size; int max_proj; } PatchySqCfg;
static void *patchysq_parse(FParse *p, const Js *cfg) {
    PatchySqCfg *s = fp_alloc(p, sizeof *s);
    s->block = fp_bsprov(p, js_get(cfg, "block")); s->through = fp_bpred(p, js_get(cfg, "project_through")); s->size = fp_intprov(p, js_get(cfg, "size"));
    s->max_proj = js_int(js_get(cfg, "max_projection_height"), 0);
    return s->block && s->through && s->size ? s : NULL;
}
static int patchysq_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const PatchySqCfg *s = cfg; FRnd *rnd = c->rnd;
    int size = intprov_sample(s->size, rnd), bound = size * size + 1;
    for (int dx = -size; dx <= size; dx++) for (int dz = -size; dz <= size; dz++) {
        int prob = iabs_(dx) * iabs_(dz);
        if (frnd_int_bound(rnd, bound) < bound - prob) {
            int bx = ox + dx, by = oy, bz = oz + dz, drop = s->max_proj;
            while (bpred_test(c, s->through, bx, by - 1, bz)) { by--; if (--drop <= 0) break; }
            int st = bsprov_optional(c, s->block, bx, by, bz);
            if (st >= 0) fc_set(c, bx, by, bz, st, 2);
        }
    }
    return 1;
}

/* ====================================================================== delta_feature (26.3: DeltaFeature без config) */
typedef struct DeltaCfg { int contents, rim; IntProv *size, *rim_size; int contents_blk; int cannot[8]; int ncannot; } DeltaCfg;
static void *delta_parse(FParse *p, const Js *cfg) {
    DeltaCfg *d = fp_alloc(p, sizeof *d);
    d->contents = bs_from_json(p->bs, js_get(cfg, "contents")); d->rim = bs_from_json(p->bs, js_get(cfg, "rim"));
    if (d->contents < 0 || d->rim < 0) { fp_fail(p, "delta_feature: плохие contents/rim"); return NULL; }
    d->contents_blk = p->g->state_block[d->contents];
    d->size = fp_intprov(p, js_get(cfg, "size")); d->rim_size = fp_intprov(p, js_get(cfg, "rim_size"));
    static const char *CANNOT[] = { "bedrock", "nether_bricks", "nether_brick_fence", "nether_brick_stairs", "nether_wart", "chest", "spawner" };
    for (int i = 0; i < 7; i++) d->cannot[d->ncannot++] = bs_block_index(p->bs, CANNOT[i]);
    return d->size && d->rim_size ? d : NULL;
}
static int delta_clear(FCtx *c, const DeltaCfg *d, int x, int y, int z) {
    int st = fc_get(c, x, y, z), blk = c->g->state_block[st];
    if (blk == d->contents_blk) return 0;
    for (int i = 0; i < d->ncannot; i++) if (blk == d->cannot[i]) return 0;
    for (int k = 0; k < 6; k++) {
        int air = fc_is_air(c, fc_get(c, x + DIR_DX[k], y + DIR_DY[k], z + DIR_DZ[k]));
        if ((air && k != DIR_UP) || (!air && k == DIR_UP)) return 0;
    }
    return 1;
}
static int delta_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const DeltaCfg *d = cfg; FRnd *rnd = c->rnd;
    int any = 0;
    int spawn_rim = frnd_double(rnd) < 0.9;
    int rimx = spawn_rim ? intprov_sample(d->rim_size, rnd) : 0;
    int rimz = spawn_rim ? intprov_sample(d->rim_size, rnd) : 0;
    int has_rim = spawn_rim && rimx != 0 && rimz != 0;
    int rx = intprov_sample(d->size, rnd), rz = intprov_sample(d->size, rnd);
    int lim = imax_(rx, rz);
    ManIt it; man_init(&it, ox, oy, oz, rx, 0, rz, rx + rz);
    int x, y, z;
    while (man_next(&it, &x, &y, &z)) {
        if (dist_manhattan(x, y, z, ox, oy, oz) > lim) break;
        if (delta_clear(c, d, x, y, z)) {
            if (has_rim) { any = 1; fc_set(c, x, y, z, d->rim, 3); }
            if (delta_clear(c, d, x + rimx, y, z + rimz)) { any = 1; fc_set(c, x + rimx, y, z + rimz, d->contents, 3); }
        }
    }
    return any;
}

/* ====================================================================== stepped_column_cluster (26.3: базальтовые колонны) */
typedef struct SteppedCfg { BSProv *block; BPred *cont, *can_replace; u8 *cannot_on; IntProv *cluster_reach, *column_count, *column_reach, *height; } SteppedCfg;
static void *stepped_parse(FParse *p, const Js *cfg) {
    SteppedCfg *s = fp_alloc(p, sizeof *s);
    s->block = fp_bsprov(p, js_get(cfg, "block")); s->cont = fp_bpred(p, js_get(cfg, "continue_through")); s->can_replace = fp_bpred(p, js_get(cfg, "can_replace"));
    s->cannot_on = fp_blockset(p, js_get(cfg, "cannot_place_on"));
    s->cluster_reach = fp_intprov(p, js_get(cfg, "cluster_reach")); s->column_count = fp_intprov(p, js_get(cfg, "column_count"));
    s->column_reach = fp_intprov(p, js_get(cfg, "column_reach")); s->height = fp_intprov(p, js_get(cfg, "height"));
    return s->block && s->cont && s->can_replace && s->cannot_on && s->cluster_reach && s->column_count && s->column_reach && s->height ? s : NULL;
}
static int stepped_can_place_at(FCtx *c, const SteppedCfg *s, int x, int y, int z) {
    if (!bpred_test(c, s->can_replace, x, y, z)) return 0;
    int below = fc_get(c, x, y - 1, z);
    return !fc_is_air(c, below) && !fc_in_set(c, below, s->cannot_on);
}
static int stepped_place_column(FCtx *c, const SteppedCfg *s, int ox, int oy, int oz, int col_height, int reach) {
    int any = 0;
    BcIt it; bc_init(&it, ox - reach, oy, oz - reach, ox + reach, oy, oz + reach);
    int x, y, z;
    while (bc_next(&it, &x, &y, &z)) {
        int step = dist_manhattan(x, y, z, ox, oy, oz);
        int found = 0, cx = x, cy = y, cz = z;
        if (bpred_test(c, s->can_replace, x, y, z)) {          /* findSurface */
            int limit = step;
            while (cy > c->min_y + 1 && limit > 0) {
                limit--;
                if (stepped_can_place_at(c, s, cx, cy, cz)) { found = 1; break; }
                cy--;
            }
        } else {                                               /* findAir */
            int limit = step;
            while (cy <= fc_max_y(c) && limit > 0) {
                limit--;
                int st = fc_get(c, cx, cy, cz);
                if (fc_in_set(c, st, s->cannot_on)) break;
                if (fc_is_air(c, st)) { found = 1; break; }
                cy++;
            }
        }
        if (!found) continue;
        int blocks_y = col_height - step / 2;
        while (blocks_y >= 0) {
            if (bpred_test(c, s->can_replace, cx, cy, cz)) {
                fc_set(c, cx, cy, cz, bsprov_state(c, s->block, cx, cy, cz), 3);
                cy++; any = 1;
            } else {
                if (!bpred_test(c, s->cont, cx, cy, cz)) break;
                cy++;
            }
            blocks_y--;
        }
    }
    return any;
}
static int stepped_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const SteppedCfg *s = cfg; FRnd *rnd = c->rnd;
    if (!stepped_can_place_at(c, s, ox, oy, oz)) return 0;
    int col_height = intprov_sample(s->height, rnd);
    int cluster_reach = imin_(col_height, intprov_sample(s->cluster_reach, rnd));
    int count = intprov_sample(s->column_count, rnd), any = 0;
    int w = 2 * cluster_reach + 1;
    for (int i = 0; i < count; i++) {
        int x = ox - cluster_reach + frnd_int_bound(rnd, w);
        int y = oy + frnd_int_bound(rnd, 1);
        int z = oz - cluster_reach + frnd_int_bound(rnd, w);
        int bpy = col_height - dist_manhattan(x, y, z, ox, oy, oz);
        if (bpy >= 0) { int reach = intprov_sample(s->column_reach, rnd); any |= stepped_place_column(c, s, x, y, z, bpy, reach); }
    }
    return any;
}

static const FeatType T_OVERLAY = { "minecraft:overlay", overlay_parse, overlay_place };
static const FeatType T_REPLBLOB = { "minecraft:netherrack_replace_blobs", replblob_parse, replblob_place };
static const FeatType T_RNS = { "minecraft:random_neighbor_spread", rns_parse, rns_place };
static const FeatType T_PILLAR = { "minecraft:single_block_pillar", pillar_parse, pillar_place };
static const FeatType T_PATCHYSQ = { "minecraft:projected_random_patchy_square", patchysq_parse, patchysq_place };
static const FeatType T_DELTA = { "minecraft:delta_feature", delta_parse, delta_place };
static const FeatType T_STEPPED = { "minecraft:stepped_column_cluster", stepped_parse, stepped_place };
void feature_register_nether(void) {
    feature_register_type(&T_OVERLAY); feature_register_type(&T_REPLBLOB); feature_register_type(&T_RNS); feature_register_type(&T_PILLAR);
    feature_register_type(&T_PATCHYSQ); feature_register_type(&T_DELTA); feature_register_type(&T_STEPPED);
}

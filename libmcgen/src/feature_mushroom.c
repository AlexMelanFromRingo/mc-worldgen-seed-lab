/* feature_mushroom.c — грибы-деревья и корневые системы (поток W11): huge_red_mushroom, huge_brown_mushroom, huge_fungus (Nether), root_system (азалия). */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>

static inline int blk_of(const FCtx *c, int st) { return c->g->state_block[st]; }
static inline int mth_next_int_(FRnd *r, int lo, int hi) { return lo >= hi ? lo : frnd_int_bound(r, hi - lo + 1) + lo; }

/* ====================================================================== huge_red_mushroom / huge_brown_mushroom */
typedef struct HugeMush { int red; BSProv *cap, *stem; int radius; BPred *can_place_on; const u8 *leaves, *repl_mush; } HugeMush;
static void *hm_parse_common(FParse *p, const Js *cfg, int red) {
    HugeMush *h = fp_alloc(p, sizeof *h);
    h->red = red;
    if (!(h->cap = fp_bsprov(p, js_get(cfg, "cap_provider"))) || !(h->stem = fp_bsprov(p, js_get(cfg, "stem_provider")))) return NULL;
    h->radius = js_int(js_get(cfg, "foliage_radius"), 2);
    if (!(h->can_place_on = fp_bpred(p, js_get(cfg, "can_place_on")))) return NULL;
    h->leaves = gen_block_tag(p->g, "minecraft:leaves"); h->repl_mush = gen_block_tag(p->g, "minecraft:replaceable_by_mushrooms");
    return h;
}
static void *hm_parse_red(FParse *p, const Js *cfg) { return hm_parse_common(p, cfg, 1); }
static void *hm_parse_brown(FParse *p, const Js *cfg) { return hm_parse_common(p, cfg, 0); }

static int hm_radius_for_height(const HugeMush *h, int yo) {          /* getTreeRadiusForHeight(−1, −1, radius, yo): у красного treeHeight = −1 → всегда 0 */
    if (h->red) return 0;
    return yo <= 3 ? 0 : h->radius;
}
static void hm_place_block(FCtx *c, const HugeMush *h, int x, int y, int z, int st) {      /* placeMushroomBlock */
    int cur = fc_get(c, x, y, z);
    if (fc_is_air(c, cur) || h->repl_mush[blk_of(c, cur)]) fc_set(c, x, y, z, st, 3);
}
static int hm_set_bool(const BsTab *bs, int st, const char *prop, int v) { int n = bs_with(bs, st, prop, v ? "true" : "false"); return n >= 0 ? n : st; }
static void hm_cap_brown(FCtx *c, const HugeMush *h, int ox, int oy, int oz, int th) {
    const BsTab *bs = c->bs; int R = h->radius;
    for (int dx = -R; dx <= R; dx++) for (int dz = -R; dz <= R; dz++) {
        int minx = dx == -R, maxx = dx == R, minz = dz == -R, maxz = dz == R, xe = minx || maxx, ze = minz || maxz;
        if (xe && ze) continue;
        int west = minx || (ze && dx == 1 - R), east = maxx || (ze && dx == R - 1), north = minz || (xe && dz == 1 - R), south = maxz || (xe && dz == R - 1);
        int st = bsprov_state(c, h->cap, ox, oy, oz);
        if (bs_has_prop(bs, st, "west") && bs_has_prop(bs, st, "east") && bs_has_prop(bs, st, "north") && bs_has_prop(bs, st, "south")) {
            st = hm_set_bool(bs, st, "west", west); st = hm_set_bool(bs, st, "east", east); st = hm_set_bool(bs, st, "north", north); st = hm_set_bool(bs, st, "south", south);
        }
        hm_place_block(c, h, ox + dx, oy + th, oz + dz, st);
    }
}
static void hm_cap_red(FCtx *c, const HugeMush *h, int ox, int oy, int oz, int th) {
    const BsTab *bs = c->bs;
    for (int dy = th - 3; dy <= th; dy++) {
        int R = dy < th ? h->radius : h->radius - 1, center = h->radius - 2;
        for (int dx = -R; dx <= R; dx++) for (int dz = -R; dz <= R; dz++) {
            int minx = dx == -R, maxx = dx == R, minz = dz == -R, maxz = dz == R, xe = minx || maxx, ze = minz || maxz;
            if (!(dy >= th || xe != ze)) continue;
            int st = bsprov_state(c, h->cap, ox, oy, oz);
            if (bs_has_prop(bs, st, "west") && bs_has_prop(bs, st, "east") && bs_has_prop(bs, st, "north") && bs_has_prop(bs, st, "south") && bs_has_prop(bs, st, "up")) {
                st = hm_set_bool(bs, st, "up", dy >= th - 1); st = hm_set_bool(bs, st, "west", dx < -center); st = hm_set_bool(bs, st, "east", dx > center);
                st = hm_set_bool(bs, st, "north", dz < -center); st = hm_set_bool(bs, st, "south", dz > center);
            }
            (void)minz; (void)maxz;
            hm_place_block(c, h, ox + dx, oy + dy, oz + dz, st);
        }
    }
}
static int hm_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const HugeMush *h = cfg; FRnd *r = c->rnd;
    int th = frnd_int_bound(r, 3) + 4;
    if (frnd_int_bound(r, 12) == 0) th *= 2;
    int maxy = c->min_y + c->height - 1;
    int dbg = getenv("MCGEN_MUSH_DEBUG") != NULL;
    if (!(oy >= c->min_y + 1 && oy + th + 1 <= maxy)) return 0;
    if (!bpred_test(c, h->can_place_on, ox, oy - 1, oz)) { if (dbg) fprintf(stderr, "MUSH canplace fail at %d,%d,%d below=%s\n", ox, oy, oz, c->bs->blk[blk_of(c, fc_get(c, ox, oy - 1, oz))].name); return 0; }
    for (int dy = 0; dy <= th; dy++) {
        int R = hm_radius_for_height(h, dy);
        for (int dx = -R; dx <= R; dx++) for (int dz = -R; dz <= R; dz++) {
            int st = fc_get(c, ox + dx, oy + dy, oz + dz);
            if (!fc_is_air(c, st) && !h->leaves[blk_of(c, st)]) { if (dbg) fprintf(stderr, "MUSH blocked at %d,%d,%d by %s (origin %d,%d,%d h=%d)\n", ox + dx, oy + dy, oz + dz, c->bs->blk[blk_of(c, st)].name, ox, oy, oz, th); return 0; }
        }
    }
    if (h->red) hm_cap_red(c, h, ox, oy, oz, th); else hm_cap_brown(c, h, ox, oy, oz, th);
    for (int dy = 0; dy < th; dy++) {
        int st = bsprov_state(c, h->stem, ox, oy, oz);
        hm_place_block(c, h, ox, oy + dy, oz, st);
    }
    return 1;
}

/* ====================================================================== huge_fungus */
typedef struct HugeFungus { int base_blk; int stem, hat, decor; int hat_blk; BPred *replaceable; int planted; int nether_wart_blk, weeping, weeping_plant; } HugeFungus;
static void *hf_parse(FParse *p, const Js *cfg) {
    HugeFungus *f = fp_alloc(p, sizeof *f);
    int base = bs_from_json(p->bs, js_get(cfg, "valid_base_block"));
    f->stem = bs_from_json(p->bs, js_get(cfg, "stem_state")); f->hat = bs_from_json(p->bs, js_get(cfg, "hat_state")); f->decor = bs_from_json(p->bs, js_get(cfg, "decor_state"));
    if (base < 0 || f->stem < 0 || f->hat < 0 || f->decor < 0) { fp_fail(p, "huge_fungus: состояния блоков"); return NULL; }
    f->base_blk = p->g->state_block[base]; f->hat_blk = p->g->state_block[f->hat];
    if (!(f->replaceable = fp_bpred(p, js_get(cfg, "replaceable_blocks")))) return NULL;
    f->planted = js_bool(js_get(cfg, "planted"), 0);
    f->nether_wart_blk = bs_block_index(p->bs, "minecraft:nether_wart_block");
    int wv = bs_block_index(p->bs, "minecraft:weeping_vines"), wp = bs_block_index(p->bs, "minecraft:weeping_vines_plant");
    f->weeping = wv >= 0 ? p->bs->blk[wv].def : -1; f->weeping_plant = wp >= 0 ? p->bs->blk[wp].def : -1;
    return f;
}
static int hf_replaceable(FCtx *c, const HugeFungus *f, int x, int y, int z, int check_plants) {
    if (c->bs->flags[fc_get(c, x, y, z)] & BSF_REPLACEABLE) return 1;
    return check_plants ? bpred_test(c, f->replaceable, x, y, z) : 0;
}
static void hf_weeping_column(FCtx *c, const HugeFungus *f, FRnd *r, int x, int y, int z, int total) {
    for (int h = 0; h <= total; h++) {
        if (fc_is_air(c, fc_get(c, x, y, z))) {
            if (h == total || !fc_is_air(c, fc_get(c, x, y - 1, z))) {
                int age = mth_next_int_(r, 23, 25); char buf[8]; snprintf(buf, sizeof buf, "%d", age);
                int st = f->weeping; if (st >= 0) { int s2 = bs_with(c->bs, st, "age", buf); if (s2 >= 0) st = s2; }
                fc_set(c, x, y, z, st, 2);
                break;
            }
            fc_set(c, x, y, z, f->weeping_plant, 2);
        }
        y--;
    }
}
static void hf_try_vines(FCtx *c, const HugeFungus *f, FRnd *r, int x, int y, int z) {
    int py = y - 1;
    if (fc_is_air(c, fc_get(c, x, py, z))) {
        int goal = mth_next_int_(r, 1, 5);
        if (frnd_int_bound(r, 7) == 0) goal *= 2;
        hf_weeping_column(c, f, r, x, py, z, goal);
    }
}
static void hf_hat_block(FCtx *c, const HugeFungus *f, FRnd *r, int x, int y, int z, float decor_p, float hat_p, float vines_p) {
    if (frnd_float(r) < decor_p) fc_set(c, x, y, z, f->decor, 3);
    else if (frnd_float(r) < hat_p) {
        fc_set(c, x, y, z, f->hat, 3);
        if (frnd_float(r) < vines_p) hf_try_vines(c, f, r, x, y, z);
    }
}
static int hf_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const HugeFungus *f = cfg; FRnd *r = c->rnd;
    if (blk_of(c, fc_get(c, ox, oy - 1, oz)) != f->base_blk) return 0;
    int total = mth_next_int_(r, 4, 13);
    if (frnd_int_bound(r, 12) == 0) total *= 2;
    if (!f->planted && oy + total + 1 >= c->gen_depth) return 0;
    int huge = !f->planted && frnd_float(r) < 0.06f;
    fc_set(c, ox, oy, oz, c->st_air, 260);
    /* placeStem */
    int sr = huge ? 1 : 0;
    for (int dx = -sr; dx <= sr; dx++) for (int dz = -sr; dz <= sr; dz++) {
        int corner = huge && abs(dx) == sr && abs(dz) == sr;
        for (int dy = 0; dy < total; dy++) {
            int x = ox + dx, y = oy + dy, z = oz + dz;
            if (hf_replaceable(c, f, x, y, z, 1)) {
                if (f->planted) fc_set(c, x, y, z, f->stem, 3);
                else if (corner) { if (frnd_float(r) < 0.1f) fc_set(c, x, y, z, f->stem, 3); }
                else fc_set(c, x, y, z, f->stem, 3);
            }
        }
    }
    /* placeHat */
    int vines = f->hat_blk == f->nether_wart_blk;
    int hat_h = frnd_int_bound(r, 1 + total / 3) + 5; if (hat_h > total) hat_h = total;
    int hat_start = total - hat_h;
    for (int dy = hat_start; dy <= total; dy++) {
        int radius = dy < total - frnd_int_bound(r, 3) ? 2 : 1;
        if (hat_h > 8 && dy < hat_start + 4) radius = 3;
        if (huge) radius++;
        for (int dx = -radius; dx <= radius; dx++) for (int dz = -radius; dz <= radius; dz++) {
            int ex = dx == -radius || dx == radius, ez = dz == -radius || dz == radius;
            int inside = !ex && !ez && dy != total, corner = ex && ez, bottom = dy < hat_start + 3;
            int x = ox + dx, y = oy + dy, z = oz + dz;
            if (!hf_replaceable(c, f, x, y, z, 0)) continue;
            if (bottom) {
                if (!inside) {
                    if (blk_of(c, fc_get(c, x, y - 1, z)) == f->hat_blk) fc_set(c, x, y, z, f->hat, 3);
                    else if ((double)frnd_float(r) < 0.15) {
                        fc_set(c, x, y, z, f->hat, 3);
                        if (vines && frnd_int_bound(r, 11) == 0) hf_try_vines(c, f, r, x, y, z);
                    }
                }
            } else if (inside) hf_hat_block(c, f, r, x, y, z, 0.1f, 0.2f, vines ? 0.1f : 0.0f);
            else if (corner) hf_hat_block(c, f, r, x, y, z, 0.01f, 0.7f, vines ? 0.083f : 0.0f);
            else hf_hat_block(c, f, r, x, y, z, 5.0E-4f, 0.98f, vines ? 0.07f : 0.0f);
        }
    }
    return 1;
}

/* ====================================================================== root_system (азалия) */
typedef struct RootSys {
    Placed *tree; int vspace, level_dist, max_dev, root_radius; u8 *root_repl; BSProv *root_prov; int root_attempts, col_max, hang_radius, hang_span;
    BSProv *hang_prov; int hang_attempts, allowed_water; BPred *allowed_pos;
} RootSys;
static void *rs_parse(FParse *p, const Js *cfg) {
    RootSys *r = fp_alloc(p, sizeof *r);
    if (!(r->tree = fp_placed(p, js_get(cfg, "feature")))) return NULL;
    r->vspace = js_int(js_get(cfg, "required_vertical_space_for_tree"), 1); r->level_dist = js_int(js_get(cfg, "level_test_distance"), 0);
    r->max_dev = js_int(js_get(cfg, "max_level_deviation"), 0); r->root_radius = js_int(js_get(cfg, "root_radius"), 1);
    if (!(r->root_repl = fp_blockset(p, js_get(cfg, "root_replaceable")))) return fp_fail(p, "root_replaceable"), NULL;
    if (!(r->root_prov = fp_bsprov(p, js_get(cfg, "root_state_provider")))) return NULL;
    r->root_attempts = js_int(js_get(cfg, "root_placement_attempts"), 1); r->col_max = js_int(js_get(cfg, "root_column_max_height"), 1);
    r->hang_radius = js_int(js_get(cfg, "hanging_root_radius"), 1); r->hang_span = js_int(js_get(cfg, "hanging_roots_vertical_span"), 1);
    if (!(r->hang_prov = fp_bsprov(p, js_get(cfg, "hanging_root_state_provider")))) return NULL;
    r->hang_attempts = js_int(js_get(cfg, "hanging_root_placement_attempts"), 1); r->allowed_water = js_int(js_get(cfg, "allowed_vertical_water_for_tree"), 1);
    if (!(r->allowed_pos = fp_bpred(p, js_get(cfg, "allowed_tree_position")))) return NULL;
    return r;
}
static int rs_is_water_fluid(const FCtx *c, int st) { int t = BS_FL_TYPE(c->bs->fluid[st]); return t == FL_WATER || t == FL_FLOWING_WATER; }
static int rs_space_for_tree(FCtx *c, const RootSys *r, int x, int y, int z) {
    for (int i = 1; i <= r->vspace; i++) {
        int st = fc_get(c, x, y + i, z);
        if (fc_is_air(c, st)) continue;
        if (!(i + 1 <= r->allowed_water && rs_is_water_fluid(c, st))) return 0;
    }
    if (r->level_dist > 0) {
        static const int DX[4] = { 0, -1, 0, 1 }, DZ[4] = { 1, 0, -1, 0 };           /* Direction.from2DDataValue: SOUTH, WEST, NORTH, EAST */
        for (int i = 0; i < 4; i++) {
            int cx = x + DX[i] * r->level_dist, cz = z + DZ[i] * r->level_dist;
            int below = fc_get(c, cx, y - r->max_dev, cz), above = fc_get(c, cx, y + r->max_dev, cz);
            if (fc_is_air(c, below) || !fc_is_air(c, above)) return 0;
        }
    }
    return 1;
}
static int rs_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const RootSys *r = cfg; FRnd *rnd = c->rnd;
    if (!fc_is_air(c, fc_get(c, ox, oy, oz))) return 0;
    int wy = oy, ok = 0, y = 0;
    for (y = 0; y < r->col_max; y++) {
        wy++;
        if (fc_height(c, HM_WORLD_SURFACE, ox, oz) < wy) break;
        if (bpred_test(c, r->allowed_pos, ox, wy, oz) && rs_space_for_tree(c, r, ox, wy, oz)) {
            int below = fc_get(c, ox, wy - 1, oz);
            int ft = BS_FL_TYPE(c->bs->fluid[below]);
            if (ft == FL_LAVA || ft == FL_FLOWING_LAVA || !(c->bs->flags[below] & BSF_SOLID)) break;
            if (placed_place(c, r->tree, ox, wy, oz, 0)) {
                /* placeDirt(origin, origin.y + y, …) */
                int target = oy + y;
                for (int yy = oy; yy < target; yy++) {
                    int wx = ox, wz = oz;
                    for (int i = 0; i < r->root_attempts; i++) {
                        int a = frnd_int_bound(rnd, r->root_radius), b = frnd_int_bound(rnd, r->root_radius);
                        int dx = a - b;
                        a = frnd_int_bound(rnd, r->root_radius); b = frnd_int_bound(rnd, r->root_radius);
                        int dz = a - b;
                        wx += dx; wz += dz;
                        if (r->root_repl[blk_of(c, fc_get(c, wx, yy, wz))]) fc_set(c, wx, yy, wz, bsprov_state(c, r->root_prov, wx, yy, wz), 2);
                        wx = ox; wz = oz;
                    }
                }
                ok = 1; break;
            }
        }
    }
    if (ok) {
        for (int i = 0; i < r->hang_attempts; i++) {
            int a = frnd_int_bound(rnd, r->hang_radius), b = frnd_int_bound(rnd, r->hang_radius); int dx = a - b;
            a = frnd_int_bound(rnd, r->hang_span); b = frnd_int_bound(rnd, r->hang_span); int dy = a - b;
            a = frnd_int_bound(rnd, r->hang_radius); b = frnd_int_bound(rnd, r->hang_radius); int dz = a - b;
            int x = ox + dx, yv = oy + dy, z = oz + dz;
            if (fc_is_air(c, fc_get(c, x, yv, z))) {
                int st = bsprov_state(c, r->hang_prov, x, yv, z);
                if (block_can_survive(c, st, x, yv, z) && ((c->bs->sturdy[fc_get(c, x, yv + 1, z)] >> DIR_DOWN) & 1)) fc_set(c, x, yv, z, st, 2);
            }
        }
    }
    return 1;
}

static const FeatType T_HRED = { "minecraft:huge_red_mushroom", hm_parse_red, hm_place };
static const FeatType T_HBROWN = { "minecraft:huge_brown_mushroom", hm_parse_brown, hm_place };
static const FeatType T_FUNGUS = { "minecraft:huge_fungus", hf_parse, hf_place };
static const FeatType T_ROOTSYS = { "minecraft:root_system", rs_parse, rs_place };
void feature_register_mushroom(void) {
    feature_register_type(&T_HRED); feature_register_type(&T_HBROWN); feature_register_type(&T_FUNGUS); feature_register_type(&T_ROOTSYS);
}

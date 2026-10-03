/* feature_tree_placers.c — TrunkPlacer, FoliagePlacer и RootPlacer деревьев (поток W11). Порядок вызовов ГСЧ — как в игре. */
#include "feature_tree.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int tp_valid_pos(const TreeRun *tr, int x, int y, int z);      /* feature_tree.c: TrunkPlacer.validTreePos (виртуальный) */

static inline int blk_of(const FCtx *c, int st) { return c->g->state_block[st]; }
static inline int mth_floor_d(double v) { int i = (int)v; return v < (double)i ? i - 1 : i; }
static inline int mth_floor_f(float v) { int i = (int)v; return v < (float)i ? i - 1 : i; }
static const char *tname(const char *t) { return !strncmp(t, "minecraft:", 10) ? t + 10 : t; }
static const int HZ_DX[4] = { 0, 1, 0, -1 }, HZ_DZ[4] = { -1, 0, 1, 0 };       /* Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST */
static inline int hz_random(FRnd *r) { return frnd_int_bound(r, 4); }           /* Plane.HORIZONTAL.getRandomDirection */

/* ====================================================================== разбор */
static IntProv *ip(FParse *p, const Js *v, const char *key) { IntProv *r = fp_intprov(p, js_get(v, key)); if (!r) fp_fail(p, "поле %s", key); return r; }

int tree_parse_trunk(FParse *p, const Js *v, TrunkCfg *tp) {
    const char *t = js_str(js_get(v, "type"), NULL);
    if (!t) return fp_fail(p, "trunk_placer: нет type");
    t = tname(t);
    memset(tp, 0, sizeof *tp);
    tp->base_height = js_int(js_get(v, "base_height"), 0); tp->rand_a = js_int(js_get(v, "height_rand_a"), 0); tp->rand_b = js_int(js_get(v, "height_rand_b"), 0);
    if (!strcmp(t, "straight_trunk_placer")) tp->type = TP_STRAIGHT;
    else if (!strcmp(t, "forking_trunk_placer")) tp->type = TP_FORKING;
    else if (!strcmp(t, "giant_trunk_placer")) tp->type = TP_GIANT;
    else if (!strcmp(t, "mega_jungle_trunk_placer")) tp->type = TP_MEGA_JUNGLE;
    else if (!strcmp(t, "dark_oak_trunk_placer")) tp->type = TP_DARK_OAK;
    else if (!strcmp(t, "fancy_trunk_placer")) tp->type = TP_FANCY;
    else if (!strcmp(t, "bending_trunk_placer")) {
        tp->type = TP_BENDING; tp->min_height_for_leaves = js_int(js_get(v, "min_height_for_leaves"), 1);
        if (!(tp->bend_length = ip(p, v, "bend_length"))) return 0;
    } else if (!strcmp(t, "upwards_branching_trunk_placer")) {
        tp->type = TP_UPWARDS;
        if (!(tp->extra_branch_steps = ip(p, v, "extra_branch_steps")) || !(tp->extra_branch_length = ip(p, v, "extra_branch_length"))) return 0;
        tp->place_branch_per_log = js_numf(js_get(v, "place_branch_per_log_probability"), 0);
        tp->can_grow_through = fp_blockset(p, js_get(v, "can_grow_through")); if (!tp->can_grow_through) return fp_fail(p, "can_grow_through");
    } else if (!strcmp(t, "cherry_trunk_placer")) {
        tp->type = TP_CHERRY;
        if (!(tp->branch_count = ip(p, v, "branch_count")) || !(tp->branch_horizontal_length = ip(p, v, "branch_horizontal_length")) ||
            !(tp->branch_end_offset = ip(p, v, "branch_end_offset_from_top"))) return 0;
        const Js *bs = js_get(v, "branch_start_offset_from_top");
        if (!bs) return fp_fail(p, "branch_start_offset_from_top");
        tp->bs_min = js_int(js_get(bs, "min_inclusive"), 0); tp->bs_max = js_int(js_get(bs, "max_inclusive"), 0);
    } else if (!strcmp(t, "poplar_trunk_placer")) {
        tp->type = TP_POPLAR;
        if (!(tp->trunk_height_above_branches = ip(p, v, "trunk_height_above_branches")) || !(tp->branch_amount = ip(p, v, "branch_amount"))) return 0;
    } else return fp_fail(p, "trunk_placer: неизвестный тип %s", t);
    return 1;
}

int tree_parse_foliage(FParse *p, const Js *v, FoliageCfg *fp) {
    const char *t = js_str(js_get(v, "type"), NULL);
    if (!t) return fp_fail(p, "foliage_placer: нет type");
    t = tname(t);
    memset(fp, 0, sizeof *fp);
    if (!(fp->radius = ip(p, v, "radius")) || !(fp->offset = ip(p, v, "offset"))) return 0;
    if (!strcmp(t, "blob_foliage_placer")) { fp->type = FP_BLOB; fp->height = js_int(js_get(v, "height"), 0); }
    else if (!strcmp(t, "bush_foliage_placer")) { fp->type = FP_BUSH; fp->height = js_int(js_get(v, "height"), 0); }
    else if (!strcmp(t, "fancy_foliage_placer")) { fp->type = FP_FANCY; fp->height = js_int(js_get(v, "height"), 0); }
    else if (!strcmp(t, "spruce_foliage_placer")) { fp->type = FP_SPRUCE; if (!(fp->height_ip = ip(p, v, "trunk_height"))) return 0; }
    else if (!strcmp(t, "pine_foliage_placer")) { fp->type = FP_PINE; if (!(fp->height_ip = ip(p, v, "height"))) return 0; }
    else if (!strcmp(t, "acacia_foliage_placer")) fp->type = FP_ACACIA;
    else if (!strcmp(t, "jungle_foliage_placer")) { fp->type = FP_MEGA_JUNGLE; fp->height = js_int(js_get(v, "height"), 0); }
    else if (!strcmp(t, "mega_pine_foliage_placer")) { fp->type = FP_MEGA_PINE; if (!(fp->height_ip = ip(p, v, "crown_height"))) return 0; }
    else if (!strcmp(t, "dark_oak_foliage_placer")) fp->type = FP_DARK_OAK;
    else if (!strcmp(t, "random_spread_foliage_placer")) {
        fp->type = FP_RANDOM_SPREAD; if (!(fp->height_ip = ip(p, v, "foliage_height"))) return 0;
        fp->leaf_attempts = js_int(js_get(v, "leaf_placement_attempts"), 0);
    } else if (!strcmp(t, "cherry_foliage_placer")) {
        fp->type = FP_CHERRY; if (!(fp->height_ip = ip(p, v, "height"))) return 0;
        fp->wide_bottom_hole = js_numf(js_get(v, "wide_bottom_layer_hole_chance"), 0); fp->corner_hole = js_numf(js_get(v, "corner_hole_chance"), 0);
        fp->hanging = js_numf(js_get(v, "hanging_leaves_chance"), 0); fp->hanging_ext = js_numf(js_get(v, "hanging_leaves_extension_chance"), 0);
    } else if (!strcmp(t, "poplar_foliage_placer")) {
        fp->type = FP_POPLAR; if (!(fp->height_ip = ip(p, v, "height"))) return 0;
        fp->side_hole = js_numf(js_get(v, "side_hole_chance"), 0);
    } else return fp_fail(p, "foliage_placer: неизвестный тип %s", t);
    return 1;
}

RootCfg *tree_parse_root(FParse *p, const Js *v) {
    const char *t = js_str(js_get(v, "type"), NULL);
    if (!t || strcmp(tname(t), "mangrove_root_placer")) { fp_fail(p, "root_placer: неизвестный тип"); return NULL; }
    RootCfg *r = fp_alloc(p, sizeof *r);
    if (!(r->trunk_offset_y = ip(p, v, "trunk_offset_y"))) return NULL;
    if (!(r->root_provider = fp_bsprov(p, js_get(v, "root_provider")))) return NULL;
    const Js *ab = js_get(v, "above_root_placement");
    if (ab) {
        r->has_above = 1;
        if (!(r->above_provider = fp_bsprov(p, js_get(ab, "above_root_provider")))) return NULL;
        r->above_chance = js_numf(js_get(ab, "above_root_placement_chance"), 0);
    }
    const Js *m = js_get(v, "mangrove_root_placement");
    if (!m) { fp_fail(p, "mangrove_root_placement"); return NULL; }
    r->can_grow_through = fp_blockset(p, js_get(m, "can_grow_through")); r->muddy_roots_in = fp_blockset(p, js_get(m, "muddy_roots_in"));
    if (!r->can_grow_through || !r->muddy_roots_in) { fp_fail(p, "mangrove_root_placement: наборы блоков"); return NULL; }
    if (!(r->muddy_provider = fp_bsprov(p, js_get(m, "muddy_roots_provider")))) return NULL;
    r->max_width = js_int(js_get(m, "max_root_width"), 3); r->max_length = js_int(js_get(m, "max_root_length"), 5); r->skew = js_numf(js_get(m, "random_skew_chance"), 0);
    return r;
}

/* ====================================================================== TrunkPlacer: общие операции */
int tp_tree_height(const TrunkCfg *tp, FRnd *r) {
    int a = frnd_int_bound(r, tp->rand_a + 1);
    int b = frnd_int_bound(r, tp->rand_b + 1);
    return tp->base_height + a + b;
}
static void place_below(TreeRun *tr, int x, int y, int z) {
    int st = bsprov_optional(tr->c, tr->t->below_prov, x, y, z);
    if (st >= 0) tr_set_trunk(tr, x, y, z, st);
}
/* placeLog: axis −1 — без изменения оси */
static int place_log(TreeRun *tr, int x, int y, int z, int axis) {
    if (!tp_valid_pos(tr, x, y, z)) return 0;
    int st = bsprov_state(tr->c, tr->t->trunk_prov, x, y, z);
    if (axis >= 0) st = tree_state_axis(tr->c->bs, st, axis);
    tr_set_trunk(tr, x, y, z, st);
    return 1;
}
static void place_log_if_free(TreeRun *tr, int x, int y, int z) { if (tr_is_free(tr, x, y, z)) place_log(tr, x, y, z, -1); }

/* ---- straight ---- */
static void trunk_straight(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    place_below(tr, ox, oy - 1, oz);
    for (int y = 0; y < h; y++) place_log(tr, ox, oy + y, oz, -1);
    att_push(out, ox, oy + h, oz, 0, 0, 1, 1);
}
/* ---- forking ---- */
static void trunk_forking(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r;
    place_below(tr, ox, oy - 1, oz);
    int lean = hz_random(r);
    int lean_h = h - frnd_int_bound(r, 4) - 1;
    int lean_steps = 3 - frnd_int_bound(r, 3);
    int tx = ox, tz = oz, ey = 0, has_ey = 0;
    for (int yo = 0; yo < h; yo++) {
        int yy = oy + yo;
        if (yo >= lean_h && lean_steps > 0) { tx += HZ_DX[lean]; tz += HZ_DZ[lean]; lean_steps--; }
        if (place_log(tr, tx, yy, tz, -1)) { ey = yy + 1; has_ey = 1; }
    }
    if (has_ey) att_push(out, tx, ey, tz, 1, 0, 1, 1);
    tx = ox; tz = oz;
    int bdir = hz_random(r);
    if (bdir != lean) {
        int branch_pos = lean_h - frnd_int_bound(r, 2) - 1;
        int branch_steps = 1 + frnd_int_bound(r, 3);
        has_ey = 0;
        for (int yo = branch_pos; yo < h && branch_steps > 0; branch_steps--) {
            if (yo >= 1) {
                int yy = oy + yo;
                tx += HZ_DX[bdir]; tz += HZ_DZ[bdir];
                if (place_log(tr, tx, yy, tz, -1)) { ey = yy + 1; has_ey = 1; }
            }
            yo++;
        }
        if (has_ey) att_push(out, tx, ey, tz, 0, 0, 1, 1);
    }
}
/* ---- giant / mega_jungle ---- */
static void trunk_giant(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    place_below(tr, ox, oy - 1, oz); place_below(tr, ox + 1, oy - 1, oz); place_below(tr, ox, oy - 1, oz + 1); place_below(tr, ox + 1, oy - 1, oz + 1);
    for (int hh = 0; hh < h; hh++) {
        place_log_if_free(tr, ox, oy + hh, oz);
        if (hh < h - 1) { place_log_if_free(tr, ox + 1, oy + hh, oz); place_log_if_free(tr, ox + 1, oy + hh, oz + 1); place_log_if_free(tr, ox, oy + hh, oz + 1); }
    }
    att_push(out, ox, oy + h, oz, 0, 0, 2, 2);
}
static void trunk_mega_jungle(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r;
    trunk_giant(tr, h, ox, oy, oz, out);
    int bh = h - 2 - frnd_int_bound(r, 4);
    while (bh > h / 2) {
        float angle = frnd_float(r) * (float)(M_PI * 2.0);
        int bx = 0, bz = 0;
        for (int b = 0; b < 5; b++) {
            bx = (int)(1.5f + fm_cos(angle) * (float)b);
            bz = (int)(1.5f + fm_sin(angle) * (float)b);
            place_log(tr, ox + bx, oy + bh - 3 + b / 2, oz + bz, -1);
        }
        att_push(out, ox + bx, oy + bh, oz + bz, -2, 0, 1, 1);
        bh -= 2 + frnd_int_bound(r, 4);
    }
}
/* ---- dark_oak ---- */
static void trunk_dark_oak(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r;
    place_below(tr, ox, oy - 1, oz); place_below(tr, ox + 1, oy - 1, oz); place_below(tr, ox, oy - 1, oz + 1); place_below(tr, ox + 1, oy - 1, oz + 1);
    int lean = hz_random(r);
    int lean_h = h - frnd_int_bound(r, 4);
    int lean_steps = 2 - frnd_int_bound(r, 3);
    int tx = ox, tz = oz, ey = oy + h - 1;
    for (int dy = 0; dy < h; dy++) {
        if (dy >= lean_h && lean_steps > 0) { tx += HZ_DX[lean]; tz += HZ_DZ[lean]; lean_steps--; }
        int yy = oy + dy;
        if (tr_is_air_or_leaves(tr, tx, yy, tz)) {
            place_log(tr, tx, yy, tz, -1); place_log(tr, tx + 1, yy, tz, -1); place_log(tr, tx, yy, tz + 1, -1); place_log(tr, tx + 1, yy, tz + 1, -1);
        }
    }
    att_push(out, tx, ey, tz, 0, 0, 2, 2);
    for (int dx = -1; dx <= 2; dx++) for (int dz = -1; dz <= 2; dz++) {
        if ((dx < 0 || dx > 1 || dz < 0 || dz > 1) && frnd_int_bound(r, 3) <= 0) {
            int len = frnd_int_bound(r, 3) + 2;
            for (int by = 0; by < len; by++) place_log(tr, ox + dx, ey - by - 1, oz + dz, -1);
            att_push(out, ox + dx, ey, oz + dz, 0, 0, 1, 1);
        }
    }
}
/* ---- bending ---- */
static void trunk_bending(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r; const TrunkCfg *tp = &tr->t->tp;
    int dir = hz_random(r);
    int log_h = h - 1;
    int px = ox, py = oy, pz = oz;
    place_below(tr, px, py - 1, pz);
    for (int i = 0; i <= log_h; i++) {
        int rr = frnd_int_bound(r, 2);
        if (i + 1 >= log_h + rr) { px += HZ_DX[dir]; pz += HZ_DZ[dir]; }
        if (tr_valid_pos(tr, px, py, pz)) place_log(tr, px, py, pz, -1);
        if (i >= tp->min_height_for_leaves) att_push(out, px, py, pz, 0, 0, 1, 1);
        py++;
    }
    int dir_len = intprov_sample(tp->bend_length, r);
    for (int i = 0; i <= dir_len; i++) {
        if (tr_valid_pos(tr, px, py, pz)) place_log(tr, px, py, pz, -1);
        att_push(out, px, py, pz, 0, 0, 1, 1);
        px += HZ_DX[dir]; pz += HZ_DZ[dir];
    }
}
/* ---- upwards_branching ---- */
static void trunk_upwards(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r; const TrunkCfg *tp = &tr->t->tp;
    for (int hp = 0; hp < h; hp++) {
        int cur = oy + hp;
        int lx = ox, lz = oz;
        if (place_log(tr, ox, cur, oz, -1) && hp < h - 1 && frnd_float(r) < tp->place_branch_per_log) {
            int dir = hz_random(r);
            int branch_len = intprov_sample(tp->extra_branch_length, r);
            int s2 = intprov_sample(tp->extra_branch_length, r);
            int branch_pos = branch_len - s2 - 1; if (branch_pos < 0) branch_pos = 0;
            int steps = intprov_sample(tp->extra_branch_steps, r);
            /* placeBranch */
            int height_along = cur + branch_pos, bx = ox, bz = oz, idx = branch_pos;
            while (idx < h && steps > 0) {
                if (idx >= 1) {
                    int ph = cur + idx;
                    bx += HZ_DX[dir]; bz += HZ_DZ[dir];
                    height_along = ph;
                    if (place_log(tr, bx, ph, bz, -1)) height_along++;
                    att_push(out, bx, ph, bz, 0, 0, 1, 1);
                }
                idx++; steps--;
            }
            if (height_along - cur > 1) {
                att_push(out, bx, height_along, bz, 0, 0, 1, 1);
                att_push(out, bx, height_along - 2, bz, 0, 0, 1, 1);
            }
        }
        (void)lx; (void)lz;
        if (hp == h - 1) att_push(out, ox, cur + 1, oz, 0, 0, 1, 1);
    }
}
/* ---- cherry ---- */
static void cherry_branch(TreeRun *tr, int h, int ox, int oy, int oz, int axis_dir_axis, int bdir_x, int bdir_z, int off, int middle_up, AttList *out) {
    FRnd *r = tr->r; const TrunkCfg *tp = &tr->t->tp;
    int lx = ox, ly = oy + off, lz = oz;
    int end_off = h - 1 + intprov_sample(tp->branch_end_offset, r);
    int extend = middle_up || end_off < off;
    int dist = intprov_sample(tp->branch_horizontal_length, r) + (extend ? 1 : 0);
    int ex = ox + bdir_x * dist, ey = oy + end_off, ez = oz + bdir_z * dist;
    int steps = extend ? 2 : 1;
    for (int i = 0; i < steps; i++) { lx += bdir_x; lz += bdir_z; place_log(tr, lx, ly, lz, axis_dir_axis); }
    int vert_up = ey > ly;
    for (;;) {
        int dx = lx > ex ? lx - ex : ex - lx, dy = ly > ey ? ly - ey : ey - ly, dz = lz > ez ? lz - ez : ez - lz;
        int d = dx + dy + dz;
        if (d == 0) { att_push(out, ex, ey + 1, ez, 0, 0, 1, 1); return; }
        float chance = (float)dy / (float)d;
        int gv = frnd_float(r) < chance;
        if (gv) ly += vert_up ? 1 : -1; else { lx += bdir_x; lz += bdir_z; }
        place_log(tr, lx, ly, lz, gv ? -1 : axis_dir_axis);
    }
}
static void trunk_cherry(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r; const TrunkCfg *tp = &tr->t->tp;
    place_below(tr, ox, oy - 1, oz);
    int a = h - 1 + frnd_between(r, tp->bs_min, tp->bs_max); int first = a > 0 ? a : 0;
    int b = h - 1 + frnd_between(r, tp->bs_min, tp->bs_max - 1); int second = b > 0 ? b : 0;
    if (second >= first) second++;
    int bc = intprov_sample(tp->branch_count, r);
    int has_mid = bc == 3, has_both = bc >= 2;
    int trunk_h = has_mid ? h : (has_both ? (first > second ? first : second) + 1 : first + 1);
    for (int y = 0; y < trunk_h; y++) place_log(tr, ox, oy + y, oz, -1);
    if (has_mid) att_push(out, ox, oy + trunk_h, oz, 0, 0, 1, 1);
    int dir = hz_random(r);
    int axis = HZ_DX[dir] != 0 ? 0 : 2;
    cherry_branch(tr, h, ox, oy, oz, axis, HZ_DX[dir], HZ_DZ[dir], first, first < trunk_h - 1, out);
    if (has_both) cherry_branch(tr, h, ox, oy, oz, axis, -HZ_DX[dir], -HZ_DZ[dir], second, second < trunk_h - 1, out);
}
/* ---- poplar ---- */
static void trunk_poplar(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r; const TrunkCfg *tp = &tr->t->tp;
    place_below(tr, ox, oy - 1, oz);
    int top = h - intprov_sample(tp->trunk_height_above_branches, r);
    for (int y = 0; y < h; y++) {
        place_log(tr, ox, oy + y, oz, -1);
        /* Direction.allShuffled(random) без вертикальных: Util.shuffle по 6 направлениям (DOWN, UP, NORTH, SOUTH, WEST, EAST) */
        int dirs[6] = { DIR_DOWN, DIR_UP, DIR_NORTH, DIR_SOUTH, DIR_WEST, DIR_EAST };
        for (int i = 6; i > 1; i--) { int j = frnd_int_bound(r, i); int t = dirs[i - 1]; dirs[i - 1] = dirs[j]; dirs[j] = t; }
        int hd[4], nh = 0;
        for (int i = 0; i < 6; i++) if (dirs[i] != DIR_DOWN && dirs[i] != DIR_UP) hd[nh++] = dirs[i];
        if (top - 1 == y) {
            int branches = intprov_sample(tp->branch_amount, r);
            for (int x = 0; x < branches; x++) {
                int d = hd[x];
                place_log(tr, ox + DIR_DX[d], oy + y, oz + DIR_DZ[d], DIR_DX[d] != 0 ? 0 : 2);
            }
        }
    }
    att_push(out, ox, oy + top, oz, 0, 0, 1, 1);
}
/* ---- fancy ---- */
typedef struct FCoord { int x, y, z, base; } FCoord;
static int fancy_free_or_place(TreeRun *tr, int sx, int sy, int sz, int ex, int ey, int ez, int do_place) {
    if (!do_place && sx == ex && sy == ey && sz == ez) return 1;
    int dx = ex - sx, dy = ey - sy, dz = ez - sz;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, az = dz < 0 ? -dz : dz;
    int steps = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
    float fx = (float)dx / (float)steps, fy = (float)dy / (float)steps, fz = (float)dz / (float)steps;
    for (int i = 0; i <= steps; i++) {
        int px = sx + mth_floor_f(0.5f + (float)i * fx), py = sy + mth_floor_f(0.5f + (float)i * fy), pz = sz + mth_floor_f(0.5f + (float)i * fz);
        if (do_place) {
            int xd = px - sx; if (xd < 0) xd = -xd; int zd = pz - sz; if (zd < 0) zd = -zd;
            int md = xd > zd ? xd : zd, axis = 1;
            if (md > 0) axis = xd == md ? 0 : 2;
            place_log(tr, px, py, pz, axis);
        } else if (!tr_is_free(tr, px, py, pz)) return 0;
    }
    return 1;
}
static float fancy_shape(int height, int y) {
    if ((float)y < (float)height * 0.3f) return -1.0f;
    float radius = (float)height / 2.0f, adjacent = radius - (float)y;
    float distance = sqrtf(radius * radius - adjacent * adjacent);
    if (adjacent == 0.0f) distance = radius;
    else if (fabsf(adjacent) >= radius) return 0.0f;
    return distance * 0.5f;
}
static void trunk_fancy(TreeRun *tr, int treeh, int ox, int oy, int oz, AttList *out) {
    FRnd *r = tr->r;
    int height = treeh + 2;
    int trunk_h = mth_floor_d((double)height * 0.618);
    place_below(tr, ox, oy - 1, oz);
    double q = (double)height / 13.0;
    int cpy = mth_floor_d(1.382 + q * q); if (cpy > 1) cpy = 1;       /* Math.min(1, Mth.floor(1.382 + Math.pow(h / 13, 2))) */
    int top = oy + trunk_h, rel_y = height - 5;
    FCoord *fc = xmalloc(sizeof(FCoord) * 256); int nfc = 0, capfc = 256;
    fc[nfc++] = (FCoord){ ox, oy + rel_y, oz, top };
    for (; rel_y >= 0; rel_y--) {
        float shape = fancy_shape(height, rel_y);
        if (shape < 0.0f) continue;
        for (int i = 0; i < cpy; i++) {
            double radius = 1.0 * (double)shape * ((double)frnd_float(r) + 0.328);
            double angle = (double)(frnd_float(r) * 2.0f) * M_PI;
            double x = radius * sin(angle) + 0.5, z = radius * cos(angle) + 0.5;
            int sx = ox + mth_floor_d(x), sy = oy + rel_y - 1, sz = oz + mth_floor_d(z);
            int ex = sx, ey = sy + 5, ez = sz;
            if (fancy_free_or_place(tr, sx, sy, sz, ex, ey, ez, 0)) {
                int dx = ox - sx, dz = oz - sz;
                double bh = (double)sy - sqrt((double)(dx * dx + dz * dz)) * 0.381;
                int btop = bh > (double)top ? top : (int)bh;
                if (fancy_free_or_place(tr, ox, btop, oz, sx, sy, sz, 0)) {
                    if (nfc == capfc) { capfc *= 2; fc = realloc(fc, sizeof(FCoord) * (size_t)capfc); }
                    fc[nfc++] = (FCoord){ sx, sy, sz, btop };
                }
            }
        }
    }
    fancy_free_or_place(tr, ox, oy, oz, ox, oy + trunk_h, oz, 1);
    /* makeBranches */
    for (int i = 0; i < nfc; i++) {
        int base = fc[i].base;
        if (!(ox == fc[i].x && base == fc[i].y && oz == fc[i].z) && (double)(base - oy) >= (double)height * 0.2)
            fancy_free_or_place(tr, ox, base, oz, fc[i].x, fc[i].y, fc[i].z, 1);
    }
    for (int i = 0; i < nfc; i++) if ((double)(fc[i].base - oy) >= (double)height * 0.2) att_push(out, fc[i].x, fc[i].y, fc[i].z, 0, 0, 1, 1);
    free(fc);
}

void tp_place_trunk(TreeRun *tr, int h, int ox, int oy, int oz, AttList *out) {
    switch (tr->t->tp.type) {
    case TP_STRAIGHT: trunk_straight(tr, h, ox, oy, oz, out); break;
    case TP_FORKING: trunk_forking(tr, h, ox, oy, oz, out); break;
    case TP_GIANT: trunk_giant(tr, h, ox, oy, oz, out); break;
    case TP_MEGA_JUNGLE: trunk_mega_jungle(tr, h, ox, oy, oz, out); break;
    case TP_DARK_OAK: trunk_dark_oak(tr, h, ox, oy, oz, out); break;
    case TP_FANCY: trunk_fancy(tr, h, ox, oy, oz, out); break;
    case TP_BENDING: trunk_bending(tr, h, ox, oy, oz, out); break;
    case TP_UPWARDS: trunk_upwards(tr, h, ox, oy, oz, out); break;
    case TP_CHERRY: trunk_cherry(tr, h, ox, oy, oz, out); break;
    case TP_POPLAR: trunk_poplar(tr, h, ox, oy, oz, out); break;
    }
}

/* ====================================================================== FoliagePlacer */
static int leaf_try_place(TreeRun *tr, int x, int y, int z) {      /* FoliagePlacer.tryPlaceLeaf */
    FCtx *c = tr->c; const BsTab *bs = c->bs;
    int cur = fc_get(c, x, y, z);
    if (!fc_is_air(c, cur)) {
        const char *v = NULL;
        if (bs_get_prop(bs, cur, "persistent", &v) && v && v[0] == 't') return 0;          /* уже persistent */
    }
    if (!tr_valid_pos(tr, x, y, z)) return 0;
    int st = bsprov_state(c, tr->t->foliage_prov, x, y, z);
    if (bs_has_prop(bs, st, "waterlogged")) {
        int f = bs->fluid[cur]; int src = BS_FL_TYPE(f) == FL_WATER;      /* isSourceOfType(WATER) */
        int ns = bs_with(bs, st, "waterlogged", src ? "true" : "false"); if (ns >= 0) st = ns;
    }
    tr_set_foliage(tr, x, y, z, st);
    return 1;
}

static int skip_base(FRnd *r, int type, int dx, int y, int dz, int cr, int dt, const FoliageCfg *fp);
static int skip_signed(FRnd *r, const FoliageCfg *fp, int dx, int y, int dz, int cr, int dt) {
    if (fp->type == FP_DARK_OAK) {
        /* y != 0 || !doubleTrunk || dx != -r && dx < r || dz != -r && dz < r ? super : true */
        if (y != 0 || !dt || (dx != -cr && dx < cr) || (dz != -cr && dz < cr)) goto base;
        return 1;
    }
base:;
    int mdx, mdz;
    if (dt) { int a = dx < 0 ? -dx : dx, b = dx - 1 < 0 ? 1 - dx : dx - 1; mdx = a < b ? a : b; a = dz < 0 ? -dz : dz; b = dz - 1 < 0 ? 1 - dz : dz - 1; mdz = a < b ? a : b; }
    else { mdx = dx < 0 ? -dx : dx; mdz = dz < 0 ? -dz : dz; }
    return skip_base(r, fp->type, mdx, y, mdz, cr, dt, fp);
}
static int skip_base(FRnd *r, int type, int dx, int y, int dz, int cr, int dt, const FoliageCfg *fp) {
    switch (type) {
    case FP_BLOB: return dx == cr && dz == cr && (frnd_int_bound(r, 2) == 0 || y == 0);
    case FP_SPRUCE: case FP_PINE: return dx == cr && dz == cr && cr > 0;
    case FP_ACACIA: return y == 0 ? ((dx > 1 || dz > 1) && dx != 0 && dz != 0) : (dx == cr && dz == cr && cr > 0);
    case FP_BUSH: return dx == cr && dz == cr && frnd_int_bound(r, 2) == 0;
    case FP_FANCY: { float a = (float)dx + 0.5f, b = (float)dz + 0.5f; return a * a + b * b > (float)(cr * cr); }
    case FP_MEGA_JUNGLE: case FP_MEGA_PINE: return dx + dz >= 7 ? 1 : dx * dx + dz * dz > cr * cr;
    case FP_DARK_OAK:
        if (y == -1 && !dt) return dx == cr && dz == cr;
        return y == 1 ? dx + dz > cr * 2 - 2 : 0;
    case FP_RANDOM_SPREAD: return 0;
    case FP_CHERRY: {
        if (y == -1 && (dx == cr || dz == cr) && frnd_float(r) < fp->wide_bottom_hole) return 1;
        int corner = dx == cr && dz == cr, wide = cr > 2;
        if (wide) return corner || (dx + dz > cr * 2 - 2 && frnd_float(r) < fp->corner_hole);
        return corner && frnd_float(r) < fp->corner_hole;
    }
    }
    return 0;
}
static void leaves_row(TreeRun *tr, const FoliageCfg *fp, int ox, int oy, int oz, int cr, int y, int dt) {
    int off = dt ? 1 : 0;
    for (int dx = -cr; dx <= cr + off; dx++) for (int dz = -cr; dz <= cr + off; dz++)
        if (!skip_signed(tr->r, fp, dx, y, dz, cr, dt)) leaf_try_place(tr, ox + dx, oy + y, oz + dz);
}
static int try_place_extension(TreeRun *tr, float chance, int lx, int ly, int lz, int px, int py, int pz) {
    int d = abs(px - lx) + abs(py - ly) + abs(pz - lz);
    if (d >= 7) return 0;
    return frnd_float(tr->r) > chance ? 0 : leaf_try_place(tr, px, py, pz);
}
static void leaves_row_hanging(TreeRun *tr, const FoliageCfg *fp, int ox, int oy, int oz, int cr, int y, int dt, float chance, float chance_ext) {
    leaves_row(tr, fp, ox, oy, oz, cr, y, dt);
    int off = dt ? 1 : 0;
    int lx = ox, ly = oy - 1, lz = oz;                       /* logPos = origin.below() */
    /* Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST; toEdge = clockwise */
    for (int k = 0; k < 4; k++) {
        int ax = HZ_DX[k], az = HZ_DZ[k];                   /* alongEdge */
        int cw = (k + 1) & 3; int tx = HZ_DX[cw], tz = HZ_DZ[cw];       /* toEdge = getClockWise */
        int pos_dir = (tx + tz) > 0;                         /* AxisDirection.POSITIVE: EAST (+x) / SOUTH (+z) */
        int off_edge = pos_dir ? cr + off : cr;
        int px = ox + tx * off_edge - ax * cr, py = oy + y - 1, pz = oz + tz * off_edge - az * cr;
        int along = -cr;
        while (along < cr + off) {
            int above = jset_contains(tr->foliage, px, py + 1, pz);
            if (above && try_place_extension(tr, chance, lx, ly, lz, px, py, pz))
                try_place_extension(tr, chance_ext, lx, ly, lz, px, py - 1, pz);
            along++; px += ax; pz += az;
        }
    }
}

int fp_foliage_height(const FoliageCfg *fp, FRnd *r, int tree_h) {
    switch (fp->type) {
    case FP_BLOB: case FP_BUSH: case FP_FANCY: case FP_MEGA_JUNGLE: return fp->height;
    case FP_SPRUCE: { int v = intprov_sample(fp->height_ip, r); int d = tree_h - v; return d > 4 ? d : 4; }
    case FP_ACACIA: return 0;
    case FP_DARK_OAK: return 4;
    default: return intprov_sample(fp->height_ip, r);    /* pine, mega_pine, random_spread, cherry, poplar */
    }
}
int fp_foliage_radius(const FoliageCfg *fp, FRnd *r, int trunk_h) {
    int v = intprov_sample(fp->radius, r);
    if (fp->type == FP_PINE) { int b = trunk_h + 1; if (b < 1) b = 1; v += frnd_int_bound(r, b); }
    return v;
}

/* ---- Poplar ---- */
static int pop_row_partial(int fh, int y) { return fh - 1 == y || fh - 2 == y; }
static int pop_corner_cut(int dx, int dz, int cr, int partial, int flip) {
    int small = flip ? ((dx > 0 && dz > 0) || (dz < 0 && dx < 0)) : ((dx > 0 && dz < 0) || (dz > 0 && dx < 0));
    return small ? cr - 1 : (partial ? cr + 1 : cr);
}
static int pop_within(int cr, int adx, int adz, int cut, int extra) { return adx + adz <= cr * 2 - (cut + extra); }
static void pop_row(TreeRun *tr, const FoliageCfg *fp, int ox, int oy, int oz, int cr, int y, int dt, int fh, int flip) {
    int off = dt ? 1 : 0;
    for (int dx = -cr; dx <= cr + off; dx++) for (int dz = -cr; dz <= cr + off; dz++) {
        int partial = pop_row_partial(fh, y);
        int cut = pop_corner_cut(dx, dz, cr, partial, flip);
        int adx = abs(dx), adz = abs(dz);
        int edge = adx == cr || adz == cr;
        int skip;
        if (partial && edge) skip = 1;
        else { int extra = frnd_float(tr->r) <= fp->side_hole ? 1 : 0; skip = !pop_within(cr, adx, adz, cut, extra); }
        if (!skip) leaf_try_place(tr, ox + dx, oy + y, oz + dz);
    }
}
static void pop_replace_with_log(TreeRun *tr, int ox, int oy, int oz, int cr, int y, int dt, int fh, int flip) {
    FCtx *c = tr->c; int off = dt ? 1 : 0;
    for (int dx = -cr; dx <= cr + off; dx++) for (int dz = -cr; dz <= cr + off; dz++) {
        int adz = abs(dz), adx = abs(dx);
        int cut = pop_corner_cut(dx, dz, cr, pop_row_partial(fh, y), flip);
        if (pop_within(cr, adx, adz, cut, 2) && ((adz == 0 && cr - adx >= 4) || (adx == 0 && cr - adz >= 4))) {
            int px = ox + dx, py = oy + y, pz = oz + dz;
            int want = bsprov_state(c, tr->t->foliage_prov, px, py, pz);
            if (fc_get(c, px, py, pz) == want) {
                int st = bsprov_state(c, tr->t->trunk_prov, px, py, pz);
                st = tree_state_axis(c->bs, st, adz == 0 ? 0 : 2);
                tr_set_foliage(tr, px, py, pz, st);
            }
        }
    }
}

void fp_create_foliage(TreeRun *tr, int tree_h, const FoliageAtt *att, int fol_h, int leaf_r) {
    const FoliageCfg *fp = &tr->t->fp; FRnd *r = tr->r;
    int offset = intprov_sample(fp->offset, r);
    int dt = att->size_x == 2 && att->size_z == 2;
    int ax = att->x, ay = att->y, az = att->z;
    int fhw = fol_h + att->height_off;
    switch (fp->type) {
    case FP_BLOB: case FP_BUSH: {
        for (int yo = offset; yo >= offset - fhw; yo--) {
            int cr = fp->type == FP_BLOB ? leaf_r + att->radius_off - 1 - yo / 2 : leaf_r + att->radius_off - 1 - yo;
            if (fp->type == FP_BLOB && cr < 0) cr = 0;
            leaves_row(tr, fp, ax, ay, az, cr, yo, dt);
        }
        break;
    }
    case FP_FANCY:
        for (int yo = offset; yo >= offset - fol_h; yo--) {
            int cr = leaf_r + (yo != offset && yo != offset - fol_h ? 1 : 0);
            leaves_row(tr, fp, ax, ay, az, cr, yo, dt);
        }
        break;
    case FP_SPRUCE: {
        int cr = frnd_int_bound(r, 2), maxr = 1, minr = 0;
        for (int yo = offset; yo >= -fhw; yo--) {
            leaves_row(tr, fp, ax, ay, az, cr, yo, dt);
            if (cr >= maxr) { cr = minr; minr = 1; int m = maxr + 1, lim = leaf_r + att->radius_off; maxr = m < lim ? m : lim; }
            else cr++;
        }
        break;
    }
    case FP_PINE: {
        int cr = 0;
        for (int yo = offset; yo >= offset - fhw; yo--) {
            leaves_row(tr, fp, ax, ay, az, cr, yo, dt);
            if (cr >= 1 && yo == offset - fhw + 1) cr--;
            else if (cr < leaf_r + att->radius_off) cr++;
        }
        break;
    }
    case FP_ACACIA: {
        int py = ay + offset;
        leaves_row(tr, fp, ax, py, az, leaf_r + att->radius_off, -1 - fhw, dt);
        leaves_row(tr, fp, ax, py, az, leaf_r - 1, -fhw, dt);
        leaves_row(tr, fp, ax, py, az, leaf_r + att->radius_off - 1, 0, dt);
        break;
    }
    case FP_MEGA_JUNGLE: {
        int lh = (dt ? fol_h : 1 + frnd_int_bound(r, 2)) + att->height_off;
        for (int yo = offset; yo >= offset - lh; yo--) {
            int cr = leaf_r + att->radius_off + 1 - yo;
            leaves_row(tr, fp, ax, ay, az, cr, yo, dt);
        }
        break;
    }
    case FP_MEGA_PINE: {
        int prev = 0;
        for (int yy = ay - fhw + offset; yy <= ay + offset; yy++) {
            int yo = ay - yy;
            int smooth = leaf_r + att->radius_off + mth_floor_f((float)yo / (float)fhw * 3.5f);
            int jag = (yo > 0 && smooth == prev && (yy & 1) == 0) ? smooth + 1 : smooth;
            leaves_row(tr, fp, ax, yy, az, jag, 0, dt);
            prev = smooth;
        }
        break;
    }
    case FP_DARK_OAK: {
        int py = ay + offset;
        if (dt) {
            leaves_row(tr, fp, ax, py, az, leaf_r + 2, -1, dt);
            leaves_row(tr, fp, ax, py, az, leaf_r + 3, 0, dt);
            leaves_row(tr, fp, ax, py, az, leaf_r + 2, 1, dt);
            if (frnd_bool(r)) leaves_row(tr, fp, ax, py, az, leaf_r, 2, dt);
        } else {
            leaves_row(tr, fp, ax, py, az, leaf_r + 2, -1, dt);
            leaves_row(tr, fp, ax, py, az, leaf_r + 1, 0, dt);
        }
        break;
    }
    case FP_RANDOM_SPREAD: {
        for (int i = 0; i < fp->leaf_attempts; i++) {
            int a = frnd_int_bound(r, leaf_r), b = frnd_int_bound(r, leaf_r); int dx = a - b;
            a = frnd_int_bound(r, fol_h); b = frnd_int_bound(r, fol_h); int dy = a - b;
            a = frnd_int_bound(r, leaf_r); b = frnd_int_bound(r, leaf_r); int dz = a - b;
            leaf_try_place(tr, ax + dx, ay + dy, az + dz);
        }
        break;
    }
    case FP_CHERRY: {
        int py = ay + offset, cr = leaf_r + att->radius_off - 1;
        leaves_row(tr, fp, ax, py, az, cr - 2, fhw - 3, dt);
        leaves_row(tr, fp, ax, py, az, cr - 1, fhw - 4, dt);
        for (int y = fhw - 5; y >= 0; y--) leaves_row(tr, fp, ax, py, az, cr, y, dt);
        leaves_row_hanging(tr, fp, ax, py, az, cr, -1, dt, fp->hanging, fp->hanging_ext);
        leaves_row_hanging(tr, fp, ax, py, az, cr - 1, -2, dt, fp->hanging, fp->hanging_ext);
        break;
    }
    case FP_POPLAR: {
        int py = ay + offset, cr = leaf_r + att->radius_off - 1;
        int flip = frnd_bool(r);
        pop_row(tr, fp, ax, py, az, cr - 2, fhw - 1, dt, fhw, flip);
        pop_row(tr, fp, ax, py, az, cr - 1, fhw - 2, dt, fhw, flip);
        pop_row(tr, fp, ax, py, az, cr - 1, fhw - 3, dt, fhw, flip);
        for (int y = fhw - 4; y >= 1; y--) pop_row(tr, fp, ax, py, az, cr, y, dt, fhw, flip);
        pop_replace_with_log(tr, ax, py, az, cr, fhw - 4, dt, fhw, flip);
        pop_row(tr, fp, ax, py, az, cr - 1, 0, dt, fhw, flip);
        int c2 = cr - 2; c2 = c2 < 1 ? 1 : (c2 > 2 ? 2 : c2);
        pop_row(tr, fp, ax, py, az, c2, -1, dt, fhw, flip);
        break;
    }
    }
}

/* ====================================================================== RootPlacer (mangrove) */
static int root_can_place(const TreeRun *tr, int x, int y, int z) {
    if (tr_valid_pos(tr, x, y, z)) return 1;
    return tr->t->root->can_grow_through[blk_of(tr->c, fc_get(tr->c, x, y, z))] != 0;
}
static int root_waterlogged_state(const TreeRun *tr, int x, int y, int z, int st) {
    const BsTab *bs = tr->c->bs;
    if (!bs_has_prop(bs, st, "waterlogged")) return st;
    int t = BS_FL_TYPE(bs->fluid[fc_get(tr->c, x, y, z)]);
    int wl = t == FL_WATER || t == FL_FLOWING_WATER;
    int ns = bs_with(bs, st, "waterlogged", wl ? "true" : "false");
    return ns >= 0 ? ns : st;
}
static void root_place_one(TreeRun *tr, int x, int y, int z) {
    const RootCfg *rc = tr->t->root; FCtx *c = tr->c;
    if (rc->muddy_roots_in[blk_of(c, fc_get(c, x, y, z))]) {
        int st = bsprov_state(c, rc->muddy_provider, x, y, z);
        tr_set_root(tr, x, y, z, root_waterlogged_state(tr, x, y, z, st));
        return;
    }
    if (!root_can_place(tr, x, y, z)) return;
    int st = bsprov_state(c, rc->root_provider, x, y, z);
    tr_set_root(tr, x, y, z, root_waterlogged_state(tr, x, y, z, st));
    if (rc->has_above) {
        if (frnd_float(tr->r) < rc->above_chance && fc_is_air(c, fc_get(c, x, y + 1, z))) {
            int ast = bsprov_state(c, rc->above_provider, x, y + 1, z);
            tr_set_root(tr, x, y + 1, z, root_waterlogged_state(tr, x, y + 1, z, ast));
        }
    }
}
typedef struct RPos { int x, y, z; } RPos;
typedef struct RList { RPos *a; int n, cap; } RList;
static void rl_push(RList *l, int x, int y, int z) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->a = realloc(l->a, sizeof(RPos) * (size_t)l->cap); if (!l->a) abort(); }
    l->a[l->n++] = (RPos){ x, y, z };
}
static int root_simulate(TreeRun *tr, int px, int py, int pz, int dir, int rx, int ry, int rz, RList *out, int layer) {
    const RootCfg *rc = tr->t->root; FRnd *r = tr->r;
    int maxlen = rc->max_length;
    if (getenv("MCGEN_ROOT_DEBUG") && getenv("MCGEN_ROOT_DEBUG")[0]=='2') fprintf(stderr, "ROOTSIM layer=%d pos=%d,%d,%d block=%s\n", layer, px, py, pz, tr->c->bs->blk[blk_of(tr->c, fc_get(tr->c, px, py, pz))].name);
    if (layer == maxlen || out->n > maxlen) { if (getenv("MCGEN_ROOT_DEBUG")) fprintf(stderr, "ROOTFAIL layer=%d n=%d at %d,%d,%d\n", layer, out->n, px, py, pz); return 0; }
    /* potentialRootPositions */
    int belowx = px, belowy = py - 1, belowz = pz;
    int nx = px + HZ_DX[dir], ny = py, nz = pz + HZ_DZ[dir];
    int width = abs(px - rx) + abs(py - ry) + abs(pz - rz);
    int maxw = rc->max_width; float skew = rc->skew;
    RPos cand[2]; int nc = 0;
    if (width > maxw - 3 && width <= maxw) {
        if (frnd_float(r) < skew) { cand[0] = (RPos){ belowx, belowy, belowz }; cand[1] = (RPos){ nx, ny - 1, nz }; nc = 2; }
        else { cand[0] = (RPos){ belowx, belowy, belowz }; nc = 1; }
    } else if (width > maxw) { cand[0] = (RPos){ belowx, belowy, belowz }; nc = 1; }
    else if (frnd_float(r) < skew) { cand[0] = (RPos){ belowx, belowy, belowz }; nc = 1; }
    else { if (frnd_bool(r)) cand[0] = (RPos){ nx, ny, nz }; else cand[0] = (RPos){ belowx, belowy, belowz }; nc = 1; }
    for (int i = 0; i < nc; i++) {
        if (root_can_place(tr, cand[i].x, cand[i].y, cand[i].z)) {
            rl_push(out, cand[i].x, cand[i].y, cand[i].z);
            if (!root_simulate(tr, cand[i].x, cand[i].y, cand[i].z, dir, rx, ry, rz, out, layer + 1)) return 0;
        }
    }
    return 1;
}
int root_place(TreeRun *tr, int ox, int oy, int oz, int tox, int toy, int toz) {
    RList all = {0}; int ok = 1;
    for (int cy = oy; cy < toy; cy++) if (!root_can_place(tr, ox, cy, oz)) { ok = 0; break; }
    if (!ok) return 0;
    rl_push(&all, tox, toy - 1, toz);
    for (int d = 0; d < 4 && ok; d++) {
        RList dirl = {0};
        if (!root_simulate(tr, tox + HZ_DX[d], toy, toz + HZ_DZ[d], d, tox, toy, toz, &dirl, 0)) ok = 0;
        else {
            for (int i = 0; i < dirl.n; i++) rl_push(&all, dirl.a[i].x, dirl.a[i].y, dirl.a[i].z);
            rl_push(&all, tox + HZ_DX[d], toy, toz + HZ_DZ[d]);
        }
        free(dirl.a);
    }
    if (ok) for (int i = 0; i < all.n; i++) root_place_one(tr, all.a[i].x, all.a[i].y, all.a[i].z);
    free(all.a);
    return ok;
}

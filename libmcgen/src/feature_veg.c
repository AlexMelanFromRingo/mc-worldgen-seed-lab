/* feature_veg.c — растительность (поток W10): block_column, bamboo, vines, vegetation_patch, waterlogged_vegetation_patch
 * и BlockState.canSurvive для классов растений (veg_survive, вызывается из block_can_survive). Подробно — docs/blender/features-veg.md. */
#include "feature_veg.h"
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== утилиты */
const u8 *veg_tag(const FCtx *c, const char *tag) {
    static _Thread_local const McGen *cg; static _Thread_local const char *ck[48]; static _Thread_local const u8 *cv[48]; static _Thread_local int cn;
    if (cg != c->g) { cg = c->g; cn = 0; }
    for (int i = 0; i < cn; i++) if (ck[i] == tag) return cv[i];
    const u8 *t = gen_block_tag(c->g, tag);
    if (cn < 48) { ck[cn] = tag; cv[cn++] = t; }
    return t;
}

/* veg_hashset_order — feature_veg_hash.c */

/* ====================================================================== WorldGenRegion.getRandom() */
Rnd *fc_region_random(FCtx *c) {
    if (!c->region_rnd_ready) {
        Rnd r1 = pos_from_hash(&c->w->pos_terrain, "minecraft:worldgen_region_random");   /* RandomState.getOrCreateRandomFactory(...).forkPositional() */
        PosRnd f = rnd_fork_positional(&r1);
        c->region_rnd = pos_at(&f, c->ccx * 16, 0, c->ccz * 16);                          /* .at(центральный чанк.getWorldPosition()) */
        c->region_rnd_ready = 1;
    }
    return &c->region_rnd;
}

/* ====================================================================== block_column */
typedef struct ColLayer { IntProv *height; BSProv *prov; } ColLayer;
typedef struct ColCfg { int n; ColLayer *layers; int dir; BPred *allowed; int prio_tip; } ColCfg;

static void *col_parse(FParse *p, const Js *cfg) {
    ColCfg *s = fp_alloc(p, sizeof *s);
    const Js *l = js_get(cfg, "layers");
    if (!js_is_arr(l)) { fp_fail(p, "block_column: нет layers"); return NULL; }
    s->n = l->n; s->layers = fp_alloc(p, sizeof(ColLayer) * (size_t)(l->n ? l->n : 1));
    for (int i = 0; i < l->n; i++) {
        s->layers[i].height = fp_intprov(p, js_get(l->items[i], "height")); if (!s->layers[i].height) return NULL;
        s->layers[i].prov = fp_bsprov(p, js_get(l->items[i], "provider")); if (!s->layers[i].prov) return NULL;
    }
    s->dir = dir_from_name(js_str(js_get(cfg, "direction"), "up")); if (s->dir < 0) { fp_fail(p, "block_column: плохое direction"); return NULL; }
    s->allowed = fp_bpred(p, js_get(cfg, "allowed_placement")); if (!s->allowed) return NULL;
    s->prio_tip = js_bool(js_get(cfg, "prioritize_tip"), 0);
    return s;
}
static int col_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const ColCfg *s = cfg;
    int h[16], total = 0;
    if (s->n > 16) return 0;
    for (int i = 0; i < s->n; i++) { h[i] = intprov_sample(s->layers[i].height, c->rnd); total += h[i]; }
    if (total == 0) return 0;
    int dx = DIR_DX[s->dir], dy = DIR_DY[s->dir], dz = DIR_DZ[s->dir];
    int nx = x + dx, ny = y + dy, nz = z + dz;
    for (int k = 0; k < total; k++) {
        if (!bpred_test(c, s->allowed, nx, ny, nz)) {
            /* truncate(layerHeights, totalHeight, k, prioritizeTip) */
            int remove = total - k;
            int step = s->prio_tip ? 1 : -1, start = s->prio_tip ? 0 : s->n - 1, end = s->prio_tip ? s->n : -1;
            for (int i = start; i != end && remove > 0; i += step) { int r = h[i] < remove ? h[i] : remove; remove -= r; h[i] -= r; }
            break;
        }
        nx += dx; ny += dy; nz += dz;
    }
    int px = x, py = y, pz = z;
    for (int i = 0; i < s->n; i++) {
        for (int k = 0; k < h[i]; k++) {
            fc_set(c, px, py, pz, bsprov_state(c, s->layers[i].prov, px, py, pz), 2);
            px += dx; py += dy; pz += dz;
        }
    }
    return 1;
}

/* ====================================================================== bamboo */
typedef struct BambooCfg { float prob; int trunk, final_large, top_large, top_small, podzol; const u8 *beneath; } BambooCfg;
static void *bamboo_parse(FParse *p, const Js *cfg) {
    BambooCfg *b = fp_alloc(p, sizeof *b);
    b->prob = js_numf(js_get(cfg, "probability"), 0);
    int blk = bs_block_index(p->bs, "minecraft:bamboo");
    int pod = bs_block_index(p->bs, "minecraft:podzol");
    if (blk < 0 || pod < 0) { fp_fail(p, "bamboo: нет блоков"); return NULL; }
    int t = bs_default(p->bs, blk);
    t = bs_with(p->bs, t, "age", "1"); t = bs_with(p->bs, t, "leaves", "none"); t = bs_with(p->bs, t, "stage", "0");
    b->trunk = t;
    b->final_large = bs_with(p->bs, bs_with(p->bs, t, "leaves", "large"), "stage", "1");
    b->top_large = bs_with(p->bs, t, "leaves", "large");
    b->top_small = bs_with(p->bs, t, "leaves", "small");
    b->podzol = bs_default(p->bs, pod);
    if (b->trunk < 0 || b->final_large < 0 || b->top_large < 0 || b->top_small < 0) { fp_fail(p, "bamboo: нет свойств"); return NULL; }
    b->beneath = gen_block_tag(p->g, "minecraft:beneath_bamboo_podzol_replaceable");
    return b;
}
static int bamboo_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const BambooCfg *b = cfg; FRnd *r = c->rnd;
    if (!fc_is_empty_block(c, ox, oy, oz)) return 0;
    if (veg_in_tag(c, veg_tag(c, "minecraft:supports_bamboo"), fc_get(c, ox, oy - 1, oz))) {
        int height = frnd_int_bound(r, 12) + 5;
        if (frnd_float(r) < b->prob) {
            int rr = frnd_int_bound(r, 4) + 1;
            for (int xx = ox - rr; xx <= ox + rr; xx++) for (int zz = oz - rr; zz <= oz + rr; zz++) {
                int xd = xx - ox, zd = zz - oz;
                if (xd * xd + zd * zd <= rr * rr) {
                    int py = fc_height(c, HM_WORLD_SURFACE, xx, zz) - 1;
                    if (veg_in_tag(c, b->beneath, fc_get(c, xx, py, zz))) fc_set(c, xx, py, zz, b->podzol, 2);
                }
            }
        }
        int y = oy;
        for (int i = 0; i < height && fc_is_empty_block(c, ox, y, oz); i++) { fc_set(c, ox, y, oz, b->trunk, 2); y++; }
        if (y - oy >= 3) {
            fc_set(c, ox, y, oz, b->final_large, 2);
            fc_set(c, ox, --y, oz, b->top_large, 2);
            fc_set(c, ox, --y, oz, b->top_small, 2);
        }
    }
    return 1;            /* placed++ даже если бамбук не выживает (в игре: placed++ внутри if isEmptyBlock) */
}

/* ====================================================================== vines */
typedef struct VinesCfg { int st[6]; } VinesCfg;
static void *vines_parse(FParse *p, const Js *cfg) {
    VinesCfg *v = fp_alloc(p, sizeof *v);
    int blk = bs_block_index(p->bs, "minecraft:vine");
    if (blk < 0) { fp_fail(p, "vines: нет блока vine"); return NULL; }
    static const char *PN[6] = { NULL, "up", "north", "south", "west", "east" };
    for (int d = 1; d < 6; d++) v->st[d] = bs_with(p->bs, bs_default(p->bs, blk), PN[d], "true");
    return v;
}
/* MultifaceBlock.canAttachTo: полная грань опорной формы или формы столкновения */
static inline int veg_attach(const FCtx *c, int st, int towards_dir) {
    int opp = towards_dir ^ 1;                         /* DOWN↔UP, NORTH↔SOUTH, WEST↔EAST: пары соседние в нумерации */
    return veg_sturdy(c, st, opp) || (c->bs->flags[st] & BSF_FULL_COLL);
}
static int vines_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const VinesCfg *v = cfg;
    if (!fc_is_empty_block(c, x, y, z)) return 0;
    for (int d = 1; d < 6; d++) {
        int nb = fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]);
        if (veg_attach(c, nb, d)) { fc_set(c, x, y, z, v->st[d], 2); return 1; }
    }
    return 0;
}

/* ====================================================================== vegetation_patch / waterlogged_vegetation_patch */
typedef struct PatchCfg {
    const u8 *replaceable; BSProv *ground; Placed *veg; int surf_dir;     /* направление «внутрь» (FLOOR = DOWN, CEILING = UP) */
    IntProv *depth; float extra_bottom; int vrange; float veg_chance; IntProv *xz_radius; float extra_edge;
    int water;
} PatchCfg;
static void *patch_parse_common(FParse *p, const Js *cfg, int water) {
    PatchCfg *s = fp_alloc(p, sizeof *s);
    s->water = water;
    s->replaceable = fp_blockset(p, js_get(cfg, "replaceable")); if (!s->replaceable) return NULL;
    s->ground = fp_bsprov(p, js_get(cfg, "ground_state")); if (!s->ground) return NULL;
    s->veg = fp_placed(p, js_get(cfg, "vegetation_feature")); if (!s->veg) return NULL;
    const char *sf = js_str(js_get(cfg, "surface"), "floor");
    s->surf_dir = !strcmp(sf, "ceiling") ? DIR_UP : DIR_DOWN;
    s->depth = fp_intprov(p, js_get(cfg, "depth")); if (!s->depth) return NULL;
    s->extra_bottom = js_numf(js_get(cfg, "extra_bottom_block_chance"), 0);
    s->vrange = js_int(js_get(cfg, "vertical_range"), 0);
    s->veg_chance = js_numf(js_get(cfg, "vegetation_chance"), 0);
    s->xz_radius = fp_intprov(p, js_get(cfg, "xz_radius")); if (!s->xz_radius) return NULL;
    s->extra_edge = js_numf(js_get(cfg, "extra_edge_column_chance"), 0);
    return s;
}
static void *patch_parse(FParse *p, const Js *cfg) { return patch_parse_common(p, cfg, 0); }
static void *wpatch_parse(FParse *p, const Js *cfg) { return patch_parse_common(p, cfg, 1); }

/* placeGround: ground_state кладётся на depth блоков от belowPos вглубь; true, если хоть что-то поставлено или цикл завершён */
static int patch_ground(FCtx *c, const PatchCfg *s, int x, int *py, int z, int depth) {
    FRnd *r = c->rnd;
    for (int i = 0; i < depth; i++) {
        int st = bsprov_state(c, s->ground, x, *py, z);
        int below = fc_get(c, x, *py, z);
        if (c->g->state_block[st] != c->g->state_block[below]) {
            if (!s->replaceable[c->g->state_block[below]]) return i != 0;
            fc_set(c, x, *py, z, st, 2);
            *py += DIR_DY[s->surf_dir];
        }
    }
    (void)r;
    return 1;
}

static int patch_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const PatchCfg *s = cfg; FRnd *r = c->rnd;
    int xr = intprov_sample(s->xz_radius, r) + 1;
    int zr = intprov_sample(s->xz_radius, r) + 1;
    int inwards = s->surf_dir, outwards = s->surf_dir ^ 1;
    int cap = (2 * xr + 1) * (2 * zr + 1);
    VPos *surf = malloc(sizeof(VPos) * (size_t)cap); int ns = 0;
    for (int dx = -xr; dx <= xr; dx++) {
        int xedge = dx == -xr || dx == xr;
        for (int dz = -zr; dz <= zr; dz++) {
            int zedge = dz == -zr || dz == zr;
            int edge = xedge || zedge, corner = xedge && zedge, edge_not_corner = edge && !corner;
            if (!corner && (!edge_not_corner || (s->extra_edge != 0.0f && !(frnd_float(r) > s->extra_edge)))) {
                int px = ox + dx, py = oy, pz = oz + dz;
                for (int off = 0; fc_is_air(c, fc_get(c, px, py, pz)) && off < s->vrange; off++) py += DIR_DY[inwards];
                for (int off = 0; !fc_is_air(c, fc_get(c, px, py, pz)) && off < s->vrange; off++) py += DIR_DY[outwards];
                int by = py + DIR_DY[s->surf_dir];
                int below = fc_get(c, px, by, pz);
                if (fc_is_air(c, fc_get(c, px, py, pz)) && veg_sturdy(c, below, outwards)) {
                    int depth = intprov_sample(s->depth, r) + (s->extra_bottom > 0.0f && frnd_float(r) < s->extra_bottom ? 1 : 0);
                    int gy = by;
                    int placed = patch_ground(c, s, px, &by, pz, depth);
                    if (placed) { surf[ns].x = px; surf[ns].y = gy; surf[ns].z = pz; ns++; }
                }
            }
        }
    }
    veg_hashset_order(surf, ns);
    int ret;
    if (!s->water) {
        for (int i = 0; i < ns; i++)
            if (s->veg_chance > 0.0f && frnd_float(r) < s->veg_chance)
                placed_place(c, s->veg, surf[i].x, surf[i].y + DIR_DY[outwards], surf[i].z, 0);
        ret = ns > 0;
    } else {
        /* WaterloggedVegetationPatchFeature: незащищённые (все 4 стороны и низ — прочные) клетки поверхности становятся водой */
        VPos *ws = malloc(sizeof(VPos) * (size_t)(ns ? ns : 1)); int nw = 0;
        static const int EXD[5] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST, DIR_DOWN };
        for (int i = 0; i < ns; i++) {
            int exposed = 0;
            for (int k = 0; k < 5 && !exposed; k++) {
                int d = EXD[k];
                int nb = fc_get(c, surf[i].x + DIR_DX[d], surf[i].y + DIR_DY[d], surf[i].z + DIR_DZ[d]);
                if (!veg_sturdy(c, nb, d ^ 1)) exposed = 1;
            }
            if (!exposed) ws[nw++] = surf[i];
        }
        for (int i = 0; i < nw; i++) fc_set(c, ws[i].x, ws[i].y, ws[i].z, c->st_water, 2);
        veg_hashset_order(ws, nw);
        for (int i = 0; i < nw; i++) {
            if (!(s->veg_chance > 0.0f && frnd_float(r) < s->veg_chance)) continue;
            /* placeVegetation(placementPos): суперметод вызывается с placementPos.below(), растительность — на шаг «наружу» от него */
            if (placed_place(c, s->veg, ws[i].x, ws[i].y - 1 + DIR_DY[outwards], ws[i].z, 0)) {
                int pl = fc_get(c, ws[i].x, ws[i].y, ws[i].z);
                const char *wl = NULL;
                if (bs_get_prop(c->bs, pl, "waterlogged", &wl) && !strcmp(wl, "false")) fc_set(c, ws[i].x, ws[i].y, ws[i].z, bs_with(c->bs, pl, "waterlogged", "true"), 2);
            }
        }
        ret = nw > 0;
        free(ws);
    }
    free(surf);
    return ret;
}

/* ====================================================================== huge_fungus (crimson/warped fungus) */
typedef struct FungusCfg { int base_blk, stem, hat, decor; BPred *repl; int planted, hat_wart, wv_head, wv_plant; } FungusCfg;
static void *fungus_parse(FParse *p, const Js *cfg) {
    FungusCfg *f = fp_alloc(p, sizeof *f);
    int b = bs_from_json(p->bs, js_get(cfg, "valid_base_block"));
    f->stem = bs_from_json(p->bs, js_get(cfg, "stem_state")); f->hat = bs_from_json(p->bs, js_get(cfg, "hat_state")); f->decor = bs_from_json(p->bs, js_get(cfg, "decor_state"));
    if (b < 0 || f->stem < 0 || f->hat < 0 || f->decor < 0) { fp_fail(p, "huge_fungus: плохие состояния"); return NULL; }
    f->base_blk = p->g->state_block[b];
    f->repl = fp_bpred(p, js_get(cfg, "replaceable_blocks")); if (!f->repl) return NULL;
    f->planted = js_bool(js_get(cfg, "planted"), 0);
    f->hat_wart = p->g->state_block[f->hat] == bs_block_index(p->bs, "minecraft:nether_wart_block");
    int wv = bs_block_index(p->bs, "minecraft:weeping_vines"), wp = bs_block_index(p->bs, "minecraft:weeping_vines_plant");
    if (wv < 0 || wp < 0) { fp_fail(p, "huge_fungus: нет weeping_vines"); return NULL; }
    f->wv_head = bs_default(p->bs, wv); f->wv_plant = bs_default(p->bs, wp);
    return f;
}
static inline int fungus_replaceable(FCtx *c, const FungusCfg *f, int x, int y, int z, int check_plants) {
    if (c->bs->flags[fc_get(c, x, y, z)] & BSF_REPLACEABLE) return 1;
    return check_plants ? bpred_test(c, f->repl, x, y, z) : 0;
}
static void fungus_weeping(FCtx *c, const FungusCfg *f, int x, int y, int z) {      /* tryPlaceWeepingVines: (x,y,z) — блок шляпы */
    FRnd *r = c->rnd;
    y--;
    if (!fc_is_empty_block(c, x, y, z)) return;
    int goal = mth_next_int(r, 1, 5);
    if (frnd_int_bound(r, 7) == 0) goal *= 2;
    for (int h = 0; h <= goal; h++) {
        if (fc_is_empty_block(c, x, y, z)) {
            if (h == goal || !fc_is_empty_block(c, x, y - 1, z)) {
                char age[8]; snprintf(age, sizeof age, "%d", mth_next_int(r, 23, 25));
                fc_set(c, x, y, z, bs_with(c->bs, f->wv_head, "age", age), 2);
                break;
            }
            fc_set(c, x, y, z, f->wv_plant, 2);
        }
        y--;
    }
}
static void fungus_hat_block(FCtx *c, const FungusCfg *f, int x, int y, int z, float decor_p, float hat_p, float vines_p) {
    FRnd *r = c->rnd;
    if (frnd_float(r) < decor_p) fc_set(c, x, y, z, f->decor, 3);
    else if (frnd_float(r) < hat_p) {
        fc_set(c, x, y, z, f->hat, 3);
        if (frnd_float(r) < vines_p) fungus_weeping(c, f, x, y, z);
    }
}
static int fungus_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const FungusCfg *f = cfg; FRnd *r = c->rnd;
    if (c->g->state_block[fc_get(c, ox, oy - 1, oz)] != f->base_blk) return 0;
    int total = mth_next_int(r, 4, 13);
    if (frnd_int_bound(r, 12) == 0) total *= 2;
    if (!f->planted && oy + total + 1 >= c->gen_depth) return 0;
    int huge = !f->planted && frnd_float(r) < 0.06f;
    if (f->planted) return 0;                    /* planted — только костная мука, в генерации мира не встречается */
    fc_set(c, ox, oy, oz, c->st_air, 260);
    /* placeStem */
    int sr = huge ? 1 : 0;
    for (int dx = -sr; dx <= sr; dx++) for (int dz = -sr; dz <= sr; dz++) {
        int corner = huge && abs(dx) == sr && abs(dz) == sr;
        for (int dy = 0; dy < total; dy++) {
            int x = ox + dx, y = oy + dy, z = oz + dz;
            if (fungus_replaceable(c, f, x, y, z, 1)) {
                if (corner) { if (frnd_float(r) < 0.1f) fc_set(c, x, y, z, f->stem, 3); }
                else fc_set(c, x, y, z, f->stem, 3);
            }
        }
    }
    /* placeHat */
    int hat_h = (frnd_int_bound(r, 1 + total / 3) + 5); if (hat_h > total) hat_h = total;
    int hat_start = total - hat_h;
    for (int dy = hat_start; dy <= total; dy++) {
        int radius = dy < total - frnd_int_bound(r, 3) ? 2 : 1;
        if (hat_h > 8 && dy < hat_start + 4) radius = 3;
        if (huge) radius++;
        for (int dx = -radius; dx <= radius; dx++) for (int dz = -radius; dz <= radius; dz++) {
            int ex = dx == -radius || dx == radius, ez = dz == -radius || dz == radius;
            int inside = !ex && !ez && dy != total, corner = ex && ez, bottom = dy < hat_start + 3;
            int x = ox + dx, y = oy + dy, z = oz + dz;
            if (!fungus_replaceable(c, f, x, y, z, 0)) continue;
            if (bottom) {
                if (!inside) {
                    if (c->g->state_block[fc_get(c, x, y - 1, z)] == c->g->state_block[f->hat]) fc_set(c, x, y, z, f->hat, 3);
                    else if ((double)frnd_float(r) < 0.15) {
                        fc_set(c, x, y, z, f->hat, 3);
                        if (f->hat_wart && frnd_int_bound(r, 11) == 0) fungus_weeping(c, f, x, y, z);
                    }
                }
            } else if (inside) fungus_hat_block(c, f, x, y, z, 0.1f, 0.2f, f->hat_wart ? 0.1f : 0.0f);
            else if (corner) fungus_hat_block(c, f, x, y, z, 0.01f, 0.7f, f->hat_wart ? 0.083f : 0.0f);
            else fungus_hat_block(c, f, x, y, z, 5.0E-4f, 0.98f, f->hat_wart ? 0.07f : 0.0f);
        }
    }
    return 1;
}

/* ====================================================================== root_system (азалия с корнями) */
typedef struct RootCfg {
    Placed *tree; int req_space, level_dist, max_dev, root_radius; const u8 *root_repl; BSProv *root_prov;
    int root_attempts, col_max, hang_radius, hang_span; BSProv *hang_prov; int hang_attempts, water_allowed; BPred *allowed;
} RootCfg;
static void *root_parse(FParse *p, const Js *cfg) {
    RootCfg *s = fp_alloc(p, sizeof *s);
    s->tree = fp_placed(p, js_get(cfg, "feature")); if (!s->tree) return NULL;
    s->req_space = js_int(js_get(cfg, "required_vertical_space_for_tree"), 1);
    s->level_dist = js_int(js_get(cfg, "level_test_distance"), 0);
    s->max_dev = js_int(js_get(cfg, "max_level_deviation"), 0);
    s->root_radius = js_int(js_get(cfg, "root_radius"), 1);
    s->root_repl = fp_blockset(p, js_get(cfg, "root_replaceable")); if (!s->root_repl) return NULL;
    s->root_prov = fp_bsprov(p, js_get(cfg, "root_state_provider")); if (!s->root_prov) return NULL;
    s->root_attempts = js_int(js_get(cfg, "root_placement_attempts"), 1);
    s->col_max = js_int(js_get(cfg, "root_column_max_height"), 1);
    s->hang_radius = js_int(js_get(cfg, "hanging_root_radius"), 1);
    s->hang_span = js_int(js_get(cfg, "hanging_roots_vertical_span"), 1);
    s->hang_prov = fp_bsprov(p, js_get(cfg, "hanging_root_state_provider")); if (!s->hang_prov) return NULL;
    s->hang_attempts = js_int(js_get(cfg, "hanging_root_placement_attempts"), 1);
    s->water_allowed = js_int(js_get(cfg, "allowed_vertical_water_for_tree"), 1);
    s->allowed = fp_bpred(p, js_get(cfg, "allowed_tree_position")); if (!s->allowed) return NULL;
    return s;
}
static int root_space_for_tree(FCtx *c, const RootCfg *s, int x, int y, int z) {
    for (int i = 1; i <= s->req_space; i++) {
        int st = fc_get(c, x, y + i, z);
        if (fc_is_air(c, st)) continue;
        if (!(i + 1 <= s->water_allowed && veg_is_water(BS_FL_TYPE(c->bs->fluid[st])))) return 0;
    }
    if (s->level_dist > 0) {
        static const int D2[4] = { DIR_SOUTH, DIR_WEST, DIR_NORTH, DIR_EAST };        /* Direction.from2DDataValue */
        for (int i = 0; i < 4; i++) {
            int cx = x + DIR_DX[D2[i]] * s->level_dist, cz = z + DIR_DZ[D2[i]] * s->level_dist;
            int below = fc_get(c, cx, y - s->max_dev, cz), above = fc_get(c, cx, y + s->max_dev, cz);
            if (fc_is_air(c, below) || !fc_is_air(c, above)) return 0;
        }
    }
    return 1;
}
static int root_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const RootCfg *s = cfg; FRnd *r = c->rnd;
    if (!fc_is_air(c, fc_get(c, ox, oy, oz))) return 0;
    int wy = oy, done = 0;
    for (int y = 0; y < s->col_max; y++) {
        wy++;
        if (fc_height(c, HM_WORLD_SURFACE, ox, oz) < wy) break;
        if (bpred_test(c, s->allowed, ox, wy, oz) && root_space_for_tree(c, s, ox, wy, oz)) {
            int below = fc_get(c, ox, wy - 1, oz);
            int fl = BS_FL_TYPE(c->bs->fluid[fc_get(c, ox, wy - 1, oz)]);
            if (fl == FL_LAVA || fl == FL_FLOWING_LAVA || !(c->bs->flags[below] & BSF_SOLID)) break;
            if (placed_place(c, s->tree, ox, wy, oz, 0)) {
                /* placeDirt */
                int target = oy + y;
                for (int yy = oy; yy < target; yy++) {
                    int px = ox, pz = oz;
                    for (int i = 0; i < s->root_attempts; i++) {
                        int dx = frnd_int_bound(r, s->root_radius) - frnd_int_bound(r, s->root_radius);
                        int dz = frnd_int_bound(r, s->root_radius) - frnd_int_bound(r, s->root_radius);
                        px += dx; pz += dz;
                        if (s->root_repl[c->g->state_block[fc_get(c, px, yy, pz)]]) fc_set(c, px, yy, pz, bsprov_state(c, s->root_prov, px, yy, pz), 2);
                        px = ox; pz = oz;
                    }
                }
                done = 1;
                break;
            }
        }
    }
    if (done) {
        for (int i = 0; i < s->hang_attempts; i++) {
            int dx = frnd_int_bound(r, s->hang_radius) - frnd_int_bound(r, s->hang_radius);
            int dy = frnd_int_bound(r, s->hang_span) - frnd_int_bound(r, s->hang_span);
            int dz = frnd_int_bound(r, s->hang_radius) - frnd_int_bound(r, s->hang_radius);
            int x = ox + dx, y = oy + dy, z = oz + dz;
            if (fc_is_empty_block(c, x, y, z)) {
                int st = bsprov_state(c, s->hang_prov, x, y, z);
                if (block_can_survive(c, st, x, y, z) && veg_sturdy(c, fc_get(c, x, y + 1, z), DIR_DOWN)) fc_set(c, x, y, z, st, 2);
            }
        }
    }
    return 1;
}

/* ====================================================================== coral_tree / coral_claw */
static void tag_collect(const McGen *g, const BsTab *bs, const char *name, int *out, int *n, int cap, int depth);
/* 26.3+: фича-«блок коралла» (placed_feature coral/*); 26.1/26.2: сам кораллы-блок CoralFeature.placeCoralBlock с тегами corals / coral_blocks / wall_corals */
typedef struct CoralCfg {
    Placed *feat;                                      /* 26.3+ */
    int old; int n_blocks, n_corals, n_walls; int *blocks, *corals, *walls; const u8 *corals_set; int water, pickle;
} CoralCfg;
static _Thread_local int g_coral_state;                /* выбранное состояние коралла-блока (старый режим) */
static void *coral_parse(FParse *p, const Js *cfg) {
    CoralCfg *s = fp_alloc(p, sizeof *s);
    const Js *f = js_get(cfg, "feature");
    if (f) { s->feat = fp_placed(p, f); return s->feat ? s : NULL; }
    if (p->newf) { fp_fail(p, "coral: нет feature"); return NULL; }
    s->old = 1;
    int tmp[64], n;
    n = 0; tag_collect(p->g, p->bs, "minecraft:coral_blocks", tmp, &n, 64, 0); s->n_blocks = n; s->blocks = fp_alloc(p, sizeof(int) * (size_t)(n + 1)); memcpy(s->blocks, tmp, sizeof(int) * (size_t)n);
    n = 0; tag_collect(p->g, p->bs, "minecraft:corals", tmp, &n, 64, 0); s->n_corals = n; s->corals = fp_alloc(p, sizeof(int) * (size_t)(n + 1)); memcpy(s->corals, tmp, sizeof(int) * (size_t)n);
    n = 0; tag_collect(p->g, p->bs, "minecraft:wall_corals", tmp, &n, 64, 0); s->n_walls = n; s->walls = fp_alloc(p, sizeof(int) * (size_t)(n + 1)); memcpy(s->walls, tmp, sizeof(int) * (size_t)n);
    s->corals_set = gen_block_tag(p->g, "minecraft:corals");
    s->water = bs_block_index(p->bs, "minecraft:water"); s->pickle = bs_block_index(p->bs, "minecraft:sea_pickle");
    if (!s->n_blocks || !s->n_corals || !s->n_walls || s->water < 0 || s->pickle < 0) { fp_fail(p, "coral: нет тегов"); return NULL; }
    return s;
}
/* CoralFeature.placeCoralBlock (26.1/26.2) */
static int coral_old_block(FCtx *c, const CoralCfg *s, int x, int y, int z) {
    FRnd *r = c->rnd; const BsTab *bs = c->bs;
    int tb = c->g->state_block[fc_get(c, x, y, z)];
    if (!((tb == s->water || s->corals_set[tb]) && c->g->state_block[fc_get(c, x, y + 1, z)] == s->water)) return 0;
    fc_set(c, x, y, z, g_coral_state, 3);
    if (frnd_float(r) < 0.25f) {
        int b = s->corals[frnd_int_bound(r, s->n_corals)];
        fc_set(c, x, y + 1, z, bs_default(bs, b), 2);
    } else if (frnd_float(r) < 0.05f) {
        char n[4]; snprintf(n, sizeof n, "%d", frnd_int_bound(r, 4) + 1);
        fc_set(c, x, y + 1, z, bs_with(bs, bs_default(bs, s->pickle), "pickles", n), 2);
    }
    static const int HZ4[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    static const char *HN[4] = { "north", "east", "south", "west" };
    for (int i = 0; i < 4; i++) {
        if (frnd_float(r) < 0.2f) {
            int d = HZ4[i], rx = x + DIR_DX[d], rz = z + DIR_DZ[d];
            if (c->g->state_block[fc_get(c, rx, y, rz)] == s->water) {
                int b = s->walls[frnd_int_bound(r, s->n_walls)];
                int st = bs_try_with(bs, bs_default(bs, b), "facing", HN[i]);
                fc_set(c, rx, y, rz, st, 2);
            }
        }
    }
    return 1;
}
static inline int coral_put(FCtx *c, const CoralCfg *s, int x, int y, int z) {
    return s->old ? coral_old_block(c, s, x, y, z) : placed_place(c, s->feat, x, y, z, 0);
}
/* CoralFeature.place (26.1/26.2): случайный блок коралла из тега coral_blocks, затем форма */
static inline int coral_old_pick(FCtx *c, const CoralCfg *s) {
    int b = s->blocks[frnd_int_bound(c->rnd, s->n_blocks)];
    g_coral_state = bs_default(c->bs, b);
    return 1;
}
static const int HZ[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };      /* Direction.Plane.HORIZONTAL */
static inline int hz_index(int d) { for (int i = 0; i < 4; i++) if (HZ[i] == d) return i; return 0; }
static void shuffle_ints(FRnd *r, int *a, int n) {                           /* Util.shuffle */
    for (int i = n; i > 1; i--) { int to = frnd_int_bound(r, i); int t = a[i - 1]; a[i - 1] = a[to]; a[to] = t; }
}
static int coral_tree_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const CoralCfg *s = cfg; FRnd *r = c->rnd;
    int x = ox, y = oy, z = oz;
    if (s->old) coral_old_pick(c, s);
    int trunk = frnd_int_bound(r, 3) + 1;
    for (int i = 0; i < trunk; i++) { if (!coral_put(c, s, x, y, z)) return 1; y++; }
    int tx = x, ty = y, tz = z;
    int nb = frnd_int_bound(r, 3) + 2;
    int dirs[4] = { HZ[0], HZ[1], HZ[2], HZ[3] };
    shuffle_ints(r, dirs, 4);
    for (int k = 0; k < nb; k++) {
        int d = dirs[k];
        x = tx + DIR_DX[d]; y = ty; z = tz + DIR_DZ[d];
        int bh = frnd_int_bound(r, 5) + 2, seg = 0;
        for (int j = 0; j < bh && coral_put(c, s, x, y, z); j++) {
            seg++; y++;
            if (j == 0 || (seg >= 2 && frnd_float(r) < 0.25f)) { x += DIR_DX[d]; z += DIR_DZ[d]; seg = 0; }
        }
    }
    return 1;
}
static int coral_claw_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const CoralCfg *s = cfg; FRnd *r = c->rnd;
    if (s->old) coral_old_pick(c, s);
    if (!coral_put(c, s, ox, oy, oz)) return 0;
    int claw = HZ[frnd_int_bound(r, 4)];
    int nb = frnd_int_bound(r, 2) + 2;
    int ci = hz_index(claw);
    int poss[3] = { claw, HZ[(ci + 1) & 3], HZ[(ci + 3) & 3] };             /* claw, getClockWise, getCounterClockWise */
    shuffle_ints(r, poss, 3);
    for (int k = 0; k < nb; k++) {
        int bd = poss[k];
        int x = ox, y = oy, z = oz;
        int sideway = frnd_int_bound(r, 2) + 1;
        x += DIR_DX[bd]; z += DIR_DZ[bd];
        int inway, seg;
        if (bd == claw) { seg = claw; inway = frnd_int_bound(r, 3) + 2; }
        else {
            y++;
            seg = frnd_int_bound(r, 2) == 0 ? bd : DIR_UP;
            inway = frnd_int_bound(r, 3) + 3;
        }
        for (int i = 0; i < sideway && coral_put(c, s, x, y, z); i++) { x += DIR_DX[seg]; y += DIR_DY[seg]; z += DIR_DZ[seg]; }
        int op = seg ^ 1;
        x += DIR_DX[op]; y += DIR_DY[op]; z += DIR_DZ[op];
        y++;
        for (int i = 0; i < inway; i++) {
            x += DIR_DX[claw]; z += DIR_DZ[claw];
            if (!coral_put(c, s, x, y, z)) break;
            if (frnd_float(r) < 0.25f) y++;
        }
    }
    return 1;
}

static int coral_mushroom_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const CoralCfg *s = cfg; FRnd *r = c->rnd;
    coral_old_pick(c, s);
    int height = frnd_int_bound(r, 3) + 3, width = frnd_int_bound(r, 3) + 3, length = frnd_int_bound(r, 3) + 3, sink = frnd_int_bound(r, 3) + 1;
    for (int x = 0; x <= width; x++) for (int y = 0; y <= height; y++) for (int z = 0; z <= length; z++) {
        if ((x != 0 && x != width || y != 0 && y != height) && (z != 0 && z != length || y != 0 && y != height) && (x != 0 && x != width || z != 0 && z != length)
            && (x == 0 || x == width || y == 0 || y == height || z == 0 || z == length) && !(frnd_float(r) < 0.1f))
            coral_put(c, s, ox + x, oy + y - sink, oz + z);
    }
    return 1;
}

/* ====================================================================== HolderSet<Block>: упорядоченный список блоков (теги раскрываются в порядке TagLoader) */
static void tag_collect(const McGen *g, const BsTab *bs, const char *name, int *out, int *n, int cap, int depth) {
    if (depth > 16) return;
    char ns[64] = "minecraft"; const char *path = name;
    const char *col = strchr(name, ':');
    if (col) { size_t l = (size_t)(col - name); if (l >= sizeof ns) return; memcpy(ns, name, l); ns[l] = 0; path = col + 1; }
    char *file = xsprintf("%s/data/%s/tags/block/%s.json", g->pack, ns, path);
    JsDoc *d = js_parse_file(file, NULL, 0); free(file);
    if (!d) return;
    const Js *vals = js_get(js_root(d), "values");
    for (int i = 0; vals && i < vals->n; i++) {
        const Js *v = vals->items[i];
        const char *id = js_is_str(v) ? v->s : js_str(js_get(v, "id"), NULL);
        if (!id) continue;
        if (id[0] == '#') { tag_collect(g, bs, id + 1, out, n, cap, depth + 1); continue; }
        int b = bs_block_index(bs, id);
        if (b < 0) continue;
        int dup = 0; for (int k = 0; k < *n; k++) if (out[k] == b) { dup = 1; break; }
        if (!dup && *n < cap) out[(*n)++] = b;
    }
    js_free(d);
}
/* HolderSet<Block> из JSON (строка/тег/массив) → блоки в порядке набора; в арене разбора; возвращает число блоков или −1 */
int veg_holderset_blocks(FParse *p, const Js *v, int **out) {
    int cap = 2048, *tmp = malloc(sizeof(int) * (size_t)cap), n = 0;
    const Js *items[1]; const Js *const *arr; int cnt;
    if (js_is_arr(v)) { arr = (const Js *const *)v->items; cnt = v->n; } else { items[0] = v; arr = items; cnt = 1; }
    for (int i = 0; i < cnt; i++) {
        const char *s = js_is_str(arr[i]) ? arr[i]->s : NULL;
        if (!s) { free(tmp); return -1; }
        if (s[0] == '#') tag_collect(p->g, p->bs, s + 1, tmp, &n, cap, 0);
        else { int b = bs_block_index(p->bs, s); if (b < 0) { free(tmp); return -1; } if (n < cap) tmp[n++] = b; }
    }
    *out = fp_alloc(p, sizeof(int) * (size_t)(n ? n : 1));
    memcpy(*out, tmp, sizeof(int) * (size_t)n);
    free(tmp);
    return n;
}

/* ====================================================================== MossyCarpetBlock.placeAt (pale_moss_carpet) */
typedef struct MossBlk { int blk; int def; } MossBlk;
static int moss_wall(const BsTab *bs, int st, int dir, const char **val) {          /* WallSide свойства north/east/south/west: none|low|tall */
    static const char *PN[6] = { NULL, NULL, "north", "south", "west", "east" };
    return bs_get_prop(bs, st, PN[dir], val);
}
static int moss_set_wall(const BsTab *bs, int st, int dir, const char *v) {
    static const char *PN[6] = { NULL, NULL, "north", "south", "west", "east" };
    return bs_with(bs, st, PN[dir], v);
}
static int moss_can_support(FCtx *c, int x, int y, int z, int dir) {                  /* canSupportAtFace: MultifaceBlock.canAttachTo */
    if (dir == DIR_UP) return 0;
    int nb = fc_get(c, x + DIR_DX[dir], y + DIR_DY[dir], z + DIR_DZ[dir]);
    return veg_attach(c, nb, dir);
}
static int moss_updated(FCtx *c, int blk, int st, int x, int y, int z, int create_sides) {   /* getUpdatedState */
    const BsTab *bs = c->bs;
    const char *bv = NULL; bs_get_prop(bs, st, "bottom", &bv);
    int is_base = bv && !strcmp(bv, "true");
    create_sides |= is_base;
    int above = -1, below = -1;
    static const int HZ4[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    for (int i = 0; i < 4; i++) {
        int d = HZ4[i];
        const char *cur = NULL; moss_wall(bs, st, d, &cur);
        const char *side = !moss_can_support(c, x, y, z, d) ? "none" : (create_sides ? "low" : cur);
        if (!strcmp(side, "low")) {
            if (above < 0) above = fc_get(c, x, y + 1, z);
            const char *av = NULL;
            if (c->g->state_block[above] == blk && moss_wall(bs, above, d, &av) && strcmp(av, "none")) {
                const char *ab = NULL; bs_get_prop(bs, above, "bottom", &ab);
                if (ab && !strcmp(ab, "false")) side = "tall";
            }
            if (!is_base) {
                if (below < 0) below = fc_get(c, x, y - 1, z);
                const char *bw = NULL;
                if (c->g->state_block[below] == blk && moss_wall(bs, below, d, &bw) && !strcmp(bw, "none")) side = "none";
            }
        }
        st = moss_set_wall(bs, st, d, side);
    }
    return st;
}
static void moss_place_at(FCtx *c, int x, int y, int z, int upd) {
    const BsTab *bs = c->bs;
    int blk = bs_block_index(bs, "minecraft:pale_moss_carpet");
    if (blk < 0) return;
    Rnd *rr = fc_region_random(c);
    int simple = bs_default(bs, blk);
    int adj = moss_updated(c, blk, simple, x, y, z, 1);
    fc_set(c, x, y, z, adj, upd);
    /* createTopperWithSideChance(level, pos, random::nextBoolean) */
    int above = fc_get(c, x, y + 1, z);
    int is_moss = c->g->state_block[above] == blk;
    const char *ab = NULL; if (is_moss) bs_get_prop(bs, above, "bottom", &ab);
    int topper = c->st_air;
    if ((!is_moss || (ab && !strcmp(ab, "false"))) && (is_moss || (bs->flags[above] & BSF_REPLACEABLE))) {
        int nb = bs_with(bs, simple, "bottom", "false");
        int a = moss_updated(c, blk, nb, x, y + 1, z, 1);
        static const int HZ4[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
        for (int i = 0; i < 4; i++) {
            const char *cur = NULL; moss_wall(bs, a, HZ4[i], &cur);
            if (strcmp(cur, "none") && !(rnd_next_long(rr) & 1)) a = moss_set_wall(bs, a, HZ4[i], "none");
        }
        /* hasFaces(aboveState): base или любая сторона != none; aboveState != abovePreviousState */
        int has = 0; const char *bv = NULL; bs_get_prop(bs, a, "bottom", &bv); if (bv && !strcmp(bv, "true")) has = 1;
        for (int i = 0; i < 4 && !has; i++) { const char *v = NULL; moss_wall(bs, a, HZ4[i], &v); if (strcmp(v, "none")) has = 1; }
        if (has && a != above) topper = a;
    }
    if (topper != c->st_air && !(bs->flags[topper] & BSF_AIR)) {
        fc_set(c, x, y + 1, z, topper, upd);
        int upd_bottom = moss_updated(c, blk, adj, x, y, z, 1);
        fc_set(c, x, y, z, upd_bottom, upd);
    }
}
/* вызывается из simple_block (feature_misc.c) для MossyCarpetBlock; ставит блок и возвращает 1 */
int veg_mossy_carpet_place(FCtx *c, int x, int y, int z) { moss_place_at(c, x, y, z, 2); return 1; }

/* ====================================================================== выживание растений (BlockState.canSurvive) */
enum { VK_UNK = 0, VK_NONE, VK_SEAGRASS, VK_TALL_SEAGRASS, VK_SEA_PICKLE, VK_LILY_PAD, VK_MUSHROOM, VK_FIRE, VK_SOUL_FIRE, VK_SUGAR_CANE, VK_CACTUS,
       VK_LEAF_LITTER, VK_SPORE_BLOSSOM, VK_BERRY, VK_BAMBOO, VK_HANGING_ROOTS, VK_MOSSY_CARPET, VK_CORAL, VK_CORAL_WALL, VK_GROW_UP, VK_GROW_DOWN, VK_DRY };

static int veg_kind_of(const BsBlock *bb) {
    const char *k = bb->cls;
    if (!k) return VK_NONE;
    if (!strcmp(k, "SeagrassBlock")) return VK_SEAGRASS;
    if (!strcmp(k, "TallSeagrassBlock")) return VK_TALL_SEAGRASS;
    if (!strcmp(k, "SeaPickleBlock")) return VK_SEA_PICKLE;
    if (!strcmp(k, "LilyPadBlock")) return VK_LILY_PAD;
    if (!strcmp(k, "MushroomBlock")) return VK_MUSHROOM;
    if (!strcmp(k, "FireBlock")) return VK_FIRE;
    if (!strcmp(k, "SoulFireBlock")) return VK_SOUL_FIRE;
    if (!strcmp(k, "SugarCaneBlock")) return VK_SUGAR_CANE;
    if (!strcmp(k, "CactusBlock")) return VK_CACTUS;
    if (!strcmp(k, "LeafLitterBlock")) return VK_LEAF_LITTER;
    if (!strcmp(k, "SporeBlossomBlock")) return VK_SPORE_BLOSSOM;
    if (!strcmp(k, "SweetBerryBushBlock")) return VK_BERRY;
    if (!strcmp(k, "HangingRootsBlock")) return VK_HANGING_ROOTS;
    if (!strcmp(k, "ShortDryGrassBlock") || !strcmp(k, "TallDryGrassBlock") || !strcmp(k, "DryVegetationBlock")) return VK_DRY;
    if (!strcmp(k, "KelpBlock") || !strcmp(k, "KelpPlantBlock") || !strcmp(k, "TwistingVinesBlock") || !strcmp(k, "TwistingVinesPlantBlock")) return VK_GROW_UP;
    if (!strcmp(k, "WeepingVinesBlock") || !strcmp(k, "WeepingVinesPlantBlock") || !strcmp(k, "CaveVinesBlock") || !strcmp(k, "CaveVinesPlantBlock")) return VK_GROW_DOWN;
    if (!strcmp(k, "CoralPlantBlock") || !strcmp(k, "BaseCoralPlantBlock") || !strcmp(k, "CoralFanBlock") || !strcmp(k, "BaseCoralFanBlock")) return VK_CORAL;
    if (!strcmp(k, "CoralWallFanBlock") || !strcmp(k, "BaseCoralWallFanBlock")) return VK_CORAL_WALL;
    if (!strcmp(k, "MossyCarpetBlock")) return VK_MOSSY_CARPET;
    if (!strcmp(k, "BambooStalkBlock") || !strcmp(k, "BambooSaplingBlock")) return VK_BAMBOO;
    return VK_NONE;
}

int veg_survive(FCtx *c, int st, int x, int y, int z, int *res) {
    static _Thread_local const McGen *cg; static _Thread_local u8 *kinds;
    const BsTab *bs = c->bs;
    if (cg != c->g || !kinds) { free(kinds); kinds = calloc((size_t)bs->nblocks, 1); cg = c->g; }
    int blk = c->g->state_block[st];
    int k = kinds[blk];
    if (!k) { k = veg_kind_of(&bs->blk[blk]); kinds[blk] = (u8)k; }
    if (k == VK_NONE) return 0;
    int below = fc_get(c, x, y - 1, z);
    *res = 1;
    switch (k) {
    case VK_SEAGRASS: *res = veg_sturdy(c, below, DIR_UP) && !veg_in_tag(c, veg_tag(c, "minecraft:cannot_support_seagrass"), below); return 1;
    case VK_TALL_SEAGRASS: {
        const char *half = NULL; bs_get_prop(bs, st, "half", &half);
        if (half && !strcmp(half, "upper")) {
            const char *bh = NULL;
            *res = c->g->state_block[below] == blk && bs_get_prop(bs, below, "half", &bh) && !strcmp(bh, "lower"); return 1;
        }
        int fs = bs->fluid[fc_get(c, x, y, z)];
        *res = veg_sturdy(c, below, DIR_UP) && !veg_in_tag(c, veg_tag(c, "minecraft:cannot_support_seagrass"), below)
               && veg_is_water(BS_FL_TYPE(fs)) && BS_FL_AMOUNT(fs) == 8;
        return 1;
    }
    case VK_SEA_PICKLE: *res = veg_sturdy(c, below, DIR_UP) || (bs->flags[below] & BSF_FULL_COLL) != 0; return 1;
    case VK_LILY_PAD: {
        int fl_below = BS_FL_TYPE(bs->fluid[below]);
        int fl_above = BS_FL_TYPE(bs->fluid[fc_get(c, x, y, z)]);       /* mayPlaceOn(pos = below): fluidAbove = fluid(pos.above()) = fluid(позиции растения) */
        *res = (fl_below == FL_WATER || veg_in_tag(c, veg_tag(c, "minecraft:supports_lily_pad"), below)) && fl_above == FL_NONE;
        return 1;
    }
    case VK_MUSHROOM:
        if (veg_in_tag(c, veg_tag(c, "minecraft:overrides_mushroom_light_requirement"), below)) { *res = 1; return 1; }
        /* getRawBrightness(pos, 0) на стадии FEATURES: чанки без данных освещения дают небесный свет 15 (SkyLightSectionStorage.getLightValue: «15»), блочный 0;
         * 15 >= 13 ⇒ гриб без «переопределяющей» опоры не выживает. Исключение игры (непредсказуемое): уже освещённые соседние чанки с настоящим светом. */
        *res = c->w->dim_kind != 1 ? 0 : (bs->flags[below] & BSF_SOLID_RENDER) != 0;      /* Nether (has_skylight=false): skyEngine == null → яркость 0 < 13; Overworld/End — небесный свет 15 */
        return 1;
    case VK_FIRE: *res = veg_sturdy(c, below, DIR_UP); return 1;
    case VK_SOUL_FIRE: *res = veg_in_tag(c, veg_tag(c, "minecraft:soul_fire_base_blocks"), below); return 1;
    case VK_SUGAR_CANE: {
        if (c->g->state_block[below] == blk) { *res = 1; return 1; }
        *res = 0;
        if (veg_in_tag(c, veg_tag(c, "minecraft:supports_sugar_cane"), below)) {
            for (int d = 2; d < 6; d++) {
                int bx = x + DIR_DX[d], bz = z + DIR_DZ[d];
                int nb = fc_get(c, bx, y - 1, bz);
                if (veg_is_water(BS_FL_TYPE(bs->fluid[nb])) || veg_in_tag(c, veg_tag(c, "minecraft:supports_sugar_cane_adjacently"), nb)) { *res = 1; break; }
            }
        }
        return 1;
    }
    case VK_CACTUS: {
        *res = 0;
        for (int d = 2; d < 6; d++) {
            int nb = fc_get(c, x + DIR_DX[d], y, z + DIR_DZ[d]);
            int fl = BS_FL_TYPE(bs->fluid[nb]);
            if ((bs->flags[nb] & BSF_SOLID) || fl == FL_LAVA || fl == FL_FLOWING_LAVA) return 1;
        }
        int above = fc_get(c, x, y + 1, z);
        *res = (c->g->state_block[below] == blk || veg_in_tag(c, veg_tag(c, "minecraft:supports_cactus"), below)) && !(bs->flags[above] & BSF_LIQUID);
        return 1;
    }
    case VK_LEAF_LITTER: *res = veg_sturdy(c, below, DIR_UP); return 1;
    case VK_SPORE_BLOSSOM: {
        int above = fc_get(c, x, y + 1, z);
        *res = veg_sturdy(c, above, DIR_DOWN) && !veg_is_water(BS_FL_TYPE(bs->fluid[fc_get(c, x, y, z)]));
        return 1;
    }
    case VK_BERRY: *res = veg_in_tag(c, veg_tag(c, "minecraft:supports_vegetation"), below); return 1;
    case VK_GROW_UP: case VK_GROW_DOWN: {          /* GrowingPlantBlock.canSurvive: опора с противоположной стороны роста */
        int up = k == VK_GROW_UP;
        int a = fc_get(c, x, up ? y - 1 : y + 1, z);
        const char *own = bs->blk[blk].name, *an = bs->blk[c->g->state_block[a]].name;
        if (!strncmp(own, "minecraft:kelp", 14) && veg_in_tag(c, veg_tag(c, "minecraft:cannot_support_kelp"), a)) { *res = 0; return 1; }
        size_t ol = strlen(own), al = strlen(an);
        if (ol > 6 && !strcmp(own + ol - 6, "_plant")) ol -= 6;
        if (al > 6 && !strcmp(an + al - 6, "_plant")) al -= 6;
        if (ol == al && !strncmp(own, an, ol)) { *res = 1; return 1; }
        *res = veg_sturdy(c, a, up ? DIR_UP : DIR_DOWN);
        return 1;
    }
    case VK_DRY: *res = veg_in_tag(c, veg_tag(c, "minecraft:supports_dry_vegetation"), below); return 1;
    case VK_CORAL: *res = veg_sturdy(c, below, DIR_UP); return 1;
    case VK_CORAL_WALL: {
        const char *fv = NULL; int d = bs_get_prop(bs, st, "facing", &fv) ? dir_from_name(fv) : -1;
        if (d < 0) { *res = 1; return 1; }
        *res = veg_sturdy(c, fc_get(c, x - DIR_DX[d], y - DIR_DY[d], z - DIR_DZ[d]), d);
        return 1;
    }
    case VK_MOSSY_CARPET: {
        const char *bv = NULL; bs_get_prop(bs, st, "bottom", &bv);
        if (bv && !strcmp(bv, "true")) *res = !fc_is_air(c, below);
        else { const char *bb = NULL; *res = c->g->state_block[below] == blk && bs_get_prop(bs, below, "bottom", &bb) && !strcmp(bb, "true"); }
        return 1;
    }
    case VK_HANGING_ROOTS: *res = veg_sturdy(c, fc_get(c, x, y + 1, z), DIR_DOWN); return 1;
    case VK_BAMBOO: *res = veg_in_tag(c, veg_tag(c, "minecraft:supports_bamboo"), below); return 1;
    }
    return 0;
}

/* ====================================================================== регистрация */
static const FeatType T_COLUMN = { "minecraft:block_column", col_parse, col_place };
static const FeatType T_BAMBOO = { "minecraft:bamboo", bamboo_parse, bamboo_place };
static const FeatType T_VINES = { "minecraft:vines", vines_parse, vines_place };
static const FeatType T_CTREE = { "minecraft:coral_tree", coral_parse, coral_tree_place };
static const FeatType T_CMUSH = { "minecraft:coral_mushroom", coral_parse, coral_mushroom_place };
static const FeatType T_CCLAW = { "minecraft:coral_claw", coral_parse, coral_claw_place };
static const FeatType T_FUNGUS = { "minecraft:huge_fungus", fungus_parse, fungus_place };
static const FeatType T_ROOTS = { "minecraft:root_system", root_parse, root_place };
static const FeatType T_VPATCH = { "minecraft:vegetation_patch", patch_parse, patch_place };
static const FeatType T_WPATCH = { "minecraft:waterlogged_vegetation_patch", wpatch_parse, patch_place };
void feature_register_veg2(void);       /* feature_veg2.c: типы 26.1/26.2 */
void feature_register_veg(void) {
    feature_register_veg2();
    feature_register_type(&T_COLUMN); feature_register_type(&T_BAMBOO); feature_register_type(&T_VINES);
    feature_register_type(&T_CTREE); feature_register_type(&T_CMUSH); feature_register_type(&T_CCLAW);
    feature_register_type(&T_FUNGUS); feature_register_type(&T_ROOTS);
    feature_register_type(&T_VPATCH); feature_register_type(&T_WPATCH);
}

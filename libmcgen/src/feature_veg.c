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

void veg_hashset_order(VPos *a, int n) {
    if (n < 2) return;
    int cap = 16;
    while (n > cap * 3 / 4) cap *= 2;
    u32 *key = malloc(sizeof(u32) * (size_t)n);
    int *idx = malloc(sizeof(int) * (size_t)n);
    VPos *tmp = malloc(sizeof(VPos) * (size_t)n);
    for (int i = 0; i < n; i++) {
        u32 h = (u32)(((u32)a[i].y + (u32)a[i].z * 31u) * 31u + (u32)a[i].x);
        h ^= h >> 16;
        key[i] = h & (u32)(cap - 1); idx[i] = i;
    }
    if (getenv("MCGEN_VEG_DEBUG")) { int mx = 0; for (int i = 0; i < n; i++) { int cnt = 0; for (int j = 0; j < n; j++) cnt += key[j] == key[i]; if (cnt > mx) mx = cnt; } fprintf(stderr, "hashset n=%d cap=%d maxchain=%d\n", n, cap, mx); }
    /* устойчивая сортировка вставками по (корзина, порядок вставки): n мало */
    for (int i = 1; i < n; i++) {
        int v = idx[i]; int j = i - 1;
        while (j >= 0 && key[idx[j]] > key[v]) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    for (int i = 0; i < n; i++) tmp[i] = a[idx[i]];
    memcpy(a, tmp, sizeof(VPos) * (size_t)n);
    free(key); free(idx); free(tmp);
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

/* ====================================================================== выживание растений (BlockState.canSurvive) */
enum { VK_UNK = 0, VK_NONE, VK_SEAGRASS, VK_TALL_SEAGRASS, VK_SEA_PICKLE, VK_LILY_PAD, VK_MUSHROOM, VK_FIRE, VK_SOUL_FIRE, VK_SUGAR_CANE, VK_CACTUS,
       VK_LEAF_LITTER, VK_SPORE_BLOSSOM, VK_BERRY, VK_BAMBOO };

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
        *res = (bs->flags[below] & BSF_SOLID_RENDER) != 0;        /* освещения на стадии FEATURES нет: raw brightness = 0 < 13 */
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
    case VK_BAMBOO: *res = veg_in_tag(c, veg_tag(c, "minecraft:supports_bamboo"), below); return 1;
    }
    return 0;
}

/* ====================================================================== регистрация */
static const FeatType T_COLUMN = { "minecraft:block_column", col_parse, col_place };
static const FeatType T_BAMBOO = { "minecraft:bamboo", bamboo_parse, bamboo_place };
static const FeatType T_VINES = { "minecraft:vines", vines_parse, vines_place };
static const FeatType T_VPATCH = { "minecraft:vegetation_patch", patch_parse, patch_place };
static const FeatType T_WPATCH = { "minecraft:waterlogged_vegetation_patch", wpatch_parse, patch_place };
void feature_register_veg(void) {
    feature_register_type(&T_COLUMN); feature_register_type(&T_BAMBOO); feature_register_type(&T_VINES);
    feature_register_type(&T_VPATCH); feature_register_type(&T_WPATCH);
}

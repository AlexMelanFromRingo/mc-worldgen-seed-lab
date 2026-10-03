/* feature_misc_old.c — фичи 26.1/26.2, которые в 26.3 переписаны через обобщённые типы (overlay/random_neighbor_spread/stepped_column_cluster/template):
 * glowstone_blob, basalt_pillar, basalt_columns, desert_well. В 26.3+ этих типов нет — регистрация безопасна для всех версий. */
#include "feature_misc.h"
#include <stdio.h>
#include <stdlib.h>

static int named_state(FParse *p, const char *name) { int b = bs_block_index(p->bs, name); return b < 0 ? -1 : bs_default(p->bs, b); }
static const int HD4[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };

/* ====================================================================== glowstone_blob (GlowstoneFeature) */
typedef struct GlowCfg { int glow, netherrack, basalt, blackstone; } GlowCfg;
static void *glow_parse(FParse *p, const Js *cfg) {
    GlowCfg *g = fp_alloc(p, sizeof *g);
    g->glow = bs_block_index(p->bs, "minecraft:glowstone"); g->netherrack = bs_block_index(p->bs, "minecraft:netherrack");
    g->basalt = bs_block_index(p->bs, "minecraft:basalt"); g->blackstone = bs_block_index(p->bs, "minecraft:blackstone");
    return g;
}
static int glow_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const GlowCfg *g = cfg; FRnd *r = c->rnd;
    if (!fc_is_empty_block(c, ox, oy, oz)) return 0;
    int above = fc_get(c, ox, oy + 1, oz);
    if (!fc_is_block(c, above, g->netherrack) && !fc_is_block(c, above, g->basalt) && !fc_is_block(c, above, g->blackstone)) return 0;
    int gl = bs_default(c->bs, g->glow);
    fc_set(c, ox, oy, oz, gl, 2);
    for (int i = 0; i < 1500; i++) {
        int a = frnd_int_bound(r, 8), b = frnd_int_bound(r, 8), dy = -frnd_int_bound(r, 12), cz = frnd_int_bound(r, 8), dz2 = frnd_int_bound(r, 8);
        int px = ox + (a - b), py = oy + dy, pz = oz + (cz - dz2);
        if (fc_is_air(c, fc_get(c, px, py, pz))) {
            int nb = 0;
            for (int d = 0; d < 6; d++) {
                if (fc_is_block(c, fc_get(c, px + DIR_DX[d], py + DIR_DY[d], pz + DIR_DZ[d]), g->glow)) nb++;
                if (nb > 1) break;
            }
            if (nb == 1) fc_set(c, px, py, pz, gl, 2);
        }
    }
    return 1;
}

/* ====================================================================== basalt_pillar (BasaltPillarFeature) */
typedef struct PillarOldCfg { int basalt; } PillarOldCfg;
static void *pillar_parse(FParse *p, const Js *cfg) { PillarOldCfg *s = fp_alloc(p, sizeof *s); s->basalt = named_state(p, "minecraft:basalt"); return s->basalt >= 0 ? s : NULL; }
static int hang_off(FCtx *c, int st, int x, int y, int z) {
    if (frnd_int_bound(c->rnd, 10) != 0) { fc_set(c, x, y, z, st, 2); return 1; }
    return 0;
}
static int pillar_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const PillarOldCfg *s = cfg; FRnd *r = c->rnd; int b = s->basalt;
    if (!(fc_is_empty_block(c, ox, oy, oz) && !fc_is_empty_block(c, ox, oy + 1, oz))) return 0;
    int x = ox, y = oy, z = oz;
    int pn = 1, ps = 1, pw = 1, pe = 1;
    while (fc_is_empty_block(c, x, y, z)) {
        if (fc_outside(c, y)) return 1;
        fc_set(c, x, y, z, b, 2);
        pn = pn && hang_off(c, b, x, y, z - 1);
        ps = ps && hang_off(c, b, x, y, z + 1);
        pw = pw && hang_off(c, b, x - 1, y, z);
        pe = pe && hang_off(c, b, x + 1, y, z);
        y--;
    }
    y++;
    if (frnd_bool(r)) fc_set(c, x, y, z - 1, b, 2);
    if (frnd_bool(r)) fc_set(c, x, y, z + 1, b, 2);
    if (frnd_bool(r)) fc_set(c, x - 1, y, z, b, 2);
    if (frnd_bool(r)) fc_set(c, x + 1, y, z, b, 2);
    y--;
    for (int dx = -3; dx < 4; dx++) for (int dz = -3; dz < 4; dz++) {
        int prob = iabs_(dx) * iabs_(dz);
        if (frnd_int_bound(r, 10) < 10 - prob) {
            int bx = x + dx, by = y, bz = z + dz, drop = 3;
            while (fc_is_empty_block(c, bx, by - 1, bz)) { by--; if (--drop <= 0) break; }
            if (!fc_is_empty_block(c, bx, by - 1, bz)) fc_set(c, bx, by, bz, b, 2);
        }
    }
    return 1;
}

/* ====================================================================== basalt_columns (BasaltColumnsFeature) */
typedef struct ColsCfg { IntProv *height, *reach; int basalt, lava; int cannot[10]; } ColsCfg;
static void *cols_parse(FParse *p, const Js *cfg) {
    ColsCfg *s = fp_alloc(p, sizeof *s);
    s->height = fp_intprov(p, js_get(cfg, "height")); s->reach = fp_intprov(p, js_get(cfg, "reach"));
    s->basalt = named_state(p, "minecraft:basalt"); s->lava = bs_block_index(p->bs, "minecraft:lava");
    static const char *CP[10] = { "lava", "bedrock", "magma_block", "soul_sand", "nether_bricks", "nether_brick_fence", "nether_brick_stairs", "nether_wart", "chest", "spawner" };
    for (int i = 0; i < 10; i++) { char nm[48]; snprintf(nm, sizeof nm, "minecraft:%s", CP[i]); s->cannot[i] = bs_block_index(p->bs, nm); }
    return s->height && s->reach ? s : NULL;
}
static int cols_cannot(const FCtx *c, const ColsCfg *s, int st) { int b = c->g->state_block[st]; for (int i = 0; i < 10; i++) if (b == s->cannot[i]) return 1; return 0; }
static int cols_air_or_lava(const FCtx *c, const ColsCfg *s, int sea, int x, int y, int z) {
    int st = fc_get(c, x, y, z); return fc_is_air(c, st) || (fc_is_block(c, st, s->lava) && y <= sea);
}
static int cols_can_place(const FCtx *c, const ColsCfg *s, int sea, int x, int y, int z) {
    if (!cols_air_or_lava(c, s, sea, x, y, z)) return 0;
    int below = fc_get(c, x, y - 1, z);
    return !fc_is_air(c, below) && !cols_cannot(c, s, below);
}
static int cols_place_column(FCtx *c, const ColsCfg *s, int sea, int ox, int oy, int oz, int col_h, int reach) {
    int any = 0; BcIt it; bc_init(&it, ox - reach, oy, oz - reach, ox + reach, oy, oz + reach);
    int x, y, z;
    while (bc_next(&it, &x, &y, &z)) {
        int step = dist_manhattan(x, y, z, ox, oy, oz), cx = x, cy = y, cz = z, found = 0;
        if (cols_air_or_lava(c, s, sea, x, y, z)) {
            int limit = step;
            while (cy > c->min_y + 1 && limit > 0) { limit--; if (cols_can_place(c, s, sea, cx, cy, cz)) { found = 1; break; } cy--; }
        } else {
            int limit = step;
            while (cy <= fc_max_y(c) && limit > 0) {
                limit--; int st = fc_get(c, cx, cy, cz);
                if (cols_cannot(c, s, st)) break;
                if (fc_is_air(c, st)) { found = 1; break; }
                cy++;
            }
        }
        if (!found) continue;
        int blocks_y = col_h - step / 2;
        while (blocks_y >= 0) {
            if (cols_air_or_lava(c, s, sea, cx, cy, cz)) { fc_set(c, cx, cy, cz, s->basalt, 3); cy++; any = 1; }
            else { if (!fc_is_block(c, fc_get(c, cx, cy, cz), c->g->state_block[s->basalt])) break; cy++; }
            blocks_y--;
        }
    }
    return any;
}
static int cols_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const ColsCfg *s = cfg; FRnd *r = c->rnd; int sea = c->sea_level;
    if (!cols_can_place(c, s, sea, ox, oy, oz)) return 0;
    int col_h = intprov_sample(s->height, r);
    int clustered = frnd_float(r) < 0.9F;
    int reach = imin_(col_h, clustered ? 5 : 8), count = clustered ? 50 : 15, any = 0;
    int w = 2 * reach + 1;
    for (int i = 0; i < count; i++) {
        int x = ox - reach + frnd_int_bound(r, w), y = oy + frnd_int_bound(r, 1), z = oz - reach + frnd_int_bound(r, w);
        int bpy = col_h - dist_manhattan(x, y, z, ox, oy, oz);
        if (bpy >= 0) { int rr = intprov_sample(s->reach, r); any |= cols_place_column(c, s, sea, x, y, z, bpy, rr); }
    }
    return any;
}

/* ====================================================================== desert_well (DesertWellFeature) */
typedef struct WellCfg { int sand_blk, sand, slab, stone, water, sus; } WellCfg;
static void *well_parse(FParse *p, const Js *cfg) {
    WellCfg *w = fp_alloc(p, sizeof *w);
    w->sand_blk = bs_block_index(p->bs, "minecraft:sand"); w->sand = named_state(p, "minecraft:sand"); w->slab = named_state(p, "minecraft:sandstone_slab");
    w->stone = named_state(p, "minecraft:sandstone"); w->water = named_state(p, "minecraft:water"); w->sus = named_state(p, "minecraft:suspicious_sand");
    return w->sus >= 0 ? w : NULL;
}
static int well_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const WellCfg *w = cfg; FRnd *r = c->rnd;
    oy++;
    while (fc_is_empty_block(c, ox, oy, oz) && oy > c->min_y + 2) oy--;
    if (!fc_is_block(c, fc_get(c, ox, oy, oz), w->sand_blk)) return 0;
    for (int dx = -2; dx <= 2; dx++) for (int dz = -2; dz <= 2; dz++)
        if (fc_is_empty_block(c, ox + dx, oy - 1, oz + dz) && fc_is_empty_block(c, ox + dx, oy - 2, oz + dz)) return 0;
    for (int dy = -2; dy <= 0; dy++) for (int dx = -2; dx <= 2; dx++) for (int dz = -2; dz <= 2; dz++) fc_set(c, ox + dx, oy + dy, oz + dz, w->stone, 2);
    fc_set(c, ox, oy, oz, w->water, 2);
    for (int i = 0; i < 4; i++) fc_set(c, ox + DIR_DX[HD4[i]], oy, oz + DIR_DZ[HD4[i]], w->water, 2);
    fc_set(c, ox, oy - 1, oz, w->sand, 2);
    for (int i = 0; i < 4; i++) fc_set(c, ox + DIR_DX[HD4[i]], oy - 1, oz + DIR_DZ[HD4[i]], w->sand, 2);
    for (int dx = -2; dx <= 2; dx++) for (int dz = -2; dz <= 2; dz++) if (dx == -2 || dx == 2 || dz == -2 || dz == 2) fc_set(c, ox + dx, oy + 1, oz + dz, w->stone, 2);
    fc_set(c, ox + 2, oy + 1, oz, w->slab, 2); fc_set(c, ox - 2, oy + 1, oz, w->slab, 2); fc_set(c, ox, oy + 1, oz + 2, w->slab, 2); fc_set(c, ox, oy + 1, oz - 2, w->slab, 2);
    for (int dx = -1; dx <= 1; dx++) for (int dz = -1; dz <= 1; dz++) fc_set(c, ox + dx, oy + 4, oz + dz, (dx == 0 && dz == 0) ? w->stone : w->slab, 2);
    for (int dy = 1; dy <= 3; dy++) {
        fc_set(c, ox - 1, oy + dy, oz - 1, w->stone, 2); fc_set(c, ox - 1, oy + dy, oz + 1, w->stone, 2);
        fc_set(c, ox + 1, oy + dy, oz - 1, w->stone, 2); fc_set(c, ox + 1, oy + dy, oz + 1, w->stone, 2);
    }
    /* waterPositions: центр, восток, юг, запад, север; Util.getRandom → nextInt(5) */
    static const int WX[5] = { 0, 1, 0, -1, 0 }, WZ[5] = { 0, 0, 1, 0, -1 };
    int i1 = frnd_int_bound(r, 5); fc_set(c, ox + WX[i1], oy - 1, oz + WZ[i1], w->sus, 3);
    int i2 = frnd_int_bound(r, 5); fc_set(c, ox + WX[i2], oy - 2, oz + WZ[i2], w->sus, 3);
    return 1;
}

static const FeatType T_GLOW = { "minecraft:glowstone_blob", glow_parse, glow_place };
static const FeatType T_PILLAR_OLD = { "minecraft:basalt_pillar", pillar_parse, pillar_place };
static const FeatType T_COLS = { "minecraft:basalt_columns", cols_parse, cols_place };
static const FeatType T_WELL = { "minecraft:desert_well", well_parse, well_place };
void feature_register_old(void) {
    feature_register_type(&T_GLOW); feature_register_type(&T_PILLAR_OLD); feature_register_type(&T_COLS); feature_register_type(&T_WELL);
}

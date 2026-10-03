/* feature_end.c — фичи Края: end_spike, end_island, end_gateway, end_platform, chorus_plant (+ end_podium/void_start_platform/fill_layer как вспомогательные). */
#include "feature_misc.h"
#include <stdio.h>
#include <stdlib.h>

static int named_state(FParse *p, const char *name) { int b = bs_block_index(p->bs, name); return b < 0 ? -1 : bs_default(p->bs, b); }
static int has_tag(const FCtx *c, const char *tag, int st) { return gen_block_tag(c->g, tag)[c->g->state_block[st]] != 0; }

/* ====================================================================== end_spike */
typedef struct Spike { int cx, cz, radius, height, guarded; } Spike;
typedef struct EndSpikeCfg { int nspikes; Spike spikes[16]; int obsidian, air, bars, bedrock, fire; } EndSpikeCfg;
static void *endspike_parse(FParse *p, const Js *cfg) {
    EndSpikeCfg *e = fp_alloc(p, sizeof *e);
    Js *l = js_get(cfg, "spikes");
    if (js_is_arr(l)) for (int i = 0; i < l->n && i < 16; i++) {
        const Js *s = l->items[i];
        e->spikes[e->nspikes++] = (Spike){ js_int(js_get(s, "centerX"), 0), js_int(js_get(s, "centerZ"), 0), js_int(js_get(s, "radius"), 0),
                                          js_int(js_get(s, "height"), 0), js_bool(js_get(s, "guarded"), 0) };
    }
    e->obsidian = named_state(p, "minecraft:obsidian"); e->air = named_state(p, "minecraft:air"); e->bars = named_state(p, "minecraft:iron_bars");
    e->bedrock = named_state(p, "minecraft:bedrock"); e->fire = named_state(p, "minecraft:fire");
    return e;
}
/* EndSpikeFeature.getSpikesForLevel: перемешанный список 0..9 на LCG от (nextLong() & 65535) LCG от сида мира */
static void spikes_for_level(i64 level_seed, Spike out[10]) {
    Rnd r0 = rnd_legacy_seed(level_seed);
    i64 key = rnd_next_long(&r0) & 65535;
    Rnd r = rnd_legacy_seed(key);
    int sizes[10]; for (int i = 0; i < 10; i++) sizes[i] = i;
    for (int i = 10; i > 1; i--) { int to = rnd_next_int_bound(&r, i); int t = sizes[i - 1]; sizes[i - 1] = sizes[to]; sizes[to] = t; }
    const double PI = 3.141592653589793;
    for (int i = 0; i < 10; i++) {
        int x = (int)floor(42.0 * cos(2.0 * (-PI + (PI / 10) * i)));
        int z = (int)floor(42.0 * sin(2.0 * (-PI + (PI / 10) * i)));
        int size = sizes[i];
        out[i] = (Spike){ x, z, 2 + size / 3, 76 + size * 3, size == 1 || size == 2 };
    }
}
static void place_spike(FCtx *c, const EndSpikeCfg *e, const Spike *s) {
    int r = s->radius;
    BcIt it; bc_init(&it, s->cx - r, c->min_y, s->cz - r, s->cx + r, s->height + 10, s->cz + r);
    int x, y, z;
    while (bc_next(&it, &x, &y, &z)) {
        int dx = x - s->cx, dz = z - s->cz;
        if (dx * dx + dz * dz <= r * r + 1 && y < s->height) fc_set(c, x, y, z, e->obsidian, 3);
        else if (y > 65) fc_set(c, x, y, z, e->air, 3);
    }
    if (s->guarded) {
        for (int dx = -2; dx <= 2; dx++) for (int dz = -2; dz <= 2; dz++) for (int dy = 0; dy <= 3; dy++) {
            int xs = iabs_(dx) == 2, zs = iabs_(dz) == 2, top = dy == 3;
            if (xs || zs || top) {
                int xe = dx == -2 || dx == 2 || top, ze = dz == -2 || dz == 2 || top;
                int st = e->bars;
                st = bs_with(c->bs, st, "north", (xe && dz != -2) ? "true" : "false");
                st = bs_with(c->bs, st, "south", (xe && dz != 2) ? "true" : "false");
                st = bs_with(c->bs, st, "west", (ze && dx != -2) ? "true" : "false");
                st = bs_with(c->bs, st, "east", (ze && dx != 2) ? "true" : "false");
                fc_set(c, s->cx + dx, s->height + dy, s->cz + dz, st, 3);
            }
        }
    }
    /* кристалл: энтити — только случайный угол (nextFloat), подставка и огонь под ним */
    (void)frnd_float(c->rnd);
    fc_set(c, s->cx, s->height, s->cz, e->bedrock, 3);
    fc_set(c, s->cx, s->height + 1, s->cz, e->fire, 3);
}
static int endspike_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const EndSpikeCfg *e = cfg; Spike gen[10]; const Spike *sp = e->spikes; int n = e->nspikes;
    if (n == 0) { spikes_for_level(c->w->seeds.features, gen); sp = gen; n = 10; }
    for (int i = 0; i < n; i++) if ((ox >> 4) == (sp[i].cx >> 4) && (oz >> 4) == (sp[i].cz >> 4)) place_spike(c, e, &sp[i]);
    return 1;
}

/* ====================================================================== end_island */
static void *endisland_parse(FParse *p, const Js *cfg) { int *s = fp_alloc(p, 16); *s = named_state(p, "minecraft:end_stone"); return s; }
static int endisland_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    int stone = *(const int *)cfg; FRnd *r = c->rnd;
    float size = (float)frnd_int_bound(r, 3) + 4.0F;
    for (int y = 0; size > 0.5F; y--) {
        int x0 = (int)floorf(-size), x1 = (int)ceilf(size);
        float lim = (size + 1.0F) * (size + 1.0F);
        for (int x = x0; x <= x1; x++) for (int z = x0; z <= x1; z++) {
            if ((float)(x * x + z * z) <= lim) fc_set(c, ox + x, oy + y, oz + z, stone, 3);
        }
        size -= (float)frnd_int_bound(r, 2) + 0.5F;
    }
    return 1;
}

/* ====================================================================== end_gateway */
typedef struct GateCfg { int gate, air, bedrock; } GateCfg;
static void *gate_parse(FParse *p, const Js *cfg) {
    GateCfg *g = fp_alloc(p, sizeof *g);
    g->gate = named_state(p, "minecraft:end_gateway"); g->air = named_state(p, "minecraft:air"); g->bedrock = named_state(p, "minecraft:bedrock");
    return g;
}
static int gate_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const GateCfg *g = cfg;
    BcIt it; bc_init(&it, ox - 1, oy - 2, oz - 1, ox + 1, oy + 2, oz + 1);
    int x, y, z;
    while (bc_next(&it, &x, &y, &z)) {
        int sx = x == ox, sy = y == oy, sz = z == oz, end = iabs_(y - oy) == 2;
        if (sx && sy && sz) fc_set(c, x, y, z, g->gate, 3);
        else if (sy) fc_set(c, x, y, z, g->air, 3);
        else if (end && sx && sz) fc_set(c, x, y, z, g->bedrock, 3);
        else if ((sx || sz) && !end) fc_set(c, x, y, z, g->bedrock, 3);
        else fc_set(c, x, y, z, g->air, 3);
    }
    return 1;
}

/* ====================================================================== end_platform */
typedef struct PlatCfg { int obsidian, air; } PlatCfg;
static void *plat_parse(FParse *p, const Js *cfg) { PlatCfg *s = fp_alloc(p, sizeof *s); s->obsidian = named_state(p, "minecraft:obsidian"); s->air = named_state(p, "minecraft:air"); return s; }
static int plat_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const PlatCfg *s = cfg;
    for (int dz = -2; dz <= 2; dz++) for (int dx = -2; dx <= 2; dx++) for (int dy = -1; dy < 3; dy++) {
        int want = dy == -1 ? s->obsidian : s->air, x = ox + dx, y = oy + dy, z = oz + dz;
        if (c->g->state_block[fc_get(c, x, y, z)] != c->g->state_block[want]) fc_set(c, x, y, z, want, 3);
    }
    return 1;
}

/* ====================================================================== chorus_plant */
typedef struct ChorusCfg { int plant, flower5; } ChorusCfg;
static void *chorus_parse(FParse *p, const Js *cfg) {
    ChorusCfg *s = fp_alloc(p, sizeof *s);
    s->plant = named_state(p, "minecraft:chorus_plant"); s->flower5 = bs_with(p->bs, named_state(p, "minecraft:chorus_flower"), "age", "5");
    return s->plant >= 0 && s->flower5 >= 0 ? s : NULL;
}
static int chorus_with_connections(FCtx *c, const ChorusCfg *s, int x, int y, int z) {
    int plant_blk = c->g->state_block[s->plant];
    int flower_blk = c->g->state_block[s->flower5];
    int st = s->plant;
    int below = fc_get(c, x, y - 1, z), up = fc_get(c, x, y + 1, z);
    int n = fc_get(c, x, y, z - 1), e = fc_get(c, x + 1, y, z), so = fc_get(c, x, y, z + 1), w = fc_get(c, x - 1, y, z);
    #define IS_C(v) (c->g->state_block[v] == plant_blk || c->g->state_block[v] == flower_blk)
    st = bs_try_with(c->bs, st, "down", (IS_C(below) || has_tag(c, "minecraft:supports_chorus_plant", below)) ? "true" : "false");
    st = bs_try_with(c->bs, st, "up", IS_C(up) ? "true" : "false");
    st = bs_try_with(c->bs, st, "north", IS_C(n) ? "true" : "false");
    st = bs_try_with(c->bs, st, "east", IS_C(e) ? "true" : "false");
    st = bs_try_with(c->bs, st, "south", IS_C(so) ? "true" : "false");
    st = bs_try_with(c->bs, st, "west", IS_C(w) ? "true" : "false");
    #undef IS_C
    return st;
}
static const int HDIR[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };   /* Direction.Plane.HORIZONTAL: N, E, S, W */
static int chorus_all_empty(FCtx *c, int x, int y, int z, int ignore) {
    for (int i = 0; i < 4; i++) { int d = HDIR[i]; if (d != ignore && !fc_is_air(c, fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]))) return 0; }
    return 1;
}
static int opposite_h(int d) { return d == DIR_NORTH ? DIR_SOUTH : d == DIR_SOUTH ? DIR_NORTH : d == DIR_EAST ? DIR_WEST : DIR_EAST; }
static void chorus_grow(FCtx *c, const ChorusCfg *s, int cx, int cy, int cz, int sx, int sz, int spread, int depth) {
    FRnd *r = c->rnd;
    int height = frnd_int_bound(r, 4) + 1;
    if (depth == 0) height++;
    for (int i = 0; i < height; i++) {
        int ty = cy + i + 1;
        if (!chorus_all_empty(c, cx, ty, cz, -1)) return;
        fc_set(c, cx, ty, cz, chorus_with_connections(c, s, cx, ty, cz), 2);
        fc_set(c, cx, ty - 1, cz, chorus_with_connections(c, s, cx, ty - 1, cz), 2);
    }
    int placed_stem = 0;
    if (depth < 4) {
        int stems = frnd_int_bound(r, 4);
        if (depth == 0) stems++;
        for (int i = 0; i < stems; i++) {
            int d = HDIR[frnd_int_bound(r, 4)];
            int tx = cx + DIR_DX[d], ty = cy + height, tz = cz + DIR_DZ[d];
            if (iabs_(tx - sx) < spread && iabs_(tz - sz) < spread && fc_is_air(c, fc_get(c, tx, ty, tz)) && fc_is_air(c, fc_get(c, tx, ty - 1, tz))
                && chorus_all_empty(c, tx, ty, tz, opposite_h(d))) {
                placed_stem = 1;
                fc_set(c, tx, ty, tz, chorus_with_connections(c, s, tx, ty, tz), 2);
                int ox = tx + DIR_DX[opposite_h(d)], oz = tz + DIR_DZ[opposite_h(d)];
                fc_set(c, ox, ty, oz, chorus_with_connections(c, s, ox, ty, oz), 2);
                chorus_grow(c, s, tx, ty, tz, sx, sz, spread, depth + 1);
            }
        }
    }
    if (!placed_stem) fc_set(c, cx, cy + height, cz, s->flower5, 2);
}
static int chorus_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const ChorusCfg *s = cfg;
    if (!fc_is_air(c, fc_get(c, x, y, z)) || !has_tag(c, "minecraft:supports_chorus_plant", fc_get(c, x, y - 1, z))) return 0;
    fc_set(c, x, y, z, chorus_with_connections(c, s, x, y, z), 2);
    chorus_grow(c, s, x, y, z, x, z, 8, 0);
    return 1;
}

static const FeatType T_ENDSPIKE = { "minecraft:end_spike", endspike_parse, endspike_place };
static const FeatType T_ENDISLAND = { "minecraft:end_island", endisland_parse, endisland_place };
static const FeatType T_GATE = { "minecraft:end_gateway", gate_parse, gate_place };
static const FeatType T_PLAT = { "minecraft:end_platform", plat_parse, plat_place };
static const FeatType T_CHORUS = { "minecraft:chorus_plant", chorus_parse, chorus_place };
void feature_register_end(void) {
    feature_register_type(&T_ENDSPIKE); feature_register_type(&T_ENDISLAND); feature_register_type(&T_GATE); feature_register_type(&T_PLAT);
    feature_register_type(&T_CHORUS);
}

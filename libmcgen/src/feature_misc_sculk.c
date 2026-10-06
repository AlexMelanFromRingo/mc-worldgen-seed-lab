/* feature_misc_sculk.c — «многогранные» блоки и скалк: multiface_growth (sculk_vein, glow_lichen), sculk_patch (SculkSpreader мира: курсоры заряда).
 * Порядок вызовов ГСЧ — как в игре (Direction.allShuffled, Util.shuffle…). Код блоков: SculkBlock, SculkVeinBlock, MultifaceBlock, MultifaceSpreader. */
#include "feature_misc.h"
#include <stdio.h>
#include <stdlib.h>

static const char *FACEP[6] = { "down", "up", "north", "south", "west", "east" };
static const int OPP6[6] = { DIR_UP, DIR_DOWN, DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST };
static inline int axis_of(int d) { return d < 2 ? 1 : (d < 4 ? 2 : 0); }      /* Y, Z, X */

/* ---------------------------------------------------------------------- состояния многогранных блоков */
typedef struct MfEnv {
    int blk, def;                   /* блок размещения и его состояние по умолчанию */
    int vein_cfg;                   /* конфигурация распространения скалковой жилы (иначе — по умолчанию) */
    int sculk_blk, catalyst_blk, piston_blk, vein_blk, water_blk;
    const u8 *fire_tag, *can_be_placed_on;
} MfEnv;

static int mf_has_face(const BsTab *bs, int st, int d) { const char *v; return bs_get_prop(bs, st, FACEP[d], &v) && v[0] == 't'; }
static int mf_has_any(const BsTab *bs, int st) { for (int d = 0; d < 6; d++) if (mf_has_face(bs, st, d)) return 1; return 0; }
/* MultifaceBlock.canAttachTo(level, dir, neighbourPos, neighbourState): грань соседа, обращённая к нам, — полная (опорная форма или коллайдер) */
static int mf_can_attach_state(const FCtx *c, int nst, int d) {
    return ((c->bs->sturdy[nst] >> OPP6[d]) & 1) || (c->bs->flags[nst] & BSF_FULL_COLL);
}
static int mf_can_attach(const FCtx *c, int x, int y, int z, int d) { return mf_can_attach_state(c, fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]), d); }
static int mf_valid_for_placement(const FCtx *c, const MfEnv *e, int old, int x, int y, int z, int d) {
    if (c->g->state_block[old] == e->blk && mf_has_face(c->bs, old, d)) return 0;
    return mf_can_attach(c, x, y, z, d);
}
/* getStateForPlacement(oldState, level, pos, dir); −1 — null */
static int mf_state_for_placement(const FCtx *c, const MfEnv *e, int old, int x, int y, int z, int d) {
    if (!mf_valid_for_placement(c, e, old, x, y, z, d)) return -1;
    int ns;
    if (c->g->state_block[old] == e->blk) ns = old;
    else if (BS_FL_TYPE(c->bs->fluid[old]) == FL_WATER) ns = bs_with(c->bs, e->def, "waterlogged", "true");        /* isSourceOfType(WATER) */
    else ns = e->def;
    return bs_with(c->bs, ns, FACEP[d], "true");
}

/* Direction.allShuffled / Util.shuffle */
static void shuffled6(FRnd *r, int out[6]) { for (int i = 0; i < 6; i++) out[i] = i; for (int i = 6; i > 1; i--) { int to = frnd_int_bound(r, i); int t = out[i - 1]; out[i - 1] = out[to]; out[to] = t; } }

/* ---------------------------------------------------------------------- MultifaceSpreader */
enum { SP_SAME_POS = 0, SP_SAME_PLANE = 1, SP_WRAP = 2 };
typedef struct SpPos { int x, y, z, face; } SpPos;
static SpPos sp_pos(int type, int x, int y, int z, int spread_dir, int from_face) {
    SpPos r;
    if (type == SP_SAME_POS) { r = (SpPos){ x, y, z, spread_dir }; }
    else if (type == SP_SAME_PLANE) { r = (SpPos){ x + DIR_DX[spread_dir], y + DIR_DY[spread_dir], z + DIR_DZ[spread_dir], from_face }; }
    else { r = (SpPos){ x + DIR_DX[spread_dir] + DIR_DX[from_face], y + DIR_DY[spread_dir] + DIR_DY[from_face], z + DIR_DZ[spread_dir] + DIR_DZ[from_face], OPP6[spread_dir] }; }
    return r;
}
static int sp_default_replaceable(const FCtx *c, const MfEnv *e, int ex) {
    return fc_is_air(c, ex) || c->g->state_block[ex] == e->blk || (c->g->state_block[ex] == e->water_blk && BS_FL_SOURCE(c->bs->fluid[ex]) && BS_FL_TYPE(c->bs->fluid[ex]) != FL_NONE);
}
static int sp_state_can_be_replaced(const FCtx *c, const MfEnv *e, int sx, int sy, int sz, int px, int py, int pz, int pdir, int ex) {
    if (!e->vein_cfg) return sp_default_replaceable(c, e, ex);
    int against = fc_get(c, px + DIR_DX[pdir], py + DIR_DY[pdir], pz + DIR_DZ[pdir]);
    int ab = c->g->state_block[against];
    if (ab == e->sculk_blk || ab == e->catalyst_blk || ab == e->piston_blk) return 0;
    if (dist_manhattan(sx, sy, sz, px, py, pz) == 2) {
        int nx = sx + DIR_DX[OPP6[pdir]], ny = sy + DIR_DY[OPP6[pdir]], nz = sz + DIR_DZ[OPP6[pdir]];
        if ((c->bs->sturdy[fc_get(c, nx, ny, nz)] >> pdir) & 1) return 0;
    }
    int ft = BS_FL_TYPE(c->bs->fluid[ex]);
    if (ft != FL_NONE && ft != FL_WATER) return 0;
    if (e->fire_tag[c->g->state_block[ex]]) return 0;
    return (c->bs->flags[ex] & BSF_REPLACEABLE) || sp_default_replaceable(c, e, ex);
}
static int sp_can_spread_into(const FCtx *c, const MfEnv *e, int sx, int sy, int sz, const SpPos *sp) {
    int ex = fc_get(c, sp->x, sp->y, sp->z);
    return sp_state_can_be_replaced(c, e, sx, sy, sz, sp->x, sp->y, sp->z, sp->face, ex) && mf_valid_for_placement(c, e, ex, sp->x, sp->y, sp->z, sp->face);
}
static int sp_other_valid_source(const MfEnv *e, const FCtx *c, int state) { return e->vein_cfg && c->g->state_block[state] != e->vein_blk; }
/* getSpreadFromFaceTowardDirection; types — набор типов размещения */
static int sp_get(const FCtx *c, const MfEnv *e, const int *types, int ntypes, int state, int x, int y, int z, int start_face, int spread_dir, SpPos *out) {
    if (axis_of(spread_dir) == axis_of(start_face)) return 0;
    if (sp_other_valid_source(e, c, state) || (mf_has_face(c->bs, state, start_face) && !mf_has_face(c->bs, state, spread_dir))) {
        for (int i = 0; i < ntypes; i++) {
            SpPos sp = sp_pos(types[i], x, y, z, spread_dir, start_face);
            if (sp_can_spread_into(c, e, x, y, z, &sp)) { *out = sp; return 1; }
        }
    }
    return 0;
}
static int sp_spread_to_face(FCtx *c, const MfEnv *e, const SpPos *sp, int post) {
    int old = fc_get(c, sp->x, sp->y, sp->z);
    int st = mf_state_for_placement(c, e, old, sp->x, sp->y, sp->z, sp->face);
    if (st < 0) return 0;
    if (post) fc_mark(c, sp->x, sp->y, sp->z);
    return fc_set(c, sp->x, sp->y, sp->z, st, 2);
}
static int sp_from_face_toward_dir(FCtx *c, const MfEnv *e, const int *types, int nt, int state, int x, int y, int z, int from, int dir, int post) {
    SpPos sp;
    if (!sp_get(c, e, types, nt, state, x, y, z, from, dir, &sp)) return 0;
    return sp_spread_to_face(c, e, &sp, post);
}
static int sp_from_face_random_dir(FCtx *c, const MfEnv *e, const int *types, int nt, int state, int x, int y, int z, int from, int post) {
    int dirs[6]; shuffled6(c->rnd, dirs);
    for (int i = 0; i < 6; i++) if (sp_from_face_toward_dir(c, e, types, nt, state, x, y, z, from, dirs[i], post)) return 1;
    return 0;
}
static long sp_spread_all(FCtx *c, const MfEnv *e, const int *types, int nt, int state, int x, int y, int z, int post) {
    long n = 0;
    for (int face = 0; face < 6; face++) {
        if (!(sp_other_valid_source(e, c, state) || mf_has_face(c->bs, state, face))) continue;     /* canSpreadFrom */
        for (int d = 0; d < 6; d++) n += sp_from_face_toward_dir(c, e, types, nt, state, x, y, z, face, d, post);
    }
    return n;
}
static const int T_ALL[3] = { SP_SAME_POS, SP_SAME_PLANE, SP_WRAP };
static const int T_SAME[1] = { SP_SAME_POS };

static void mf_env_init(FParse *p, MfEnv *e, const char *block) {
    e->blk = bs_block_index(p->bs, block); e->def = e->blk >= 0 ? bs_default(p->bs, e->blk) : -1;
    e->vein_cfg = !strcmp(block, "minecraft:sculk_vein");
    e->sculk_blk = bs_block_index(p->bs, "minecraft:sculk"); e->catalyst_blk = bs_block_index(p->bs, "minecraft:sculk_catalyst");
    e->piston_blk = bs_block_index(p->bs, "minecraft:moving_piston"); e->vein_blk = bs_block_index(p->bs, "minecraft:sculk_vein"); e->water_blk = bs_block_index(p->bs, "minecraft:water");
    e->fire_tag = gen_block_tag(p->g, "minecraft:fire");
}

/* ====================================================================== multiface_growth */
typedef struct MfCfg { MfEnv e; int search_range, floor, ceiling, wall; float chance; u8 *placed_on; } MfCfg;
static void *mf_parse(FParse *p, const Js *cfg) {
    MfCfg *m = fp_alloc(p, sizeof *m);
    const char *b = js_str(js_get(cfg, "block"), NULL); if (!b) { fp_fail(p, "multiface_growth: нет block"); return NULL; }
    mf_env_init(p, &m->e, b); if (m->e.blk < 0) { fp_fail(p, "multiface_growth: нет блока %s", b); return NULL; }
    m->search_range = js_get(cfg, "search_range") ? js_int(js_get(cfg, "search_range"), 10) : 10;
    m->floor = js_bool(js_get(cfg, "can_place_on_floor"), 0); m->ceiling = js_bool(js_get(cfg, "can_place_on_ceiling"), 0); m->wall = js_bool(js_get(cfg, "can_place_on_wall"), 0);
    m->chance = js_get(cfg, "chance_of_spreading") ? js_numf(js_get(cfg, "chance_of_spreading"), 0.5f) : 0.5f;
    m->placed_on = fp_blockset(p, js_get(cfg, "can_be_placed_on")); if (!m->placed_on) { fp_fail(p, "multiface_growth: can_be_placed_on"); return NULL; }
    return m;
}
static int mf_valid_dirs(const MfCfg *m, int out[6]) {
    int n = 0;
    if (m->ceiling) out[n++] = DIR_UP;
    if (m->floor) out[n++] = DIR_DOWN;
    if (m->wall) { out[n++] = DIR_NORTH; out[n++] = DIR_EAST; out[n++] = DIR_SOUTH; out[n++] = DIR_WEST; }
    return n;
}
static void shuffle_list(FRnd *r, int *l, int n) { for (int i = n; i > 1; i--) { int to = frnd_int_bound(r, i); int t = l[i - 1]; l[i - 1] = l[to]; l[to] = t; } }
static int mf_place_growth(FCtx *c, const MfCfg *m, int x, int y, int z, int old, const int *dirs, int nd) {
    FRnd *r = c->rnd;
    for (int i = 0; i < nd; i++) {
        int d = dirs[i];
        int nb = fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]);
        if (m->placed_on[c->g->state_block[nb]]) {
            int ns = mf_state_for_placement(c, &m->e, old, x, y, z, d);
            if (ns < 0) return 0;
            fc_set(c, x, y, z, ns, 3);
            fc_mark(c, x, y, z);
            if (frnd_float(r) < m->chance) sp_from_face_random_dir(c, &m->e, m->e.vein_cfg ? T_ALL : T_ALL, 3, ns, x, y, z, d, 1);
            return 1;
        }
    }
    return 0;
}
static int mf_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const MfCfg *m = cfg; FRnd *r = c->rnd;
    int st0 = fc_get(c, ox, oy, oz);
    #define AIR_OR_WATER(s_) (fc_is_air(c, s_) || c->g->state_block[s_] == m->e.water_blk)
    if (!AIR_OR_WATER(st0)) return 0;
    int dirs[6]; int nd = mf_valid_dirs(m, dirs);
    shuffle_list(r, dirs, nd);
    if (mf_place_growth(c, m, ox, oy, oz, fc_get(c, ox, oy, oz), dirs, nd)) return 1;
    for (int k = 0; k < nd; k++) {
        int sd = dirs[k];
        int pd[6], np = 0, all[6]; int na = mf_valid_dirs(m, all);
        for (int i = 0; i < na; i++) if (all[i] != OPP6[sd]) pd[np++] = all[i];
        shuffle_list(r, pd, np);
        for (int i = 0; i < m->search_range; i++) {
            int px = ox + DIR_DX[sd], py = oy + DIR_DY[sd], pz = oz + DIR_DZ[sd];
            int st = fc_get(c, px, py, pz);
            if (!AIR_OR_WATER(st) && c->g->state_block[st] != m->e.blk) break;
            if (mf_place_growth(c, m, px, py, pz, st, pd, np)) return 1;
        }
    }
    #undef AIR_OR_WATER
    return 0;
}

/* ====================================================================== sculk_patch */
static int sculk_new(const FCtx *c) { static int ov = -2; if (ov == -2) { const char *e = getenv("MCGEN_SCULK_NEW"); ov = e ? atoi(e) : -1; } return ov >= 0 ? ov : c->g->newf; }
typedef struct Cursor { int x, y, z, charge, update_delay, decay_delay, has_fac; u8 fac; } Cursor;
typedef struct PatchCfg { int charge_count, per_charge, attempts, growth_rounds, spread_rounds; MfEnv e; const u8 *replace_wg, *inhibitors; int sculk_state, sensor, shrieker, shrieker_can, vein_state;
    int old_post; float catalyst_chance; IntProv *extra_rare; int catalyst; } PatchCfg;
static void *patch_parse(FParse *p, const Js *cfg) {
    PatchCfg *s = fp_alloc(p, sizeof *s);
    s->charge_count = js_int(js_get(cfg, "charge_count"), 1); s->per_charge = js_int(js_get(cfg, "amount_per_charge"), 1);
    s->attempts = js_int(js_get(cfg, "spread_attempts"), 1); s->growth_rounds = js_int(js_get(cfg, "growth_rounds"), 0); s->spread_rounds = js_int(js_get(cfg, "spread_rounds"), 0);
    mf_env_init(p, &s->e, "minecraft:sculk_vein");
    s->replace_wg = gen_block_tag(p->g, "minecraft:sculk_replaceable_world_gen"); s->inhibitors = gen_block_tag(p->g, "minecraft:sculk_growth_inhibitors");
    if (!p->newf) {            /* 26.1/26.2: тега sculk_growth_inhibitors нет — SculkBlock.canPlaceGrowth считает sculk_sensor и sculk_shrieker прямо в коде */
        u8 *t = fp_alloc(p, (size_t)(p->bs->nblocks ? p->bs->nblocks : 1));
        int a = bs_block_index(p->bs, "minecraft:sculk_sensor"), b = bs_block_index(p->bs, "minecraft:sculk_shrieker");
        if (a >= 0) t[a] = 1;
        if (b >= 0) t[b] = 1;
        s->inhibitors = t;
    }
    s->sculk_state = bs_default(p->bs, s->e.sculk_blk); s->sensor = bs_default(p->bs, bs_block_index(p->bs, "minecraft:sculk_sensor"));
    s->shrieker = bs_default(p->bs, bs_block_index(p->bs, "minecraft:sculk_shrieker")); s->shrieker_can = bs_with(p->bs, s->shrieker, "can_summon", "true");
    s->vein_state = s->e.def;
    /* 26.1/26.2: катализатор и редкие шрикеры — часть самой фичи (после раундов); в 26.3 вынесены в sequence/simple_block */
    if (js_get(cfg, "catalyst_chance")) {
        s->old_post = 1; s->catalyst_chance = js_numf(js_get(cfg, "catalyst_chance"), 0.0f);
        s->extra_rare = js_get(cfg, "extra_rare_growths") ? fp_intprov(p, js_get(cfg, "extra_rare_growths")) : NULL;
        s->catalyst = bs_default(p->bs, bs_block_index(p->bs, "minecraft:sculk_catalyst"));
    }
    return s;
}
static inline int is_sculk_behaviour(const PatchCfg *s, const FCtx *c, int st) { int b = c->g->state_block[st]; return b == s->e.sculk_blk || b == s->e.vein_blk; }

/* SculkVeinBlock.onDischarged(level, state, pos, random) — state: состояние, запомненное курсором */
static void vein_on_discharged(FCtx *c, const PatchCfg *s, int state, int x, int y, int z) {
    const BsTab *bs = c->bs;
    if (c->g->state_block[state] != s->e.vein_blk) return;
    for (int d = 0; d < 6; d++) {
        if (mf_has_face(bs, state, d) && c->g->state_block[fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d])] == s->e.sculk_blk) state = bs_with(bs, state, FACEP[d], "false");
    }
    if (!mf_has_any(bs, state)) state = BS_FL_TYPE(bs->fluid[fc_get(c, x, y, z)]) == FL_NONE ? c->st_air : bs_default(bs, s->e.water_blk);
    fc_set(c, x, y, z, state, 3);
}
/* SculkVeinBlock.regrow */
static int vein_regrow(FCtx *c, const PatchCfg *s, int x, int y, int z, int existing, u8 faces) {
    const BsTab *bs = c->bs; int ns = s->vein_state, any = 0;
    for (int d = 0; d < 6; d++) if (((faces >> d) & 1) && mf_can_attach(c, x, y, z, d)) { ns = bs_with(bs, ns, FACEP[d], "true"); any = 1; }
    if (!any) return 0;
    if (BS_FL_TYPE(bs->fluid[existing]) != FL_NONE) ns = bs_with(bs, ns, "waterlogged", "true");
    fc_set(c, x, y, z, ns, 3);
    return 1;
}
/* SculkVeinBlock.attemptPlaceSculk */
static int vein_place_sculk(FCtx *c, const PatchCfg *s, int x, int y, int z) {
    int state = fc_get(c, x, y, z), dirs[6]; shuffled6(c->rnd, dirs);
    for (int i = 0; i < 6; i++) {
        int sup = dirs[i];
        if (!mf_has_face(c->bs, state, sup)) continue;
        int sx = x + DIR_DX[sup], sy = y + DIR_DY[sup], sz = z + DIR_DZ[sup];
        if (s->replace_wg[c->g->state_block[fc_get(c, sx, sy, sz)]]) {
            fc_set(c, sx, sy, sz, s->sculk_state, 3);
            sp_spread_all(c, &s->e, T_ALL, 3, s->sculk_state, sx, sy, sz, 1);
            int skip = OPP6[sup];
            for (int d = 0; d < 6; d++) {
                if (d == skip) continue;
                int vx = sx + DIR_DX[d], vy = sy + DIR_DY[d], vz = sz + DIR_DZ[d], pv = fc_get(c, vx, vy, vz);
                if (c->g->state_block[pv] == s->e.vein_blk) vein_on_discharged(c, s, pv, vx, vy, vz);
            }
            return 1;
        }
    }
    return 0;
}
static int count_inhibitors_ok(const FCtx *c, const PatchCfg *s, int x, int y, int z) {
    int n = 0;
    for (int xx = x - 4; xx <= x + 4; xx++) for (int yy = y; yy <= y + 2; yy++) for (int zz = z - 4; zz <= z + 4; zz++) {
        if (s->inhibitors[c->g->state_block[fc_get(c, xx, yy, zz)]] && ++n > 2) return 0;
    }
    return 1;
}
/* SculkBlock.attemptUseCharge */
static int sculk_use_charge(FCtx *c, const PatchCfg *s, const Cursor *cu, int ox, int oy, int oz) {
    FRnd *r = c->rnd; const int DECAY = 5, GROWTH_COST = 50, NO_GROWTH = 1, ADD_DECAY = 10;
    int charge = cu->charge;
    if (charge != 0 && frnd_int_bound(r, DECAY) == 0) {
        int close = ((cu->x - ox) * (cu->x - ox) + (cu->y - oy) * (cu->y - oy) + (cu->z - oz) * (cu->z - oz)) < NO_GROWTH * NO_GROWTH;
        int above = fc_get(c, cu->x, cu->y + 1, cu->z);
        int can_growth = 0;
        if (fc_is_air(c, above) || (c->g->state_block[above] == s->e.water_blk && BS_FL_TYPE(c->bs->fluid[above]) == FL_WATER)) can_growth = count_inhibitors_ok(c, s, cu->x, cu->y, cu->z);
        if (!close && can_growth) {
            if (frnd_int_bound(r, GROWTH_COST) < charge) {
                int gy = cu->y + 1;
                int st = frnd_int_bound(r, 11) == 0 ? s->shrieker_can : s->sensor;
                if (bs_has_prop(c->bs, st, "waterlogged") && BS_FL_TYPE(c->bs->fluid[fc_get(c, cu->x, gy, cu->z)]) != FL_NONE) st = bs_with(c->bs, st, "waterlogged", "true");
                fc_set(c, cu->x, gy, cu->z, st, 3);
            }
            return imax_(0, charge - GROWTH_COST);
        }
        if (frnd_int_bound(r, ADD_DECAY) != 0) return charge;
        if (close) return charge - 1;
        /* getDecayPenalty */
        double d2 = (double)(cu->x - ox) * (double)(cu->x - ox) + (double)(cu->y - oy) * (double)(cu->y - oy) + (double)(cu->z - oz) * (double)(cu->z - oz);
        float outer = (float)sqrt(d2) - (float)NO_GROWTH; outer = outer * outer;
        int reach = (24 - NO_GROWTH) * (24 - NO_GROWTH);
        float f = outer / (float)reach; f = f < 1.0F ? f : 1.0F;
        int pen = (int)((float)charge * f * 0.5F); pen = imax_(1, pen);
        return charge - pen;
    }
    return charge;
}
/* behaviour: 0 — DEFAULT, 1 — sculk, 2 — sculk_vein */
static int behaviour_of(const PatchCfg *s, const FCtx *c, int st) { int b = c->g->state_block[st]; return b == s->e.sculk_blk ? 1 : (b == s->e.vein_blk ? 2 : 0); }
static u8 available_faces(const FCtx *c, const PatchCfg *s, int st) {
    if (c->g->state_block[st] != s->e.vein_blk) return 0;      /* только MultifaceBlock; sculk — пустой набор */
    u8 f = 0; for (int d = 0; d < 6; d++) if (mf_has_face(c->bs, st, d)) f |= (u8)(1 << d);
    return f;
}
static int attempt_spread_vein(FCtx *c, const PatchCfg *s, int beh, const Cursor *cu, int state) {
    if (beh == 0) {
        if (!cu->has_fac) return sp_spread_all(c, &s->e, T_SAME, 1, fc_get(c, cu->x, cu->y, cu->z), cu->x, cu->y, cu->z, 1) > 0;
        if (cu->fac) {
            if (!fc_is_air(c, state) && BS_FL_TYPE(c->bs->fluid[state]) != FL_WATER) return 0;
            return vein_regrow(c, s, cu->x, cu->y, cu->z, state, cu->fac);
        }
    }
    return sp_spread_all(c, &s->e, T_ALL, 3, state, cu->x, cu->y, cu->z, 1) > 0;
}
static int use_charge(FCtx *c, const PatchCfg *s, int beh, Cursor *cu, int ox, int oy, int oz, int spread_veins) {
    FRnd *r = c->rnd;
    if (beh == 0) return cu->decay_delay > 0 ? cu->charge : 0;
    if (beh == 1) return sculk_use_charge(c, s, cu, ox, oy, oz);
    /* вена */
    if (spread_veins && vein_place_sculk(c, s, cu->x, cu->y, cu->z)) return cu->charge - 1;
    return frnd_int_bound(r, 5) == 0 ? (int)floorf((float)cu->charge * 0.5F) : cu->charge;
}
static int has_substrate_access(const FCtx *c, const PatchCfg *s, int st, int x, int y, int z) {
    if (c->g->state_block[st] != s->e.vein_blk) return 0;
    const u8 *rep = gen_block_tag(c->g, "minecraft:sculk_replaceable");
    for (int d = 0; d < 6; d++) if (mf_has_face(c->bs, st, d) && rep[c->g->state_block[fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d])]]) return 1;
    return 0;
}
static int unobstructed(const FCtx *c, int x, int y, int z, int d) { int t = fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]); return !((c->bs->sturdy[t] >> OPP6[d]) & 1); }
static int movement_unobstructed(const FCtx *c, int fx, int fy, int fz, int tx, int ty, int tz) {
    if (dist_manhattan(fx, fy, fz, tx, ty, tz) == 1) return 1;
    int dx = tx - fx, dy = ty - fy, dz = tz - fz;
    int DX = dx < 0 ? DIR_WEST : DIR_EAST, DY = dy < 0 ? DIR_DOWN : DIR_UP, DZ = dz < 0 ? DIR_NORTH : DIR_SOUTH;
    if (dx == 0) return unobstructed(c, fx, fy, fz, DY) || unobstructed(c, fx, fy, fz, DZ);
    if (dy == 0) return unobstructed(c, fx, fy, fz, DX) || unobstructed(c, fx, fy, fz, DZ);
    return unobstructed(c, fx, fy, fz, DX) || unobstructed(c, fx, fy, fz, DY);
}
static int g_nc[18][3]; static int g_nc_ready;
static void nc_init(void) {
    if (g_nc_ready) return;
    int n = 0;
    for (int z = -1; z <= 1; z++) for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) if ((x == 0 || y == 0 || z == 0) && !(x == 0 && y == 0 && z == 0)) { g_nc[n][0] = x; g_nc[n][1] = y; g_nc[n][2] = z; n++; }
    g_nc_ready = 1;
}
/* ChargeCursor.getValidMovementPos: 1 — есть новая позиция (в out) */
static int valid_move(FCtx *c, const PatchCfg *s, const Cursor *cu, int ox, int oz, int out[3]) {
    int idx[18]; for (int i = 0; i < 18; i++) idx[i] = i;
    shuffle_list(c->rnd, idx, 18);
    int sx = cu->x, sy = cu->y, sz = cu->z;
    for (int i = 0; i < 18; i++) {
        int nx = cu->x + g_nc[idx[i]][0], ny = cu->y + g_nc[idx[i]][1], nz = cu->z + g_nc[idx[i]][2];
        int dsq = (ox - nx) * (ox - nx) + (oz - nz) * (oz - nz);
        if (!sculk_new(c) || dsq <= 144) {                   /* canMoveToPos — только 26.3+ */
            int tr = fc_get(c, nx, ny, nz);
            if (is_sculk_behaviour(s, c, tr) && movement_unobstructed(c, cu->x, cu->y, cu->z, nx, ny, nz)) {
                sx = nx; sy = ny; sz = nz;
                if (has_substrate_access(c, s, tr, nx, ny, nz)) break;
            }
        }
    }
    if (sx == cu->x && sy == cu->y && sz == cu->z) return 0;
    out[0] = sx; out[1] = sy; out[2] = sz; return 1;
}
static void cursor_update(FCtx *c, const PatchCfg *s, Cursor *cu, int ox, int oy, int oz, int spread_veins) {
    if (cu->charge <= 0) return;
    if (cu->update_delay > 0) { cu->update_delay--; return; }
    int cur = fc_get(c, cu->x, cu->y, cu->z), beh = behaviour_of(s, c, cur);
    if (spread_veins && attempt_spread_vein(c, s, beh, cu, cur)) {
        if (beh != 1) { cur = fc_get(c, cu->x, cu->y, cu->z); beh = behaviour_of(s, c, cur); }    /* canChangeBlockStateOnSpread: у SculkBlock false */
    }
    cu->charge = use_charge(c, s, beh, cu, ox, oy, oz, spread_veins);
    if (cu->charge <= 0) { if (beh == 2) vein_on_discharged(c, s, cur, cu->x, cu->y, cu->z); return; }
    int to[3];
    if (valid_move(c, s, cu, ox, oz, to)) {
        if (beh == 2) vein_on_discharged(c, s, cur, cu->x, cu->y, cu->z);
        cu->x = to[0]; cu->y = to[1]; cu->z = to[2];
        /* 26.1/26.2: после перехода курсор гаснет, если ушёл от центра на ≥ 15 по x/z (closerThan(Vec3i(ox, y, oz), 15.0)) */
        if (!sculk_new(c) && (cu->x - ox) * (cu->x - ox) + (cu->z - oz) * (cu->z - oz) >= 225) { cu->charge = 0; return; }
        cur = fc_get(c, to[0], to[1], to[2]);
    } else if (sculk_new(c)) {                              /* 26.3+: в генерации мира курсор без допустимого хода гаснет */
        if (beh == 2) vein_on_discharged(c, s, cur, cu->x, cu->y, cu->z);
        cu->charge = 0; return;
    }
    if (is_sculk_behaviour(s, c, cur)) { cu->has_fac = 1; cu->fac = available_faces(c, s, cur); }
    cu->decay_delay = beh == 0 ? imax_(cu->decay_delay - 1, 0) : 1;
    cu->update_delay = 1;
}
static int patch_can_spread_from(FCtx *c, const PatchCfg *s, int x, int y, int z) {
    int st = fc_get(c, x, y, z);
    if (is_sculk_behaviour(s, c, st)) return 1;
    if (!(fc_is_air(c, st) || (c->g->state_block[st] == s->e.water_blk && BS_FL_SOURCE(c->bs->fluid[st])))) return 0;
    for (int d = 0; d < 6; d++) if (c->bs->flags[fc_get(c, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d])] & BSF_FULL_COLL) return 1;
    return 0;
}
static int patch_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const PatchCfg *s = cfg; FRnd *r = c->rnd; (void)r;
    nc_init();
    if (!patch_can_spread_from(c, s, ox, oy, oz)) return 0;
    Cursor cur[32]; int n = 0;
    int total = s->spread_rounds + s->growth_rounds;
    for (int round = 0; round < total; round++) {
        for (int i = 0; i < s->charge_count; i++) {                       /* addCursors(origin, amountPerCharge) */
            int charge = s->per_charge;
            while (charge > 0) { int cc = imin_(charge, 1000); if (n < 32) cur[n++] = (Cursor){ ox, oy, oz, cc, 0, 1, 0, 0 }; charge -= cc; }
        }
        int spread_veins = round < s->spread_rounds;
        for (int i = 0; i < s->attempts; i++) {
            if (n == 0) continue;
            Cursor keep[32]; int nk = 0;
            for (int k = 0; k < n; k++) {
                cursor_update(c, s, &cur[k], ox, oy, oz, spread_veins);
                if (cur[k].charge > 0) keep[nk++] = cur[k];                 /* генерация мира: слияния нет, остаются все с зарядом > 0 */
            }
            memcpy(cur, keep, sizeof(Cursor) * (size_t)nk); n = nk;
        }
        n = 0;
    }
    if (s->old_post) {
        int below = fc_get(c, ox, oy - 1, oz);
        if (frnd_float(r) <= s->catalyst_chance && (c->bs->flags[below] & BSF_FULL_COLL)) fc_set(c, ox, oy, oz, s->catalyst, 3);
        int extra = s->extra_rare ? intprov_sample(s->extra_rare, r) : 0;
        for (int i = 0; i < extra; i++) {
            int cx = ox + frnd_int_bound(r, 5) - 2, cz = oz + frnd_int_bound(r, 5) - 2;
            if (fc_is_air(c, fc_get(c, cx, oy, cz)) && ((c->bs->sturdy[fc_get(c, cx, oy - 1, cz)] >> DIR_UP) & 1)) fc_set(c, cx, oy, cz, s->shrieker_can, 3);
        }
    }
    return 1;
}

static const FeatType T_MF = { "minecraft:multiface_growth", mf_parse, mf_place };
static const FeatType T_PATCH = { "minecraft:sculk_patch", patch_parse, patch_place };
void feature_register_sculk(void) { feature_register_type(&T_MF); feature_register_type(&T_PATCH); }

/* feature_miscx.c — единая точка регистрации групп W12 (подземные, ледяные, особые, Nether, End) и «особые» фичи: monster_room, underwater_magma,
 * freeze_top_layer (snow_and_freeze), fill_layer, void_start_platform, block_pile, replace_single_block. Остальные группы — feature_nether.c, feature_geode.c,
 * feature_drip.c, feature_ice.c, feature_end.c, feature_misc_*.c. */
#include "feature_misc.h"
#include "surface.h"
#include <stdio.h>
#include <stdlib.h>

static int named_state(FParse *p, const char *name) { int b = bs_block_index(p->bs, name); return b < 0 ? -1 : bs_default(p->bs, b); }
static int has_tag(const FCtx *c, const char *tag, int st) { return gen_block_tag(c->g, tag)[c->g->state_block[st]] != 0; }
static const char *FACE_NAMES[6] = { "down", "up", "north", "south", "west", "east" };
static const int HDIR4[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };

/* ====================================================================== monster_room */
typedef struct RoomCfg { int cave_air, cobble, mossy, chest, spawner; int chest_blk; int spawner_blk; } RoomCfg;
static void *room_parse(FParse *p, const Js *cfg) {
    RoomCfg *r = fp_alloc(p, sizeof *r);
    r->cave_air = p->g->st_cave_air; r->cobble = named_state(p, "minecraft:cobblestone"); r->mossy = named_state(p, "minecraft:mossy_cobblestone");
    r->chest = named_state(p, "minecraft:chest"); r->spawner = named_state(p, "minecraft:spawner");
    r->chest_blk = bs_block_index(p->bs, "minecraft:chest"); r->spawner_blk = bs_block_index(p->bs, "minecraft:spawner");
    return r->chest >= 0 && r->spawner >= 0 ? r : NULL;
}
/* StructurePiece.reorient */
static int room_reorient(FCtx *c, const RoomCfg *r, int x, int y, int z) {
    const BsTab *bs = c->bs; int solid_dir = -1;
    for (int i = 0; i < 4; i++) {
        int d = HDIR4[i]; int st = fc_get(c, x + DIR_DX[d], y, z + DIR_DZ[d]);
        if (fc_is_block(c, st, r->chest_blk)) return r->chest;
        if (bs->flags[st] & BSF_SOLID_RENDER) { if (solid_dir >= 0) { solid_dir = -2; break; } solid_dir = d; }
    }
    static const int OPP[6] = { DIR_UP, DIR_DOWN, DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST };
    if (solid_dir >= 0) return bs_with(bs, r->chest, "facing", FACE_NAMES[OPP[solid_dir]]);
    int lock = DIR_NORTH;                                   /* состояние по умолчанию: facing=north */
    #define SR(d) ((bs->flags[fc_get(c, x + DIR_DX[d], y, z + DIR_DZ[d])] & BSF_SOLID_RENDER) != 0)
    if (SR(lock)) lock = OPP[lock];
    if (SR(lock)) { static const int CW[6] = { 0, 1, DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH }; lock = CW[lock]; }   /* getClockWise: N→E, E→S, S→W, W→N */
    if (SR(lock)) lock = OPP[lock];
    #undef SR
    return bs_with(bs, r->chest, "facing", FACE_NAMES[lock]);
}
static int room_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const RoomCfg *rc = cfg; FRnd *r = c->rnd; const BsTab *bs = c->bs;
    const u8 *cannot = gen_block_tag(c->g, "minecraft:features_cannot_replace");
    #define REPL(st) (!cannot[c->g->state_block[(st)]])
    #define SAFE(x_, y_, z_, state_) do { if (REPL(fc_get(c, x_, y_, z_))) fc_set(c, x_, y_, z_, state_, 2); } while (0)
    int xr = frnd_int_bound(r, 2) + 2, minx = -xr - 1, maxx = xr + 1;
    int zr = frnd_int_bound(r, 2) + 2, minz = -zr - 1, maxz = zr + 1;
    int holes = 0;
    for (int dx = minx; dx <= maxx; dx++) for (int dy = -1; dy <= 4; dy++) for (int dz = minz; dz <= maxz; dz++) {
        int x = ox + dx, y = oy + dy, z = oz + dz;
        int solid = (bs->flags[fc_get(c, x, y, z)] & BSF_SOLID) != 0;
        if (dy == -1 && !solid) return 0;
        if (dy == 4 && !solid) return 0;
        if ((dx == minx || dx == maxx || dz == minz || dz == maxz) && dy == 0 && fc_is_empty_block(c, x, y, z) && fc_is_empty_block(c, x, y + 1, z)) holes++;
    }
    if (holes < 1 || holes > 5) return 0;
    for (int dx = minx; dx <= maxx; dx++) for (int dy = 3; dy >= -1; dy--) for (int dz = minz; dz <= maxz; dz++) {
        int x = ox + dx, y = oy + dy, z = oz + dz;
        int ws = fc_get(c, x, y, z);
        if (dx == minx || dy == -1 || dz == minz || dx == maxx || dy == 4 || dz == maxz) {
            if (y >= c->min_y && !(bs->flags[fc_get(c, x, y - 1, z)] & BSF_SOLID)) fc_set(c, x, y, z, rc->cave_air, 2);
            else if ((bs->flags[ws] & BSF_SOLID) && !fc_is_block(c, ws, rc->chest_blk)) {
                if (dy == -1 && frnd_int_bound(r, 4) != 0) SAFE(x, y, z, rc->mossy); else SAFE(x, y, z, rc->cobble);
            }
        } else if (!fc_is_block(c, ws, rc->chest_blk) && !fc_is_block(c, ws, rc->spawner_blk)) SAFE(x, y, z, rc->cave_air);
    }
    for (int cc = 0; cc < 2; cc++) for (int i = 0; i < 3; i++) {
        int xc = ox + frnd_int_bound(r, xr * 2 + 1) - xr, yc = oy, zc = oz + frnd_int_bound(r, zr * 2 + 1) - zr;
        if (fc_is_empty_block(c, xc, yc, zc)) {
            int walls = 0;
            for (int k = 0; k < 4; k++) { int d = HDIR4[k]; if (bs->flags[fc_get(c, xc + DIR_DX[d], yc, zc + DIR_DZ[d])] & BSF_SOLID) walls++; }
            if (walls == 1) {
                int ch = room_reorient(c, rc, xc, yc, zc);
                if (REPL(fc_get(c, xc, yc, zc))) { fc_set(c, xc, yc, zc, ch, 2); (void)frnd_long(r); }   /* RandomizableContainer.setBlockEntityLootTable: random.nextLong() */
                break;
            }
        }
    }
    if (REPL(fc_get(c, ox, oy, oz))) { fc_set(c, ox, oy, oz, rc->spawner, 2); (void)frnd_int_bound(r, 4); }       /* spawner.setEntityId(MOBS[nextInt(4)]) */
    #undef SAFE
    #undef REPL
    return 1;
}

/* ====================================================================== underwater_magma */
typedef struct MagmaCfg { int range, radius; float prob; int water_blk, magma; } MagmaCfg;
static void *magma_parse(FParse *p, const Js *cfg) {
    MagmaCfg *m = fp_alloc(p, sizeof *m);
    m->range = js_int(js_get(cfg, "floor_search_range"), 5); m->radius = js_int(js_get(cfg, "placement_radius_around_floor"), 1);
    m->prob = js_numf(js_get(cfg, "placement_probability_per_valid_position"), 0.5f);
    m->water_blk = bs_block_index(p->bs, "minecraft:water"); m->magma = named_state(p, "minecraft:magma_block");
    return m;
}
/* isVisibleFromOutside: у блока грань covered не «полностью закрыта» (приближение: полный куб по окклюзии — BSF_SOLID_RENDER) */
static int magma_visible(const FCtx *c, int x, int y, int z) { return !(c->bs->flags[fc_get(c, x, y, z)] & BSF_SOLID_RENDER); }
static int magma_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const MagmaCfg *m = cfg; FRnd *r = c->rnd;
    if (!fc_is_block(c, fc_get(c, ox, oy, oz), m->water_blk)) return 0;
    /* Column.scan: вниз до края (не вода) за range−1 шагов */
    int y = oy;
    for (int i = 1; i < m->range && fc_is_block(c, fc_get(c, ox, y, oz), m->water_blk); i++) y--;
    if (fc_is_block(c, fc_get(c, ox, y, oz), m->water_blk)) return 0;       /* validEdge: не вода */
    int floor_y = y, rad = m->radius, any = 0;
    BcIt it; bc_init(&it, ox - rad, floor_y - rad, oz - rad, ox + rad, floor_y + rad, oz + rad);
    int x, yy, z;
    while (bc_next(&it, &x, &yy, &z)) {
        if (!(frnd_float(r) < m->prob)) continue;
        int st = fc_get(c, x, yy, z);
        if (fc_is_air(c, st) || fc_is_block(c, st, m->water_blk)) continue;
        if (magma_visible(c, x, yy - 1, z)) continue;       /* isVisibleFromOutside(pos.below(), UP) */
        int ok = 1;
        for (int k = 0; k < 4 && ok; k++) { int d = HDIR4[k], o = d == DIR_NORTH ? DIR_SOUTH : d == DIR_SOUTH ? DIR_NORTH : d == DIR_EAST ? DIR_WEST : DIR_EAST; (void)o;
            if (magma_visible(c, x + DIR_DX[d], yy, z + DIR_DZ[d])) ok = 0; }
        if (!ok) continue;
        fc_set(c, x, yy, z, m->magma, 2); any = 1;
    }
    return any;
}

/* ====================================================================== freeze_top_layer (SnowAndFreezeFeature) */
typedef struct FreezeCfg { u8 *precip; int nbiomes; int water_blk, ice, snow, snow_blk; } FreezeCfg;
static void *freeze_parse(FParse *p, const Js *cfg) {
    FreezeCfg *f = fp_alloc(p, sizeof *f);
    f->nbiomes = p->g->nbiomes; f->precip = fp_alloc(p, (size_t)(f->nbiomes ? f->nbiomes : 1));
    for (int b = 0; b < f->nbiomes; b++) {
        const char *nm = p->g->biome_names[b]; const char *ns = "minecraft"; const char *name = nm; char nsb[64];
        const char *col = strchr(nm, ':'); if (col) { size_t l = (size_t)(col - nm); if (l < sizeof nsb) { memcpy(nsb, nm, l); nsb[l] = 0; ns = nsb; name = col + 1; } }
        char *path = xsprintf("%s/data/%s/worldgen/biome/%s.json", p->g->pack, ns, name);
        JsDoc *d = js_parse_file(path, NULL, 0); free(path);
        if (d) { f->precip[b] = (u8)js_bool(js_get(js_root(d), "has_precipitation"), 0); js_free(d); }
    }
    f->water_blk = bs_block_index(p->bs, "minecraft:water"); f->ice = named_state(p, "minecraft:ice"); f->snow = named_state(p, "minecraft:snow"); f->snow_blk = bs_block_index(p->bs, "minecraft:snow");
    return f;
}
/* SnowLayerBlock.canSurvive(state, level, pos) для слоя снега (нужна верхняя грань блока снизу: полный коллайдер) */
static int snow_can_survive(FCtx *c, const FreezeCfg *f, int x, int y, int z) {
    int below = fc_get(c, x, y - 1, z);
    if (has_tag(c, "minecraft:cannot_support_snow_layer", below)) return 0;
    if (has_tag(c, "minecraft:support_override_snow_layer", below)) return 1;
    if (c->bs->flags[below] & BSF_FULL_COLL) return 1;
    const char *layers;
    return fc_is_block(c, below, f->snow_blk) && bs_get_prop(c->bs, below, "layers", &layers) && !strcmp(layers, "8");
}
static int freeze_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const FreezeCfg *f = cfg; const BsTab *bs = c->bs;
    for (int dx = 0; dx < 16; dx++) for (int dz = 0; dz < 16; dz++) {
        int x = ox + dx, z = oz + dz;
        int y = fc_height(c, HM_MOTION_BLOCKING, x, z);
        int by = y - 1;
        int biome = fc_biome(c, x, y, z);
        /* shouldFreeze(level, belowPos, false): температура >= 0.15 — не замерзает; яркость блока < 10 (на этой стадии света нет) */
        if (!(surface_biome_temperature(c->w, biome, x, by, z) >= 0.15F)) {
            if (by >= c->min_y && by < c->min_y + c->height) {
                int st = fc_get(c, x, by, z);
                if (BS_FL_TYPE(bs->fluid[st]) == FL_WATER && fc_is_block(c, st, f->water_blk)) fc_set(c, x, by, z, f->ice, 2);
            }
        }
        /* shouldSnow(level, topPos) */
        if (f->precip[biome] && !(surface_biome_temperature(c->w, biome, x, y, z) >= 0.15F) && y >= c->min_y && y < c->min_y + c->height) {
            int st = fc_get(c, x, y, z);
            if ((fc_is_air(c, st) || fc_is_block(c, st, f->snow_blk)) && snow_can_survive(c, f, x, y, z)) {
                fc_set(c, x, y, z, f->snow, 2);
                int bs_ = fc_get(c, x, by, z);
                if (bs_has_prop(bs, bs_, "snowy")) fc_set(c, x, by, z, bs_with(bs, bs_, "snowy", "true"), 2);
            }
        }
    }
    return 1;
}

/* ====================================================================== fill_layer, void_start_platform, block_pile, replace_single_block */
typedef struct FillCfg { int height, state; } FillCfg;
static void *fill_parse(FParse *p, const Js *cfg) {
    FillCfg *f = fp_alloc(p, sizeof *f); f->height = js_int(js_get(cfg, "height"), 0); f->state = bs_from_json(p->bs, js_get(cfg, "state"));
    return f->state >= 0 ? f : NULL;
}
static int fill_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const FillCfg *f = cfg;
    for (int dx = 0; dx < 16; dx++) for (int dz = 0; dz < 16; dz++) {
        int x = ox + dx, z = oz + dz, y = c->min_y + f->height;
        if (fc_is_air(c, fc_get(c, x, y, z))) fc_set(c, x, y, z, f->state, 2);
    }
    return 1;
}
typedef struct VoidCfg { int cobble, stone; } VoidCfg;
static void *void_parse(FParse *p, const Js *cfg) { VoidCfg *v = fp_alloc(p, sizeof *v); v->cobble = named_state(p, "minecraft:cobblestone"); v->stone = named_state(p, "minecraft:stone"); return v; }
static int void_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const VoidCfg *v = cfg;
    int ccx = ox >> 4, ccz = oz >> 4;
    if (imax_(iabs_(ccx - 0), iabs_(ccz - 0)) > 1) return 1;            /* PLATFORM_ORIGIN_CHUNK = chunk(8, 3, 8) = (0, 0) */
    int py = oy + 3;
    for (int z = ccz * 16; z <= ccz * 16 + 15; z++) for (int x = ccx * 16; x <= ccx * 16 + 15; x++) {
        if (imax_(iabs_(8 - x), iabs_(8 - z)) <= 16) fc_set(c, x, py, z, (x == 8 && z == 8) ? v->cobble : v->stone, 2);
    }
    return 1;
}
typedef struct PileCfg { BSProv *prov; int path_blk; } PileCfg;
static void *pile_parse(FParse *p, const Js *cfg) {
    PileCfg *s = fp_alloc(p, sizeof *s); s->prov = fp_bsprov(p, js_get(cfg, "state_provider")); s->path_blk = bs_block_index(p->bs, "minecraft:dirt_path");
    return s->prov ? s : NULL;
}
static void pile_try(FCtx *c, const PileCfg *s, int x, int y, int z) {
    if (!fc_is_empty_block(c, x, y, z)) return;
    int below = fc_get(c, x, y - 1, z);
    int may = fc_is_block(c, below, s->path_blk) ? frnd_bool(c->rnd) : ((c->bs->sturdy[below] >> DIR_UP) & 1);
    if (may) fc_set(c, x, y, z, bsprov_state(c, s->prov, x, y, z), 260);
}
static int pile_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const PileCfg *s = cfg; FRnd *r = c->rnd;
    if (oy < c->min_y + 5) return 0;
    int xr = 2 + frnd_int_bound(r, 2), zr = 2 + frnd_int_bound(r, 2);
    BcIt it; bc_init(&it, ox - xr, oy, oz - zr, ox + xr, oy + 1, oz + zr);
    int x, y, z;
    while (bc_next(&it, &x, &y, &z)) {
        int xd = ox - x, zd = oz - z;
        float a = frnd_float(r) * 10.0F, b = frnd_float(r) * 6.0F;
        if ((float)(xd * xd + zd * zd) <= a - b) pile_try(c, s, x, y, z);
        else if ((double)frnd_float(r) < 0.031) pile_try(c, s, x, y, z);
    }
    return 1;
}
typedef struct ReplCfg { int n; RuleTest *t[16]; int st[16]; } ReplCfg;
static void *repl_parse(FParse *p, const Js *cfg) {
    ReplCfg *s = fp_alloc(p, sizeof *s); const Js *l = js_get(cfg, "targets");
    if (!js_is_arr(l) || l->n > 16) { fp_fail(p, "replace_single_block: targets"); return NULL; }
    for (int i = 0; i < l->n; i++) { s->t[i] = fp_ruletest(p, js_get(l->items[i], "target")); s->st[i] = bs_from_json(p->bs, js_get(l->items[i], "state")); if (!s->t[i] || s->st[i] < 0) return NULL; }
    s->n = l->n; return s;
}
static int repl_place(FCtx *c, const void *cfg, int x, int y, int z) {
    const ReplCfg *s = cfg;
    for (int i = 0; i < s->n; i++) if (ruletest_test(c, s->t[i], fc_get(c, x, y, z), x, y, z)) { fc_set(c, x, y, z, s->st[i], 2); break; }
    return 1;
}

static const FeatType T_ROOM = { "minecraft:monster_room", room_parse, room_place };
static const FeatType T_MAGMA = { "minecraft:underwater_magma", magma_parse, magma_place };
static const FeatType T_FREEZE = { "minecraft:freeze_top_layer", freeze_parse, freeze_place };
static const FeatType T_FILL = { "minecraft:fill_layer", fill_parse, fill_place };
static const FeatType T_VOID = { "minecraft:void_start_platform", void_parse, void_place };
static const FeatType T_PILE = { "minecraft:block_pile", pile_parse, pile_place };
static const FeatType T_REPL = { "minecraft:replace_single_block", repl_parse, repl_place };

void feature_register_nether(void);
void feature_register_end(void);
void feature_register_ice(void);
void feature_register_geode(void);
void feature_register_drip(void);
void feature_register_sculk(void);
void feature_register_tmpl(void);
void feature_register_old(void);
void feature_register_misc_all(void) {
    feature_register_type(&T_ROOM); feature_register_type(&T_MAGMA); feature_register_type(&T_FREEZE); feature_register_type(&T_FILL); feature_register_type(&T_VOID);
    feature_register_type(&T_PILE); feature_register_type(&T_REPL);
    feature_register_nether();
    feature_register_end();
    feature_register_ice();
    feature_register_geode();
    feature_register_drip();
    feature_register_sculk();
    feature_register_tmpl();
    feature_register_old();
}

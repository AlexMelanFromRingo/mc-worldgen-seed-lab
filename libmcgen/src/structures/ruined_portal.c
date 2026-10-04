/* structures/ruined_portal.c — RuinedPortalStructure/RuinedPortalPiece (minecraft:ruined_portal; часть minecraft:rupo).
 * Старт: выбор setup по весам, «воздушный карман», шаблон ruined_portal/(giant_)portal_*, поворот, отражение FRONT_BACK, опорная точка —
 * центр шаблона; высота — findSuitableY по вертикальному размещению и колонкам заполнения шумом в 4 углах (getBaseColumn).
 * Процессоры (в коде): BlockIgnore (STRUCTURE_BLOCK при кармане, иначе STRUCTURE_AND_AIR), Rule (золото → воздух 0.3, лава → магма/незерак,
 * незерак → магма 0.07 у не холодных), BlockAge(мшистость), Protected(#features_cannot_replace), LavaSubmerged, [BlackstoneReplace].
 * Рисование — только в чанке, где центр bb: chunkBB расширяется до bb портала; затем незерак вокруг, «капли» под порталом, лоза, листва. */
#include "shipwreck_tpl.h"
#include "../surface.h"
#include <stdio.h>
#include <stdlib.h>

extern int gen_base_column(GenCtx *c, int x, int z, int *out, int *y0) __attribute__((weak));   /* ChunkGenerator.getBaseColumn (structure.c) */

enum { VP_ON_LAND_SURFACE, VP_PARTLY_BURIED, VP_ON_OCEAN_FLOOR, VP_IN_MOUNTAIN, VP_UNDERGROUND, VP_IN_NETHER };
static const char *const VPN[6] = { "on_land_surface", "partly_buried", "on_ocean_floor", "in_mountain", "underground", "in_nether" };

typedef struct RPSetup { int placement; float air_pocket, mossiness, weight; int overgrown, vines, can_be_cold, blackstone; } RPSetup;
typedef struct RPCfg {
    int n; RPSetup *s;
    McMutex *lk;
    const Proc *rules[2][2];      /* [на дне океана][холодный] */
    const Proc *protect;
    float *btemp; u8 *bfrozen; int nb;   /* Biome.climateSettings: temperature, temperature_modifier == frozen (по id биома) */
} RPCfg;

extern const PieceVT PIECE_RUINED_PORTAL;
typedef struct RPData {
    TplPiece tp;
    int placement, cold, air_pocket, overgrown, vines, blackstone; float mossiness;
} RPData;

static void rp_free(void *v) { free(v); }
static void rp_move(StPiece *p, int dx, int dy, int dz) { RPData *d = p->data; tplp_move(&d->tp, dx, dy, dz); }
static void rp_dump(const StPiece *p, StrBuf *o) {
    const RPData *d = p->data; tplp_dump(&d->tp, o);
    sb_printf(o, ",\"vp\":\"%s\",\"cold\":%d,\"air_pocket\":%d,\"mossiness\":%.9g", VPN[d->placement], d->cold, d->air_pocket, d->mossiness);
}

/* ---------------------------------------------------------------- рисование */
static int can_replace_nr(StCtx *c, const RPData *d, int st) {      /* canBlockBeReplacedByNetherrackOrMagma */
    const BsTab *bs = c->sw->bs; const McGen *g = bs->g;
    static _Thread_local const BsTab *kb; static _Thread_local int air, obs, lava; static _Thread_local const u8 *fcr;
    if (kb != bs) { air = bs_block_index(bs, "minecraft:air"); obs = bs_block_index(bs, "minecraft:obsidian"); lava = bs_block_index(bs, "minecraft:lava"); fcr = gen_block_tag(g, "minecraft:features_cannot_replace"); kb = bs; }
    int b = g->state_block[st];
    return b != air && b != obs && !(fcr && fcr[b]) && (d->placement == VP_IN_NETHER || b != lava);
}
static void place_nr(StCtx *c, const RPData *d, int x, int y, int z) {  /* placeNetherrackOrMagma */
    if (!can_replace_nr(c, d, fc_get(c->fc, x, y, z))) return;
    if (!d->cold && rs_float(c->rs) < 0.07f) fc_set(c->fc, x, y, z, sp_st(c, "minecraft:magma_block"), 3);
    else fc_set(c->fc, x, y, z, sp_st(c, "minecraft:netherrack"), 3);
}
static void drip_column(StCtx *c, const RPData *d, int x, int y, int z) {   /* addNetherrackDripColumn */
    place_nr(c, d, x, y, z);
    int cap = 8;
    while (cap > 0 && rs_float(c->rs) < 0.5f) { y--; cap--; place_nr(c, d, x, y, z); }
}
static int is_block(StCtx *c, int st, const char *name) { return c->sw->bs->g->state_block[st] == bs_block_index(c->sw->bs, name); }
static void leaves_above(StCtx *c, int x, int y, int z) {             /* maybeAddLeavesAbove */
    if (rs_float(c->rs) < 0.5f && is_block(c, fc_get(c->fc, x, y, z), "minecraft:netherrack") && (c->sw->bs->flags[fc_get(c->fc, x, y + 1, z)] & BSF_AIR))
        fc_set(c->fc, x, y + 1, z, sp_st(c, "minecraft:jungle_leaves[persistent=true]"), 3);
}
static void add_vines(StCtx *c, int x, int y, int z) {                /* maybeAddVines */
    const BsTab *bs = c->sw->bs;
    int st = fc_get(c->fc, x, y, z);
    if ((bs->flags[st] & BSF_AIR) || is_block(c, st, "minecraft:vine")) return;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    int dir = HD[rs_bound(c->rs, 4)];
    int nx = x + DIR_DX[dir], nz = z + DIR_DZ[dir];
    if (!(bs->flags[fc_get(c->fc, nx, y, nz)] & BSF_AIR)) return;
    if (!((bs->flags[st] & BSF_FULL_COLL) || ((bs->sturdy[st] >> dir) & 1))) return;     /* Block.isFaceFull(collisionShape, direction): полный куб (листва: коллизия полная, опорная форма пуста) или прочная грань */
    static const char *DN[6] = { "down", "up", "north", "south", "west", "east" };
    int v = bs_with(bs, sp_st(c, "minecraft:vine"), DN[dir_opp(dir)], "true");
    if (v >= 0) fc_set(c->fc, nx, y, nz, v, 3);
}

static void rp_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    RPData *d = p->data;
    BB bb = tplp_bb(&d->tp);
    if (!bb_inside(&c->chunk, bb_cx(&bb), bb_cy(&bb), bb_cz(&bb))) return;
    c->chunk = bb_union(c->chunk, bb);                               /* chunkBB.encapsulate(boundingBox) */
    tplp_post(c, p, &d->tp, &c->chunk, rx, ry, rz, NULL, NULL);
    bb = p->bb;
    /* spreadNetherrack */
    {
        int follow = d->placement == VP_ON_LAND_SURFACE || d->placement == VP_ON_OCEAN_FLOOR;
        int cx = bb_cx(&bb), cz = bb_cz(&bb);
        static const float PROB[14] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f };
        const int maxd = 14;
        int avg = (bb_xspan(&bb) + bb_zspan(&bb)) / 2;
        int bound = 8 - avg / 2; if (bound < 1) bound = 1;
        int adj = rs_bound(c->rs, bound);
        int hm = d->placement == VP_ON_OCEAN_FLOOR ? HM_OCEAN_FLOOR_WG : HM_WORLD_SURFACE_WG;
        for (int x = cx - maxd; x <= cx + maxd; x++) for (int z = cz - maxd; z <= cz + maxd; z++) {
            int dist = abs(x - cx) + abs(z - cz);
            int ad = dist + adj; if (ad < 0) ad = 0;
            if (ad >= maxd) continue;
            if (!(rs_double(c->rs) < (double)PROB[ad])) continue;
            int sy = sp_height(c, hm, x, z) - 1;                         /* getSurfaceY: level.getHeight(WG) − 1 */
            int y = follow ? sy : (bb.y0 < sy ? bb.y0 : sy);
            if (abs(y - bb.y0) <= 3 && can_replace_nr(c, d, fc_get(c->fc, x, y, z))) {
                place_nr(c, d, x, y, z);
                if (d->overgrown) leaves_above(c, x, y, z);
                drip_column(c, d, x, y - 1, z);
            }
        }
    }
    /* addNetherrackDripColumnsBelowPortal */
    for (int x = bb.x0 + 1; x < bb.x1; x++) for (int z = bb.z0 + 1; z < bb.z1; z++)
        if (is_block(c, fc_get(c->fc, x, bb.y0, z), "minecraft:netherrack")) drip_column(c, d, x, bb.y0 - 1, z);
    if (d->vines || d->overgrown) {
        for (int z = bb.z0; z <= bb.z1; z++) for (int y = bb.y0; y <= bb.y1; y++) for (int x = bb.x0; x <= bb.x1; x++) {   /* betweenClosed: x, y, z */
            if (d->vines) add_vines(c, x, y, z);
            if (d->overgrown) leaves_above(c, x, y, z);
        }
    }
}
const PieceVT PIECE_RUINED_PORTAL = { "minecraft:rupo", rp_post, rp_move, rp_free, rp_dump, NULL, NULL };

/* ---------------------------------------------------------------- процессоры */
static const Proc *proc_json(McWorld *w, const char *json) {
    char err[128];
    JsDoc *doc = js_parse(json, strlen(json), err, sizeof err);
    if (!doc) return NULL;
    const ProcList *l = proclist_from_json(w, js_root(doc));
    js_free(doc);
    return l && l->n > 0 ? l->p[0] : NULL;
}
#define RULE_RND(blk, p, out) "{\"input_predicate\":{\"predicate_type\":\"minecraft:random_block_match\",\"block\":\"minecraft:" blk "\",\"probability\":" p "}," \
    "\"location_predicate\":{\"predicate_type\":\"minecraft:always_true\"},\"output_state\":{\"Name\":\"minecraft:" out "\"}}"
#define RULE(blk, out) "{\"input_predicate\":{\"predicate_type\":\"minecraft:block_match\",\"block\":\"minecraft:" blk "\"}," \
    "\"location_predicate\":{\"predicate_type\":\"minecraft:always_true\"},\"output_state\":{\"Name\":\"minecraft:" out "\"}}"
static void rp_procs(McWorld *w, RPCfg *cfg) {
    mutex_lock(cfg->lk);
    if (!cfg->protect) {
        for (int ocean = 0; ocean < 2; ocean++) for (int cold = 0; cold < 2; cold++) {
            const char *lava = ocean ? RULE("lava", "magma_block") : cold ? RULE("lava", "netherrack") : RULE_RND("lava", "0.2", "magma_block");
            char js[2048];
            snprintf(js, sizeof js, "[{\"processor_type\":\"minecraft:rule\",\"rules\":[" RULE_RND("gold_block", "0.3", "air") ",%s%s]}]", lava,
                     cold ? "" : "," RULE_RND("netherrack", "0.07", "magma_block"));
            cfg->rules[ocean][cold] = proc_json(w, js);
        }
        cfg->protect = proc_json(w, "[{\"processor_type\":\"minecraft:protected_blocks\",\"value\":\"#minecraft:features_cannot_replace\"}]");
    }
    mutex_unlock(cfg->lk);
}

/* ---------------------------------------------------------------- структура */
/* Biome.coldEnoughToSnow(pos, seaLevel) = getTemperature(pos, seaLevel) < 0.15. surface_biome_temperature знает базовую температуру только
 * у «замёрзших» биомов (остальные загружаются, лишь если правила поверхности используют temperature), поэтому базу берём из JSON биома,
 * а поправку на высоту — как разность surface_biome_temperature(y) − surface_biome_temperature(уровень моря). */
static int rp_cold(GenCtx *c, RPCfg *cfg, int b, int x, int y, int z) {
    const McGen *g = c->w->g;
    mutex_lock(cfg->lk);
    if (!cfg->btemp) {
        cfg->nb = g->nbiomes; cfg->btemp = xcalloc((size_t)(cfg->nb ? cfg->nb : 1), sizeof(float)); cfg->bfrozen = xcalloc((size_t)(cfg->nb ? cfg->nb : 1), 1);
        for (int i = 0; i < cfg->nb; i++) {
            const char *nm = g->biome_names[i]; const char *col = strchr(nm, ':');
            char path[1024]; snprintf(path, sizeof path, "%s/data/%.*s/worldgen/biome/%s.json", g->pack, col ? (int)(col - nm) : 9, col ? nm : "minecraft", col ? col + 1 : nm);
            char err[128]; JsDoc *d = js_parse_file(path, err, sizeof err);
            if (!d) continue;
            const Js *r = js_root(d);
            cfg->btemp[i] = js_numf(js_get(r, "temperature"), 0.0f);
            const char *m = js_str(js_get(r, "temperature_modifier"), "none");
            cfg->bfrozen[i] = !strcmp(m, "frozen") || !strcmp(m, "minecraft:frozen");
            js_free(d);
        }
    }
    mutex_unlock(cfg->lk);
    if (b < 0 || b >= cfg->nb) return 0;
    float t = cfg->bfrozen[b] ? surface_biome_temperature(c->w, b, x, y, z)
                              : cfg->btemp[b] + (surface_biome_temperature(c->w, b, x, y, z) - surface_biome_temperature(c->w, b, x, c->w->sea_level, z));
    return !(t >= 0.15f);
}

static void *rp_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) {
    (void)sw;
    const Js *a = js_get(cfg, "setups");
    if (!js_is_arr(a) || a->n == 0) { snprintf(err, errlen, "ruined_portal: нет setups"); return NULL; }
    RPCfg *c = xcalloc(1, sizeof *c);
    c->n = a->n; c->s = xcalloc((size_t)a->n, sizeof *c->s);
    for (int i = 0; i < a->n; i++) {
        const Js *s = a->items[i]; RPSetup *o = &c->s[i];
        const char *pl = js_str(js_get(s, "placement"), "");
        o->placement = -1; for (int k = 0; k < 6; k++) if (!strcmp(pl, VPN[k])) o->placement = k;
        if (o->placement < 0) { snprintf(err, errlen, "ruined_portal: placement %s", pl); free(c->s); free(c); return NULL; }
        o->air_pocket = js_numf(js_get(s, "air_pocket_probability"), 0.0f); o->mossiness = js_numf(js_get(s, "mossiness"), 0.0f);
        o->weight = js_numf(js_get(s, "weight"), 1.0f);
        o->overgrown = js_bool(js_get(s, "overgrown"), 0); o->vines = js_bool(js_get(s, "vines"), 0);
        o->can_be_cold = js_bool(js_get(s, "can_be_cold"), 0); o->blackstone = js_bool(js_get(s, "replace_with_blackstone"), 0);
    }
    c->lk = mutex_new();
    return c;
}

typedef struct RPStub { int setup, air_pocket, rot, mir, px, pz, y; char loc[64]; } RPStub;

static int find_suitable_y(GenCtx *c, int vp, int air_pocket, int surf, int yspan, const BB *bb) {
    int min_y = c->min_y + 15, ny;
    RS *r = &c->rs;
    if (vp == VP_IN_NETHER) {
        if (air_pocket) ny = rs_between(r, 32, 100);
        else if (rs_float(r) < 0.5f) ny = rs_between(r, 27, 29);
        else ny = rs_between(r, 29, 100);
    } else if (vp == VP_IN_MOUNTAIN) { int maxy = surf - yspan; ny = 70 < maxy ? rs_between(r, 70, maxy) : maxy; }
    else if (vp == VP_UNDERGROUND) { int maxy = surf - yspan; ny = min_y < maxy ? rs_between(r, min_y, maxy) : maxy; }
    else if (vp == VP_PARTLY_BURIED) ny = surf - yspan + rs_between(r, 2, 8);
    else ny = surf;
    int cxs[4] = { bb->x0, bb->x1, bb->x0, bb->x1 }, czs[4] = { bb->z0, bb->z0, bb->z1, bb->z1 };
    int hm = vp == VP_ON_OCEAN_FLOOR ? HM_OCEAN_FLOOR_WG : HM_WORLD_SURFACE_WG;
    int *col[4]; int y0[4], n[4];
    for (int i = 0; i < 4; i++) { col[i] = xmalloc((size_t)(c->height > 0 ? c->height : 1) * sizeof(int)); n[i] = gen_base_column(c, cxs[i], czs[i], col[i], &y0[i]); }
    const BsTab *bs = c->bs; int air = c->w->g->st_air;
    int py;
    for (py = ny; py > min_y; py--) {
        int cnt = 0, done = 0;
        for (int i = 0; i < 4; i++) {
            int st = (py >= y0[i] && py - y0[i] < n[i]) ? col[i][py - y0[i]] : air;
            if ((bs->hmcls[st] >> hm) & 1) { if (++cnt == 3) { done = 1; break; } }
        }
        if (done) break;
    }
    for (int i = 0; i < 4; i++) free(col[i]);
    return py;
}

static int rp_find(GenCtx *c, const void *cfgp, Stub *out) {
    const RPCfg *cfg = cfgp;
    if (!gen_base_column) {
        static int warned; if (!warned) { warned = 1; fprintf(stderr, "ruined_portal: в каркасе нет gen_base_column — постройка пропущена\n"); }
        return 0;
    }
    RS *r = &c->rs;
    int si = 0;
    if (cfg->n > 1) {
        float total = 0.0f; for (int i = 0; i < cfg->n; i++) total += cfg->s[i].weight;
        float pick = rs_float(r);
        si = -1;
        for (int i = 0; i < cfg->n; i++) { pick -= cfg->s[i].weight / total; if (pick < 0.0f) { si = i; break; } }
        if (si < 0) return 0;                                         /* IllegalStateException в игре */
    }
    const RPSetup *s = &cfg->s[si];
    int air_pocket = s->air_pocket == 0.0f ? 0 : s->air_pocket == 1.0f ? 1 : rs_float(r) < s->air_pocket;
    RPStub *st = xcalloc(1, sizeof *st);
    if (rs_float(r) < 0.05f) snprintf(st->loc, sizeof st->loc, "minecraft:ruined_portal/giant_portal_%d", rs_bound(r, 3) + 1);
    else snprintf(st->loc, sizeof st->loc, "minecraft:ruined_portal/portal_%d", rs_bound(r, 10) + 1);
    const Template *t = template_get(c->w, st->loc);
    if (!t) { free(st); return 0; }
    st->rot = rs_bound(r, 4);
    st->mir = rs_float(r) < 0.5f ? MIR_NONE : MIR_FRONT_BACK;
    st->px = t->sx / 2; st->pz = t->sz / 2;
    int bx = c->cx * 16, bz = c->cz * 16;
    BB bb = tpl_bounding_box(t, bx, 0, bz, st->rot, st->mir, st->px, st->pz);
    int hm = s->placement == VP_ON_OCEAN_FLOOR ? HM_OCEAN_FLOOR_WG : HM_WORLD_SURFACE_WG;
    int surf = gen_first_free_height(c, bb_cx(&bb), bb_cz(&bb), hm) - 1;
    int y = find_suitable_y(c, s->placement, air_pocket, surf, bb_yspan(&bb), &bb);
    st->setup = si; st->air_pocket = air_pocket; st->y = y;
    out->x = bx; out->y = y; out->z = bz; out->state = st;
    return 1;
}
static void rp_free_stub(Stub *s) { free(s->state); s->state = NULL; }
static int rp_build(GenCtx *c, const void *cfgp, Stub *stub, PieceVec *out) {
    RPCfg *cfg = (RPCfg *)cfgp; RPStub *st = stub->state;
    rp_procs(c->w, cfg);
    const RPSetup *s = &cfg->s[st->setup];
    int cold = 0;
    if (s->can_be_cold) {
        int b = gen_biome_quart(c, stub->x >> 2, stub->y >> 2, stub->z >> 2);
        cold = rp_cold(c, cfg, b, stub->x, stub->y, stub->z);
    }
    RPData *d = xcalloc(1, sizeof *d);
    if (!tplp_init(c->w, &d->tp, st->loc, st->loc, stub->x, stub->y, stub->z, st->rot, st->mir, st->px, st->pz)) { free(d); rp_free_stub(stub); return 0; }
    d->placement = s->placement; d->cold = cold; d->air_pocket = st->air_pocket; d->overgrown = s->overgrown; d->vines = s->vines;
    d->blackstone = s->blackstone; d->mossiness = s->mossiness;
    tplp_add_proc(&d->tp, proc_builtin(c->w, st->air_pocket ? PB_STRUCTURE_BLOCK : PB_STRUCTURE_AND_AIR));
    tplp_add_proc(&d->tp, cfg->rules[s->placement == VP_ON_OCEAN_FLOOR][cold]);
    tplp_add_proc(&d->tp, proc_block_age(c->w, s->mossiness));
    tplp_add_proc(&d->tp, cfg->protect);
    tplp_add_proc(&d->tp, proc_lava_submerged(c->w));
    if (s->blackstone) tplp_add_proc(&d->tp, proc_blackstone_replace(c->w));
    StPiece *p = piece_new(&PIECE_RUINED_PORTAL, tplp_bb(&d->tp), -1, 0, d);
    sp_set_orientation(p, DIR_NORTH);
    pvec_push(out, p);
    rp_free_stub(stub);
    return 1;
}
static void rp_free_cfg(void *p) { RPCfg *c = p; if (c) { mutex_free(c->lk); free(c->s); free(c->btemp); free(c->bfrozen); free(c); } }
const StructType STRUCT_RUINED_PORTAL = { "minecraft:ruined_portal", rp_parse, rp_find, rp_build, NULL, rp_free_cfg, rp_free_stub };

void structures_register_ruined_portal(void) { structure_register_type(&STRUCT_RUINED_PORTAL); piece_register_type(&PIECE_RUINED_PORTAL); }

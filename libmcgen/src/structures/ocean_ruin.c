/* structures/ocean_ruin.c — OceanRuinStructure/OceanRuinPieces (minecraft:ocean_ruin; часть minecraft:orp).
 * Тёплые: один шаблон underwater_ruin/(big_)warm_*; холодные: три наложенных шаблона brick/cracked/mossy с одним индексом
 * (целостность базовая/0.7/0.5). Большие (вероятность large_probability) с вероятностью cluster_probability окружены 4–8 малыми.
 * Процессоры: BlockRotProcessor(целостность), BlockIgnore STRUCTURE_AND_AIR, Capped(Rule песок→подозрительный песок | гравий→подозрительный гравий, 5).
 * Высота: при каждом рисовании части — OCEAN_FLOOR_WG в templatePosition, затем getHeight по блокам мира под прямоугольником шаблона. */
#include "shipwreck_tpl.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_OCEAN_RUIN;

static const char *const WARM[8] = { "warm_1", "warm_2", "warm_3", "warm_4", "warm_5", "warm_6", "warm_7", "warm_8" };
static const char *const BIG_WARM[4] = { "big_warm_4", "big_warm_5", "big_warm_6", "big_warm_7" };
static const char *const BRICK[8] = { "brick_1", "brick_2", "brick_3", "brick_4", "brick_5", "brick_6", "brick_7", "brick_8" };
static const char *const CRACKED[8] = { "cracked_1", "cracked_2", "cracked_3", "cracked_4", "cracked_5", "cracked_6", "cracked_7", "cracked_8" };
static const char *const MOSSY[8] = { "mossy_1", "mossy_2", "mossy_3", "mossy_4", "mossy_5", "mossy_6", "mossy_7", "mossy_8" };
static const char *const BIG_BRICK[4] = { "big_brick_1", "big_brick_2", "big_brick_3", "big_brick_8" };
static const char *const BIG_CRACKED[4] = { "big_cracked_1", "big_cracked_2", "big_cracked_3", "big_cracked_8" };
static const char *const BIG_MOSSY[4] = { "big_mossy_1", "big_mossy_2", "big_mossy_3", "big_mossy_8" };

typedef struct ORCfg {
    int warm; float large_p, cluster_p;
    McMutex *lk; int ready;
    const Proc *rot09, *rot08, *rot07, *rot05, *archy;    /* BlockRotProcessor(0.9/0.8/0.7/0.5), Capped(Rule) */
} ORCfg;

typedef struct ORData {
    TplPiece tp;
    float integrity; int warm, large;
    int y0;
} ORData;

static void or_reset(StPiece *p) { ORData *d = p->data; d->tp.y = d->y0; }
static void or_free(void *v) { free(v); }
static void or_move(StPiece *p, int dx, int dy, int dz) { ORData *d = p->data; tplp_move(&d->tp, dx, dy, dz); }
static void or_dump(const StPiece *p, StrBuf *o) { const ORData *d = p->data; tplp_dump(&d->tp, o); sb_printf(o, ",\"integrity\":%.9g,\"large\":%d,\"warm\":%d", d->integrity, d->large, d->warm); }

static void or_marker(StCtx *c, StPiece *p, void *ud, const char *m, int x, int y, int z, const BB *bounds) {
    (void)p; (void)ud; (void)bounds;
    FCtx *fc = c->fc; const BsTab *bs = c->sw->bs;
    if (!strcmp(m, "chest")) {
        int fl = bs->fluid[fc_get(fc, x, y, z)];
        int water = BS_FL_TYPE(fl) == FL_WATER || BS_FL_TYPE(fl) == FL_FLOWING_WATER;     /* is(FluidTags.WATER) */
        int st = sp_st(c, water ? "minecraft:chest[waterlogged=true]" : "minecraft:chest[waterlogged=false]");
        if (fc_set(fc, x, y, z, st, 2) && !fc_outside(fc, y)) (void)rs_long(c->rs);       /* setLootTable(…, random.nextLong()) */
    } else if (!strcmp(m, "drowned")) {
        /* утопленник (сущность; Mob.finalizeSpawn берёт числа из level.getRandom() — на блоки построек не влияет), затем блок */
        fc_set(fc, x, y, z, y > c->w->sea_level ? c->w->g->st_air : c->fc->st_water, 2);
    }
}

/* OceanRuinPiece.getHeight(pos, level, corner) */
static int ruin_height(StCtx *c, int px, int py, int pz, int cx, int cz) {
    FCtx *fc = c->fc; const BsTab *bs = c->sw->bs; const McGen *g = bs->g;
    static _Thread_local const BsTab *kb; static _Thread_local const u8 *ice;
    if (kb != bs) { ice = gen_block_tag(g, "minecraft:ice"); kb = bs; }
    int new_y = py, min_y = 512, top_y = py - 1, area = 0;
    int x0 = px < cx ? px : cx, x1 = px < cx ? cx : px, z0 = pz < cz ? pz : cz, z1 = pz < cz ? cz : pz;
    for (int z = z0; z <= z1; z++) for (int x = x0; x <= x1; x++) {
        int fy = py - 1;
        for (;;) {
            int st = fc_get(fc, x, fy, z);
            int fl = bs->fluid[st];
            int water = BS_FL_TYPE(fl) == FL_WATER || BS_FL_TYPE(fl) == FL_FLOWING_WATER;
            if (!((bs->flags[st] & BSF_AIR) || water || (ice && ice[g->state_block[st]])) || !(fy > c->w->min_y + 1)) break;
            fy--;
        }
        if (fy < min_y) min_y = fy;
        if (fy < top_y - 2) area++;
    }
    int width = abs(px - cx);
    if (top_y - min_y > 2 && area > width - 2) new_y = min_y + 1;
    return new_y;
}

static void or_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    ORData *d = p->data; const Template *t = d->tp.t;
    int h = sp_height(c, HM_OCEAN_FLOOR_WG, d->tp.x, d->tp.z);
    d->tp.y = h;
    int cx, cz; tpl_transform(t->sx - 1, t->sz - 1, MIR_NONE, d->tp.rot, 0, 0, &cx, &cz);
    cx += d->tp.x; cz += d->tp.z;
    d->tp.y = ruin_height(c, d->tp.x, d->tp.y, d->tp.z, cx, cz);
    tplp_post(c, p, &d->tp, &c->chunk, rx, ry, rz, or_marker, NULL);
}

const PieceVT PIECE_OCEAN_RUIN = { "minecraft:orp", or_post, or_move, or_free, or_dump, or_reset, NULL };

/* ---------------------------------------------------------------- структура */
static const Proc *proc_json(McWorld *w, const char *json) {
    char err[128];
    JsDoc *doc = js_parse(json, strlen(json), err, sizeof err);
    if (!doc) return NULL;
    const ProcList *l = proclist_from_json(w, js_root(doc));
    js_free(doc);
    return l && l->n > 0 ? l->p[0] : NULL;
}
static void or_procs(McWorld *w, ORCfg *cfg) {
    mutex_lock(cfg->lk);
    if (!cfg->ready) {
        cfg->rot09 = proc_json(w, "[{\"processor_type\":\"minecraft:block_rot\",\"integrity\":0.9}]");
        cfg->rot08 = proc_json(w, "[{\"processor_type\":\"minecraft:block_rot\",\"integrity\":0.8}]");
        cfg->rot07 = proc_json(w, "[{\"processor_type\":\"minecraft:block_rot\",\"integrity\":0.7}]");
        cfg->rot05 = proc_json(w, "[{\"processor_type\":\"minecraft:block_rot\",\"integrity\":0.5}]");
        char js[1024];
        snprintf(js, sizeof js, "[{\"processor_type\":\"minecraft:capped\",\"limit\":5,\"delegate\":{\"processor_type\":\"minecraft:rule\",\"rules\":[{"
                 "\"input_predicate\":{\"predicate_type\":\"minecraft:block_match\",\"block\":\"minecraft:%s\"},"
                 "\"location_predicate\":{\"predicate_type\":\"minecraft:always_true\"},\"position_predicate\":{\"predicate_type\":\"minecraft:always_true\"},"
                 "\"output_state\":{\"Name\":\"minecraft:suspicious_%s\"}}]}}]", cfg->warm ? "sand" : "gravel", cfg->warm ? "sand" : "gravel");
        cfg->archy = proc_json(w, js);
        cfg->ready = 1;
    }
    mutex_unlock(cfg->lk);
}

static void add_piece(GenCtx *c, const ORCfg *cfg, PieceVec *out, const char *nm, int x, int z, int rot, float integrity, int large) {
    char loc[128]; snprintf(loc, sizeof loc, "minecraft:underwater_ruin/%s", nm);
    ORData *d = xcalloc(1, sizeof *d);
    tplp_init(c->w, &d->tp, loc, loc, x, 90, z, rot, MIR_NONE, 0, 0);
    const Proc *rp = integrity == 0.9f ? cfg->rot09 : integrity == 0.8f ? cfg->rot08 : integrity == 0.7f ? cfg->rot07 : cfg->rot05;
    tplp_add_proc(&d->tp, rp);
    tplp_add_proc(&d->tp, proc_builtin(c->w, PB_STRUCTURE_AND_AIR));
    tplp_add_proc(&d->tp, cfg->archy);
    d->integrity = integrity; d->warm = cfg->warm; d->large = large; d->y0 = 90;
    StPiece *p = piece_new(&PIECE_OCEAN_RUIN, tplp_bb(&d->tp), -1, 0, d);
    sp_set_orientation(p, DIR_NORTH);
    pvec_push(out, p);
}
/* OceanRuinPieces.addPiece */
static void or_add(GenCtx *c, const ORCfg *cfg, PieceVec *out, int x, int z, int rot, int large, float integrity) {
    if (cfg->warm) {
        const char *nm = large ? BIG_WARM[rs_bound(&c->rs, 4)] : WARM[rs_bound(&c->rs, 8)];
        add_piece(c, cfg, out, nm, x, z, rot, integrity, large);
    } else {
        int idx = rs_bound(&c->rs, large ? 4 : 8);
        add_piece(c, cfg, out, large ? BIG_BRICK[idx] : BRICK[idx], x, z, rot, integrity, large);
        add_piece(c, cfg, out, large ? BIG_CRACKED[idx] : CRACKED[idx], x, z, rot, 0.7f, large);
        add_piece(c, cfg, out, large ? BIG_MOSSY[idx] : MOSSY[idx], x, z, rot, 0.5f, large);
    }
}

static void *or_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) {
    (void)sw;
    ORCfg *c = xcalloc(1, sizeof *c);
    const char *t = js_str(js_get(cfg, "biome_temp"), "warm");
    if (strcmp(t, "warm") && strcmp(t, "cold")) { snprintf(err, errlen, "ocean_ruin: biome_temp %s", t); free(c); return NULL; }
    c->warm = !strcmp(t, "warm");
    c->large_p = js_numf(js_get(cfg, "large_probability"), 0.0f);
    c->cluster_p = js_numf(js_get(cfg, "cluster_probability"), 0.0f);
    c->lk = mutex_new();
    return c;
}
static int or_find(GenCtx *c, const void *cfgp, Stub *out) {
    (void)cfgp;
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;                  /* onTopOfChunkCenter(OCEAN_FLOOR_WG) */
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, HM_OCEAN_FLOOR_WG); out->state = NULL;
    return 1;
}
static int or_build(GenCtx *c, const void *cfgp, Stub *stub, PieceVec *out) {
    ORCfg *cfg = (ORCfg *)cfgp; (void)stub;
    or_procs(c->w, cfg);
    int x = c->cx * 16, z = c->cz * 16;
    int rot = rs_bound(&c->rs, 4);
    int large = rs_float(&c->rs) <= cfg->large_p;
    float base = large ? 0.9f : 0.8f;
    or_add(c, cfg, out, x, z, rot, large, base);
    if (large && rs_float(&c->rs) <= cfg->cluster_p) {
        /* addClusterRuins */
        int ccx, ccz; tpl_transform(15, 15, MIR_NONE, rot, 0, 0, &ccx, &ccz); ccx += x; ccz += z;
        BB parent = bb_from_corners(x, 90, z, ccx, 90, ccz);
        int ox = x < ccx ? x : ccx, oz = z < ccz ? z : ccz;
        int pos[8][2]; int np = 0;
#define ADDP(dx, dz) do { int _x = (dx); int _z = (dz); pos[np][0] = ox + _x; pos[np][1] = oz + _z; np++; } while (0)
        ADDP(-16 + rs_mth_int(&c->rs, 1, 8), 16 + rs_mth_int(&c->rs, 1, 7));
        ADDP(-16 + rs_mth_int(&c->rs, 1, 8), rs_mth_int(&c->rs, 1, 7));
        ADDP(-16 + rs_mth_int(&c->rs, 1, 8), -16 + rs_mth_int(&c->rs, 4, 8));
        ADDP(rs_mth_int(&c->rs, 1, 7), 16 + rs_mth_int(&c->rs, 1, 7));
        ADDP(rs_mth_int(&c->rs, 1, 7), -16 + rs_mth_int(&c->rs, 4, 6));
        ADDP(16 + rs_mth_int(&c->rs, 1, 7), 16 + rs_mth_int(&c->rs, 3, 8));
        ADDP(16 + rs_mth_int(&c->rs, 1, 7), rs_mth_int(&c->rs, 1, 7));
        ADDP(16 + rs_mth_int(&c->rs, 1, 7), -16 + rs_mth_int(&c->rs, 4, 8));
#undef ADDP
        int ruins = rs_mth_int(&c->rs, 4, 8);
        for (int i = 0; i < ruins; i++) {
            if (np == 0) continue;
            int idx = rs_bound(&c->rs, np);
            int px = pos[idx][0], pz = pos[idx][1];
            for (int k = idx; k < np - 1; k++) { pos[k][0] = pos[k + 1][0]; pos[k][1] = pos[k + 1][1]; }
            np--;
            int nrot = rs_bound(&c->rs, 4);
            int ncx, ncz; tpl_transform(5, 6, MIR_NONE, nrot, 0, 0, &ncx, &ncz); ncx += px; ncz += pz;
            BB nb = bb_from_corners(px, 90, pz, ncx, 90, ncz);
            if (!bb_intersects(&nb, &parent)) or_add(c, cfg, out, px, pz, nrot, 0, 0.8f);
        }
    }
    return 1;
}
static void or_free_cfg(void *p) { ORCfg *c = p; if (c) { mutex_free(c->lk); free(c); } }
const StructType STRUCT_OCEAN_RUIN = { "minecraft:ocean_ruin", or_parse, or_find, or_build, NULL, or_free_cfg, NULL };

void structures_register_ocean_ruin(void) { structure_register_type(&STRUCT_OCEAN_RUIN); piece_register_type(&PIECE_OCEAN_RUIN); }

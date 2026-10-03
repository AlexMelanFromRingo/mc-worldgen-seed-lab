/* structures/shipwreck.c — ShipwreckStructure/ShipwreckPieces (minecraft:shipwreck; часть minecraft:shipwreck).
 * Один шаблон shipwreck/* у угла чанка (y = 90), поворот вокруг опорной точки (4, 0, 15), процессор BlockIgnore STRUCTURE_AND_AIR.
 * Высота: если шаблон крупнее 32 (по x или y) — сразу при создании старта (по углам), иначе при ПЕРВОМ рисовании части
 * (среднее/минимум карты высот WG по прямоугольнику шаблона без поворота; у «выброшенных на берег» ещё −size.y/2 − random.nextInt(3)). */
#include "shipwreck_tpl.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_SHIPWRECK;

static const char *const BEACHED[] = { "with_mast", "sideways_full", "sideways_fronthalf", "sideways_backhalf", "rightsideup_full", "rightsideup_fronthalf",
    "rightsideup_backhalf", "with_mast_degraded", "rightsideup_full_degraded", "rightsideup_fronthalf_degraded", "rightsideup_backhalf_degraded" };
static const char *const OCEAN[] = { "with_mast", "upsidedown_full", "upsidedown_fronthalf", "upsidedown_backhalf", "sideways_full", "sideways_fronthalf",
    "sideways_backhalf", "rightsideup_full", "rightsideup_fronthalf", "rightsideup_backhalf", "with_mast_degraded", "upsidedown_full_degraded",
    "upsidedown_fronthalf_degraded", "upsidedown_backhalf_degraded", "sideways_full_degraded", "sideways_fronthalf_degraded", "sideways_backhalf_degraded",
    "rightsideup_full_degraded", "rightsideup_fronthalf_degraded", "rightsideup_backhalf_degraded" };

typedef struct SWData {
    TplPiece tp;
    int beached;
    int adjusted, adjusted0;      /* heightAdjusted (текущее / после создания старта) */
    int y0;                       /* templatePosition.y после создания старта */
} SWData;

static int too_big(const SWData *d) { return d->tp.t && (d->tp.t->sx > 32 || d->tp.t->sy > 32); }

static void sw_reset(StPiece *p) { SWData *d = p->data; d->adjusted = d->adjusted0; d->tp.y = d->y0; }
static void sw_free(void *v) { free(v); }
static void sw_move(StPiece *p, int dx, int dy, int dz) { SWData *d = p->data; tplp_move(&d->tp, dx, dy, dz); }
static void sw_dump(const StPiece *p, StrBuf *o) { const SWData *d = p->data; tplp_dump(&d->tp, o); sb_printf(o, ",\"beached\":%d,\"adj\":%d", d->beached, d->adjusted); }

static void sw_marker(StCtx *c, StPiece *p, void *ud, const char *m, int x, int y, int z, const BB *bounds) {
    (void)p; (void)ud; (void)bounds;
    if (!strcmp(m, "map_chest") || !strcmp(m, "treasure_chest") || !strcmp(m, "supply_chest")) tplp_loot(c, x, y - 1, z);
}

static void sw_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    SWData *d = p->data;
    if (!d->adjusted && !too_big(d)) {
        const Template *t = d->tp.t;
        int hm = d->beached ? HM_WORLD_SURFACE_WG : HM_OCEAN_FLOOR_WG;
        int min_y = c->w->min_y + c->w->height;          /* level.getMaxY() + 1 */
        int mean = 0, base = t->sx * t->sz;
        if (base == 0) mean = sp_height(c, hm, d->tp.x, d->tp.z);
        else {
            for (int z = d->tp.z; z <= d->tp.z + t->sz - 1; z++) for (int x = d->tp.x; x <= d->tp.x + t->sx - 1; x++) {   /* betweenClosed: x быстрее */
                int h = sp_height(c, hm, x, z);
                mean += h; if (h < min_y) min_y = h;
            }
            mean /= base;
        }
        int ny = d->beached ? min_y - t->sy / 2 - rs_bound(c->rs, 3) : mean;
        d->adjusted = 1; d->tp.y = ny;
    }
    tplp_post(c, p, &d->tp, &c->chunk, rx, ry, rz, sw_marker, NULL);
}

const PieceVT PIECE_SHIPWRECK = { "minecraft:shipwreck", sw_post, sw_move, sw_free, sw_dump, sw_reset, NULL };

/* ---------------------------------------------------------------- структура */
typedef struct SWCfg { int beached; } SWCfg;
static void *sw_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) {
    (void)sw; (void)err; (void)errlen;
    SWCfg *c = xcalloc(1, sizeof *c); c->beached = js_bool(js_get(cfg, "is_beached"), 0);
    return c;
}
static int sw_find(GenCtx *c, const void *cfgp, Stub *out) {
    const SWCfg *cfg = cfgp;
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;                  /* onTopOfChunkCenter */
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, cfg->beached ? HM_WORLD_SURFACE_WG : HM_OCEAN_FLOOR_WG); out->state = NULL;
    return 1;
}
static int sw_build(GenCtx *c, const void *cfgp, Stub *stub, PieceVec *out) {
    const SWCfg *cfg = cfgp; (void)stub;
    int rot = rs_bound(&c->rs, 4);                                   /* Rotation.getRandom */
    const char *nm = cfg->beached ? BEACHED[rs_bound(&c->rs, 11)] : OCEAN[rs_bound(&c->rs, 20)];
    char loc[128]; snprintf(loc, sizeof loc, "minecraft:shipwreck/%s", nm);
    SWData *d = xcalloc(1, sizeof *d);
    if (!tplp_init(c->w, &d->tp, loc, loc, c->cx * 16, 90, c->cz * 16, rot, MIR_NONE, 4, 15)) { free(d); return 0; }
    tplp_add_proc(&d->tp, proc_builtin(c->w, PB_STRUCTURE_AND_AIR));
    d->beached = cfg->beached;
    StPiece *p = piece_new(&PIECE_SHIPWRECK, tplp_bb(&d->tp), -1, 0, d);
    sp_set_orientation(p, DIR_NORTH);
    pvec_push(out, p);
    if (too_big(d)) {
        BB bb = p->bb; int h;
        if (d->beached) {
            int miny = gen_lowest_y(c, bb.x0, bb.z0, bb_xspan(&bb), bb_zspan(&bb));
            h = miny - d->tp.t->sy / 2 - rs_bound(&c->rs, 3);
        } else h = gen_mean_first_occupied_height(c, bb.x0, bb_xspan(&bb), bb.z0, bb_zspan(&bb));
        d->adjusted = 1; d->tp.y = h;                                 /* adjustPositionHeight: bounding box части не меняется до рисования */
    }
    d->adjusted0 = d->adjusted; d->y0 = d->tp.y;
    return 1;
}
static void sw_free_cfg(void *p) { free(p); }
const StructType STRUCT_SHIPWRECK = { "minecraft:shipwreck", sw_parse, sw_find, sw_build, NULL, sw_free_cfg, NULL };

void structures_register_shipwreck(void) { structure_register_type(&STRUCT_SHIPWRECK); piece_register_type(&PIECE_SHIPWRECK); }

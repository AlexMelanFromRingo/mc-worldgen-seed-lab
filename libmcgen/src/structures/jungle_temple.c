/* structures/jungle_temple.c — JungleTempleStructure/JungleTemplePiece (Java-кодированная постройка).
 * Часть — ScatteredFeaturePiece 12×10×15: высота по среднему рельефу чанка; стены из мшистого/обычного булыжника (BlockSelector на ГСЧ),
 * подвал с ловушками (раздатчики, растяжки, редстоун), два сундука. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_JUNGLE_TEMPLE;
typedef struct JTData { int hpos, main_chest, hidden_chest, trap1, trap2; } JTData;

static void jt_reset(StPiece *p) { JTData *d = p->data; d->hpos = -1; d->main_chest = d->hidden_chest = d->trap1 = d->trap2 = 0; }
static void jt_free(void *v) { free(v); }

typedef struct MS { int cobble, mossy; } MS;
static int ms_next(SPSel *s, RS *r, int x, int y, int z, int edge) {       /* MossStoneSelector */
    MS *m = s->ud; (void)x; (void)y; (void)z; (void)edge;
    return rs_float(r) < 0.4f ? m->cobble : m->mossy;
}

static void jt_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    JTData *d = p->data;
    (void)rx; (void)ry; (void)rz;
    const int W = 12, D = 15;
    if (!sp_update_avg_ground(c, p, &d->hpos, 0)) return;
    MS ms = { sp_st(c, "minecraft:cobblestone"), sp_st(c, "minecraft:mossy_cobblestone") };
    SPSel sel = { &ms, ms_next };
    const int AIR = sp_air(c), MOSSY = ms.mossy;
#define SEL(x0, y0, z0, x1, y1, z1) sp_box_sel(c, p, x0, y0, z0, x1, y1, z1, 0, &sel)
#define AIRB(x0, y0, z0, x1, y1, z1) sp_air_box(c, p, x0, y0, z0, x1, y1, z1)
#define PL(st, x, y, z) sp_place(c, p, st, x, y, z)
    SEL(0, -4, 0, W - 1, 0, D - 1);
    SEL(2, 1, 2, 9, 2, 2);
    SEL(2, 1, 12, 9, 2, 12);
    SEL(2, 1, 3, 2, 2, 11);
    SEL(9, 1, 3, 9, 2, 11);
    SEL(1, 3, 1, 10, 6, 1);
    SEL(1, 3, 13, 10, 6, 13);
    SEL(1, 3, 2, 1, 6, 12);
    SEL(10, 3, 2, 10, 6, 12);
    SEL(2, 3, 2, 9, 3, 12);
    SEL(2, 6, 2, 9, 6, 12);
    SEL(3, 7, 3, 8, 7, 11);
    SEL(4, 8, 4, 7, 8, 10);
    AIRB(3, 1, 3, 8, 2, 11);
    AIRB(4, 3, 6, 7, 3, 9);
    AIRB(2, 4, 2, 9, 5, 12);
    AIRB(4, 6, 5, 7, 6, 9);
    AIRB(5, 7, 6, 6, 7, 8);
    AIRB(5, 1, 2, 6, 2, 2);
    AIRB(5, 2, 12, 6, 2, 12);
    AIRB(5, 5, 1, 6, 5, 1);
    AIRB(5, 5, 13, 6, 5, 13);
    PL(AIR, 1, 5, 5); PL(AIR, 10, 5, 5); PL(AIR, 1, 5, 9); PL(AIR, 10, 5, 9);
    for (int z = 0; z <= 14; z += 14) {
        SEL(2, 4, z, 2, 5, z);
        SEL(4, 4, z, 4, 5, z);
        SEL(7, 4, z, 7, 5, z);
        SEL(9, 4, z, 9, 5, z);
    }
    SEL(5, 6, 0, 6, 6, 0);
    for (int x = 0; x <= 11; x += 11) {
        for (int z = 2; z <= 12; z += 2) SEL(x, 4, z, x, 5, z);
        SEL(x, 6, 5, x, 6, 5);
        SEL(x, 6, 9, x, 6, 9);
    }
    SEL(2, 7, 2, 2, 9, 2);
    SEL(9, 7, 2, 9, 9, 2);
    SEL(2, 7, 12, 2, 9, 12);
    SEL(9, 7, 12, 9, 9, 12);
    SEL(4, 9, 4, 4, 9, 4);
    SEL(7, 9, 4, 7, 9, 4);
    SEL(4, 9, 10, 4, 9, 10);
    SEL(7, 9, 10, 7, 9, 10);
    SEL(5, 9, 7, 6, 9, 7);
    int cs = sp_st(c, "minecraft:cobblestone_stairs");
    int eastS = sp_with(c, cs, "facing", "east"), westS = sp_with(c, cs, "facing", "west"), southS = sp_with(c, cs, "facing", "south"), northS = sp_with(c, cs, "facing", "north");
    PL(northS, 5, 9, 6); PL(northS, 6, 9, 6); PL(southS, 5, 9, 8); PL(southS, 6, 9, 8);
    PL(northS, 4, 0, 0); PL(northS, 5, 0, 0); PL(northS, 6, 0, 0); PL(northS, 7, 0, 0);
    PL(northS, 4, 1, 8); PL(northS, 4, 2, 9); PL(northS, 4, 3, 10);
    PL(northS, 7, 1, 8); PL(northS, 7, 2, 9); PL(northS, 7, 3, 10);
    SEL(4, 1, 9, 4, 1, 9);
    SEL(7, 1, 9, 7, 1, 9);
    SEL(4, 1, 10, 7, 2, 10);
    SEL(5, 4, 5, 6, 4, 5);
    PL(eastS, 4, 4, 5);
    PL(westS, 7, 4, 5);
    for (int i = 0; i < 4; i++) {
        PL(southS, 5, 0 - i, 6 + i);
        PL(southS, 6, 0 - i, 6 + i);
        AIRB(5, 0 - i, 7 + i, 6, 0 - i, 9 + i);
    }
    AIRB(1, -3, 12, 10, -1, 13);
    AIRB(1, -3, 1, 3, -1, 13);
    AIRB(1, -3, 1, 9, -1, 5);
    for (int z = 1; z <= 13; z += 2) SEL(1, -3, z, 1, -2, z);
    for (int z = 2; z <= 12; z += 2) SEL(1, -1, z, 3, -1, z);
    SEL(2, -2, 1, 5, -2, 1);
    SEL(7, -2, 1, 9, -2, 1);
    SEL(6, -3, 1, 6, -3, 1);
    SEL(6, -1, 1, 6, -1, 1);
    int hook = sp_st(c, "minecraft:tripwire_hook");
    int hookE = sp_with(c, sp_with(c, hook, "facing", "east"), "attached", "true");
    int hookW = sp_with(c, sp_with(c, hook, "facing", "west"), "attached", "true");
    int hookN = sp_with(c, sp_with(c, hook, "facing", "north"), "attached", "true");
    int hookS = sp_with(c, sp_with(c, hook, "facing", "south"), "attached", "true");
    int tw = sp_st(c, "minecraft:tripwire");
    int twEW = sp_with(c, sp_with(c, sp_with(c, tw, "east", "true"), "west", "true"), "attached", "true");
    int twNS = sp_with(c, sp_with(c, sp_with(c, tw, "north", "true"), "south", "true"), "attached", "true");
    PL(hookE, 1, -3, 8);
    PL(hookW, 4, -3, 8);
    PL(twEW, 2, -3, 8);
    PL(twEW, 3, -3, 8);
    int rw = sp_st(c, "minecraft:redstone_wire");
    int rwNS = sp_with(c, sp_with(c, rw, "north", "side"), "south", "side");
    int rwEW = sp_with(c, sp_with(c, rw, "east", "side"), "west", "side");
    int rwNW = sp_with(c, sp_with(c, rw, "north", "side"), "west", "side");
    int rwWS = sp_with(c, sp_with(c, rw, "west", "side"), "south", "side");
    int rwNSu = sp_with(c, sp_with(c, rw, "north", "side"), "south", "up");
    int rwAll = sp_with(c, sp_with(c, rwNS, "east", "side"), "west", "side");
    PL(rwNS, 5, -3, 7); PL(rwNS, 5, -3, 6); PL(rwNS, 5, -3, 5); PL(rwNS, 5, -3, 4); PL(rwNS, 5, -3, 3); PL(rwNS, 5, -3, 2);
    PL(rwNW, 5, -3, 1);
    PL(rwEW, 4, -3, 1);
    PL(MOSSY, 3, -3, 1);
    if (!d->trap1) d->trap1 = sp_create_dispenser(c, p, 3, -2, 1, DIR_NORTH);
    PL(sp_with(c, sp_st(c, "minecraft:vine"), "south", "true"), 3, -2, 2);
    PL(hookN, 7, -3, 1);
    PL(hookS, 7, -3, 5);
    PL(twNS, 7, -3, 2); PL(twNS, 7, -3, 3); PL(twNS, 7, -3, 4);
    PL(rwEW, 8, -3, 6);
    PL(rwWS, 9, -3, 6);
    PL(rwNSu, 9, -3, 5);
    PL(MOSSY, 9, -3, 4);
    PL(rwNS, 9, -2, 4);
    if (!d->trap2) d->trap2 = sp_create_dispenser(c, p, 9, -2, 3, DIR_WEST);
    int vineE = sp_with(c, sp_st(c, "minecraft:vine"), "east", "true");
    PL(vineE, 8, -1, 3);
    PL(vineE, 8, -2, 3);
    if (!d->main_chest) d->main_chest = sp_create_chest(c, p, 8, -3, 3, -1);
    PL(MOSSY, 9, -3, 2);
    PL(MOSSY, 8, -3, 1);
    PL(MOSSY, 4, -3, 5);
    PL(MOSSY, 5, -2, 5);
    PL(MOSSY, 5, -1, 5);
    PL(MOSSY, 6, -3, 5);
    PL(MOSSY, 7, -2, 5);
    PL(MOSSY, 7, -1, 5);
    PL(MOSSY, 8, -3, 5);
    SEL(9, -1, 1, 9, -1, 5);
    AIRB(8, -3, 8, 10, -1, 10);
    int chis = sp_st(c, "minecraft:chiseled_stone_bricks");
    PL(chis, 8, -2, 11); PL(chis, 9, -2, 11); PL(chis, 10, -2, 11);
    int lever = sp_with(c, sp_with(c, sp_st(c, "minecraft:lever"), "facing", "north"), "face", "wall");
    PL(lever, 8, -2, 12); PL(lever, 9, -2, 12); PL(lever, 10, -2, 12);
    SEL(8, -3, 8, 8, -3, 10);
    SEL(10, -3, 8, 10, -3, 10);
    PL(MOSSY, 10, -2, 9);
    PL(rwNS, 8, -2, 9);
    PL(rwNS, 8, -2, 10);
    PL(rwAll, 10, -1, 9);
    int sticky = sp_st(c, "minecraft:sticky_piston");
    PL(sp_with(c, sticky, "facing", "up"), 9, -2, 8);
    PL(sp_with(c, sticky, "facing", "west"), 10, -2, 8);
    PL(sp_with(c, sticky, "facing", "west"), 10, -1, 8);
    PL(sp_with(c, sp_st(c, "minecraft:repeater"), "facing", "north"), 10, -2, 10);
    if (!d->hidden_chest) d->hidden_chest = sp_create_chest(c, p, 9, -3, 10, -1);
#undef SEL
#undef AIRB
#undef PL
}

const PieceVT PIECE_JUNGLE_TEMPLE = { "minecraft:tejp", jt_post, NULL, jt_free, NULL, jt_reset, NULL };

/* ---------------------------------------------------------------- структура: SinglePieceStructure(JungleTemplePiece::new, 12, 15) */
static void *jt_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int jt_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    if (!gen_could_exist_on_chunk_center(c)) return 0;
    if (gen_lowest_y(c, c->cx * 16, c->cz * 16, 12, 15) < c->w->sea_level) return 0;
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, HM_WORLD_SURFACE_WG); out->state = NULL;
    return 1;
}
static int jt_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    int dir = HD[rs_bound(&c->rs, 4)];
    JTData *d = xcalloc(1, sizeof *d); d->hpos = -1;
    StPiece *p = piece_new(&PIECE_JUNGLE_TEMPLE, sp_make_bb(c->cx * 16, 64, c->cz * 16, dir, 12, 10, 15), -1, 0, d);
    sp_set_orientation(p, dir);
    pvec_push(out, p);
    return 1;
}
static void jt_free_cfg(void *p) { free(p); }
const StructType STRUCT_JUNGLE_TEMPLE = { "minecraft:jungle_temple", jt_parse, jt_find, jt_build, NULL, jt_free_cfg, NULL };
void structures_register_jungle_temple(void) { structure_register_type(&STRUCT_JUNGLE_TEMPLE); piece_register_type(&PIECE_JUNGLE_TEMPLE); }

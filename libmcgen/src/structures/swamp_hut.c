/* structures/swamp_hut.c — SwampHutStructure/SwampHutPiece (Java-кодированная постройка).
 * Часть — ScatteredFeaturePiece 7×7×9: высота по среднему рельефу чанка при первом рисовании; ведьма и кот (сущности) не создаются. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_SWAMP_HUT;
typedef struct SHData { int hpos; } SHData;

static void sh_reset(StPiece *p) { ((SHData *)p->data)->hpos = -1; }
static void sh_free(void *v) { free(v); }

static void sh_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    SHData *d = p->data;
    (void)rx; (void)ry; (void)rz;
    if (!sp_update_avg_ground(c, p, &d->hpos, 0)) return;
    const int PL = sp_st(c, "minecraft:spruce_planks"), LOG = sp_st(c, "minecraft:oak_log"), FENCE = sp_st(c, "minecraft:oak_fence"), AIR = sp_air(c);
    sp_box(c, p, 1, 1, 1, 5, 1, 7, PL, PL, 0);
    sp_box(c, p, 1, 4, 2, 5, 4, 7, PL, PL, 0);
    sp_box(c, p, 2, 1, 0, 4, 1, 0, PL, PL, 0);
    sp_box(c, p, 2, 2, 2, 3, 3, 2, PL, PL, 0);
    sp_box(c, p, 1, 2, 3, 1, 3, 6, PL, PL, 0);
    sp_box(c, p, 5, 2, 3, 5, 3, 6, PL, PL, 0);
    sp_box(c, p, 2, 2, 7, 4, 3, 7, PL, PL, 0);
    sp_box(c, p, 1, 0, 2, 1, 3, 2, LOG, LOG, 0);
    sp_box(c, p, 5, 0, 2, 5, 3, 2, LOG, LOG, 0);
    sp_box(c, p, 1, 0, 7, 1, 3, 7, LOG, LOG, 0);
    sp_box(c, p, 5, 0, 7, 5, 3, 7, LOG, LOG, 0);
    sp_place(c, p, FENCE, 2, 3, 2);
    sp_place(c, p, FENCE, 3, 3, 7);
    sp_place(c, p, AIR, 1, 3, 4);
    sp_place(c, p, AIR, 5, 3, 4);
    sp_place(c, p, AIR, 5, 3, 5);
    sp_place(c, p, sp_st(c, "minecraft:potted_red_mushroom"), 1, 3, 5);
    sp_place(c, p, sp_st(c, "minecraft:crafting_table"), 3, 2, 6);
    sp_place(c, p, sp_st(c, "minecraft:cauldron"), 4, 2, 6);
    sp_place(c, p, FENCE, 1, 2, 1);
    sp_place(c, p, FENCE, 5, 2, 1);
    int stairs = sp_st(c, "minecraft:spruce_stairs");
    int northS = sp_with(c, stairs, "facing", "north"), eastS = sp_with(c, stairs, "facing", "east");
    int westS = sp_with(c, stairs, "facing", "west"), southS = sp_with(c, stairs, "facing", "south");
    sp_box(c, p, 0, 4, 1, 6, 4, 1, northS, northS, 0);
    sp_box(c, p, 0, 4, 2, 0, 4, 7, eastS, eastS, 0);
    sp_box(c, p, 6, 4, 2, 6, 4, 7, westS, westS, 0);
    sp_box(c, p, 0, 4, 8, 6, 4, 8, southS, southS, 0);
    sp_place(c, p, sp_with(c, northS, "shape", "outer_right"), 0, 4, 1);
    sp_place(c, p, sp_with(c, northS, "shape", "outer_left"), 6, 4, 1);
    sp_place(c, p, sp_with(c, southS, "shape", "outer_left"), 0, 4, 8);
    sp_place(c, p, sp_with(c, southS, "shape", "outer_right"), 6, 4, 8);
    for (int z = 2; z <= 7; z += 5) for (int x = 1; x <= 5; x += 4) sp_fill_column_down(c, p, LOG, x, -1, z);
    /* ведьма и кот: сущности не воспроизводятся (ГСЧ они не тратят) */
}

const PieceVT PIECE_SWAMP_HUT = { "minecraft:tesh", sh_post, NULL, sh_free, NULL, sh_reset, NULL };

/* ---------------------------------------------------------------- структура */
static void *sh_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int sh_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    if (!gen_could_exist_on_chunk_center(c)) return 0;               /* onTopOfChunkCenter */
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, HM_WORLD_SURFACE_WG); out->state = NULL;
    return 1;
}
static int sh_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    int dir = HD[rs_bound(&c->rs, 4)];
    SHData *d = xcalloc(1, sizeof *d); d->hpos = -1;
    StPiece *p = piece_new(&PIECE_SWAMP_HUT, sp_make_bb(c->cx * 16, 64, c->cz * 16, dir, 7, 7, 9), -1, 0, d);
    sp_set_orientation(p, dir);
    pvec_push(out, p);
    return 1;
}
static void sh_free_cfg(void *p) { free(p); }
const StructType STRUCT_SWAMP_HUT = { "minecraft:swamp_hut", sh_parse, sh_find, sh_build, NULL, sh_free_cfg, NULL };
void structures_register_swamp_hut(void) { structure_register_type(&STRUCT_SWAMP_HUT); piece_register_type(&PIECE_SWAMP_HUT); }

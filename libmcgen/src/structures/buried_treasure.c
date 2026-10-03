/* structures/buried_treasure.c — BuriedTreasureStructure/BuriedTreasurePieces (Java-кодированная постройка).
 * Старт: одна часть-«точка» (x = minX+9, y = 90, z = minZ+9); при рисовании ищется дно (OCEAN_FLOOR_WG), под первым твёрдым слоем породы
 * (песчаник/камень/андезит/гранит/диорит) ставится сундук, соседи-пустоты заполняются песком/породой, bounding box схлопывается на позицию сундука. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_BURIED_TREASURE;

/* BlockState.is(Blocks.WATER) || is(Blocks.LAVA) — по блоку (а не по жидкости: незатопленные блоки не считаются) */
static int is_water_lava(StCtx *c, int st) {
    static _Thread_local const BsTab *kb; static _Thread_local int water, lava;
    const BsTab *bs = c->sw->bs;
    if (kb != bs) { water = bs_block_index(bs, "minecraft:water"); lava = bs_block_index(bs, "minecraft:lava"); kb = bs; }
    int b = c->w->g->state_block[st];
    return b == water || b == lava;
}

static void bt_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    const McGen *g = c->w->g;
    int x = p->bb.x0, z = p->bb.z0;
    int y = sp_height(c, HM_OCEAN_FLOOR_WG, x, z);
    static _Thread_local const BsTab *kb; static _Thread_local int sandstone, stone, andesite, granite, diorite;
    const BsTab *bs = c->sw->bs;
    if (kb != bs) {
        sandstone = bs_block_index(bs, "minecraft:sandstone"); stone = bs_block_index(bs, "minecraft:stone"); andesite = bs_block_index(bs, "minecraft:andesite");
        granite = bs_block_index(bs, "minecraft:granite"); diorite = bs_block_index(bs, "minecraft:diorite"); kb = bs;
    }
    int sand = sp_st(c, "minecraft:sand");
    while (y > c->w->min_y) {
        int cur = sp_get_world(c, x, y, z), below = sp_get_world(c, x, y - 1, z);
        int bb = g->state_block[below];
        if (bb == sandstone || bb == stone || bb == andesite || bb == granite || bb == diorite) {
            int soft = (!gen_is_air(g, cur) && !is_water_lava(c, cur)) ? cur : sand;
            for (int d = 0; d < 6; d++) {        /* Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST */
                int rx2 = x + DIR_DX[d], ry2 = y + DIR_DY[d], rz2 = z + DIR_DZ[d];
                int rel = sp_get_world(c, rx2, ry2, rz2);
                if (gen_is_air(g, rel) || is_water_lava(c, rel)) {
                    int brel = sp_get_world(c, rx2, ry2 - 1, rz2);
                    if ((gen_is_air(g, brel) || is_water_lava(c, brel)) && d != DIR_UP) sp_set_world(c, rx2, ry2, rz2, below);
                    else sp_set_world(c, rx2, ry2, rz2, soft);
                }
            }
            p->bb = bb_make(x, y, z, x, y, z);
            sp_create_chest(c, p, x, y, z, -1);         /* orient < 0: координаты мировые */
            return;
        }
        y--;
    }
}

const PieceVT PIECE_BURIED_TREASURE = { "minecraft:btp", bt_post, NULL, NULL, NULL, NULL, NULL };

/* ---------------------------------------------------------------- структура */
static void *bt_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int bt_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    if (!gen_could_exist_on_chunk_center(c)) return 0;               /* onTopOfChunkCenter(OCEAN_FLOOR_WG) */
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, HM_OCEAN_FLOOR_WG); out->state = NULL;
    return 1;
}
static int bt_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    int x = c->cx * 16 + 9, z = c->cz * 16 + 9;
    pvec_push(out, piece_new(&PIECE_BURIED_TREASURE, bb_make(x, 90, z, x, 90, z), -1, 0, NULL));
    return 1;
}
static void bt_free_cfg(void *p) { free(p); }
const StructType STRUCT_BURIED_TREASURE = { "minecraft:buried_treasure", bt_parse, bt_find, bt_build, NULL, bt_free_cfg, NULL };
void structures_register_buried_treasure(void) { structure_register_type(&STRUCT_BURIED_TREASURE); piece_register_type(&PIECE_BURIED_TREASURE); }

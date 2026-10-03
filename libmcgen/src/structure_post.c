/* structure_post.c — пост-обработка позиций, помеченных постройками (LevelChunk.postProcessGeneration): Block.updateFromNeighbourShapes для
 * блоков SHAPE_CHECK_BLOCKS (заборы, железные решётки/стёкла-панели, факелы, лестницы-ladder). Жидкости тикает fluidpp.c; сюда попадают
 * не-жидкие блоки. Вызывается из fluidpp_chunk через FluidWorld.shape_update (region.c ставит указатель при включённой стадии STRUCTURES). */
#include "structure.h"
#include "fluidpp.h"
#include <stdio.h>

static int is_leaves_or_exception(const BsTab *bs, int st) {
    static _Thread_local const BsTab *kb; static _Thread_local int barrier, cp, jl, melon, pumpkin; static _Thread_local const u8 *shulk;
    if (kb != bs) {
        barrier = bs_block_index(bs, "minecraft:barrier"); cp = bs_block_index(bs, "minecraft:carved_pumpkin"); jl = bs_block_index(bs, "minecraft:jack_o_lantern");
        melon = bs_block_index(bs, "minecraft:melon"); pumpkin = bs_block_index(bs, "minecraft:pumpkin"); shulk = gen_block_tag(bs->g, "minecraft:shulker_boxes"); kb = bs;
    }
    int b = bs->g->state_block[st];
    return (bs->flags[st] & BSF_LEAVES) || b == barrier || b == cp || b == jl || b == melon || b == pumpkin || (shulk && shulk[b]);
}
static int sturdy_face(const BsTab *bs, int st, int dir) { return (bs->sturdy[st] >> dir) & 1; }

/* BlockState.updateShape для поддержанных классов; −1 — класс не обрабатывается */
static int update_shape(const BsTab *bs, FluidWorld *fw, int st, int x, int y, int z, int dir, int nst) {
    const McGen *g = bs->g;
    static const char *NM[6] = { "down", "up", "north", "south", "west", "east" };
    if (bs_is_a(bs, st, "FenceBlock") || bs_is_a(bs, st, "IronBarsBlock")) {
        if (dir == DIR_DOWN || dir == DIR_UP) return st;
        int opp = dir_opp(dir);
        int sturdy = sturdy_face(bs, nst, opp);
        int conn;
        if (bs_is_a(bs, st, "FenceBlock")) {
            static _Thread_local const BsTab *kb; static _Thread_local const u8 *fences, *wooden;
            if (kb != bs) { fences = gen_block_tag(g, "minecraft:fences"); wooden = gen_block_tag(g, "minecraft:wooden_fences"); kb = bs; }
            int nb = g->state_block[nst], sb = g->state_block[st];
            int same = fences[nb] && (wooden[nb] != 0) == (wooden[sb] != 0);
            int gate = 0;
            if (bs_is_a(bs, nst, "FenceGateBlock")) {
                const char *fv; if (bs_get_prop(bs, nst, "facing", &fv)) { int fd = !strcmp(fv, "north") ? DIR_NORTH : !strcmp(fv, "south") ? DIR_SOUTH : !strcmp(fv, "west") ? DIR_WEST : DIR_EAST; int cw = dir_cw(opp);
                    gate = ((fd == DIR_NORTH || fd == DIR_SOUTH) == (cw == DIR_NORTH || cw == DIR_SOUTH)); }
            }
            conn = (!is_leaves_or_exception(bs, nst) && sturdy) || same || gate;
        } else {
            static _Thread_local const BsTab *kb2; static _Thread_local const u8 *walls;
            if (kb2 != bs) { walls = gen_block_tag(g, "minecraft:walls"); kb2 = bs; }
            conn = (!is_leaves_or_exception(bs, nst) && sturdy) || bs_is_a(bs, nst, "IronBarsBlock") || walls[g->state_block[nst]];
        }
        int r = bs_with(bs, st, NM[dir], conn ? "true" : "false");
        return r < 0 ? st : r;
    }
    if (bs_is_a(bs, st, "WallTorchBlock") || bs_is_a(bs, st, "LadderBlock")) {
        const char *fv; if (!bs_get_prop(bs, st, "facing", &fv)) return st;
        int f = !strcmp(fv, "north") ? DIR_NORTH : !strcmp(fv, "south") ? DIR_SOUTH : !strcmp(fv, "west") ? DIR_WEST : DIR_EAST;
        if (dir_opp(dir) == f) {
            int ax = x + DIR_DX[dir_opp(f)], ay = y, az = z + DIR_DZ[dir_opp(f)];
            int as = fw->get(fw->ud, ax, ay, az);
            if (!sturdy_face(bs, as, f)) return g->st_air;
        }
        return st;
    }
    if (bs_is_a(bs, st, "TorchBlock") && !bs_is_a(bs, st, "RedstoneTorchBlock") && !bs_is_a(bs, st, "WallTorchBlock")) {
        if (dir == DIR_DOWN) {
            int below = fw->get(fw->ud, x, y - 1, z);
            /* canSupportCenter(below, UP): прочная верхняя грань либо «центральная» опора (заборы, стены) */
            static _Thread_local const BsTab *kb3; static _Thread_local const u8 *fences, *walls;
            if (kb3 != bs) { fences = gen_block_tag(g, "minecraft:fences"); walls = gen_block_tag(g, "minecraft:walls"); kb3 = bs; }
            int bb = g->state_block[below];
            int ok = sturdy_face(bs, below, DIR_UP) || fences[bb] || walls[bb];
            if (!ok) return g->st_air;
        }
        return st;
    }
    return -1;
}

/* Block.updateFromNeighbourShapes + setBlock(pos, new, 20) для не-жидкого блока */
void structure_shape_update(void *fwp, int x, int y, int z) {
    FluidWorld *fw = fwp;
    const BsTab *bs = bs_get(fw->g);
    int st = fw->get(fw->ud, x, y, z);
    if (bs->flags[st] & BSF_LIQUID) return;                    /* LiquidBlock — не обновляется формой */
    static const int ORDER[6] = { DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH, DIR_DOWN, DIR_UP };
    int cur = st;
    for (int i = 0; i < 6; i++) {
        int d = ORDER[i];
        int nst = fw->get(fw->ud, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]);
        int r = update_shape(bs, fw, cur, x, y, z, d, nst);
        if (r >= 0) cur = r;
    }
    if (cur != st) fw->set(fw->ud, x, y, z, cur);
}

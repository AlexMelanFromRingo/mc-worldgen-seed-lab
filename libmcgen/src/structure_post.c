/* structure_post.c — пост-обработка позиций, помеченных постройками (LevelChunk.postProcessGeneration): Block.updateFromNeighbourShapes для
 * блоков SHAPE_CHECK_BLOCKS (заборы, железные решётки/стёкла-панели, факелы, лестницы-ladder). Жидкости тикает fluidpp.c; сюда попадают
 * не-жидкие блоки. Вызывается из fluidpp_chunk через FluidWorld.shape_update (region.c ставит указатель при включённой стадии STRUCTURES). */
#include "structure.h"
#include "fluidpp.h"
#include "feature.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

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

/* доступ к миру для обновления форм */
typedef struct SGet { void *ud; int (*get)(void *ud, int x, int y, int z); int dark; FCtx *fc; } SGet;   /* dark: свет ещё не посчитан (генерация: getRawBrightness = 0) */
static int fw_get(void *ud, int x, int y, int z) { FluidWorld *fw = ud; return fw->get(fw->ud, x, y, z); }
static int stairs_shape_of(const BsTab *bs, const SGet *wg, int st, int x, int y, int z);

static int dir_of_prop(const BsTab *bs, int st, const char *prop);
static int sturdy_face(const BsTab *bs, int st, int dir);
static int is_leaves_or_exception(const BsTab *bs, int st);

/* WallBlock.isCovered(above.getCollisionShape().getFaceShape(DOWN), test): test 0..3 — TEST_SHAPES_WALL N/E/S/W (x 7..9, z 0..9 и повороты),
 * 4 — TEST_SHAPE_POST (столбик 7..9). Полная нижняя грань накрывает всё; стена сверху: сторона накрыта её стороной того же направления
 * (коллизия стороны 5..11 до края), столбик — её столбом (4..12) или любой стороной (проходят через центр). Прочие формы — нет. */
static int wall_covered(const BsTab *bs, int above, int test) {
    if (sturdy_face(bs, above, DIR_DOWN)) return 1;
    if (!bs_is_a(bs, above, "WallBlock")) return 0;
    static const char *SN[4] = { "north", "east", "south", "west" };
    const char *v;
    if (test < 4) { v = "none"; bs_get_prop(bs, above, SN[test], &v); return strcmp(v, "none") != 0; }
    v = "false"; bs_get_prop(bs, above, "up", &v); if (!strcmp(v, "true")) return 1;
    for (int i = 0; i < 4; i++) { v = "none"; bs_get_prop(bs, above, SN[i], &v); if (strcmp(v, "none") != 0) return 1; }
    return 0;
}
/* WallBlock.updateShape: стороны (none/low/tall по isCovered) и столб (shouldRaisePost) */
static int wall_update(const BsTab *bs, const SGet *wg, int st, int x, int y, int z, int dir, int nst) {
    const McGen *g = bs->g;
    static const char *DN6[6] = { "down", "up", "north", "south", "west", "east" };
    if (dir == DIR_DOWN) return st;
    static _Thread_local const BsTab *kb; static _Thread_local const u8 *walls, *post_ovr;
    if (kb != bs) { walls = gen_block_tag(g, "minecraft:walls"); post_ovr = gen_block_tag(g, "minecraft:wall_post_override"); kb = bs; }
    int conn[6] = { 0 };
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    for (int i = 0; i < 4; i++) { const char *v = "none"; bs_get_prop(bs, st, DN6[HD[i]], &v); conn[HD[i]] = strcmp(v, "none") != 0; }
    int above;
    if (dir == DIR_UP) above = nst;
    else {
        int opp = dir_opp(dir);
        int gate = 0;
        if (bs_is_a(bs, nst, "FenceGateBlock")) { int fd = dir_of_prop(bs, nst, "facing"); if (fd >= 0) { int cw = dir_cw(fd); gate = (cw == DIR_NORTH || cw == DIR_SOUTH) == (opp == DIR_NORTH || opp == DIR_SOUTH); } }
        conn[dir] = (walls && walls[g->state_block[nst]]) || (!is_leaves_or_exception(bs, nst) && sturdy_face(bs, nst, opp)) || bs_is_a(bs, nst, "IronBarsBlock") || gate;
        above = wg->get(wg->ud, x, y + 1, z);
    }
    int side[4];                                               /* 0 none, 1 low, 2 tall (порядок HD) */
    int r = st;
    for (int i = 0; i < 4; i++) {
        side[i] = conn[HD[i]] ? (wall_covered(bs, above, i) ? 2 : 1) : 0;
        int q = bs_with(bs, r, DN6[HD[i]], side[i] == 2 ? "tall" : side[i] == 1 ? "low" : "none"); if (q >= 0) r = q;
    }
    int up; const char *av;
    if (bs_is_a(bs, above, "WallBlock") && bs_get_prop(bs, above, "up", &av) && !strcmp(av, "true")) up = 1;
    else {
        int nn = !side[0], en = !side[1], sn = !side[2], wn = !side[3];
        if ((nn && sn && wn && en) || nn != sn || wn != en) up = 1;
        else if ((side[0] == 2 && side[2] == 2) || (side[1] == 2 && side[3] == 2)) up = 0;
        else up = (post_ovr && post_ovr[g->state_block[above]]) || wall_covered(bs, above, 4);
    }
    int q = bs_with(bs, r, "up", up ? "true" : "false"); if (q >= 0) r = q;
    return r;
}
/* BlockState.updateShape для поддержанных классов (заборы, решётки, лестницы, «снежные» блоки, факелы, ladder); −1 — класс не обрабатывается */
int tree_upd_shape(FCtx *c, int st, int x, int y, int z, int d);                 /* feature_tree_deco.c */
static int update_shape_interest(const BsTab *bs, int st) {         /* блоки классов, обрабатываемых update_shape (остальные — сразу −1): кэш по блокам */
    static _Thread_local const BsTab *kb; static _Thread_local u8 *tab;
    if (kb != bs || !tab) {
        free(tab); tab = xcalloc((size_t)bs->nblocks, 1); kb = bs;
        static const char *CL[] = { "CropBlock", "SnowyBlock", "SpreadingSnowyBlock", "StairBlock", "FenceBlock", "IronBarsBlock", "WallTorchBlock", "LadderBlock", "TorchBlock",
                                    "WallBlock", "DoorBlock", "ChestBlock", "WallBannerBlock", "DoublePlantBlock", "VegetationBlock", "MultifaceBlock", "VineBlock", "CocoaBlock", "PointedDripstoneBlock", "SpeleothemBlock", NULL };
        for (int b = 0; b < bs->nblocks; b++) {
            if (bs->blk[b].count <= 0) continue;
            for (int i = 0; CL[i]; i++) if (bs_is_a(bs, bs->blk[b].first, CL[i])) { tab[b] = 1; break; }
        }
    }
    return tab[bs->g->state_block[st]];
}
/* сталактит/сталагмит (PointedDripstoneBlock; с 26.2 — SpeleothemBlock) с направлением кончика d */
static int spel_dir(const BsTab *bs, int st, int d) {
    if (!bs_is_a(bs, st, "PointedDripstoneBlock") && !bs_is_a(bs, st, "SpeleothemBlock")) return 0;
    const char *v = NULL;
    return bs_get_prop(bs, st, "vertical_direction", &v) && v && !strcmp(v, d == DIR_UP ? "up" : "down");
}
static int update_shape(const BsTab *bs, const SGet *wg, int st, int x, int y, int z, int dir, int nst) {
    const McGen *g = bs->g;
    if (!update_shape_interest(bs, st)) return -1;
    /* CropBlock (VegetationBlock.updateShape → canSurvive): при генерации свет не посчитан, getRawBrightness < 8 → воздух */
    if (wg->dark && bs_is_a(bs, st, "CropBlock")) return g->st_air;
    /* VegetationBlock.updateShape (и DoublePlantBlock: super): !canSurvive → воздух; почва могла измениться после размещения (диски, деревья), поэтому помеченные растения проверяются заново */
    if (wg->fc && bs_is_a(bs, st, "VegetationBlock") && !block_can_survive(wg->fc, st, x, y, z)) return g->st_air;
    if (bs_is_a(bs, st, "SnowyBlock") || bs_is_a(bs, st, "SpreadingSnowyBlock")) {
        if (dir != DIR_UP) return st;
        static _Thread_local const BsTab *kb; static _Thread_local const u8 *snow;
        if (kb != bs) { snow = gen_block_tag(g, "minecraft:snow"); kb = bs; }
        int r = bs_with(bs, st, "snowy", snow[g->state_block[nst]] ? "true" : "false");
        return r < 0 ? st : r;
    }
    if (bs_is_a(bs, st, "StairBlock")) {
        if (dir == DIR_DOWN || dir == DIR_UP) return st;
        return stairs_shape_of(bs, wg, st, x, y, z);
    }
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
            int as = wg->get(wg->ud, ax, ay, az);
            if (!sturdy_face(bs, as, f)) return g->st_air;
        }
        return st;
    }
    if (bs_is_a(bs, st, "TorchBlock") && !bs_is_a(bs, st, "RedstoneTorchBlock") && !bs_is_a(bs, st, "WallTorchBlock")) {
        if (dir == DIR_DOWN) {
            int below = wg->get(wg->ud, x, y - 1, z);
            /* canSupportCenter(below, UP): прочная верхняя грань либо «центральная» опора (заборы, стены) */
            static _Thread_local const BsTab *kb3; static _Thread_local const u8 *fences, *walls;
            if (kb3 != bs) { fences = gen_block_tag(g, "minecraft:fences"); walls = gen_block_tag(g, "minecraft:walls"); kb3 = bs; }
            int bb = g->state_block[below];
            int ok = sturdy_face(bs, below, DIR_UP) || fences[bb] || walls[bb];
            if (!ok) return g->st_air;
        }
        return st;
    }
    if (bs_is_a(bs, st, "WallBlock")) return wall_update(bs, wg, st, x, y, z, dir, nst);
    if (bs_is_a(bs, st, "DoorBlock")) {
        const char *half = ""; bs_get_prop(bs, st, "half", &half);
        int lower = !strcmp(half, "lower");
        if ((dir != DIR_UP && dir != DIR_DOWN) || lower != (dir == DIR_UP)) {
            if (lower && dir == DIR_DOWN && !sturdy_face(bs, wg->get(wg->ud, x, y - 1, z), DIR_UP)) return g->st_air;
            return st;
        }
        const char *nh = "";
        if (bs_is_a(bs, nst, "DoorBlock") && bs_get_prop(bs, nst, "half", &nh) && strcmp(nh, half) != 0) { int r = bs_with(bs, nst, "half", half); return r < 0 ? nst : r; }
        return g->st_air;
    }
    if (bs_is_a(bs, st, "ChestBlock")) {
        static const char *TYPES[3] = { "single", "left", "right" };
        const char *tv = "single"; bs_get_prop(bs, st, "type", &tv);
        int f = dir_of_prop(bs, st, "facing");
        int conn_dir = !strcmp(tv, "left") ? dir_cw(f) : dir_ccw(f);
        if (g->state_block[nst] == g->state_block[st] && dir != DIR_UP && dir != DIR_DOWN) {
            const char *nt = "single"; bs_get_prop(bs, nst, "type", &nt);
            int nf = dir_of_prop(bs, nst, "facing");
            int nconn = !strcmp(nt, "left") ? dir_cw(nf) : dir_ccw(nf);
            if (!strcmp(tv, "single") && strcmp(nt, "single") != 0 && f == nf && nconn == dir_opp(dir)) {
                int r = bs_with(bs, st, "type", !strcmp(nt, "left") ? TYPES[2] : TYPES[1]); return r < 0 ? st : r;
            }
        } else if (conn_dir == dir) { int r = bs_with(bs, st, "type", "single"); return r < 0 ? st : r; }
        return st;
    }
    if (bs_is_a(bs, st, "WallBannerBlock")) {
        int f = dir_of_prop(bs, st, "facing");
        if (dir == dir_opp(f)) {
            int o = dir_opp(f);
            if (!(bs->flags[wg->get(wg->ud, x + DIR_DX[o], y, z + DIR_DZ[o])] & BSF_SOLID)) return g->st_air;
        }
        return st;
    }
    if (wg->fc && (bs_is_a(bs, st, "VineBlock") || bs_is_a(bs, st, "CocoaBlock"))) return tree_upd_shape(wg->fc, st, x, y, z, dir);   /* VineBlock/CocoaBlock.updateShape (опора: feature_tree_deco.c) */
    if (bs_is_a(bs, st, "MultifaceBlock")) {            /* MultifaceBlock.updateShape (светящийся лишайник, скульк-жилы): грань без опоры снимается, без граней — воздух */
        static const char *FN[6] = { "down", "up", "north", "south", "west", "east" };
        const char *fv = NULL;
        if (!bs_get_prop(bs, st, FN[dir], &fv) || !fv || strcmp(fv, "true")) return st;
        if ((bs->sturdy[nst] >> dir_opp(dir)) & 1 || (bs->flags[nst] & BSF_FULL_COLL)) return st;      /* canAttachTo: прочная/полная грань напротив */
        int r = bs_with(bs, st, FN[dir], "false");
        if (r < 0) return st;
        for (int i = 0; i < 6; i++) { const char *v = NULL; if (bs_get_prop(bs, r, FN[i], &v) && v && !strcmp(v, "true")) return r; }
        return g->st_air;
    }
    if (bs_is_a(bs, st, "PointedDripstoneBlock") || bs_is_a(bs, st, "SpeleothemBlock")) {
        /* SpeleothemBlock.updateShape (26.1: PointedDripstoneBlock): только по вертикали; опоры нет — игра ставит отложенный тик (состояние не меняется);
         * иначе толщина пересчитывается (calculateSpeleothemThickness) — после замены соседа водой «tip_merge» без пары становится «tip» */
        if (dir != DIR_UP && dir != DIR_DOWN) return st;
        const char *vd = NULL; if (!bs_get_prop(bs, st, "vertical_direction", &vd) || !vd) return st;
        int tip = !strcmp(vd, "up") ? DIR_UP : DIR_DOWN, base = dir_opp(tip);
        int behind = wg->get(wg->ud, x + DIR_DX[base], y + DIR_DY[base], z + DIR_DZ[base]);
        if (dir == base) {                                      /* BlockBehaviour.canSurvive: за спиной прочная грань либо тот же сталактит/сталагмит */
            int ok = sturdy_face(bs, behind, tip) || (spel_dir(bs, behind, tip) && g->state_block[behind] == g->state_block[st]);
            if (!ok) return st;
        }
        const char *th = NULL; bs_get_prop(bs, st, "thickness", &th);
        int merge = th && !strcmp(th, "tip_merge");
        int front = wg->get(wg->ud, x + DIR_DX[tip], y + DIR_DY[tip], z + DIR_DZ[tip]);
        const char *nt = "tip";
        if (spel_dir(bs, front, base) && g->state_block[front] == g->state_block[st]) {
            const char *ft = NULL; bs_get_prop(bs, front, "thickness", &ft);
            nt = (merge || (ft && !strcmp(ft, "tip_merge"))) ? "tip_merge" : "tip";
        } else if (spel_dir(bs, front, tip)) {
            const char *ft = NULL; bs_get_prop(bs, front, "thickness", &ft);
            if (ft && (!strcmp(ft, "tip") || !strcmp(ft, "tip_merge"))) nt = "frustum";
            else nt = spel_dir(bs, behind, tip) ? "middle" : "base";
        }
        int r = bs_with(bs, st, "thickness", nt);
        return r < 0 ? st : r;
    }
    if (bs_is_a(bs, st, "DoublePlantBlock")) {
        const char *half = ""; bs_get_prop(bs, st, "half", &half);
        int lower = !strcmp(half, "lower");
        if ((dir == DIR_UP && lower) || (dir == DIR_DOWN && !lower)) {
            if (g->state_block[nst] != g->state_block[st]) return g->st_air;
            const char *nh = ""; bs_get_prop(bs, nst, "half", &nh);
            if (!strcmp(nh, half)) return g->st_air;
        }
        return st;
    }
    return -1;
}

/* лестницы: StairBlock.getStairsShape */
static int dir_of_prop(const BsTab *bs, int st, const char *prop) {
    const char *v; if (!bs_get_prop(bs, st, prop, &v)) return -1;
    return !strcmp(v, "north") ? DIR_NORTH : !strcmp(v, "south") ? DIR_SOUTH : !strcmp(v, "west") ? DIR_WEST : !strcmp(v, "east") ? DIR_EAST : -1;
}
static int is_stairs(const BsTab *bs, int st) { return bs_is_a(bs, st, "StairBlock"); }
static int same_half(const BsTab *bs, int a, int b) { const char *x, *y; return bs_get_prop(bs, a, "half", &x) && bs_get_prop(bs, b, "half", &y) && !strcmp(x, y); }
static int can_take_shape(const BsTab *bs, const SGet *wg, int st, int x, int y, int z, int nd) {
    int ns = wg->get(wg->ud, x + DIR_DX[nd], y, z + DIR_DZ[nd]);
    return !is_stairs(bs, ns) || dir_of_prop(bs, ns, "facing") != dir_of_prop(bs, st, "facing") || !same_half(bs, ns, st);
}
static int stairs_shape_of(const BsTab *bs, const SGet *wg, int st, int x, int y, int z) {
    int facing = dir_of_prop(bs, st, "facing");
    const char *shape = "straight";
    int behind = wg->get(wg->ud, x + DIR_DX[facing], y, z + DIR_DZ[facing]);
    int found = 0;
    if (is_stairs(bs, behind) && same_half(bs, st, behind)) {
        int bf = dir_of_prop(bs, behind, "facing");
        int ax_b = (bf == DIR_NORTH || bf == DIR_SOUTH), ax_f = (facing == DIR_NORTH || facing == DIR_SOUTH);
        if (ax_b != ax_f && can_take_shape(bs, wg, st, x, y, z, dir_opp(bf))) { shape = bf == dir_ccw(facing) ? "outer_left" : "outer_right"; found = 1; }
    }
    if (!found) {
        int fo = dir_opp(facing);
        int front = wg->get(wg->ud, x + DIR_DX[fo], y, z + DIR_DZ[fo]);
        if (is_stairs(bs, front) && same_half(bs, st, front)) {
            int ff = dir_of_prop(bs, front, "facing");
            int ax_b = (ff == DIR_NORTH || ff == DIR_SOUTH), ax_f = (facing == DIR_NORTH || facing == DIR_SOUTH);
            if (ax_b != ax_f && can_take_shape(bs, wg, st, x, y, z, ff)) shape = ff == dir_ccw(facing) ? "inner_left" : "inner_right";
        }
    }
    int r = bs_with(bs, st, "shape", shape);
    return r < 0 ? st : r;
}

/* Block.updateFromNeighbourShapes для одной позиции (порядок W, E, N, S, D, U); get/set — доступ к миру */
static int update_from_neighbours(const BsTab *bs, const SGet *wg, int st, int x, int y, int z) {
    static const int ORDER[6] = { DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH, DIR_DOWN, DIR_UP };
    int cur = st;
    for (int i = 0; i < 6; i++) {
        int d = ORDER[i];
        int nst = wg->get(wg->ud, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]);
        int r = update_shape(bs, wg, cur, x, y, z, d, nst);
        if (r >= 0) cur = r;
    }
    return cur;
}

/* LiquidBlock.tick (LevelChunk.postProcessGeneration для помеченной жидкости: магма/песок душ помечают блок над собой):
 * вода-источник над магмой/песком душ → BubbleColumnBlock.updateColumn (столб вверх по воде-источникам) */
static void liquid_block_tick(FluidWorld *fw, const BsTab *bs, int st, int x, int y, int z) {
    const McGen *g = bs->g;
    static _Thread_local const BsTab *kb; static _Thread_local int water0, bubble, bub_drag, bub_up; static _Thread_local const u8 *down, *up;
    if (kb != bs) {
        water0 = gen_state_id(g, "minecraft:water[level=0]"); bubble = bs_block_index(bs, "minecraft:bubble_column");
        bub_drag = gen_state_id(g, "minecraft:bubble_column[drag=true]"); bub_up = gen_state_id(g, "minecraft:bubble_column[drag=false]");
        down = gen_block_tag(g, "minecraft:enables_bubble_column_drag_down"); up = gen_block_tag(g, "minecraft:enables_bubble_column_push_up"); kb = bs;
    }
    if (st != water0 || bubble < 0) return;                    /* shouldBubbleColumnOccupy: вода-источник, полная */
    int below = fw->get(fw->ud, x, y - 1, z), bb = g->state_block[below];
    int col;                                                   /* getColumnState */
    if (bb == bubble) col = below;
    else if (up && up[bb]) col = bub_up;
    else if (down && down[bb]) col = bub_drag;
    else return;                                               /* → occupyState (вода): без изменений */
    for (int yy = y; yy < 4096; yy++) {                        /* canOccupy: вода-источник (LiquidBlock) или столб */
        int o = fw->get(fw->ud, x, yy, z);
        if (o != water0 && g->state_block[o] != bubble) break;
        fw->set(fw->ud, x, yy, z, col);
    }
}

/* Level.setBlock(pos, state, 3) в пост-обработке (жидкость заменила блок): BlockStateBase.updateNeighbourShapes — у 6 соседей (W, E, N, S, D, U) updateShape от изменившейся клетки;
 * изменённый сосед пишется с флагами без обновления соседей (flags & -34), а его замена воздухом идёт через destroyBlock (флаги 3) и тянет следующий круг (лимит рекурсии). */
static void neighbors_update_rec(FluidWorld *fw, const BsTab *bs, const SGet *wg, int x, int y, int z, int limit) {
    static const int ORDER[6] = { DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH, DIR_DOWN, DIR_UP };
    if (limit <= 0) return;
    const McGen *g = bs->g;
    for (int i = 0; i < 6; i++) {
        int d = ORDER[i];
        int nx = x + DIR_DX[d], ny = y + DIR_DY[d], nz = z + DIR_DZ[d];
        int cur = fw->get(fw->ud, x, y, z);                        /* изменившаяся клетка (могла поменяться из-за предыдущего соседа) */
        int nst = fw->get(fw->ud, nx, ny, nz);
        if (bs->flags[nst] & BSF_LIQUID) continue;
        int r = update_shape(bs, wg, nst, nx, ny, nz, dir_opp(d), cur);
        if (r < 0 || r == nst) continue;
        if (bs->flags[r] & BSF_AIR) {                               /* Block.updateOrDestroy → destroyBlock: на месте блока — его жидкость (waterlogged → вода), флаги 3 */
            int fl = bs->fluid[nst]; int ty = BS_FL_TYPE(fl);
            int repl = g->st_air;
            if (ty == FL_WATER || ty == FL_FLOWING_WATER) repl = gen_state_id(g, "minecraft:water[level=0]");
            fw->set(fw->ud, nx, ny, nz, repl);
            neighbors_update_rec(fw, bs, wg, nx, ny, nz, limit - 1);
        } else fw->set(fw->ud, nx, ny, nz, r);
    }
}
void structure_neighbors_update(void *fwp, int x, int y, int z) {
    FluidWorld *fw = fwp;
    const BsTab *bs = bs_get(fw->g);
    SGet wg = { fw, fw_get, 0, NULL };
    static _Thread_local FCtx fcx; static _Thread_local const void *fcw;
    if ((fw->post_flags & 1) && fw->world) {
        if (fcw != fw->world) {
            McWorld *mw = fw->world; memset(&fcx, 0, sizeof fcx);
            fcx.w = mw; fcx.g = fw->g; fcx.bs = bs; fcx.min_y = mw->min_y; fcx.height = mw->height; fcx.sea_level = mw->sea_level;
            fcx.st_air = fw->g->st_air; fcx.st_cave_air = fw->g->st_cave_air; fcx.st_void_air = bs->st_void_air; fcx.st_water = fw->g->st_water; fcx.st_lava = fw->g->st_lava;
            fcx.ccx = INT_MIN / 2; fcx.ccz = INT_MIN / 2; fcx.post = 1; fcw = fw->world;
        }
        fcx.ext_get = fw->get; fcx.ext_ud = fw->ud; wg.fc = &fcx;
    }
    neighbors_update_rec(fw, bs, &wg, x, y, z, 64);
}


/* Block.updateFromNeighbourShapes + setBlock(pos, new, 20) для не-жидкого блока (пометки построек) */
void structure_shape_update(void *fwp, int x, int y, int z) {
    FluidWorld *fw = fwp;
    const BsTab *bs = bs_get(fw->g);
    int st = fw->pp_state;      /* blockState, считанный ДО fluidState.tick (так в LevelChunk.postProcessGeneration): жидкость, затёкшая в клетку растения, не делает её LiquidBlock */
    if (bs->flags[st] & BSF_LIQUID) { liquid_block_tick(fw, bs, st, x, y, z); return; }   /* LiquidBlock: blockState.tick вместо обновления формы */
    SGet wg = { fw, fw_get, 0, NULL };
    /* контекст FCtx пост-обработки: canSurvive растений (feature_bpred.c / feature_veg.c) читает мир через FluidWorld; свет — настоящий (c.post) */
    static _Thread_local FCtx fcx; static _Thread_local const void *fcw;
    if ((fw->post_flags & 1) && fw->world) {
        if (fcw != fw->world) {
            McWorld *mw = fw->world; memset(&fcx, 0, sizeof fcx);
            fcx.w = mw; fcx.g = fw->g; fcx.bs = bs; fcx.min_y = mw->min_y; fcx.height = mw->height; fcx.sea_level = mw->sea_level;
            fcx.st_air = fw->g->st_air; fcx.st_cave_air = fw->g->st_cave_air; fcx.st_void_air = bs->st_void_air; fcx.st_water = fw->g->st_water; fcx.st_lava = fw->g->st_lava;
            fcx.ccx = INT_MIN / 2; fcx.ccz = INT_MIN / 2; fcx.post = 1; fcw = fw->world;
        }
        fcx.ext_get = fw->get; fcx.ext_ud = fw->ud; wg.fc = &fcx;
    }
    int cur = update_from_neighbours(bs, &wg, st, x, y, z);
    if (cur != st) fw->set(fw->ud, x, y, z, cur);
}

/* ---- StructureTemplate.placeInWorld при knownShape = false: updateShapeAtEdge + updateFromNeighbourShapes ---- */
typedef struct { FCtx *fc; } FcAcc;
static int fc_acc_get(void *ud, int x, int y, int z) { return fc_get(((FcAcc *)ud)->fc, x, y, z); }
static void edge_face(FCtx *fc, const BsTab *bs, int flags, int dir, int x, int y, int z) {
    FcAcc a = { fc }; SGet wg = { &a, fc_acc_get, 1, fc };
    int nx = x + DIR_DX[dir], ny = y + DIR_DY[dir], nz = z + DIR_DZ[dir];
    int st = fc_get(fc, x, y, z), nst = fc_get(fc, nx, ny, nz);
    int r = update_shape(bs, &wg, st, x, y, z, dir, nst);
    int newst = r >= 0 ? r : st;
    if (newst != st) fc_set(fc, x, y, z, newst, flags & ~1);
    int r2 = update_shape(bs, &wg, nst, nx, ny, nz, dir_opp(dir), newst);
    int newn = r2 >= 0 ? r2 : nst;
    if (newn != nst) fc_set(fc, nx, ny, nz, newn, flags & ~1);
}
/* placed — тройки (x, y, z) размещённых позиций в порядке размещения; flags — updateMode */
void structure_update_after_template(FCtx *fc, const int *placed, int n, int flags) {
    if (n <= 0) return;
    const BsTab *bs = fc->bs;
    int minx = placed[0], miny = placed[1], minz = placed[2], maxx = minx, maxy = miny, maxz = minz;
    for (int i = 1; i < n; i++) {
        int x = placed[3 * i], y = placed[3 * i + 1], z = placed[3 * i + 2];
        if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; if (z < minz) minz = z; if (z > maxz) maxz = z;
    }
    int sx = maxx - minx + 1, sy = maxy - miny + 1, sz = maxz - minz + 1;
    u8 *full = xcalloc((size_t)sx * sy * sz, 1);
    #define IDX(x, y, z) (((size_t)(x) * sy + (y)) * sz + (z))
    for (int i = 0; i < n; i++) full[IDX(placed[3 * i] - minx, placed[3 * i + 1] - miny, placed[3 * i + 2] - minz)] = 1;
    /* forAllFaces: циклы осей NONE (z), FORWARD (y), BACKWARD (x) — порядок как в DiscreteVoxelShape */
    for (int cyc = 0; cyc < 3; cyc++) {
        int aSize = cyc == 0 ? sx : cyc == 1 ? sz : sy, bSize = cyc == 0 ? sy : cyc == 1 ? sx : sz, cSize = cyc == 0 ? sz : cyc == 1 ? sy : sx;
        int neg = cyc == 0 ? DIR_NORTH : cyc == 1 ? DIR_DOWN : DIR_WEST, pos = cyc == 0 ? DIR_SOUTH : cyc == 1 ? DIR_UP : DIR_EAST;
        for (int a = 0; a < aSize; a++) for (int b = 0; b < bSize; b++) {
            int last = 0;
            for (int c = 0; c <= cSize; c++) {
                int fl = 0;
                if (c != cSize) {
                    int x = cyc == 0 ? a : cyc == 1 ? b : c, y = cyc == 0 ? b : cyc == 1 ? c : a, z = cyc == 0 ? c : cyc == 1 ? a : b;
                    fl = full[IDX(x, y, z)];
                }
                if (!last && fl) {
                    int x = cyc == 0 ? a : cyc == 1 ? b : c, y = cyc == 0 ? b : cyc == 1 ? c : a, z = cyc == 0 ? c : cyc == 1 ? a : b;
                    edge_face(fc, bs, flags, neg, minx + x, miny + y, minz + z);
                }
                if (last && !fl) {
                    int cc = c - 1;
                    int x = cyc == 0 ? a : cyc == 1 ? b : cc, y = cyc == 0 ? b : cyc == 1 ? cc : a, z = cyc == 0 ? cc : cyc == 1 ? a : b;
                    edge_face(fc, bs, flags, pos, minx + x, miny + y, minz + z);
                }
                last = fl;
            }
        }
    }
    #undef IDX
    free(full);
    FcAcc a = { fc }; SGet wg = { &a, fc_acc_get, 1, fc };
    for (int i = 0; i < n; i++) {
        int x = placed[3 * i], y = placed[3 * i + 1], z = placed[3 * i + 2];
        int st = fc_get(fc, x, y, z);
        int ns = update_from_neighbours(bs, &wg, st, x, y, z);
        if (ns != st) fc_set(fc, x, y, z, ns, (flags & ~1) | 16);
    }
}

/* BlockState.updateShape в контексте FEATURES (updateShapeAtEdge деревьев): новое состояние либо −1, если класс не обрабатывается. Свет читается как на стадии FEATURES (fc_sky_light) */
int structure_update_shape_fc(FCtx *fc, int st, int x, int y, int z, int dir, int nst) {
    FcAcc a = { fc }; SGet wg = { &a, fc_acc_get, 0, fc };
    return update_shape(fc->bs, &wg, st, x, y, z, dir, nst);
}

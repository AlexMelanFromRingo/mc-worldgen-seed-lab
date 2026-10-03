/* structures/desert_pyramid.c — DesertPyramidStructure/DesertPyramidPiece (Java-кодированная постройка; образец модуля).
 * Часть — ScatteredFeaturePiece 21×15×21: высота по самой низкой точке MOTION_BLOCKING_NO_LEAVES при первом рисовании, сундуки и «подозрительный» песок. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_DESERT_PYRAMID;
typedef struct DPData {
    int hpos;                     /* heightPosition (−1 — не задан) */
    int chest[4];                 /* hasPlacedChest по Direction.get2DDataValue */
    int *sand; int nsand, csand;  /* potentialSuspiciousSandWorldPositions (x, y, z подряд) */
    int roof[3];                  /* randomCollapsedRoofPos */
} DPData;

static void dp_reset(StPiece *p) {
    DPData *d = p->data; d->hpos = -1; memset(d->chest, 0, sizeof d->chest); d->nsand = 0; d->roof[0] = d->roof[1] = d->roof[2] = 0;
}
static void dp_free(void *v) { DPData *d = v; if (d) { free(d->sand); free(d); } }

static void add_sand(DPData *d, const StPiece *p, int x, int y, int z) {
    if (d->nsand == d->csand) { d->csand = d->csand ? d->csand * 2 : 256; d->sand = xrealloc(d->sand, (size_t)d->csand * 3 * sizeof(int)); }
    d->sand[d->nsand * 3] = sp_wx(p, x, z); d->sand[d->nsand * 3 + 1] = sp_wy(p, y); d->sand[d->nsand * 3 + 2] = sp_wz(p, x, z); d->nsand++;
}
static void sand_box(DPData *d, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) add_sand(d, p, x, y, z);
}

typedef struct Blk { int sandstone, cut, chis, orange, blue, sand, air, slab; int stairs[4]; } Blk;   /* N, S, E, W */

static void dp_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    DPData *d = p->data; RS *rs = c->rs;
    const int W = 21, D = 21;
    Blk b;
    b.sandstone = sp_st(c, "minecraft:sandstone"); b.cut = sp_st(c, "minecraft:cut_sandstone"); b.chis = sp_st(c, "minecraft:chiseled_sandstone");
    b.orange = sp_st(c, "minecraft:orange_terracotta"); b.blue = sp_st(c, "minecraft:blue_terracotta"); b.sand = sp_st(c, "minecraft:sand");
    b.air = sp_air(c); b.slab = sp_st(c, "minecraft:sandstone_slab");
    int stairs = sp_st(c, "minecraft:sandstone_stairs");
    b.stairs[0] = sp_with(c, stairs, "facing", "north"); b.stairs[1] = sp_with(c, stairs, "facing", "south");
    b.stairs[2] = sp_with(c, stairs, "facing", "east"); b.stairs[3] = sp_with(c, stairs, "facing", "west");
    int northStairs = b.stairs[0], southStairs = b.stairs[1], eastStairs = b.stairs[2], westStairs = b.stairs[3];
    int SS = b.sandstone, AIR = b.air, CUT = b.cut, CHIS = b.chis, OR = b.orange;
    (void)rx; (void)ry; (void)rz;
    int off = -rs_bound(rs, 3);
    if (!sp_update_lowest_ground(c, p, &d->hpos, off)) return;
    sp_box(c, p, 0, -4, 0, W - 1, 0, D - 1, SS, SS, 0);
    for (int pos = 1; pos <= 9; pos++) {
        sp_box(c, p, pos, pos, pos, W - 1 - pos, pos, D - 1 - pos, SS, SS, 0);
        sp_box(c, p, pos + 1, pos, pos + 1, W - 2 - pos, pos, D - 2 - pos, AIR, AIR, 0);
    }
    for (int x = 0; x < W; x++) for (int z = 0; z < D; z++) sp_fill_column_down(c, p, SS, x, -5, z);
    sp_box(c, p, 0, 0, 0, 4, 9, 4, SS, AIR, 0);
    sp_box(c, p, 1, 10, 1, 3, 10, 3, SS, SS, 0);
    sp_place(c, p, northStairs, 2, 10, 0); sp_place(c, p, southStairs, 2, 10, 4); sp_place(c, p, eastStairs, 0, 10, 2); sp_place(c, p, westStairs, 4, 10, 2);
    sp_box(c, p, W - 5, 0, 0, W - 1, 9, 4, SS, AIR, 0);
    sp_box(c, p, W - 4, 10, 1, W - 2, 10, 3, SS, SS, 0);
    sp_place(c, p, northStairs, W - 3, 10, 0); sp_place(c, p, southStairs, W - 3, 10, 4); sp_place(c, p, eastStairs, W - 5, 10, 2); sp_place(c, p, westStairs, W - 1, 10, 2);
    sp_box(c, p, 8, 0, 0, 12, 4, 4, SS, AIR, 0);
    sp_box(c, p, 9, 1, 0, 11, 3, 4, AIR, AIR, 0);
    sp_place(c, p, CUT, 9, 1, 1); sp_place(c, p, CUT, 9, 2, 1); sp_place(c, p, CUT, 9, 3, 1); sp_place(c, p, CUT, 10, 3, 1); sp_place(c, p, CUT, 11, 3, 1); sp_place(c, p, CUT, 11, 2, 1); sp_place(c, p, CUT, 11, 1, 1);
    sp_box(c, p, 4, 1, 1, 8, 3, 3, SS, AIR, 0);
    sp_box(c, p, 4, 1, 2, 8, 2, 2, AIR, AIR, 0);
    sp_box(c, p, 12, 1, 1, 16, 3, 3, SS, AIR, 0);
    sp_box(c, p, 12, 1, 2, 16, 2, 2, AIR, AIR, 0);
    sp_box(c, p, 5, 4, 5, W - 6, 4, D - 6, SS, SS, 0);
    sp_box(c, p, 9, 4, 9, 11, 4, 11, AIR, AIR, 0);
    sp_box(c, p, 8, 1, 8, 8, 3, 8, CUT, CUT, 0); sp_box(c, p, 12, 1, 8, 12, 3, 8, CUT, CUT, 0);
    sp_box(c, p, 8, 1, 12, 8, 3, 12, CUT, CUT, 0); sp_box(c, p, 12, 1, 12, 12, 3, 12, CUT, CUT, 0);
    sp_box(c, p, 1, 1, 5, 4, 4, 11, SS, SS, 0);
    sp_box(c, p, W - 5, 1, 5, W - 2, 4, 11, SS, SS, 0);
    sp_box(c, p, 6, 7, 9, 6, 7, 11, SS, SS, 0);
    sp_box(c, p, W - 7, 7, 9, W - 7, 7, 11, SS, SS, 0);
    sp_box(c, p, 5, 5, 9, 5, 7, 11, CUT, CUT, 0);
    sp_box(c, p, W - 6, 5, 9, W - 6, 7, 11, CUT, CUT, 0);
    sp_place(c, p, AIR, 5, 5, 10); sp_place(c, p, AIR, 5, 6, 10); sp_place(c, p, AIR, 6, 6, 10);
    sp_place(c, p, AIR, W - 6, 5, 10); sp_place(c, p, AIR, W - 6, 6, 10); sp_place(c, p, AIR, W - 7, 6, 10);
    sp_box(c, p, 2, 4, 4, 2, 6, 4, AIR, AIR, 0);
    sp_box(c, p, W - 3, 4, 4, W - 3, 6, 4, AIR, AIR, 0);
    sp_place(c, p, northStairs, 2, 4, 5); sp_place(c, p, northStairs, 2, 3, 4); sp_place(c, p, northStairs, W - 3, 4, 5); sp_place(c, p, northStairs, W - 3, 3, 4);
    sp_box(c, p, 1, 1, 3, 2, 2, 3, SS, SS, 0);
    sp_box(c, p, W - 3, 1, 3, W - 2, 2, 3, SS, SS, 0);
    sp_place(c, p, SS, 1, 1, 2); sp_place(c, p, SS, W - 2, 1, 2);
    sp_place(c, p, b.slab, 1, 2, 2); sp_place(c, p, b.slab, W - 2, 2, 2);
    sp_place(c, p, westStairs, 2, 1, 2); sp_place(c, p, eastStairs, W - 3, 1, 2);
    sp_box(c, p, 4, 3, 5, 4, 3, 17, SS, SS, 0);
    sp_box(c, p, W - 5, 3, 5, W - 5, 3, 17, SS, SS, 0);
    sp_box(c, p, 3, 1, 5, 4, 2, 16, AIR, AIR, 0);
    sp_box(c, p, W - 6, 1, 5, W - 5, 2, 16, AIR, AIR, 0);
    for (int z = 5; z <= 17; z += 2) {
        sp_place(c, p, CUT, 4, 1, z); sp_place(c, p, CHIS, 4, 2, z); sp_place(c, p, CUT, W - 5, 1, z); sp_place(c, p, CHIS, W - 5, 2, z);
    }
    static const int OT[12][3] = { {10,0,7},{10,0,8},{9,0,9},{11,0,9},{8,0,10},{12,0,10},{7,0,10},{13,0,10},{9,0,11},{11,0,11},{10,0,12},{10,0,13} };
    for (int i = 0; i < 12; i++) sp_place(c, p, OR, OT[i][0], OT[i][1], OT[i][2]);
    sp_place(c, p, b.blue, 10, 0, 10);
    for (int x = 0; x <= W - 1; x += W - 1) {
        sp_place(c, p, CUT, x, 2, 1); sp_place(c, p, OR, x, 2, 2); sp_place(c, p, CUT, x, 2, 3);
        sp_place(c, p, CUT, x, 3, 1); sp_place(c, p, OR, x, 3, 2); sp_place(c, p, CUT, x, 3, 3);
        sp_place(c, p, OR, x, 4, 1); sp_place(c, p, CHIS, x, 4, 2); sp_place(c, p, OR, x, 4, 3);
        sp_place(c, p, CUT, x, 5, 1); sp_place(c, p, OR, x, 5, 2); sp_place(c, p, CUT, x, 5, 3);
        sp_place(c, p, OR, x, 6, 1); sp_place(c, p, CHIS, x, 6, 2); sp_place(c, p, OR, x, 6, 3);
        sp_place(c, p, OR, x, 7, 1); sp_place(c, p, OR, x, 7, 2); sp_place(c, p, OR, x, 7, 3);
        sp_place(c, p, CUT, x, 8, 1); sp_place(c, p, CUT, x, 8, 2); sp_place(c, p, CUT, x, 8, 3);
    }
    for (int x = 2; x <= W - 3; x += W - 3 - 2) {
        sp_place(c, p, CUT, x - 1, 2, 0); sp_place(c, p, OR, x, 2, 0); sp_place(c, p, CUT, x + 1, 2, 0);
        sp_place(c, p, CUT, x - 1, 3, 0); sp_place(c, p, OR, x, 3, 0); sp_place(c, p, CUT, x + 1, 3, 0);
        sp_place(c, p, OR, x - 1, 4, 0); sp_place(c, p, CHIS, x, 4, 0); sp_place(c, p, OR, x + 1, 4, 0);
        sp_place(c, p, CUT, x - 1, 5, 0); sp_place(c, p, OR, x, 5, 0); sp_place(c, p, CUT, x + 1, 5, 0);
        sp_place(c, p, OR, x - 1, 6, 0); sp_place(c, p, CHIS, x, 6, 0); sp_place(c, p, OR, x + 1, 6, 0);
        sp_place(c, p, OR, x - 1, 7, 0); sp_place(c, p, OR, x, 7, 0); sp_place(c, p, OR, x + 1, 7, 0);
        sp_place(c, p, CUT, x - 1, 8, 0); sp_place(c, p, CUT, x, 8, 0); sp_place(c, p, CUT, x + 1, 8, 0);
    }
    sp_box(c, p, 8, 4, 0, 12, 6, 0, CUT, CUT, 0);
    sp_place(c, p, AIR, 8, 6, 0); sp_place(c, p, AIR, 12, 6, 0);
    sp_place(c, p, OR, 9, 5, 0); sp_place(c, p, CHIS, 10, 5, 0); sp_place(c, p, OR, 11, 5, 0);
    sp_box(c, p, 8, -14, 8, 12, -11, 12, CUT, CUT, 0);
    sp_box(c, p, 8, -10, 8, 12, -10, 12, CHIS, CHIS, 0);
    sp_box(c, p, 8, -9, 8, 12, -9, 12, CUT, CUT, 0);
    sp_box(c, p, 8, -8, 8, 12, -1, 12, SS, SS, 0);
    sp_box(c, p, 9, -11, 9, 11, -1, 11, AIR, AIR, 0);
    sp_place(c, p, sp_st(c, "minecraft:stone_pressure_plate"), 10, -11, 10);
    sp_box(c, p, 9, -13, 9, 11, -13, 11, sp_st(c, "minecraft:tnt"), AIR, 0);
    sp_place(c, p, AIR, 8, -11, 10); sp_place(c, p, AIR, 8, -10, 10); sp_place(c, p, CHIS, 7, -10, 10); sp_place(c, p, CUT, 7, -11, 10);
    sp_place(c, p, AIR, 12, -11, 10); sp_place(c, p, AIR, 12, -10, 10); sp_place(c, p, CHIS, 13, -10, 10); sp_place(c, p, CUT, 13, -11, 10);
    sp_place(c, p, AIR, 10, -11, 8); sp_place(c, p, AIR, 10, -10, 8); sp_place(c, p, CHIS, 10, -10, 7); sp_place(c, p, CUT, 10, -11, 7);
    sp_place(c, p, AIR, 10, -11, 12); sp_place(c, p, AIR, 10, -10, 12); sp_place(c, p, CHIS, 10, -10, 13); sp_place(c, p, CUT, 10, -11, 13);
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };     /* Direction.Plane.HORIZONTAL */
    for (int i = 0; i < 4; i++) {
        int dv = dir_to_2d(HD[i]);
        if (!d->chest[dv]) {
            int xo = DIR_DX[HD[i]] * 2, zo = DIR_DZ[HD[i]] * 2;
            d->chest[dv] = sp_create_chest(c, p, 10 + xo, -11, 10 + zo, -1);
        }
    }
    /* addCellar → addCellarStairs, addCellarRoom */
    int rcx = 16, rcy = -4, rcz = 13;
    int csstairs = sp_st(c, "minecraft:sandstone_stairs");
    int ccw = bsx_rotate(c->sw->bs, csstairs, ROT_CCW90);
    sp_place(c, p, ccw, 13, -1, 17); sp_place(c, p, ccw, 14, -2, 17); sp_place(c, p, ccw, 15, -3, 17);
    Rnd *rr = sp_region_random(c);                                   /* level.getRandom() */
    {
        int variant = (rnd_next_long(rr) & 1) != 0;                  /* XoroshiroRandomSource.nextBoolean */
        int x = rcx, y = rcy, z = rcz;
        sp_place(c, p, b.sand, x - 4, y + 4, z + 4); sp_place(c, p, b.sand, x - 3, y + 4, z + 4); sp_place(c, p, b.sand, x - 2, y + 4, z + 4);
        sp_place(c, p, b.sand, x - 1, y + 4, z + 4); sp_place(c, p, b.sand, x, y + 4, z + 4); sp_place(c, p, b.sand, x - 2, y + 3, z + 4);
        sp_place(c, p, variant ? b.sand : SS, x - 1, y + 3, z + 4); sp_place(c, p, !variant ? b.sand : SS, x, y + 3, z + 4);
        sp_place(c, p, b.sand, x - 1, y + 2, z + 4); sp_place(c, p, SS, x, y + 2, z + 4); sp_place(c, p, b.sand, x, y + 1, z + 4);
    }
    {
        int x = rcx, y = rcy, z = rcz;
        sp_box(c, p, x - 3, y + 1, z - 3, x - 3, y + 1, z + 2, CUT, CUT, 1);
        sp_box(c, p, x + 3, y + 1, z - 3, x + 3, y + 1, z + 2, CUT, CUT, 1);
        sp_box(c, p, x - 3, y + 1, z - 3, x + 3, y + 1, z - 2, CUT, CUT, 1);
        sp_box(c, p, x - 3, y + 1, z + 3, x + 3, y + 1, z + 3, CUT, CUT, 1);
        sp_box(c, p, x - 3, y + 2, z - 3, x - 3, y + 2, z + 2, CHIS, CHIS, 1);
        sp_box(c, p, x + 3, y + 2, z - 3, x + 3, y + 2, z + 2, CHIS, CHIS, 1);
        sp_box(c, p, x - 3, y + 2, z - 3, x + 3, y + 2, z - 2, CHIS, CHIS, 1);
        sp_box(c, p, x - 3, y + 2, z + 3, x + 3, y + 2, z + 3, CHIS, CHIS, 1);
        sp_box(c, p, x - 3, -1, z - 3, x - 3, -1, z + 2, CUT, CUT, 1);
        sp_box(c, p, x + 3, -1, z - 3, x + 3, -1, z + 2, CUT, CUT, 1);
        sp_box(c, p, x - 3, -1, z - 3, x + 3, -1, z - 2, CUT, CUT, 1);
        sp_box(c, p, x - 3, -1, z + 3, x + 3, -1, z + 3, CUT, CUT, 1);
        sand_box(d, p, x - 2, y + 1, z - 2, x + 2, y + 3, z + 2);
        /* placeCollapsedRoof(x - 2, y + 4, z - 2, x + 2, z + 2) */
        int x0 = x - 2, y0 = y + 4, z0 = z - 2, x1 = x + 2, z1 = z + 2;
        for (int xx = x0; xx <= x1; xx++) for (int zz = z0; zz <= z1; zz++) sp_place(c, p, rnd_next_float(rr) < 0.33f ? SS : b.sand, xx, y0, zz);
        RS pr = sp_seed_positional(c, sp_wx(p, x0, z0), sp_wy(p, y0), sp_wz(p, x0, z0));
        int rpx = rs_bound(&pr, x1 - x0 + 1) + x0, rpz = rs_bound(&pr, z1 - z0 + 1) + z0;
        d->roof[0] = sp_wx(p, rpx, rpz); d->roof[1] = sp_wy(p, y0); d->roof[2] = sp_wz(p, rpx, rpz);
        int BLU = b.blue;
        sp_place(c, p, BLU, x, y, z);
        sp_place(c, p, OR, x + 1, y, z - 1); sp_place(c, p, OR, x + 1, y, z + 1); sp_place(c, p, OR, x - 1, y, z - 1); sp_place(c, p, OR, x - 1, y, z + 1);
        sp_place(c, p, OR, x + 2, y, z); sp_place(c, p, OR, x - 2, y, z); sp_place(c, p, OR, x, y, z + 2); sp_place(c, p, OR, x, y, z - 2);
        sp_place(c, p, OR, x + 3, y, z); add_sand(d, p, x + 3, y + 1, z); add_sand(d, p, x + 3, y + 2, z);
        sp_place(c, p, CUT, x + 4, y + 1, z); sp_place(c, p, CHIS, x + 4, y + 2, z);
        sp_place(c, p, OR, x - 3, y, z); add_sand(d, p, x - 3, y + 1, z); add_sand(d, p, x - 3, y + 2, z);
        sp_place(c, p, CUT, x - 4, y + 1, z); sp_place(c, p, CHIS, x - 4, y + 2, z);
        sp_place(c, p, OR, x, y, z + 3); add_sand(d, p, x, y + 1, z + 3); add_sand(d, p, x, y + 2, z + 3);
        sp_place(c, p, OR, x, y, z - 3); add_sand(d, p, x, y + 1, z - 3); add_sand(d, p, x, y + 2, z - 3);
        sp_place(c, p, CUT, x, y + 1, z - 4); sp_place(c, p, CHIS, x, -2, z - 4);
    }
}

const PieceVT PIECE_DESERT_PYRAMID = { "minecraft:tedp", dp_post, NULL, dp_free, NULL, dp_reset, NULL };

/* DesertPyramidStructure.afterPlace */
static int cmp3(const void *a, const void *b) {       /* Vec3i.compareTo: y, затем z, затем x */
    const int *p = a, *q = b;
    if (p[1] != q[1]) return p[1] < q[1] ? -1 : 1;
    if (p[2] != q[2]) return p[2] < q[2] ? -1 : 1;
    return p[0] < q[0] ? -1 : p[0] > q[0];
}
static void place_susp_sand(StCtx *c, int x, int y, int z) { if (sp_inside(c, x, y, z)) sp_set_world(c, x, y, z, sp_st(c, "minecraft:suspicious_sand")); }
static void dp_after(StCtx *c, const StStart *s) {
    int *all = NULL, n = 0, cap = 0;
    for (int i = 0; i < s->n; i++) {
        StPiece *p = s->pieces[i]; if (p->vt != &PIECE_DESERT_PYRAMID) continue;
        DPData *d = p->data;
        for (int k = 0; k < d->nsand; k++) { if (n == cap) { cap = cap ? cap * 2 : 256; all = xrealloc(all, (size_t)cap * 3 * sizeof(int)); } memcpy(&all[n * 3], &d->sand[k * 3], 3 * sizeof(int)); n++; }
        place_susp_sand(c, d->roof[0], d->roof[1], d->roof[2]);
    }
    qsort(all, (size_t)n, 3 * sizeof(int), cmp3);
    int m = 0; for (int i = 0; i < n; i++) if (i == 0 || memcmp(&all[i * 3], &all[(i - 1) * 3], 3 * sizeof(int))) { if (m != i) memcpy(&all[m * 3], &all[i * 3], 3 * sizeof(int)); m++; }   /* SortedArraySet: уникальные */
    int cx = bb_cx(&s->bb), cy = bb_cy(&s->bb), cz = bb_cz(&s->bb);
    RS pr = sp_seed_positional(c, cx, cy, cz);
    for (int i = m; i > 1; i--) { int sw = rs_bound(&pr, i); int t[3]; memcpy(t, &all[(i - 1) * 3], 12); memcpy(&all[(i - 1) * 3], &all[sw * 3], 12); memcpy(&all[sw * 3], t, 12); }   /* Util.shuffle */
    int to_place = rs_bound(&pr, 3) + 5; if (m < to_place) to_place = m;   /* min(size, nextInt(5, 8)): порядок вычислений — rnd после shuffle */
    int placed = 0;
    for (int i = 0; i < m; i++) {
        int x = all[i * 3], y = all[i * 3 + 1], z = all[i * 3 + 2];
        if (to_place > 0) { to_place--; place_susp_sand(c, x, y, z); placed++; }
        else if (sp_inside(c, x, y, z)) sp_set_world(c, x, y, z, sp_st(c, "minecraft:sand"));
    }
    (void)placed; free(all);
}

/* ---------------------------------------------------------------- структура: SinglePieceStructure(DesertPyramidPiece::new, 21, 21) */
static void *dp_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int dp_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    if (!gen_could_exist_on_chunk_center(c)) return 0;
    if (gen_lowest_y(c, c->cx * 16, c->cz * 16, 21, 21) < c->w->sea_level) return 0;
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, HM_WORLD_SURFACE_WG); out->state = NULL;
    return 1;
}
static int dp_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    int dir = HD[rs_bound(&c->rs, 4)];                              /* Direction.Plane.HORIZONTAL.getRandomDirection */
    DPData *d = xcalloc(1, sizeof *d); d->hpos = -1;
    StPiece *p = piece_new(&PIECE_DESERT_PYRAMID, sp_make_bb(c->cx * 16, 64, c->cz * 16, dir, 21, 15, 21), -1, 0, d);
    sp_set_orientation(p, dir);
    pvec_push(out, p);
    return 1;
}
static void dp_free_cfg(void *p) { free(p); }
const StructType STRUCT_DESERT_PYRAMID = { "minecraft:desert_pyramid", dp_parse, dp_find, dp_build, dp_after, dp_free_cfg, NULL };
void structures_register_desert_pyramid(void) { structure_register_type(&STRUCT_DESERT_PYRAMID); piece_register_type(&PIECE_DESERT_PYRAMID); }

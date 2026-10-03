/* structures/nether_fortress.c — NetherFortressStructure / NetherFortressPieces (`minecraft:fortress`, Java-кодированная постройка).
 *
 * Старт: BridgeCrossing (тип части NeBCr — у StartPiece тот же тип, что у перекрёстка мостов) в (minBlockX+2, 64, minBlockZ+2) со случайным
 * горизонтальным направлением; затем «рекурсивная» достройка через очередь pendingChildren (случайный выбор из очереди), выбор частей по весам
 * BRIDGE_PIECE_WEIGHTS / CASTLE_PIECE_WEIGHTS (placeCount/maxPlaceCount, allowInRow, previousPiece), глубина ≤ 30, расстояние ≤ 112 от старта,
 * при неудаче — BridgeEndFiller (свой LCG по selfSeed). В конце moveInsideHeights(48, 70).
 * Рисование: generateBox/placeBlock/fillColumnDown; ограды из адского кирпича — SHAPE_CHECK_BLOCKS (форма уточняется каркасом после генерации).
 * ГСЧ рисования (c->rs) тратят только сундуки (setLootTable → nextLong); спаунер (пустой spawnPotentials) ГСЧ не тратит. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

/* виды частей (порядок произвольный, внутренний) */
enum { K_BS, K_BCR, K_RC, K_SR, K_MT, K_CE, K_SC, K_SCSC, K_SCRT, K_SCLT, K_CCS, K_CTB, K_CSR, K_BEF, K__N };

/* BoundingBox.orientBox: смещения (offX, offY, offZ) и размеры (width, height, depth) каждой части в createPiece */
static const int GEOM[K__N][6] = {
    [K_BS] = { -1, -3, 0, 5, 10, 19 },   [K_BCR] = { -8, -3, 0, 19, 10, 19 }, [K_RC] = { -2, 0, 0, 7, 9, 7 },     [K_SR] = { -2, 0, 0, 7, 11, 7 },
    [K_MT] = { -2, 0, 0, 7, 8, 9 },      [K_CE] = { -5, -3, 0, 13, 14, 13 },  [K_SC] = { -1, 0, 0, 5, 7, 5 },     [K_SCSC] = { -1, 0, 0, 5, 7, 5 },
    [K_SCRT] = { -1, 0, 0, 5, 7, 5 },    [K_SCLT] = { -1, 0, 0, 5, 7, 5 },    [K_CCS] = { -1, -7, 0, 5, 14, 10 }, [K_CTB] = { -3, 0, 0, 9, 7, 9 },
    [K_CSR] = { -5, -3, 0, 13, 14, 13 }, [K_BEF] = { -1, -3, 0, 5, 10, 8 },
};

typedef struct NFP {
    int kind;
    int seed;            /* BridgeEndFiller.selfSeed */
    int chest, chest0;   /* isNeedingChest (поворотные коридоры): текущее / после создания старта */
    int mob;             /* MonsterThrone.hasPlacedSpawner */
} NFP;

extern const PieceVT NF_VT[K__N];

static void nf_reset(StPiece *p) { NFP *d = p->data; d->chest = d->chest0; d->mob = 0; }
static void nf_free(void *v) { free(v); }

/* ---------------------------------------------------------------- генерация старта (StructurePiecesBuilder) */
typedef struct NFW { int kind, weight, max, row, count; } NFW;     /* PieceWeight: класс, вес, maxPlaceCount, allowInRow, placeCount */
typedef struct NFB {
    RS *r; PieceVec *out;
    NFW w[13];                    /* объекты PieceWeight (тождество важно для previousPiece) */
    int bl[6], nb;                /* availableBridgePieces: индексы в w */
    int cl[7], nc;                /* availableCastlePieces */
    int prev;                     /* previousPiece (индекс в w, −1 — null) */
    StPiece **pend; int np, cp;   /* pendingChildren */
    int sx0, sz0;                 /* startPiece.getBoundingBox().minX/minZ */
} NFB;

static BB orient_box(int fx, int fy, int fz, int ox, int oy, int oz, int w, int h, int d, int dir) {
    switch (dir) {
    case DIR_NORTH: return bb_make(fx + ox, fy + oy, fz - d + 1 + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + oz);
    case DIR_WEST: return bb_make(fx - d + 1 + oz, fy + oy, fz + ox, fx + oz, fy + h - 1 + oy, fz + w - 1 + ox);
    case DIR_EAST: return bb_make(fx + oz, fy + oy, fz + ox, fx + d - 1 + oz, fy + h - 1 + oy, fz + w - 1 + ox);
    default: return bb_make(fx + ox, fy + oy, fz + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + d - 1 + oz);   /* SOUTH */
    }
}

/* <Piece>.createPiece: isOkBox (minY > 10) и нет пересечений; конструкторы BridgeEndFiller и поворотных коридоров тратят ГСЧ */
static StPiece *nf_create(NFB *b, int kind, int fx, int fy, int fz, int dir, int depth) {
    const int *g = GEOM[kind];
    BB box = orient_box(fx, fy, fz, g[0], g[1], g[2], g[3], g[4], g[5], dir);
    if (!(box.y0 > 10) || pvec_collision(b->out, &box)) return NULL;
    NFP *d = xcalloc(1, sizeof *d); d->kind = kind;
    if (kind == K_BEF) d->seed = rs_int(b->r);
    else if (kind == K_SCRT || kind == K_SCLT) d->chest = d->chest0 = rs_bound(b->r, 3) == 0;
    StPiece *p = piece_new(&NF_VT[kind], box, -1, depth, d);
    sp_set_orientation(p, dir);
    return p;
}

static int list_total(NFB *b, const int *list, int n) {          /* updatePieceWeight */
    int any = 0, tot = 0;
    for (int i = 0; i < n; i++) { NFW *w = &b->w[list[i]]; if (w->max > 0 && w->count < w->max) any = 1; tot += w->weight; }
    return any ? tot : -1;
}

/* NetherBridgePiece.generatePiece */
static StPiece *nf_generate_piece(NFB *b, int castle, int fx, int fy, int fz, int dir, int depth) {
    int *list = castle ? b->cl : b->bl; int *n = castle ? &b->nc : &b->nb;
    int total = list_total(b, list, *n);
    int doit = total > 0 && depth <= 30;
    for (int att = 0; att < 5 && doit; att++) {
        int sel = rs_bound(b->r, total);
        for (int i = 0; i < *n; i++) {
            int wi = list[i]; NFW *w = &b->w[wi];
            sel -= w->weight;
            if (sel < 0) {
                int can = w->max == 0 || w->count < w->max;                              /* doPlace */
                if (!can || (wi == b->prev && !w->row)) break;
                StPiece *p = nf_create(b, w->kind, fx, fy, fz, dir, depth);
                if (p) {
                    w->count++; b->prev = wi;
                    if (!(w->max == 0 || w->count < w->max)) { memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof(int)); (*n)--; }   /* isValid → remove */
                    return p;
                }
            }
        }
    }
    return nf_create(b, K_BEF, fx, fy, fz, dir, depth);
}

/* NetherBridgePiece.generateAndAddPiece */
static void nf_add(NFB *b, int fx, int fy, int fz, int dir, int depth, int castle) {
    if (abs(fx - b->sx0) <= 112 && abs(fz - b->sz0) <= 112) {
        StPiece *p = nf_generate_piece(b, castle, fx, fy, fz, dir, depth + 1);
        if (p) {
            pvec_push(b->out, p);
            if (b->np == b->cp) { b->cp = b->cp ? b->cp * 2 : 64; b->pend = xrealloc(b->pend, (size_t)b->cp * sizeof *b->pend); }
            b->pend[b->np++] = p;
        }
    } else {
        StPiece *p = nf_create(b, K_BEF, fx, fy, fz, dir, depth);          /* создаётся (тратит ГСЧ), но не добавляется */
        if (p) piece_free(p);
    }
}

static void child_forward(NFB *b, const StPiece *p, int xo, int yo, int castle) {
    const BB *q = &p->bb; int dir = sp_orientation_dir(p);
    switch (dir) {
    case DIR_NORTH: nf_add(b, q->x0 + xo, q->y0 + yo, q->z0 - 1, dir, p->depth, castle); break;
    case DIR_SOUTH: nf_add(b, q->x0 + xo, q->y0 + yo, q->z1 + 1, dir, p->depth, castle); break;
    case DIR_WEST: nf_add(b, q->x0 - 1, q->y0 + yo, q->z0 + xo, dir, p->depth, castle); break;
    case DIR_EAST: nf_add(b, q->x1 + 1, q->y0 + yo, q->z0 + xo, dir, p->depth, castle); break;
    default: break;
    }
}
static void child_left(NFB *b, const StPiece *p, int yo, int zo, int castle) {
    const BB *q = &p->bb;
    switch (sp_orientation_dir(p)) {
    case DIR_NORTH: case DIR_SOUTH: nf_add(b, q->x0 - 1, q->y0 + yo, q->z0 + zo, DIR_WEST, p->depth, castle); break;
    case DIR_WEST: case DIR_EAST: nf_add(b, q->x0 + zo, q->y0 + yo, q->z0 - 1, DIR_NORTH, p->depth, castle); break;
    default: break;
    }
}
static void child_right(NFB *b, const StPiece *p, int yo, int zo, int castle) {
    const BB *q = &p->bb;
    switch (sp_orientation_dir(p)) {
    case DIR_NORTH: case DIR_SOUTH: nf_add(b, q->x1 + 1, q->y0 + yo, q->z0 + zo, DIR_EAST, p->depth, castle); break;
    case DIR_WEST: case DIR_EAST: nf_add(b, q->x0 + zo, q->y0 + yo, q->z1 + 1, DIR_SOUTH, p->depth, castle); break;
    default: break;
    }
}

/* <Piece>.addChildren */
static void nf_add_children(NFB *b, const StPiece *p) {
    const NFP *d = p->data;
    switch (d->kind) {
    case K_BCR: child_forward(b, p, 8, 3, 0); child_left(b, p, 3, 8, 0); child_right(b, p, 3, 8, 0); break;
    case K_BS: child_forward(b, p, 1, 3, 0); break;
    case K_CCS: child_forward(b, p, 1, 0, 1); break;
    case K_CTB: {
        int dir = sp_orientation_dir(p), zo = (dir == DIR_WEST || dir == DIR_NORTH) ? 5 : 1;
        int c1 = rs_bound(b->r, 8) > 0; child_left(b, p, 0, zo, c1);
        int c2 = rs_bound(b->r, 8) > 0; child_right(b, p, 0, zo, c2);
        break;
    }
    case K_CE: child_forward(b, p, 5, 3, 1); break;
    case K_SCSC: child_forward(b, p, 1, 0, 1); child_left(b, p, 0, 1, 1); child_right(b, p, 0, 1, 1); break;
    case K_SCLT: child_left(b, p, 0, 1, 1); break;
    case K_SC: child_forward(b, p, 1, 0, 1); break;
    case K_SCRT: child_right(b, p, 0, 1, 1); break;
    case K_CSR: child_forward(b, p, 5, 3, 1); child_forward(b, p, 5, 11, 1); break;
    case K_RC: child_forward(b, p, 2, 0, 0); child_left(b, p, 0, 2, 0); child_right(b, p, 0, 2, 0); break;
    case K_SR: child_right(b, p, 6, 2, 0); break;
    default: break;                     /* MonsterThrone, BridgeEndFiller: нет детей */
    }
}

/* ---------------------------------------------------------------- рисование */
typedef struct NS { int nb, air, fence, ns, we, nse, nsw, lava, soul, wart, stairs; } NS;
static int fence(StCtx *c, int n, int e, int s, int w) {
    int st = sp_st(c, "minecraft:nether_brick_fence");
    if (n) st = sp_with(c, st, "north", "true");
    if (e) st = sp_with(c, st, "east", "true");
    if (s) st = sp_with(c, st, "south", "true");
    if (w) st = sp_with(c, st, "west", "true");
    return st;
}
static void ns_init(StCtx *c, NS *s) {
    s->nb = sp_st(c, "minecraft:nether_bricks"); s->air = sp_air(c); s->fence = fence(c, 0, 0, 0, 0);
    s->ns = fence(c, 1, 0, 1, 0); s->we = fence(c, 0, 1, 0, 1); s->nse = fence(c, 1, 1, 1, 0); s->nsw = fence(c, 1, 0, 1, 1);
    s->lava = sp_st(c, "minecraft:lava"); s->soul = sp_st(c, "minecraft:soul_sand"); s->wart = sp_st(c, "minecraft:nether_wart");
    s->stairs = sp_st(c, "minecraft:nether_brick_stairs");
}
#define B(x0, y0, z0, x1, y1, z1, st) sp_box(c, p, x0, y0, z0, x1, y1, z1, st, st, 0)
#define P(st, x, y, z) sp_place(c, p, st, x, y, z)
#define COL(x, z) sp_fill_column_down(c, p, S.nb, x, -1, z)
#define NB S.nb
#define AIR S.air

static void post_bcr(StCtx *c, StPiece *p, int rx, int ry, int rz) {        /* BridgeCrossing (и StartPiece) */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    B(7, 3, 0, 11, 4, 18, NB); B(0, 3, 7, 18, 4, 11, NB); B(8, 5, 0, 10, 7, 18, AIR); B(0, 5, 8, 18, 7, 10, AIR);
    B(7, 5, 0, 7, 5, 7, NB); B(7, 5, 11, 7, 5, 18, NB); B(11, 5, 0, 11, 5, 7, NB); B(11, 5, 11, 11, 5, 18, NB);
    B(0, 5, 7, 7, 5, 7, NB); B(11, 5, 7, 18, 5, 7, NB); B(0, 5, 11, 7, 5, 11, NB); B(11, 5, 11, 18, 5, 11, NB);
    B(7, 2, 0, 11, 2, 5, NB); B(7, 2, 13, 11, 2, 18, NB); B(7, 0, 0, 11, 1, 3, NB); B(7, 0, 15, 11, 1, 18, NB);
    for (int x = 7; x <= 11; x++) for (int z = 0; z <= 2; z++) { COL(x, z); COL(x, 18 - z); }
    B(0, 2, 7, 5, 2, 11, NB); B(13, 2, 7, 18, 2, 11, NB); B(0, 0, 7, 3, 1, 11, NB); B(15, 0, 7, 18, 1, 11, NB);
    for (int x = 0; x <= 2; x++) for (int z = 7; z <= 11; z++) { COL(x, z); COL(18 - x, z); }
}

static void post_bef(StCtx *c, StPiece *p, int rx, int ry, int rz) {        /* BridgeEndFiller: свой LCG (SingleThreadedRandomSource(selfSeed)) */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    const NFP *d = p->data;
    RS r; rs_seed_lcg(&r, (i64)d->seed);
    for (int x = 0; x <= 4; x++) for (int y = 3; y <= 4; y++) { int z = rs_bound(&r, 8); B(x, y, 0, x, y, z, NB); }
    int z = rs_bound(&r, 8); B(0, 5, 0, 0, 5, z, NB);
    z = rs_bound(&r, 8); B(4, 5, 0, 4, 5, z, NB);
    for (int x = 0; x <= 4; x++) { int zx = rs_bound(&r, 5); B(x, 2, 0, x, 2, zx, NB); }
    for (int x = 0; x <= 4; x++) for (int y = 0; y <= 1; y++) { int zx = rs_bound(&r, 3); B(x, y, 0, x, y, zx, NB); }
}

static void post_bs(StCtx *c, StPiece *p, int rx, int ry, int rz) {         /* BridgeStraight */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    B(0, 3, 0, 4, 4, 18, NB); B(1, 5, 0, 3, 7, 18, AIR); B(0, 5, 0, 0, 5, 18, NB); B(4, 5, 0, 4, 5, 18, NB);
    B(0, 2, 0, 4, 2, 5, NB); B(0, 2, 13, 4, 2, 18, NB); B(0, 0, 0, 4, 1, 3, NB); B(0, 0, 15, 4, 1, 18, NB);
    for (int x = 0; x <= 4; x++) for (int z = 0; z <= 2; z++) { COL(x, z); COL(x, 18 - z); }
    B(0, 1, 1, 0, 4, 1, S.nse); B(0, 3, 4, 0, 4, 4, S.nse); B(0, 3, 14, 0, 4, 14, S.nse); B(0, 1, 17, 0, 4, 17, S.nse);
    B(4, 1, 1, 4, 4, 1, S.nsw); B(4, 3, 4, 4, 4, 4, S.nsw); B(4, 3, 14, 4, 4, 14, S.nsw); B(4, 1, 17, 4, 4, 17, S.nsw);
}

static void post_ccs(StCtx *c, StPiece *p, int rx, int ry, int rz) {        /* CastleCorridorStairsPiece */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    int stairs = sp_with(c, S.stairs, "facing", "south");
    for (int step = 0; step <= 9; step++) {
        int floor = 7 - step > 1 ? 7 - step : 1;
        int roof = floor + 5 > 14 - step ? floor + 5 : 14 - step; if (roof > 13) roof = 13;
        int z = step;
        B(0, 0, z, 4, floor, z, NB);
        B(1, floor + 1, z, 3, roof - 1, z, AIR);
        if (step <= 6) { P(stairs, 1, floor + 1, z); P(stairs, 2, floor + 1, z); P(stairs, 3, floor + 1, z); }
        B(0, roof, z, 4, roof, z, NB);
        B(0, floor + 1, z, 0, roof - 1, z, NB);
        B(4, floor + 1, z, 4, roof - 1, z, NB);
        if ((step & 1) == 0) { B(0, floor + 2, z, 0, floor + 3, z, S.ns); B(4, floor + 2, z, 4, floor + 3, z, S.ns); }
        for (int x = 0; x <= 4; x++) COL(x, z);
    }
}

static void post_ctb(StCtx *c, StPiece *p, int rx, int ry, int rz) {        /* CastleCorridorTBalconyPiece */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    B(0, 0, 0, 8, 1, 8, NB); B(0, 2, 0, 8, 5, 8, AIR); B(0, 6, 0, 8, 6, 5, NB); B(0, 2, 0, 2, 5, 0, NB); B(6, 2, 0, 8, 5, 0, NB);
    B(1, 3, 0, 1, 4, 0, S.we); B(7, 3, 0, 7, 4, 0, S.we); B(0, 2, 4, 8, 2, 8, NB); B(1, 1, 4, 2, 2, 4, AIR); B(6, 1, 4, 7, 2, 4, AIR);
    B(1, 3, 8, 7, 3, 8, S.we);
    P(fence(c, 0, 1, 1, 0), 0, 3, 8); P(fence(c, 0, 0, 1, 1), 8, 3, 8);
    B(0, 3, 6, 0, 3, 7, S.ns); B(8, 3, 6, 8, 3, 7, S.ns);
    B(0, 3, 4, 0, 5, 5, NB); B(8, 3, 4, 8, 5, 5, NB); B(1, 3, 5, 2, 5, 5, NB); B(6, 3, 5, 7, 5, 5, NB);
    B(1, 4, 5, 1, 5, 5, S.we); B(7, 4, 5, 7, 5, 5, S.we);
    for (int z = 0; z <= 5; z++) for (int x = 0; x <= 8; x++) COL(x, z);
}

/* общая «коробка» замка 13×14×13 (CastleEntrance и CastleStalkRoom: стены, бойницы, зубцы) */
static void castle_shell(StCtx *c, StPiece *p, const NS *s, int stalk) {
    NS S = *s;
    B(0, 3, 0, 12, 4, 12, NB); B(0, 5, 0, 12, 13, 12, AIR); B(0, 5, 0, 1, 12, 12, NB); B(11, 5, 0, 12, 12, 12, NB);
    B(2, 5, 11, 4, 12, 12, NB); B(8, 5, 11, 10, 12, 12, NB); B(5, 9, 11, 7, 12, 12, NB); B(2, 5, 0, 4, 12, 1, NB);
    B(8, 5, 0, 10, 12, 1, NB); B(5, 9, 0, 7, 12, 1, NB); B(2, 11, 2, 10, 12, 10, NB);
    if (!stalk) B(5, 8, 0, 7, 8, 0, S.fence);
    for (int i = 1; i <= 11; i += 2) {
        B(i, 10, 0, i, 11, 0, S.we); B(i, 10, 12, i, 11, 12, S.we); B(0, 10, i, 0, 11, i, S.ns); B(12, 10, i, 12, 11, i, S.ns);
        P(NB, i, 13, 0); P(NB, i, 13, 12); P(NB, 0, 13, i); P(NB, 12, 13, i);
        if (i != 11) { P(S.we, i + 1, 13, 0); P(S.we, i + 1, 13, 12); P(S.ns, 0, 13, i + 1); P(S.ns, 12, 13, i + 1); }
    }
    P(fence(c, 1, 1, 0, 0), 0, 13, 0); P(fence(c, 0, 1, 1, 0), 0, 13, 12); P(fence(c, 0, 0, 1, 1), 12, 13, 12); P(fence(c, 1, 0, 0, 1), 12, 13, 0);
    for (int z = 3; z <= 9; z += 2) { B(1, 7, z, 1, 8, z, S.nsw); B(11, 7, z, 11, 8, z, S.nse); }
}
static void castle_floor(StCtx *c, StPiece *p, const NS *s) {
    NS S = *s;
    B(4, 2, 0, 8, 2, 12, NB); B(0, 2, 4, 12, 2, 8, NB); B(4, 0, 0, 8, 1, 3, NB); B(4, 0, 9, 8, 1, 12, NB); B(0, 0, 4, 3, 1, 8, NB); B(9, 0, 4, 12, 1, 8, NB);
    for (int x = 4; x <= 8; x++) for (int z = 0; z <= 2; z++) { COL(x, z); COL(x, 12 - z); }
    for (int x = 0; x <= 2; x++) for (int z = 4; z <= 8; z++) { COL(x, z); COL(12 - x, z); }
}

static void post_ce(StCtx *c, StPiece *p, int rx, int ry, int rz) {         /* CastleEntrance */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    castle_shell(c, p, &S, 0);
    castle_floor(c, p, &S);
    B(5, 5, 5, 7, 5, 7, NB); B(6, 1, 6, 6, 4, 6, AIR); P(NB, 6, 0, 6); P(S.lava, 6, 5, 6);
}

static void post_csr(StCtx *c, StPiece *p, int rx, int ry, int rz) {        /* CastleStalkRoom */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    castle_shell(c, p, &S, 1);
    int stairs = sp_with(c, S.stairs, "facing", "north");
    for (int i = 0; i <= 6; i++) {
        int z = i + 4;
        for (int x = 5; x <= 7; x++) P(stairs, x, 5 + i, z);
        if (z >= 5 && z <= 8) B(5, 5, z, 7, i + 4, z, NB);
        else if (z >= 9 && z <= 10) B(5, 8, z, 7, i + 4, z, NB);
        if (i >= 1) B(5, 6 + i, z, 7, 9 + i, z, AIR);
    }
    for (int x = 5; x <= 7; x++) P(stairs, x, 12, 11);
    B(5, 6, 7, 5, 7, 7, S.nse); B(7, 6, 7, 7, 7, 7, S.nsw); B(5, 13, 12, 7, 13, 12, AIR);
    B(2, 5, 2, 3, 5, 3, NB); B(2, 5, 9, 3, 5, 10, NB); B(2, 5, 4, 2, 5, 8, NB); B(9, 5, 2, 10, 5, 3, NB); B(9, 5, 9, 10, 5, 10, NB); B(10, 5, 4, 10, 5, 8, NB);
    int east = sp_with(c, stairs, "facing", "east"), west = sp_with(c, stairs, "facing", "west");
    P(west, 4, 5, 2); P(west, 4, 5, 3); P(west, 4, 5, 9); P(west, 4, 5, 10);
    P(east, 8, 5, 2); P(east, 8, 5, 3); P(east, 8, 5, 9); P(east, 8, 5, 10);
    B(3, 4, 4, 4, 4, 8, S.soul); B(8, 4, 4, 9, 4, 8, S.soul); B(3, 5, 4, 4, 5, 8, S.wart); B(8, 5, 4, 9, 5, 8, S.wart);
    castle_floor(c, p, &S);
}

static void small_base(StCtx *c, StPiece *p, const NS *s) {                 /* пол и полость малых коридоров 5×7×5 */
    NS S = *s; B(0, 0, 0, 4, 1, 4, NB); B(0, 2, 0, 4, 5, 4, AIR);
}
static void small_top(StCtx *c, StPiece *p, const NS *s) {                  /* потолок и столбы вниз */
    NS S = *s; B(0, 6, 0, 4, 6, 4, NB);
    for (int x = 0; x <= 4; x++) for (int z = 0; z <= 4; z++) COL(x, z);
}

static void post_scsc(StCtx *c, StPiece *p, int rx, int ry, int rz) {       /* CastleSmallCorridorCrossingPiece */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    small_base(c, p, &S);
    B(0, 2, 0, 0, 5, 0, NB); B(4, 2, 0, 4, 5, 0, NB); B(0, 2, 4, 0, 5, 4, NB); B(4, 2, 4, 4, 5, 4, NB);
    small_top(c, p, &S);
}

static void post_sc(StCtx *c, StPiece *p, int rx, int ry, int rz) {         /* CastleSmallCorridorPiece */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    small_base(c, p, &S);
    B(0, 2, 0, 0, 5, 4, NB); B(4, 2, 0, 4, 5, 4, NB);
    B(0, 3, 1, 0, 4, 1, S.ns); B(0, 3, 3, 0, 4, 3, S.ns); B(4, 3, 1, 4, 4, 1, S.ns); B(4, 3, 3, 4, 4, 3, S.ns);
    small_top(c, p, &S);
}

static void post_sclt(StCtx *c, StPiece *p, int rx, int ry, int rz) {       /* CastleSmallCorridorLeftTurnPiece */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    NFP *d = p->data;
    small_base(c, p, &S);
    B(4, 2, 0, 4, 5, 4, NB); B(4, 3, 1, 4, 4, 1, S.ns); B(4, 3, 3, 4, 4, 3, S.ns);
    B(0, 2, 0, 0, 5, 0, NB); B(0, 2, 4, 3, 5, 4, NB); B(1, 3, 4, 1, 4, 4, S.we); B(3, 3, 4, 3, 4, 4, S.we);
    if (d->chest && sp_inside(c, sp_wx(p, 3, 3), sp_wy(p, 2), sp_wz(p, 3, 3))) { d->chest = 0; sp_create_chest(c, p, 3, 2, 3, -1); }
    small_top(c, p, &S);
}

static void post_scrt(StCtx *c, StPiece *p, int rx, int ry, int rz) {       /* CastleSmallCorridorRightTurnPiece */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    NFP *d = p->data;
    small_base(c, p, &S);
    B(0, 2, 0, 0, 5, 4, NB); B(0, 3, 1, 0, 4, 1, S.ns); B(0, 3, 3, 0, 4, 3, S.ns);
    B(4, 2, 0, 4, 5, 0, NB); B(1, 2, 4, 4, 5, 4, NB); B(1, 3, 4, 1, 4, 4, S.we); B(3, 3, 4, 3, 4, 4, S.we);
    if (d->chest && sp_inside(c, sp_wx(p, 1, 3), sp_wy(p, 2), sp_wz(p, 1, 3))) { d->chest = 0; sp_create_chest(c, p, 1, 2, 3, -1); }
    small_top(c, p, &S);
}

static void post_mt(StCtx *c, StPiece *p, int rx, int ry, int rz) {         /* MonsterThrone */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    NFP *d = p->data;
    B(0, 2, 0, 6, 7, 7, AIR); B(1, 0, 0, 5, 1, 7, NB); B(1, 2, 1, 5, 2, 7, NB); B(1, 3, 2, 5, 3, 7, NB); B(1, 4, 3, 5, 4, 7, NB);
    B(1, 2, 0, 1, 4, 2, NB); B(5, 2, 0, 5, 4, 2, NB); B(1, 5, 2, 1, 5, 3, NB); B(5, 5, 2, 5, 5, 3, NB);
    B(0, 5, 3, 0, 5, 8, NB); B(6, 5, 3, 6, 5, 8, NB); B(1, 5, 8, 5, 5, 8, NB);
    P(fence(c, 0, 0, 0, 1), 1, 6, 3); P(fence(c, 0, 1, 0, 0), 5, 6, 3);
    P(fence(c, 1, 1, 0, 0), 0, 6, 3); P(fence(c, 1, 0, 0, 1), 6, 6, 3);
    B(0, 6, 4, 0, 6, 7, S.ns); B(6, 6, 4, 6, 6, 7, S.ns);
    P(fence(c, 0, 1, 1, 0), 0, 6, 8); P(fence(c, 0, 0, 1, 1), 6, 6, 8);
    B(1, 6, 8, 5, 6, 8, S.we);
    P(fence(c, 0, 1, 0, 0), 1, 7, 8); B(2, 7, 8, 4, 7, 8, S.we); P(fence(c, 0, 0, 0, 1), 5, 7, 8);
    P(fence(c, 0, 1, 0, 0), 2, 8, 8); P(S.we, 3, 8, 8); P(fence(c, 0, 0, 0, 1), 4, 8, 8);
    if (!d->mob) {
        int wx = sp_wx(p, 3, 5), wy = sp_wy(p, 5), wz = sp_wz(p, 3, 5);
        if (sp_inside(c, wx, wy, wz)) { d->mob = 1; sp_set_world(c, wx, wy, wz, sp_st(c, "minecraft:spawner")); }   /* setEntityId: ГСЧ не тратит */
    }
    for (int x = 0; x <= 6; x++) for (int z = 0; z <= 6; z++) COL(x, z);
}

static void post_rc(StCtx *c, StPiece *p, int rx, int ry, int rz) {         /* RoomCrossing */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    B(0, 0, 0, 6, 1, 6, NB); B(0, 2, 0, 6, 7, 6, AIR); B(0, 2, 0, 1, 6, 0, NB); B(0, 2, 6, 1, 6, 6, NB); B(5, 2, 0, 6, 6, 0, NB); B(5, 2, 6, 6, 6, 6, NB);
    B(0, 2, 0, 0, 6, 1, NB); B(0, 2, 5, 0, 6, 6, NB); B(6, 2, 0, 6, 6, 1, NB); B(6, 2, 5, 6, 6, 6, NB);
    B(2, 6, 0, 4, 6, 0, NB); B(2, 5, 0, 4, 5, 0, S.we); B(2, 6, 6, 4, 6, 6, NB); B(2, 5, 6, 4, 5, 6, S.we);
    B(0, 6, 2, 0, 6, 4, NB); B(0, 5, 2, 0, 5, 4, S.ns); B(6, 6, 2, 6, 6, 4, NB); B(6, 5, 2, 6, 5, 4, S.ns);
    for (int x = 0; x <= 6; x++) for (int z = 0; z <= 6; z++) COL(x, z);
}

static void post_sr(StCtx *c, StPiece *p, int rx, int ry, int rz) {         /* StairsRoom */
    (void)rx; (void)ry; (void)rz; NS S; ns_init(c, &S);
    B(0, 0, 0, 6, 1, 6, NB); B(0, 2, 0, 6, 10, 6, AIR); B(0, 2, 0, 1, 8, 0, NB); B(5, 2, 0, 6, 8, 0, NB); B(0, 2, 1, 0, 8, 6, NB);
    B(6, 2, 1, 6, 8, 6, NB); B(1, 2, 6, 5, 8, 6, NB);
    B(0, 3, 2, 0, 5, 4, S.ns); B(6, 3, 2, 6, 5, 2, S.ns); B(6, 3, 4, 6, 5, 4, S.ns);
    P(NB, 5, 2, 5); B(4, 2, 5, 4, 3, 5, NB); B(3, 2, 5, 3, 4, 5, NB); B(2, 2, 5, 2, 5, 5, NB); B(1, 2, 5, 1, 6, 5, NB);
    B(1, 7, 1, 5, 7, 4, NB); B(6, 8, 2, 6, 8, 4, AIR); B(2, 6, 0, 4, 8, 0, NB); B(2, 5, 0, 4, 5, 0, S.we);
    for (int x = 0; x <= 6; x++) for (int z = 0; z <= 6; z++) COL(x, z);
}
#undef B
#undef P
#undef COL
#undef NB
#undef AIR

const PieceVT NF_VT[K__N] = {
    [K_BS] = { "minecraft:nebs", post_bs, NULL, nf_free, NULL, nf_reset, NULL },
    [K_BCR] = { "minecraft:nebcr", post_bcr, NULL, nf_free, NULL, nf_reset, NULL },
    [K_RC] = { "minecraft:nerc", post_rc, NULL, nf_free, NULL, nf_reset, NULL },
    [K_SR] = { "minecraft:nesr", post_sr, NULL, nf_free, NULL, nf_reset, NULL },
    [K_MT] = { "minecraft:nemt", post_mt, NULL, nf_free, NULL, nf_reset, NULL },
    [K_CE] = { "minecraft:nece", post_ce, NULL, nf_free, NULL, nf_reset, NULL },
    [K_SC] = { "minecraft:nesc", post_sc, NULL, nf_free, NULL, nf_reset, NULL },
    [K_SCSC] = { "minecraft:nescsc", post_scsc, NULL, nf_free, NULL, nf_reset, NULL },
    [K_SCRT] = { "minecraft:nescrt", post_scrt, NULL, nf_free, NULL, nf_reset, NULL },
    [K_SCLT] = { "minecraft:nesclt", post_sclt, NULL, nf_free, NULL, nf_reset, NULL },
    [K_CCS] = { "minecraft:neccs", post_ccs, NULL, nf_free, NULL, nf_reset, NULL },
    [K_CTB] = { "minecraft:nectb", post_ctb, NULL, nf_free, NULL, nf_reset, NULL },
    [K_CSR] = { "minecraft:necsr", post_csr, NULL, nf_free, NULL, nf_reset, NULL },
    [K_BEF] = { "minecraft:nebef", post_bef, NULL, nf_free, NULL, nf_reset, NULL },
};

/* ---------------------------------------------------------------- структура */
static void *nf_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int nf_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    out->x = c->cx * 16; out->y = 64; out->z = c->cz * 16; out->state = NULL;      /* (minBlockX, 64, minBlockZ) */
    return 1;
}
static int nf_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    static const NFW BW[6] = { { K_BS, 30, 0, 1, 0 }, { K_BCR, 10, 4, 0, 0 }, { K_RC, 10, 4, 0, 0 }, { K_SR, 10, 3, 0, 0 }, { K_MT, 5, 2, 0, 0 }, { K_CE, 5, 1, 0, 0 } };
    static const NFW CW[7] = { { K_SC, 25, 0, 1, 0 }, { K_SCSC, 15, 5, 0, 0 }, { K_SCRT, 5, 10, 0, 0 }, { K_SCLT, 5, 10, 0, 0 }, { K_CCS, 10, 3, 1, 0 },
                               { K_CTB, 7, 2, 0, 0 }, { K_CSR, 5, 2, 0, 0 } };
    NFB b; memset(&b, 0, sizeof b);
    b.r = &c->rs; b.out = out; b.prev = -1;
    for (int i = 0; i < 6; i++) { b.w[i] = BW[i]; b.bl[i] = i; } b.nb = 6;
    for (int i = 0; i < 7; i++) { b.w[6 + i] = CW[i]; b.cl[i] = 6 + i; } b.nc = 7;
    /* StartPiece(random, getBlockX(2), getBlockZ(2)): направление — Direction.Plane.HORIZONTAL.getRandomDirection */
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    int dir = HD[rs_bound(&c->rs, 4)];
    NFP *d = xcalloc(1, sizeof *d); d->kind = K_BCR;
    StPiece *start = piece_new(&NF_VT[K_BCR], sp_make_bb(c->cx * 16 + 2, 64, c->cz * 16 + 2, dir, 19, 10, 19), -1, 0, d);
    sp_set_orientation(start, dir);
    pvec_push(out, start);
    b.sx0 = start->bb.x0; b.sz0 = start->bb.z0;
    nf_add_children(&b, start);
    while (b.np > 0) {
        int pos = rs_bound(&c->rs, b.np);
        StPiece *p = b.pend[pos];
        memmove(&b.pend[pos], &b.pend[pos + 1], (size_t)(b.np - pos - 1) * sizeof *b.pend); b.np--;
        nf_add_children(&b, p);
    }
    free(b.pend);
    pvec_move_inside_heights(out, &c->rs, 48, 70);
    return 1;
}
static void nf_free_cfg(void *p) { free(p); }
const StructType STRUCT_NETHER_FORTRESS = { "minecraft:fortress", nf_parse, nf_find, nf_build, NULL, nf_free_cfg, NULL };

void structures_register_nether_fortress(void) {
    structure_register_type(&STRUCT_NETHER_FORTRESS);
    for (int i = 0; i < K__N; i++) piece_register_type(&NF_VT[i]);
}

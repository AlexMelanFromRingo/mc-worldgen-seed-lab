/* structures/stronghold.c — StrongholdStructure/StrongholdPieces (`minecraft:stronghold`, Java-кодированная постройка).
 *
 * Старт: позиция — кольца concentric_rings (structure_place.c), точка проверки биома — угол чанка (y = 0).
 * build: цикл «пока нет портальной комнаты»: перезасев ГСЧ setLargeFeatureSeed(seed + попытка, cx, cz), сброс таблицы весов
 * частей, стартовая лестница (SHStart, источник: следующая часть — FiveCrossing), обход отложенных частей в случайном порядке,
 * moveBelowSeaLevel(…, 10). Части: коридоры, повороты, лестницы, перекрёстки, тюрьма, библиотека, комната с порталом и
 * заполнители (SHFC). Каменные кирпичи стен — селектор «обычные/треснувшие/замшелые/с чешуйницей» (SmoothStoneSelector).
 * Состояние таблицы весов (в игре статическое) — в контексте построения (потокобезопасно). */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- виды частей */
enum { K_START, K_STRAIGHT, K_PRISON, K_LEFT, K_RIGHT, K_ROOMX, K_SSD, K_SD, K_FIVEX, K_CHEST, K_LIB, K_PORTAL, K_FILLER, K__N };
enum { D_OPENING, D_WOOD, D_GRATES, D_IRON };          /* SmallDoorType */
static const char *DOOR_NAME[4] = { "OPENING", "WOOD_DOOR", "GRATES", "IRON_DOOR" };

typedef struct SHData {
    int kind, door;
    int f[4];          /* Straight: left, right; FiveCrossing: leftLow, leftHigh, rightLow, rightHigh; RoomCrossing: type; Library: tall; StairsDown: source */
    int steps;         /* FillerCorridor */
    int placed;        /* ChestCorridor.hasPlacedChest / PortalRoom.hasPlacedSpawner (изменяемое) */
} SHData;

extern const PieceVT PIECE_SH[K__N];

/* таблица весов: STRONGHOLD_PIECE_WEIGHTS (вид, вес, максимум; 0 — без ограничения) */
#define NW 11
static const int W_KIND[NW] = { K_STRAIGHT, K_PRISON, K_LEFT, K_RIGHT, K_ROOMX, K_SSD, K_SD, K_FIVEX, K_CHEST, K_LIB, K_PORTAL };
static const int W_WEIGHT[NW] = { 40, 5, 20, 20, 10, 5, 5, 5, 5, 10, 20 };
static const int W_MAX[NW] = { 0, 5, 0, 0, 6, 5, 5, 4, 4, 2, 1 };

/* ---------------------------------------------------------------- построение */
typedef struct SHB {
    GenCtx *c; PieceVec *pv;
    int place_count[NW];
    int cur[NW], ncur;           /* currentPieces (индексы таблицы весов, по порядку) */
    int total_weight;
    int imposed;                 /* imposedPiece: вид или −1 */
    int prev;                    /* startPiece.previousPiece: индекс таблицы или −1 */
    StPiece *start, *portal;
    StPiece **pend; int npend, cpend;   /* startPiece.pendingChildren */
} SHB;

static BB orient_box(int fx, int fy, int fz, int ox, int oy, int oz, int w, int h, int d, int dir) {   /* BoundingBox.orientBox */
    switch (dir) {
    case DIR_NORTH: return bb_make(fx + ox, fy + oy, fz - d + 1 + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + oz);
    case DIR_WEST: return bb_make(fx - d + 1 + oz, fy + oy, fz + ox, fx + oz, fy + h - 1 + oy, fz + w - 1 + ox);
    case DIR_EAST: return bb_make(fx + oz, fy + oy, fz + ox, fx + d - 1 + oz, fy + h - 1 + oy, fz + w - 1 + ox);
    default: return bb_make(fx + ox, fy + oy, fz + oz, fx + w - 1 + ox, fy + h - 1 + oy, fz + d - 1 + oz);
    }
}
static int ok_box(const BB *b) { return b->y0 > 10; }      /* StrongholdPiece.isOkBox */

static int random_small_door(RS *r) {
    switch (rs_bound(r, 5)) { case 2: return D_WOOD; case 3: return D_GRATES; case 4: return D_IRON; default: return D_OPENING; }
}

static StPiece *new_piece(int kind, int depth, BB bb, int dir, int door) {
    SHData *d = xcalloc(1, sizeof *d); d->kind = kind; d->door = door;
    StPiece *p = piece_new(&PIECE_SH[kind], bb, -1, depth, d);
    sp_set_orientation(p, dir);
    return p;
}
static SHData *shd(const StPiece *p) { return p->data; }

/* findAndCreatePieceFactory: создание части вида kind у «ног» (fx, fy, fz) в направлении dir */
static StPiece *create_piece(SHB *b, int kind, int fx, int fy, int fz, int dir, int depth) {
    RS *r = &b->c->rs; BB box; StPiece *p;
    switch (kind) {
    case K_STRAIGHT:
        box = orient_box(fx, fy, fz, -1, -1, 0, 5, 5, 7, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        p = new_piece(kind, depth, box, dir, random_small_door(r));
        shd(p)->f[0] = rs_bound(r, 2) == 0; shd(p)->f[1] = rs_bound(r, 2) == 0;
        return p;
    case K_PRISON:
        box = orient_box(fx, fy, fz, -1, -1, 0, 9, 5, 11, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        return new_piece(kind, depth, box, dir, random_small_door(r));
    case K_LEFT: case K_RIGHT:
        box = orient_box(fx, fy, fz, -1, -1, 0, 5, 5, 5, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        return new_piece(kind, depth, box, dir, random_small_door(r));
    case K_ROOMX:
        box = orient_box(fx, fy, fz, -4, -1, 0, 11, 7, 11, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        p = new_piece(kind, depth, box, dir, random_small_door(r));
        shd(p)->f[0] = rs_bound(r, 5);
        return p;
    case K_SSD:
        box = orient_box(fx, fy, fz, -1, -7, 0, 5, 11, 8, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        return new_piece(kind, depth, box, dir, random_small_door(r));
    case K_SD:
        box = orient_box(fx, fy, fz, -1, -7, 0, 5, 11, 5, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        return new_piece(kind, depth, box, dir, random_small_door(r));      /* isSource = false */
    case K_FIVEX:
        box = orient_box(fx, fy, fz, -4, -3, 0, 10, 9, 11, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        p = new_piece(kind, depth, box, dir, random_small_door(r));
        shd(p)->f[0] = rs_bool(r); shd(p)->f[1] = rs_bool(r); shd(p)->f[2] = rs_bool(r); shd(p)->f[3] = rs_bound(r, 3) > 0;
        return p;
    case K_CHEST:
        box = orient_box(fx, fy, fz, -1, -1, 0, 5, 5, 7, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        return new_piece(kind, depth, box, dir, random_small_door(r));
    case K_LIB:
        box = orient_box(fx, fy, fz, -4, -1, 0, 14, 11, 15, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) {
            box = orient_box(fx, fy, fz, -4, -1, 0, 14, 6, 15, dir);
            if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        }
        p = new_piece(kind, depth, box, dir, random_small_door(r));
        shd(p)->f[0] = bb_yspan(&box) > 6;
        return p;
    case K_PORTAL:
        box = orient_box(fx, fy, fz, -4, -1, 0, 11, 8, 16, dir);
        if (!ok_box(&box) || pvec_collision(b->pv, &box)) return NULL;
        return new_piece(kind, depth, box, dir, D_OPENING);
    }
    return NULL;
}

/* FillerCorridor.findPieceBox */
static int filler_box(SHB *b, int fx, int fy, int fz, int dir, BB *out) {
    BB box = orient_box(fx, fy, fz, -1, -1, 0, 5, 5, 4, dir);
    StPiece *col = pvec_collision(b->pv, &box);
    if (!col) return 0;
    if (col->bb.y0 == box.y0) {
        for (int depth = 2; depth >= 1; depth--) {
            box = orient_box(fx, fy, fz, -1, -1, 0, 5, 5, depth, dir);
            if (!bb_intersects(&col->bb, &box)) { *out = orient_box(fx, fy, fz, -1, -1, 0, 5, 5, depth + 1, dir); return 1; }
        }
    }
    return 0;
}

static int update_piece_weight(SHB *b) {
    int any = 0; b->total_weight = 0;
    for (int i = 0; i < b->ncur; i++) {
        int k = b->cur[i];
        if (W_MAX[k] > 0 && b->place_count[k] < W_MAX[k]) any = 1;
        b->total_weight += W_WEIGHT[k];
    }
    return any;
}
static int w_do_place(SHB *b, int k, int depth) {
    int ok = W_MAX[k] == 0 || b->place_count[k] < W_MAX[k];
    if (W_KIND[k] == K_LIB) return ok && depth > 4;
    if (W_KIND[k] == K_PORTAL) return ok && depth > 5;
    return ok;
}
static int w_is_valid(SHB *b, int k) { return W_MAX[k] == 0 || b->place_count[k] < W_MAX[k]; }

/* generatePieceFromSmallDoor */
static StPiece *piece_from_small_door(SHB *b, int fx, int fy, int fz, int dir, int depth) {
    if (!update_piece_weight(b)) return NULL;
    if (b->imposed >= 0) {
        StPiece *p = create_piece(b, b->imposed, fx, fy, fz, dir, depth);
        b->imposed = -1;
        if (p) return p;
    }
    for (int attempt = 0; attempt < 5; attempt++) {
        int sel = rs_bound(&b->c->rs, b->total_weight);
        for (int i = 0; i < b->ncur; i++) {
            int k = b->cur[i];
            sel -= W_WEIGHT[k];
            if (sel < 0) {
                if (!w_do_place(b, k, depth) || k == b->prev) break;
                StPiece *p = create_piece(b, W_KIND[k], fx, fy, fz, dir, depth);
                if (p) {
                    b->place_count[k]++;
                    b->prev = k;
                    if (!w_is_valid(b, k)) { memmove(&b->cur[i], &b->cur[i + 1], (size_t)(b->ncur - i - 1) * sizeof(int)); b->ncur--; }
                    return p;
                }
            }
        }
    }
    BB box;
    if (filler_box(b, fx, fy, fz, dir, &box) && box.y0 > 1) {
        StPiece *p = new_piece(K_FILLER, depth, box, dir, D_OPENING);
        shd(p)->steps = (dir != DIR_NORTH && dir != DIR_SOUTH) ? bb_xspan(&box) : bb_zspan(&box);
        return p;
    }
    return NULL;
}

/* generateAndAddPiece */
static StPiece *gen_and_add(SHB *b, int fx, int fy, int fz, int dir, int depth) {
    if (depth > 50) return NULL;
    if (abs(fx - b->start->bb.x0) > 112 || abs(fz - b->start->bb.z0) > 112) return NULL;
    StPiece *p = piece_from_small_door(b, fx, fy, fz, dir, depth + 1);
    if (p) {
        pvec_push(b->pv, p);
        if (b->npend == b->cpend) { b->cpend = b->cpend ? b->cpend * 2 : 32; b->pend = xrealloc(b->pend, (size_t)b->cpend * sizeof *b->pend); }
        b->pend[b->npend++] = p;
    }
    return p;
}

static void child_forward(SHB *b, StPiece *p, int xo, int yo) {
    int dir = sp_orientation_dir(p); const BB *bb = &p->bb;
    switch (dir) {
    case DIR_NORTH: gen_and_add(b, bb->x0 + xo, bb->y0 + yo, bb->z0 - 1, dir, p->depth); break;
    case DIR_SOUTH: gen_and_add(b, bb->x0 + xo, bb->y0 + yo, bb->z1 + 1, dir, p->depth); break;
    case DIR_WEST: gen_and_add(b, bb->x0 - 1, bb->y0 + yo, bb->z0 + xo, dir, p->depth); break;
    case DIR_EAST: gen_and_add(b, bb->x1 + 1, bb->y0 + yo, bb->z0 + xo, dir, p->depth); break;
    }
}
static void child_left(SHB *b, StPiece *p, int yo, int zo) {
    int dir = sp_orientation_dir(p); const BB *bb = &p->bb;
    switch (dir) {
    case DIR_NORTH: case DIR_SOUTH: gen_and_add(b, bb->x0 - 1, bb->y0 + yo, bb->z0 + zo, DIR_WEST, p->depth); break;
    case DIR_WEST: case DIR_EAST: gen_and_add(b, bb->x0 + zo, bb->y0 + yo, bb->z0 - 1, DIR_NORTH, p->depth); break;
    }
}
static void child_right(SHB *b, StPiece *p, int yo, int zo) {
    int dir = sp_orientation_dir(p); const BB *bb = &p->bb;
    switch (dir) {
    case DIR_NORTH: case DIR_SOUTH: gen_and_add(b, bb->x1 + 1, bb->y0 + yo, bb->z0 + zo, DIR_EAST, p->depth); break;
    case DIR_WEST: case DIR_EAST: gen_and_add(b, bb->x0 + zo, bb->y0 + yo, bb->z1 + 1, DIR_SOUTH, p->depth); break;
    }
}

/* StructurePiece.addChildren по видам */
static void add_children(SHB *b, StPiece *p) {
    SHData *d = shd(p); int o = sp_orientation_dir(p);
    switch (d->kind) {
    case K_START: case K_SD:
        if (d->f[0]) b->imposed = K_FIVEX;
        child_forward(b, p, 1, 1); break;
    case K_CHEST: case K_PRISON: case K_SSD: child_forward(b, p, 1, 1); break;
    case K_STRAIGHT:
        child_forward(b, p, 1, 1);
        if (d->f[0]) child_left(b, p, 1, 2);
        if (d->f[1]) child_right(b, p, 1, 2);
        break;
    case K_FIVEX: {
        int za = 3, zb = 5;
        if (o == DIR_WEST || o == DIR_NORTH) { za = 8 - za; zb = 8 - zb; }
        child_forward(b, p, 5, 1);
        if (d->f[0]) child_left(b, p, za, 1);
        if (d->f[1]) child_left(b, p, zb, 7);
        if (d->f[2]) child_right(b, p, za, 1);
        if (d->f[3]) child_right(b, p, zb, 7);
        break;
    }
    case K_LEFT:
        if (o != DIR_NORTH && o != DIR_EAST) child_right(b, p, 1, 1); else child_left(b, p, 1, 1);
        break;
    case K_RIGHT:
        if (o != DIR_NORTH && o != DIR_EAST) child_left(b, p, 1, 1); else child_right(b, p, 1, 1);
        break;
    case K_ROOMX:
        child_forward(b, p, 4, 1); child_left(b, p, 1, 4); child_right(b, p, 1, 4); break;
    case K_PORTAL: b->portal = p; break;
    default: break;          /* Library, FillerCorridor: детей нет */
    }
}

static void pv_clear(PieceVec *pv) { for (int i = 0; i < pv->n; i++) piece_free(pv->v[i]); pv->n = 0; }

static int sh_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };   /* Direction.Plane.HORIZONTAL */
    SHB b; memset(&b, 0, sizeof b); b.c = c; b.pv = out;
    for (int tries = 0; tries < 100000; tries++) {
        pv_clear(out);                                                         /* builder.clear() */
        rs_large_feature_seed(&c->rs, c->seed + tries, c->cx, c->cz);
        /* resetPieces */
        b.ncur = NW; for (int i = 0; i < NW; i++) { b.cur[i] = i; b.place_count[i] = 0; }
        b.imposed = -1; b.prev = -1; b.portal = NULL; b.npend = 0;
        int dir = HD[rs_bound(&c->rs, 4)];
        StPiece *st = new_piece(K_START, 0, sp_make_bb(c->cx * 16 + 2, 64, c->cz * 16 + 2, dir, 5, 11, 5), dir, D_OPENING);
        shd(st)->f[0] = 1;                                                     /* isSource */
        b.start = st;
        pvec_push(out, st);
        add_children(&b, st);
        while (b.npend > 0) {
            int pos = rs_bound(&c->rs, b.npend);
            StPiece *p = b.pend[pos];
            memmove(&b.pend[pos], &b.pend[pos + 1], (size_t)(b.npend - pos - 1) * sizeof *b.pend); b.npend--;
            add_children(&b, p);
        }
        pvec_move_below_sea_level(out, c->w->ns->sea_level, c->w->ns->min_y, &c->rs, 10);
        if (out->n > 0 && b.portal) { free(b.pend); return 1; }
    }
    free(b.pend);
    pv_clear(out);
    return 0;
}

/* ---------------------------------------------------------------- состояния блоков (кэш на поток по McGen) */
typedef struct SHSt {
    const McGen *g; int nstates;
    int sb, cracked, mossy, infested, cave_air, sb_slab, ss_slab, ss_double;
    int wtorch[4];       /* N, S, E, W */
    int torch, planks, bookshelf, cobweb, cobble, lava, water;
    int f_we, f_ns, f_ne, f_se, f_nw, f_sw, f_e, f_w, f_nswe;
    int ladder_s, ladder_w;
    int bars_ns, bars_nse, bars_we, bars_w, bars_e;
    int idoor_w, idoor_w_up, idoor, idoor_up, odoor, odoor_up;
    int btn_n, btn_s;
    int sbstairs_n, cstairs_s;
    int frame[4][2];     /* N, S, E, W × eye */
    int portal, spawner;
} SHSt;

static int stp(StCtx *c, const char *base, ...) {
    int s = sp_st(c, base);
    va_list ap; va_start(ap, base);
    for (;;) { const char *k = va_arg(ap, const char *); if (!k) break; const char *v = va_arg(ap, const char *); s = sp_with(c, s, k, v); }
    va_end(ap);
    return s;
}
#define T "true"
static const SHSt *sh_states(StCtx *c) {
    static _Thread_local SHSt S;
    const McGen *g = c->w->g;
    if (S.g == g && S.nstates == g->nstates) return &S;
    S.sb = sp_st(c, "minecraft:stone_bricks"); S.cracked = sp_st(c, "minecraft:cracked_stone_bricks"); S.mossy = sp_st(c, "minecraft:mossy_stone_bricks");
    S.infested = sp_st(c, "minecraft:infested_stone_bricks"); S.cave_air = sp_st(c, "minecraft:cave_air");
    S.sb_slab = sp_st(c, "minecraft:stone_brick_slab"); S.ss_slab = sp_st(c, "minecraft:smooth_stone_slab"); S.ss_double = stp(c, "minecraft:smooth_stone_slab", "type", "double", NULL);
    static const char *DN[4] = { "north", "south", "east", "west" };
    for (int i = 0; i < 4; i++) {
        S.wtorch[i] = stp(c, "minecraft:wall_torch", "facing", DN[i], NULL);
        S.frame[i][0] = stp(c, "minecraft:end_portal_frame", "facing", DN[i], "eye", "false", NULL);
        S.frame[i][1] = stp(c, "minecraft:end_portal_frame", "facing", DN[i], "eye", "true", NULL);
    }
    S.torch = sp_st(c, "minecraft:torch"); S.planks = sp_st(c, "minecraft:oak_planks"); S.bookshelf = sp_st(c, "minecraft:bookshelf");
    S.cobweb = sp_st(c, "minecraft:cobweb"); S.cobble = sp_st(c, "minecraft:cobblestone"); S.lava = sp_st(c, "minecraft:lava"); S.water = sp_st(c, "minecraft:water");
    const char *F = "minecraft:oak_fence";
    S.f_we = stp(c, F, "west", T, "east", T, NULL); S.f_ns = stp(c, F, "north", T, "south", T, NULL);
    S.f_ne = stp(c, F, "north", T, "east", T, NULL); S.f_se = stp(c, F, "south", T, "east", T, NULL);
    S.f_nw = stp(c, F, "north", T, "west", T, NULL); S.f_sw = stp(c, F, "south", T, "west", T, NULL);
    S.f_e = stp(c, F, "east", T, NULL); S.f_w = stp(c, F, "west", T, NULL);
    S.f_nswe = stp(c, F, "north", T, "south", T, "west", T, "east", T, NULL);
    S.ladder_s = stp(c, "minecraft:ladder", "facing", "south", NULL); S.ladder_w = stp(c, "minecraft:ladder", "facing", "west", NULL);
    const char *B = "minecraft:iron_bars";
    S.bars_ns = stp(c, B, "north", T, "south", T, NULL); S.bars_nse = stp(c, B, "north", T, "south", T, "east", T, NULL);
    S.bars_we = stp(c, B, "west", T, "east", T, NULL); S.bars_w = stp(c, B, "west", T, NULL); S.bars_e = stp(c, B, "east", T, NULL);
    S.idoor_w = stp(c, "minecraft:iron_door", "facing", "west", NULL); S.idoor_w_up = stp(c, "minecraft:iron_door", "facing", "west", "half", "upper", NULL);
    S.idoor = sp_st(c, "minecraft:iron_door"); S.idoor_up = stp(c, "minecraft:iron_door", "half", "upper", NULL);
    S.odoor = sp_st(c, "minecraft:oak_door"); S.odoor_up = stp(c, "minecraft:oak_door", "half", "upper", NULL);
    S.btn_n = stp(c, "minecraft:stone_button", "facing", "north", NULL); S.btn_s = stp(c, "minecraft:stone_button", "facing", "south", NULL);
    S.sbstairs_n = stp(c, "minecraft:stone_brick_stairs", "facing", "north", NULL); S.cstairs_s = stp(c, "minecraft:cobblestone_stairs", "facing", "south", NULL);
    S.portal = sp_st(c, "minecraft:end_portal"); S.spawner = sp_st(c, "minecraft:spawner");
    S.nstates = g->nstates; S.g = g;
    return &S;
}
#undef T

/* ---------------------------------------------------------------- рисование */
static int smooth_next(SPSel *s, RS *r, int x, int y, int z, int edge) {     /* SmoothStoneSelector */
    (void)x; (void)y; (void)z;
    const SHSt *S = s->ud;
    if (!edge) return S->cave_air;
    float f = rs_float(r);
    if (f < 0.2f) return S->cracked;
    if (f < 0.5f) return S->mossy;
    if (f < 0.55f) return S->infested;
    return S->sb;
}

typedef struct PC { StCtx *c; StPiece *p; const SHSt *S; SPSel sel; } PC;
static void PL(PC *q, int st, int x, int y, int z) { sp_place(q->c, q->p, st, x, y, z); }
static void BOX(PC *q, int x0, int y0, int z0, int x1, int y1, int z1, int st) { sp_box(q->c, q->p, x0, y0, z0, x1, y1, z1, st, st, 0); }
static void SEL(PC *q, int x0, int y0, int z0, int x1, int y1, int z1, int skip_air) { sp_box_sel(q->c, q->p, x0, y0, z0, x1, y1, z1, skip_air, &q->sel); }

/* StrongholdPiece.generateSmallDoor */
static void small_door(PC *q, int type, int x, int y, int z) {
    const SHSt *S = q->S;
    switch (type) {
    case D_OPENING: BOX(q, x, y, z, x + 2, y + 2, z, S->cave_air); break;
    case D_WOOD: case D_IRON:
        PL(q, S->sb, x, y, z); PL(q, S->sb, x, y + 1, z); PL(q, S->sb, x, y + 2, z); PL(q, S->sb, x + 1, y + 2, z);
        PL(q, S->sb, x + 2, y + 2, z); PL(q, S->sb, x + 2, y + 1, z); PL(q, S->sb, x + 2, y, z);
        if (type == D_WOOD) { PL(q, S->odoor, x + 1, y, z); PL(q, S->odoor_up, x + 1, y + 1, z); }
        else {
            PL(q, S->idoor, x + 1, y, z); PL(q, S->idoor_up, x + 1, y + 1, z);
            PL(q, S->btn_n, x + 2, y + 1, z + 1); PL(q, S->btn_s, x + 2, y + 1, z - 1);
        }
        break;
    case D_GRATES:
        PL(q, S->cave_air, x + 1, y, z); PL(q, S->cave_air, x + 1, y + 1, z);
        PL(q, S->bars_w, x, y, z); PL(q, S->bars_w, x, y + 1, z);
        PL(q, S->bars_we, x, y + 2, z); PL(q, S->bars_we, x + 1, y + 2, z); PL(q, S->bars_we, x + 2, y + 2, z);
        PL(q, S->bars_e, x + 2, y + 1, z); PL(q, S->bars_e, x + 2, y, z);
        break;
    }
}

static void post_chest_corridor(PC *q, SHData *d) {
    const SHSt *S = q->S;
    SEL(q, 0, 0, 0, 4, 4, 6, 1);
    small_door(q, d->door, 1, 1, 0); small_door(q, D_OPENING, 1, 1, 6);
    BOX(q, 3, 1, 2, 3, 1, 4, S->sb);
    PL(q, S->sb_slab, 3, 1, 1); PL(q, S->sb_slab, 3, 1, 5); PL(q, S->sb_slab, 3, 2, 2); PL(q, S->sb_slab, 3, 2, 4);
    for (int z = 2; z <= 4; z++) PL(q, S->sb_slab, 2, 1, z);
    if (!d->placed && sp_inside(q->c, sp_wx(q->p, 3, 3), sp_wy(q->p, 2), sp_wz(q->p, 3, 3))) {
        d->placed = 1;
        sp_create_chest(q->c, q->p, 3, 2, 3, -1);
    }
}

static void post_filler(PC *q, SHData *d) {
    const SHSt *S = q->S;
    for (int i = 0; i < d->steps; i++) {
        for (int x = 0; x <= 4; x++) PL(q, S->sb, x, 0, i);
        for (int y = 1; y <= 3; y++) { PL(q, S->sb, 0, y, i); PL(q, S->cave_air, 1, y, i); PL(q, S->cave_air, 2, y, i); PL(q, S->cave_air, 3, y, i); PL(q, S->sb, 4, y, i); }
        for (int x = 0; x <= 4; x++) PL(q, S->sb, x, 4, i);
    }
}

static void post_five_crossing(PC *q, SHData *d) {
    const SHSt *S = q->S; int CA = S->cave_air;
    SEL(q, 0, 0, 0, 9, 8, 10, 1);
    small_door(q, d->door, 4, 3, 0);
    if (d->f[0]) BOX(q, 0, 3, 1, 0, 5, 3, CA);
    if (d->f[2]) BOX(q, 9, 3, 1, 9, 5, 3, CA);
    if (d->f[1]) BOX(q, 0, 5, 7, 0, 7, 9, CA);
    if (d->f[3]) BOX(q, 9, 5, 7, 9, 7, 9, CA);
    BOX(q, 5, 1, 10, 7, 3, 10, CA);
    SEL(q, 1, 2, 1, 8, 2, 6, 0);
    SEL(q, 4, 1, 5, 4, 4, 9, 0);
    SEL(q, 8, 1, 5, 8, 4, 9, 0);
    SEL(q, 1, 4, 7, 3, 4, 9, 0);
    SEL(q, 1, 3, 5, 3, 3, 6, 0);
    BOX(q, 1, 3, 4, 3, 3, 4, S->ss_slab);
    BOX(q, 1, 4, 6, 3, 4, 6, S->ss_slab);
    SEL(q, 5, 1, 7, 7, 1, 8, 0);
    BOX(q, 5, 1, 9, 7, 1, 9, S->ss_slab);
    BOX(q, 5, 2, 7, 7, 2, 7, S->ss_slab);
    BOX(q, 4, 5, 7, 4, 5, 9, S->ss_slab);
    BOX(q, 8, 5, 7, 8, 5, 9, S->ss_slab);
    BOX(q, 5, 5, 7, 7, 5, 9, S->ss_double);
    PL(q, S->wtorch[1], 6, 5, 6);
}

static void post_turn(PC *q, SHData *d) {
    const SHSt *S = q->S;
    SEL(q, 0, 0, 0, 4, 4, 4, 1);
    small_door(q, d->door, 1, 1, 0);
    int o = sp_orientation_dir(q->p);
    int other = (o != DIR_NORTH && o != DIR_EAST);
    int right_side = d->kind == K_LEFT ? other : !other;      /* LeftTurn: «не N/E» → x = 4; RightTurn — наоборот */
    if (right_side) BOX(q, 4, 1, 1, 4, 3, 3, S->cave_air); else BOX(q, 0, 1, 1, 0, 3, 3, S->cave_air);
}

static void post_library(PC *q, SHData *d) {
    const SHSt *S = q->S; int tall = d->f[0];
    int h = tall ? 11 : 6;
    SEL(q, 0, 0, 0, 13, h - 1, 14, 1);
    small_door(q, d->door, 4, 1, 0);
    sp_maybe_box(q->c, q->p, 0.07f, 2, 1, 1, 11, 4, 13, S->cobweb, S->cobweb, 0, 0);
    for (int z = 1; z <= 13; z++) {
        if ((z - 1) % 4 == 0) {
            BOX(q, 1, 1, z, 1, 4, z, S->planks); BOX(q, 12, 1, z, 12, 4, z, S->planks);
            PL(q, S->wtorch[2], 2, 3, z); PL(q, S->wtorch[3], 11, 3, z);
            if (tall) { BOX(q, 1, 6, z, 1, 9, z, S->planks); BOX(q, 12, 6, z, 12, 9, z, S->planks); }
        } else {
            BOX(q, 1, 1, z, 1, 4, z, S->bookshelf); BOX(q, 12, 1, z, 12, 4, z, S->bookshelf);
            if (tall) { BOX(q, 1, 6, z, 1, 9, z, S->bookshelf); BOX(q, 12, 6, z, 12, 9, z, S->bookshelf); }
        }
    }
    for (int z = 3; z < 12; z += 2) {
        BOX(q, 3, 1, z, 4, 3, z, S->bookshelf); BOX(q, 6, 1, z, 7, 3, z, S->bookshelf); BOX(q, 9, 1, z, 10, 3, z, S->bookshelf);
    }
    if (tall) {
        BOX(q, 1, 5, 1, 3, 5, 13, S->planks); BOX(q, 10, 5, 1, 12, 5, 13, S->planks);
        BOX(q, 4, 5, 1, 9, 5, 2, S->planks); BOX(q, 4, 5, 12, 9, 5, 13, S->planks);
        PL(q, S->planks, 9, 5, 11); PL(q, S->planks, 8, 5, 11); PL(q, S->planks, 9, 5, 10);
        BOX(q, 3, 6, 3, 3, 6, 11, S->f_ns); BOX(q, 10, 6, 3, 10, 6, 9, S->f_ns);
        BOX(q, 4, 6, 2, 9, 6, 2, S->f_we); BOX(q, 4, 6, 12, 7, 6, 12, S->f_we);
        PL(q, S->f_ne, 3, 6, 2); PL(q, S->f_se, 3, 6, 12); PL(q, S->f_nw, 10, 6, 2);
        for (int i = 0; i <= 2; i++) {
            PL(q, S->f_sw, 8 + i, 6, 12 - i);
            if (i != 2) PL(q, S->f_ne, 8 + i, 6, 11 - i);
        }
        for (int y = 1; y <= 7; y++) PL(q, S->ladder_s, 10, y, 13);
        PL(q, S->f_e, 6, 9, 7); PL(q, S->f_w, 7, 9, 7); PL(q, S->f_e, 6, 8, 7); PL(q, S->f_w, 7, 8, 7);
        PL(q, S->f_nswe, 6, 7, 7); PL(q, S->f_nswe, 7, 7, 7);
        PL(q, S->f_e, 5, 7, 7); PL(q, S->f_w, 8, 7, 7);
        PL(q, S->f_ne, 6, 7, 6); PL(q, S->f_se, 6, 7, 8); PL(q, S->f_nw, 7, 7, 6); PL(q, S->f_sw, 7, 7, 8);
        PL(q, S->torch, 5, 8, 7); PL(q, S->torch, 8, 8, 7); PL(q, S->torch, 6, 8, 6); PL(q, S->torch, 6, 8, 8);
        PL(q, S->torch, 7, 8, 6); PL(q, S->torch, 7, 8, 8);
    }
    sp_create_chest(q->c, q->p, 3, 3, 5, -1);
    if (tall) {
        PL(q, S->cave_air, 12, 9, 1);
        sp_create_chest(q->c, q->p, 12, 8, 1, -1);
    }
}

static void post_portal_room(PC *q, SHData *d) {
    const SHSt *S = q->S;
    SEL(q, 0, 0, 0, 10, 7, 15, 0);
    small_door(q, D_GRATES, 4, 1, 0);
    SEL(q, 1, 6, 1, 1, 6, 14, 0);
    SEL(q, 9, 6, 1, 9, 6, 14, 0);
    SEL(q, 2, 6, 1, 8, 6, 2, 0);
    SEL(q, 2, 6, 14, 8, 6, 14, 0);
    SEL(q, 1, 1, 1, 2, 1, 4, 0);
    SEL(q, 8, 1, 1, 9, 1, 4, 0);
    BOX(q, 1, 1, 1, 1, 1, 3, S->lava);
    BOX(q, 9, 1, 1, 9, 1, 3, S->lava);
    SEL(q, 3, 1, 8, 7, 1, 12, 0);
    BOX(q, 4, 1, 9, 6, 1, 11, S->lava);
    for (int z = 3; z < 14; z += 2) { BOX(q, 0, 3, z, 0, 4, z, S->bars_ns); BOX(q, 10, 3, z, 10, 4, z, S->bars_ns); }
    for (int x = 2; x < 9; x += 2) BOX(q, x, 3, 15, x, 4, 15, S->bars_we);
    SEL(q, 4, 1, 5, 6, 1, 7, 0);
    SEL(q, 4, 2, 6, 6, 2, 7, 0);
    SEL(q, 4, 3, 7, 6, 3, 7, 0);
    for (int x = 4; x <= 6; x++) { PL(q, S->sbstairs_n, x, 1, 4); PL(q, S->sbstairs_n, x, 2, 5); PL(q, S->sbstairs_n, x, 3, 6); }
    int eyes[12], all = 1;
    for (int i = 0; i < 12; i++) { eyes[i] = rs_float(q->c->rs) > 0.9f; all &= eyes[i]; }
    PL(q, S->frame[0][eyes[0]], 4, 3, 8); PL(q, S->frame[0][eyes[1]], 5, 3, 8); PL(q, S->frame[0][eyes[2]], 6, 3, 8);
    PL(q, S->frame[1][eyes[3]], 4, 3, 12); PL(q, S->frame[1][eyes[4]], 5, 3, 12); PL(q, S->frame[1][eyes[5]], 6, 3, 12);
    PL(q, S->frame[2][eyes[6]], 3, 3, 9); PL(q, S->frame[2][eyes[7]], 3, 3, 10); PL(q, S->frame[2][eyes[8]], 3, 3, 11);
    PL(q, S->frame[3][eyes[9]], 7, 3, 9); PL(q, S->frame[3][eyes[10]], 7, 3, 10); PL(q, S->frame[3][eyes[11]], 7, 3, 11);
    if (all) for (int z = 9; z <= 11; z++) for (int x = 4; x <= 6; x++) PL(q, S->portal, x, 3, z);
    if (!d->placed) {
        int wx = sp_wx(q->p, 5, 6), wy = sp_wy(q->p, 3), wz = sp_wz(q->p, 5, 6);
        if (sp_inside(q->c, wx, wy, wz)) { d->placed = 1; sp_set_world(q->c, wx, wy, wz, S->spawner); }   /* setEntityId: ГСЧ не тратит (пустой spawnPotentials) */
    }
}

static void post_prison(PC *q, SHData *d) {
    const SHSt *S = q->S;
    SEL(q, 0, 0, 0, 8, 4, 10, 1);
    small_door(q, d->door, 1, 1, 0);
    BOX(q, 1, 1, 10, 3, 3, 10, S->cave_air);
    SEL(q, 4, 1, 1, 4, 3, 1, 0);
    SEL(q, 4, 1, 3, 4, 3, 3, 0);
    SEL(q, 4, 1, 7, 4, 3, 7, 0);
    SEL(q, 4, 1, 9, 4, 3, 9, 0);
    for (int y = 1; y <= 3; y++) {
        PL(q, S->bars_ns, 4, y, 4); PL(q, S->bars_nse, 4, y, 5); PL(q, S->bars_ns, 4, y, 6);
        PL(q, S->bars_we, 5, y, 5); PL(q, S->bars_we, 6, y, 5); PL(q, S->bars_we, 7, y, 5);
    }
    PL(q, S->bars_ns, 4, 3, 2); PL(q, S->bars_ns, 4, 3, 8);
    PL(q, S->idoor_w, 4, 1, 2); PL(q, S->idoor_w_up, 4, 2, 2); PL(q, S->idoor_w, 4, 1, 8); PL(q, S->idoor_w_up, 4, 2, 8);
}

static void post_room_crossing(PC *q, SHData *d) {
    const SHSt *S = q->S; int CA = S->cave_air;
    SEL(q, 0, 0, 0, 10, 6, 10, 1);
    small_door(q, d->door, 4, 1, 0);
    BOX(q, 4, 1, 10, 6, 3, 10, CA);
    BOX(q, 0, 1, 4, 0, 3, 6, CA);
    BOX(q, 10, 1, 4, 10, 3, 6, CA);
    switch (d->f[0]) {
    case 0:
        PL(q, S->sb, 5, 1, 5); PL(q, S->sb, 5, 2, 5); PL(q, S->sb, 5, 3, 5);
        PL(q, S->wtorch[3], 4, 3, 5); PL(q, S->wtorch[2], 6, 3, 5); PL(q, S->wtorch[1], 5, 3, 4); PL(q, S->wtorch[0], 5, 3, 6);
        PL(q, S->ss_slab, 4, 1, 4); PL(q, S->ss_slab, 4, 1, 5); PL(q, S->ss_slab, 4, 1, 6);
        PL(q, S->ss_slab, 6, 1, 4); PL(q, S->ss_slab, 6, 1, 5); PL(q, S->ss_slab, 6, 1, 6);
        PL(q, S->ss_slab, 5, 1, 4); PL(q, S->ss_slab, 5, 1, 6);
        break;
    case 1:
        for (int i = 0; i < 5; i++) {
            PL(q, S->sb, 3, 1, 3 + i); PL(q, S->sb, 7, 1, 3 + i); PL(q, S->sb, 3 + i, 1, 3); PL(q, S->sb, 3 + i, 1, 7);
        }
        PL(q, S->sb, 5, 1, 5); PL(q, S->sb, 5, 2, 5); PL(q, S->sb, 5, 3, 5);
        PL(q, S->water, 5, 4, 5);
        break;
    case 2:
        for (int z = 1; z <= 9; z++) { PL(q, S->cobble, 1, 3, z); PL(q, S->cobble, 9, 3, z); }
        for (int x = 1; x <= 9; x++) { PL(q, S->cobble, x, 3, 1); PL(q, S->cobble, x, 3, 9); }
        PL(q, S->cobble, 5, 1, 4); PL(q, S->cobble, 5, 1, 6); PL(q, S->cobble, 5, 3, 4); PL(q, S->cobble, 5, 3, 6);
        PL(q, S->cobble, 4, 1, 5); PL(q, S->cobble, 6, 1, 5); PL(q, S->cobble, 4, 3, 5); PL(q, S->cobble, 6, 3, 5);
        for (int y = 1; y <= 3; y++) { PL(q, S->cobble, 4, y, 4); PL(q, S->cobble, 6, y, 4); PL(q, S->cobble, 4, y, 6); PL(q, S->cobble, 6, y, 6); }
        PL(q, S->wtorch[0], 5, 3, 5);
        for (int z = 2; z <= 8; z++) {
            PL(q, S->planks, 2, 3, z); PL(q, S->planks, 3, 3, z);
            if (z <= 3 || z >= 7) { PL(q, S->planks, 4, 3, z); PL(q, S->planks, 5, 3, z); PL(q, S->planks, 6, 3, z); }
            PL(q, S->planks, 7, 3, z); PL(q, S->planks, 8, 3, z);
        }
        PL(q, S->ladder_w, 9, 1, 3); PL(q, S->ladder_w, 9, 2, 3); PL(q, S->ladder_w, 9, 3, 3);
        sp_create_chest(q->c, q->p, 3, 4, 8, -1);
        break;
    }
}

static void post_stairs_down(PC *q, SHData *d) {
    const SHSt *S = q->S; int SB = S->sb, SL = S->ss_slab;
    SEL(q, 0, 0, 0, 4, 10, 4, 1);
    small_door(q, d->door, 1, 7, 0);
    small_door(q, D_OPENING, 1, 1, 4);
    PL(q, SB, 2, 6, 1); PL(q, SB, 1, 5, 1); PL(q, SL, 1, 6, 1); PL(q, SB, 1, 5, 2); PL(q, SB, 1, 4, 3); PL(q, SL, 1, 5, 3);
    PL(q, SB, 2, 4, 3); PL(q, SB, 3, 3, 3); PL(q, SL, 3, 4, 3); PL(q, SB, 3, 3, 2); PL(q, SB, 3, 2, 1); PL(q, SL, 3, 3, 1);
    PL(q, SB, 2, 2, 1); PL(q, SB, 1, 1, 1); PL(q, SL, 1, 2, 1); PL(q, SB, 1, 1, 2); PL(q, SL, 1, 1, 3);
}

static void post_straight(PC *q, SHData *d) {
    const SHSt *S = q->S;
    SEL(q, 0, 0, 0, 4, 4, 6, 1);
    small_door(q, d->door, 1, 1, 0);
    small_door(q, D_OPENING, 1, 1, 6);
    sp_maybe_block(q->c, q->p, 0.1f, 1, 2, 1, S->wtorch[2]);
    sp_maybe_block(q->c, q->p, 0.1f, 3, 2, 1, S->wtorch[3]);
    sp_maybe_block(q->c, q->p, 0.1f, 1, 2, 5, S->wtorch[2]);
    sp_maybe_block(q->c, q->p, 0.1f, 3, 2, 5, S->wtorch[3]);
    if (d->f[0]) BOX(q, 0, 1, 2, 0, 3, 4, S->cave_air);
    if (d->f[1]) BOX(q, 4, 1, 2, 4, 3, 4, S->cave_air);
}

static void post_straight_stairs(PC *q, SHData *d) {
    const SHSt *S = q->S;
    SEL(q, 0, 0, 0, 4, 10, 7, 1);
    small_door(q, d->door, 1, 7, 0);
    small_door(q, D_OPENING, 1, 1, 7);
    for (int i = 0; i < 6; i++) {
        PL(q, S->cstairs_s, 1, 6 - i, 1 + i); PL(q, S->cstairs_s, 2, 6 - i, 1 + i); PL(q, S->cstairs_s, 3, 6 - i, 1 + i);
        if (i < 5) { PL(q, S->sb, 1, 5 - i, 1 + i); PL(q, S->sb, 2, 5 - i, 1 + i); PL(q, S->sb, 3, 5 - i, 1 + i); }
    }
}

static void sh_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    SHData *d = p->data;
    PC q; q.c = c; q.p = p; q.S = sh_states(c); q.sel.ud = (void *)q.S; q.sel.next = smooth_next;
    switch (d->kind) {
    case K_START: case K_SD: post_stairs_down(&q, d); break;
    case K_STRAIGHT: post_straight(&q, d); break;
    case K_PRISON: post_prison(&q, d); break;
    case K_LEFT: case K_RIGHT: post_turn(&q, d); break;
    case K_ROOMX: post_room_crossing(&q, d); break;
    case K_SSD: post_straight_stairs(&q, d); break;
    case K_FIVEX: post_five_crossing(&q, d); break;
    case K_CHEST: post_chest_corridor(&q, d); break;
    case K_LIB: post_library(&q, d); break;
    case K_PORTAL: post_portal_room(&q, d); break;
    case K_FILLER: post_filler(&q, d); break;
    }
}

static void sh_reset(StPiece *p) { SHData *d = p->data; d->placed = 0; }
static void sh_free(void *v) { free(v); }
static void sh_dump(const StPiece *p, StrBuf *o) {
    const SHData *d = p->data;
    sb_printf(o, ",\"door\":\"%s\"", DOOR_NAME[d->door]);
    switch (d->kind) {
    case K_STRAIGHT: sb_printf(o, ",\"left\":%d,\"right\":%d", d->f[0], d->f[1]); break;
    case K_FIVEX: sb_printf(o, ",\"leftLow\":%d,\"leftHigh\":%d,\"rightLow\":%d,\"rightHigh\":%d", d->f[0], d->f[1], d->f[2], d->f[3]); break;
    case K_ROOMX: sb_printf(o, ",\"type\":%d", d->f[0]); break;
    case K_LIB: sb_printf(o, ",\"tall\":%d", d->f[0]); break;
    case K_START: case K_SD: sb_printf(o, ",\"source\":%d", d->f[0]); break;
    case K_FILLER: sb_printf(o, ",\"steps\":%d", d->steps); break;
    default: break;
    }
}

#define SHVT(id) { id, sh_post, NULL, sh_free, sh_dump, sh_reset, NULL }
const PieceVT PIECE_SH[K__N] = {
    [K_START] = SHVT("minecraft:shstart"), [K_STRAIGHT] = SHVT("minecraft:shs"), [K_PRISON] = SHVT("minecraft:shph"),
    [K_LEFT] = SHVT("minecraft:shlt"), [K_RIGHT] = SHVT("minecraft:shrt"), [K_ROOMX] = SHVT("minecraft:shrc"),
    [K_SSD] = SHVT("minecraft:shssd"), [K_SD] = SHVT("minecraft:shsd"), [K_FIVEX] = SHVT("minecraft:sh5c"),
    [K_CHEST] = SHVT("minecraft:shcc"), [K_LIB] = SHVT("minecraft:shli"), [K_PORTAL] = SHVT("minecraft:shpr"),
    [K_FILLER] = SHVT("minecraft:shfc"),
};
#undef SHVT

/* ---------------------------------------------------------------- структура */
static void *sh_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int sh_find(GenCtx *c, const void *cfg, Stub *out) {     /* findGenerationPoint: chunkPos.getWorldPosition() */
    (void)cfg;
    out->x = c->cx * 16; out->y = 0; out->z = c->cz * 16; out->state = NULL;
    return 1;
}
static void sh_free_cfg(void *p) { free(p); }
const StructType STRUCT_STRONGHOLD = { "minecraft:stronghold", sh_parse, sh_find, sh_build, NULL, sh_free_cfg, NULL };

void structures_register_stronghold(void) {
    structure_register_type(&STRUCT_STRONGHOLD);
    for (int k = 0; k < K__N; k++) piece_register_type(&PIECE_SH[k]);
}

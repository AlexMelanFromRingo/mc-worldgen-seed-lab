/* structures/ocean_monument.c — OceanMonumentStructure / OceanMonumentPieces (`minecraft:ocean_monument`).
 *
 * Старт содержит ОДНУ часть — MonumentBuilding (`minecraft:omb`, 58×23×58 на y = 39); комнаты (вход, ядро, двойные/простые комнаты,
 * крылья, «пентхаус») — внутренний список дочерних частей здания (в NBT не пишутся, при загрузке здание пересоздаётся тем же ГСЧ).
 * Конструктор здания строит граф комнат 5×3×5 (generateRoomGraph: ядро, перемешивание, закрытие проёмов с проверкой связности до входа),
 * подбирает тип каждой комнаты (fitters) и ставит крылья/пентхаус. Рисование: здание рисует стены/крышу/колонны/воду в каждом чанке
 * пересечения, затем вызывает postProcess каждой дочерней части, пересекающей чанк (общий ГСЧ структуры: простые комнаты — nextBoolean,
 * «губочные» верхние — nextInt(3)/nextInt(4) на каждую клетку, на каждом чанке пересечения).
 * Все блоки постройки без направлений (призмарин, морской фонарь, губка, золото, вода, воздух): зеркало/поворот на состояния не влияют. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- граф комнат (RoomDefinition) */
enum { D_DOWN = 0, D_UP = 1, D_NORTH = 2, D_SOUTH = 3, D_WEST = 4, D_EAST = 5 };      /* Direction.get3DDataValue */
typedef struct Room {
    int index;
    struct Room *conn[6];
    u8 open[6];
    u8 claimed, is_source;
    int scan;
} Room;

static void room_connect(Room *a, int d, Room *b) { a->conn[d] = b; b->conn[d ^ 1] = a; }
static void room_update_openings(Room *r) { for (int i = 0; i < 6; i++) r->open[i] = r->conn[i] != NULL; }
static int room_count_openings(const Room *r) { int n = 0; for (int i = 0; i < 6; i++) n += r->open[i] != 0; return n; }
static int room_find_source(Room *r, int scan) {
    if (r->is_source) return 1;
    r->scan = scan;
    for (int i = 0; i < 6; i++) {
        Room *o = r->conn[i];
        if (o && r->open[i] && o->scan != scan && room_find_source(o, scan)) return 1;
    }
    return 0;
}
static inline int room_index(int x, int y, int z) { return y * 25 + z * 5 + x; }

/* ---------------------------------------------------------------- части */
enum { K_CORE, K_DX, K_DXY, K_DY, K_DYZ, K_DZ, K_ENTRY, K_PENT, K_SIMPLE, K_STOP, K_WING };
typedef struct OMChild { int kind; Room *room; int design; } OMChild;

typedef struct OMB {
    Room rooms[78];           /* 0..74 — сетка (exists[]), 75 — крыша (1003), 76 — левое крыло (1001), 77 — правое (1002) */
    u8 exists[75];
    StPiece **ch; int nch;    /* childPieces по порядку игры */
} OMB;

extern const PieceVT PIECE_OMB;
extern const PieceVT PIECE_OM_CHILD[11];

/* ---------------------------------------------------------------- рисование: общие примитивы OceanMonumentPiece */
typedef struct K {
    StCtx *c; StPiece *p;
    int G, L, D, S, W, A, SPONGE, GOLD;       /* prismarine, prismarine_bricks, dark_prismarine, sea_lantern, water, air */
    int b_ice, b_packed, b_blue, b_water;
    int sea;
} K;

static void k_init(K *k, StCtx *c, StPiece *p) {
    k->c = c; k->p = p;
    k->G = sp_st(c, "minecraft:prismarine"); k->L = sp_st(c, "minecraft:prismarine_bricks"); k->D = sp_st(c, "minecraft:dark_prismarine");
    k->S = sp_st(c, "minecraft:sea_lantern"); k->W = sp_st(c, "minecraft:water"); k->A = sp_air(c);
    k->SPONGE = sp_st(c, "minecraft:wet_sponge"); k->GOLD = sp_st(c, "minecraft:gold_block");
    const BsTab *bs = c->sw->bs;
    k->b_ice = bs_block_index(bs, "minecraft:ice"); k->b_packed = bs_block_index(bs, "minecraft:packed_ice");
    k->b_blue = bs_block_index(bs, "minecraft:blue_ice"); k->b_water = bs_block_index(bs, "minecraft:water");
    k->sea = c->w->sea_level;
}
static K k_child(const K *k, StPiece *p) { K r = *k; r.p = p; return r; }

/* мировой бокс локального бокса части пересекает chunkBB? (иначе вызов не меняет мир и не тратит ГСЧ) */
static int k_touch(const K *k, int x0, int y0, int z0, int x1, int y1, int z1) {
    const StPiece *p = k->p;
    BB b = bb_from_corners(sp_wx(p, x0, z0), sp_wy(p, y0), sp_wz(p, x0, z0), sp_wx(p, x1, z1), sp_wy(p, y1), sp_wz(p, x1, z1));
    return bb_intersects(&b, &k->c->chunk);
}
static void box(const K *k, int x0, int y0, int z0, int x1, int y1, int z1, int st) {
    if (x0 > x1 || y0 > y1 || z0 > z1 || !k_touch(k, x0, y0, z0, x1, y1, z1)) return;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) sp_place(k->c, k->p, st, x, y, z);
}
static void put(const K *k, int st, int x, int y, int z) { sp_place(k->c, k->p, st, x, y, z); }
/* generateWaterBox: лёд/вода остаются; выше уровня моря — воздух, ниже — вода */
static void water(const K *k, int x0, int y0, int z0, int x1, int y1, int z1) {
    if (x0 > x1 || y0 > y1 || z0 > z1 || !k_touch(k, x0, y0, z0, x1, y1, z1)) return;
    const u16 *sb = k->c->w->g->state_block;
    for (int y = y0; y <= y1; y++) {
        int above = sp_wy(k->p, y) >= k->sea;
        for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) {
            int st = sp_get(k->c, k->p, x, y, z); int b = sb[st];
            if (b == k->b_ice || b == k->b_packed || b == k->b_blue || b == k->b_water) continue;
            sp_place(k->c, k->p, (above && st != k->W) ? k->A : k->W, x, y, z);
        }
    }
}
/* generateBoxOnFillOnly: только там, где стоит FILL_BLOCK (вода по умолчанию) */
static void fill_only(const K *k, int x0, int y0, int z0, int x1, int y1, int z1, int st) {
    if (!k_touch(k, x0, y0, z0, x1, y1, z1)) return;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++)
        if (sp_get(k->c, k->p, x, y, z) == k->W) sp_place(k->c, k->p, st, x, y, z);
}
static void default_floor(const K *k, int xo, int zo, int down_open) {
    if (down_open) {
        box(k, xo + 0, 0, zo + 0, xo + 2, 0, zo + 7, k->G);
        box(k, xo + 5, 0, zo + 0, xo + 7, 0, zo + 7, k->G);
        box(k, xo + 3, 0, zo + 0, xo + 4, 0, zo + 2, k->G);
        box(k, xo + 3, 0, zo + 5, xo + 4, 0, zo + 7, k->G);
        box(k, xo + 3, 0, zo + 2, xo + 4, 0, zo + 2, k->L);
        box(k, xo + 3, 0, zo + 5, xo + 4, 0, zo + 5, k->L);
        box(k, xo + 2, 0, zo + 3, xo + 2, 0, zo + 4, k->L);
        box(k, xo + 5, 0, zo + 3, xo + 5, 0, zo + 4, k->L);
    } else box(k, xo + 0, 0, zo + 0, xo + 7, 0, zo + 7, k->G);
}
static int chunk_intersects(const K *k, int x0, int z0, int x1, int z1) {
    const StPiece *p = k->p;
    int wx0 = sp_wx(p, x0, z0), wz0 = sp_wz(p, x0, z0), wx1 = sp_wx(p, x1, z1), wz1 = sp_wz(p, x1, z1);
    return bb_intersects_xz(&k->c->chunk, wx0 < wx1 ? wx0 : wx1, wz0 < wz1 ? wz0 : wz1, wx0 > wx1 ? wx0 : wx1, wz0 > wz1 ? wz0 : wz1);
}
/* spawnElder: сущность не сравнивается, но Mob.finalizeSpawn тратит level.getRandom(): triangle (2× nextDouble) и nextFloat */
static void spawn_elder(const K *k, int x, int y, int z) {
    const StPiece *p = k->p;
    if (!sp_inside(k->c, sp_wx(p, x, z), sp_wy(p, y), sp_wz(p, x, z))) return;
    Rnd *r = sp_region_random(k->c);
    (void)rnd_next_double(r); (void)rnd_next_double(r); (void)rnd_next_float(r);
}

/* ---------------------------------------------------------------- MonumentBuilding: секции */
static void gen_wing(const K *k, int flipped, int xo) {
    if (!chunk_intersects(k, xo, 0, xo + 23, 20)) return;
    int G = k->G, L = k->L;
    box(k, xo + 0, 0, 0, xo + 24, 0, 20, G);
    water(k, xo + 0, 1, 0, xo + 24, 10, 20);
    for (int i = 0; i < 4; i++) {
        box(k, xo + i, i + 1, i, xo + i, i + 1, 20, L);
        box(k, xo + i + 7, i + 5, i + 7, xo + i + 7, i + 5, 20, L);
        box(k, xo + 17 - i, i + 5, i + 7, xo + 17 - i, i + 5, 20, L);
        box(k, xo + 24 - i, i + 1, i, xo + 24 - i, i + 1, 20, L);
        box(k, xo + i + 1, i + 1, i, xo + 23 - i, i + 1, i, L);
        box(k, xo + i + 8, i + 5, i + 7, xo + 16 - i, i + 5, i + 7, L);
    }
    box(k, xo + 4, 4, 4, xo + 6, 4, 20, G);
    box(k, xo + 7, 4, 4, xo + 17, 4, 6, G);
    box(k, xo + 18, 4, 4, xo + 20, 4, 20, G);
    box(k, xo + 11, 8, 11, xo + 13, 8, 20, G);
    put(k, L, xo + 12, 9, 12); put(k, L, xo + 12, 9, 15); put(k, L, xo + 12, 9, 18);
    int left = xo + (flipped ? 19 : 5), right = xo + (flipped ? 5 : 19);
    for (int z = 20; z >= 5; z -= 3) put(k, L, left, 5, z);
    for (int z = 19; z >= 7; z -= 3) put(k, L, right, 5, z);
    for (int i = 0; i < 4; i++) put(k, L, flipped ? xo + 24 - (17 - i * 3) : xo + 17 - i * 3, 5, 5);
    put(k, L, right, 5, 5);
    box(k, xo + 11, 1, 12, xo + 13, 7, 12, G);
    box(k, xo + 12, 1, 11, xo + 12, 7, 13, G);
}
static void gen_entrance_archs(const K *k) {
    if (!chunk_intersects(k, 22, 5, 35, 17)) return;
    int G = k->G, L = k->L, S = k->S;
    water(k, 25, 0, 0, 32, 8, 20);
    for (int i = 0; i < 4; i++) {
        int z = 5 + i * 4;
        box(k, 24, 2, z, 24, 4, z, L); box(k, 22, 4, z, 23, 4, z, L);
        put(k, L, 25, 5, z); put(k, L, 26, 6, z); put(k, S, 26, 5, z);
        box(k, 33, 2, z, 33, 4, z, L); box(k, 34, 4, z, 35, 4, z, L);
        put(k, L, 32, 5, z); put(k, L, 31, 6, z); put(k, S, 31, 5, z);
        box(k, 27, 6, z, 30, 6, z, G);
    }
}
static void gen_entrance_wall(const K *k) {
    if (!chunk_intersects(k, 15, 20, 42, 21)) return;
    int G = k->G, L = k->L, D = k->D;
    box(k, 15, 0, 21, 42, 0, 21, G);
    water(k, 26, 1, 21, 31, 3, 21);
    box(k, 21, 12, 21, 36, 12, 21, G); box(k, 17, 11, 21, 40, 11, 21, G); box(k, 16, 10, 21, 41, 10, 21, G);
    box(k, 15, 7, 21, 42, 9, 21, G); box(k, 16, 6, 21, 41, 6, 21, G); box(k, 17, 5, 21, 40, 5, 21, G);
    box(k, 21, 4, 21, 36, 4, 21, G); box(k, 22, 3, 21, 26, 3, 21, G); box(k, 31, 3, 21, 35, 3, 21, G);
    box(k, 23, 2, 21, 25, 2, 21, G); box(k, 32, 2, 21, 34, 2, 21, G);
    box(k, 28, 4, 20, 29, 4, 21, L);
    put(k, L, 27, 3, 21); put(k, L, 30, 3, 21); put(k, L, 26, 2, 21); put(k, L, 31, 2, 21); put(k, L, 25, 1, 21); put(k, L, 32, 1, 21);
    for (int i = 0; i < 7; i++) { put(k, D, 28 - i, 6 + i, 21); put(k, D, 29 + i, 6 + i, 21); }
    for (int i = 0; i < 4; i++) { put(k, D, 28 - i, 9 + i, 21); put(k, D, 29 + i, 9 + i, 21); }
    put(k, D, 28, 12, 21); put(k, D, 29, 12, 21);
    for (int i = 0; i < 3; i++) {
        put(k, D, 22 - i * 2, 8, 21); put(k, D, 22 - i * 2, 9, 21); put(k, D, 35 + i * 2, 8, 21); put(k, D, 35 + i * 2, 9, 21);
    }
    water(k, 15, 13, 21, 42, 15, 21);
    water(k, 15, 1, 21, 15, 6, 21); water(k, 16, 1, 21, 16, 5, 21); water(k, 17, 1, 21, 20, 4, 21); water(k, 21, 1, 21, 21, 3, 21);
    water(k, 22, 1, 21, 22, 2, 21); water(k, 23, 1, 21, 24, 1, 21);
    water(k, 42, 1, 21, 42, 6, 21); water(k, 41, 1, 21, 41, 5, 21); water(k, 37, 1, 21, 40, 4, 21); water(k, 36, 1, 21, 36, 3, 21);
    water(k, 33, 1, 21, 34, 1, 21); water(k, 35, 1, 21, 35, 2, 21);
}
static void gen_roof(const K *k) {
    if (!chunk_intersects(k, 21, 21, 36, 36)) return;
    int G = k->G, L = k->L, S = k->S;
    box(k, 21, 0, 22, 36, 0, 36, G);
    water(k, 21, 1, 22, 36, 23, 36);
    for (int i = 0; i < 4; i++) {
        box(k, 21 + i, 13 + i, 21 + i, 36 - i, 13 + i, 21 + i, L);
        box(k, 21 + i, 13 + i, 36 - i, 36 - i, 13 + i, 36 - i, L);
        box(k, 21 + i, 13 + i, 22 + i, 21 + i, 13 + i, 35 - i, L);
        box(k, 36 - i, 13 + i, 22 + i, 36 - i, 13 + i, 35 - i, L);
    }
    box(k, 25, 16, 25, 32, 16, 32, G);
    box(k, 25, 17, 25, 25, 19, 25, L); box(k, 32, 17, 25, 32, 19, 25, L); box(k, 25, 17, 32, 25, 19, 32, L); box(k, 32, 17, 32, 32, 19, 32, L);
    put(k, L, 26, 20, 26); put(k, L, 27, 21, 27); put(k, S, 27, 20, 27);
    put(k, L, 26, 20, 31); put(k, L, 27, 21, 30); put(k, S, 27, 20, 30);
    put(k, L, 31, 20, 31); put(k, L, 30, 21, 30); put(k, S, 30, 20, 30);
    put(k, L, 31, 20, 26); put(k, L, 30, 21, 27); put(k, S, 30, 20, 27);
    box(k, 28, 21, 27, 29, 21, 27, G); box(k, 27, 21, 28, 27, 21, 29, G); box(k, 28, 21, 30, 29, 21, 30, G); box(k, 30, 21, 28, 30, 21, 29, G);
}
static void gen_lower_wall(const K *k) {
    int G = k->G, L = k->L;
    if (chunk_intersects(k, 0, 21, 6, 58)) {
        box(k, 0, 0, 21, 6, 0, 57, G);
        water(k, 0, 1, 21, 6, 7, 57);
        box(k, 4, 4, 21, 6, 4, 53, G);
        for (int i = 0; i < 4; i++) box(k, i, i + 1, 21, i, i + 1, 57 - i, L);
        for (int z = 23; z < 53; z += 3) put(k, L, 5, 5, z);
        put(k, L, 5, 5, 52);
        for (int i = 0; i < 4; i++) box(k, i, i + 1, 21, i, i + 1, 57 - i, L);
        box(k, 4, 1, 52, 6, 3, 52, G);
        box(k, 5, 1, 51, 5, 3, 53, G);
    }
    if (chunk_intersects(k, 51, 21, 58, 58)) {
        box(k, 51, 0, 21, 57, 0, 57, G);
        water(k, 51, 1, 21, 57, 7, 57);
        box(k, 51, 4, 21, 53, 4, 53, G);
        for (int i = 0; i < 4; i++) box(k, 57 - i, i + 1, 21, 57 - i, i + 1, 57 - i, L);
        for (int z = 23; z < 53; z += 3) put(k, L, 52, 5, z);
        put(k, L, 52, 5, 52);
        box(k, 51, 1, 52, 53, 3, 52, G);
        box(k, 52, 1, 51, 52, 3, 53, G);
    }
    if (chunk_intersects(k, 0, 51, 57, 57)) {
        box(k, 7, 0, 51, 50, 0, 57, G);
        water(k, 7, 1, 51, 50, 10, 57);
        for (int i = 0; i < 4; i++) box(k, i + 1, i + 1, 57 - i, 56 - i, i + 1, 57 - i, L);
    }
}
static void gen_middle_wall(const K *k) {
    int G = k->G, L = k->L;
    if (chunk_intersects(k, 7, 21, 13, 50)) {
        box(k, 7, 0, 21, 13, 0, 50, G);
        water(k, 7, 1, 21, 13, 10, 50);
        box(k, 11, 8, 21, 13, 8, 53, G);
        for (int i = 0; i < 4; i++) box(k, i + 7, i + 5, 21, i + 7, i + 5, 54, L);
        for (int z = 21; z <= 45; z += 3) put(k, L, 12, 9, z);
    }
    if (chunk_intersects(k, 44, 21, 50, 54)) {
        box(k, 44, 0, 21, 50, 0, 50, G);
        water(k, 44, 1, 21, 50, 10, 50);
        box(k, 44, 8, 21, 46, 8, 53, G);
        for (int i = 0; i < 4; i++) box(k, 50 - i, i + 5, 21, 50 - i, i + 5, 54, L);
        for (int z = 21; z <= 45; z += 3) put(k, L, 45, 9, z);
    }
    if (chunk_intersects(k, 8, 44, 49, 54)) {
        box(k, 14, 0, 44, 43, 0, 50, G);
        water(k, 14, 1, 44, 43, 10, 50);
        for (int x = 12; x <= 45; x += 3) {
            put(k, L, x, 9, 45); put(k, L, x, 9, 52);
            if (x == 12 || x == 18 || x == 24 || x == 33 || x == 39 || x == 45) {
                put(k, L, x, 9, 47); put(k, L, x, 9, 50); put(k, L, x, 10, 45); put(k, L, x, 10, 46); put(k, L, x, 10, 51);
                put(k, L, x, 10, 52); put(k, L, x, 11, 47); put(k, L, x, 11, 50); put(k, L, x, 12, 48); put(k, L, x, 12, 49);
            }
        }
        for (int i = 0; i < 3; i++) box(k, 8 + i, 5 + i, 54, 49 - i, 5 + i, 54, G);
        box(k, 11, 8, 54, 46, 8, 54, L);
        box(k, 14, 8, 44, 43, 8, 53, G);
    }
}
static void gen_upper_wall(const K *k) {
    int G = k->G, L = k->L;
    if (chunk_intersects(k, 14, 21, 20, 43)) {
        box(k, 14, 0, 21, 20, 0, 43, G);
        water(k, 14, 1, 22, 20, 14, 43);
        box(k, 18, 12, 22, 20, 12, 39, G);
        box(k, 18, 12, 21, 20, 12, 21, L);
        for (int i = 0; i < 4; i++) box(k, i + 14, i + 9, 21, i + 14, i + 9, 43 - i, L);
        for (int z = 23; z <= 39; z += 3) put(k, L, 19, 13, z);
    }
    if (chunk_intersects(k, 37, 21, 43, 43)) {
        box(k, 37, 0, 21, 43, 0, 43, G);
        water(k, 37, 1, 22, 43, 14, 43);
        box(k, 37, 12, 22, 39, 12, 39, G);
        box(k, 37, 12, 21, 39, 12, 21, L);
        for (int i = 0; i < 4; i++) box(k, 43 - i, i + 9, 21, 43 - i, i + 9, 43 - i, L);
        for (int z = 23; z <= 39; z += 3) put(k, L, 38, 13, z);
    }
    if (chunk_intersects(k, 15, 37, 42, 43)) {
        box(k, 21, 0, 37, 36, 0, 43, G);
        water(k, 21, 1, 37, 36, 14, 43);
        box(k, 21, 12, 37, 36, 12, 39, G);
        for (int i = 0; i < 4; i++) box(k, 15 + i, i + 9, 43 - i, 42 - i, i + 9, 43 - i, L);
        for (int x = 21; x <= 36; x += 3) put(k, L, x, 13, 38);
    }
}

static void omb_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    OMB *b = p->data;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int sea = c->w->sea_level;
    int wh = (sea > 64 ? sea : 64) - p->bb.y0;
    water(k, 0, 0, 0, 58, wh, 58);
    gen_wing(k, 0, 0);
    gen_wing(k, 1, 33);
    gen_entrance_archs(k);
    gen_entrance_wall(k);
    gen_roof(k);
    gen_lower_wall(k);
    gen_middle_wall(k);
    gen_upper_wall(k);
    for (int px = 0; px < 7; px++) {
        int pz = 0;
        while (pz < 7) {
            if (pz == 0 && px == 3) pz = 6;
            int bx = px * 9, bz = pz * 9;
            for (int w = 0; w < 4; w++) for (int d = 0; d < 4; d++) {
                put(k, k->L, bx + w, 0, bz + d);
                sp_fill_column_down(c, p, k->L, bx + w, -1, bz + d);
            }
            if (px != 0 && px != 6) pz += 6; else pz++;
        }
    }
    for (int i = 0; i < 5; i++) {
        water(k, -1 - i, 0 + i * 2, -1 - i, -1 - i, 23, 58 + i);
        water(k, 58 + i, 0 + i * 2, -1 - i, 58 + i, 23, 58 + i);
        water(k, 0 - i, 0 + i * 2, -1 - i, 57 + i, 23, -1 - i);
        water(k, 0 - i, 0 + i * 2, 58 + i, 57 + i, 23, 58 + i);
    }
    for (int i = 0; i < b->nch; i++) {
        StPiece *ch = b->ch[i];
        if (bb_intersects(&ch->bb, &c->chunk)) ch->vt->post(c, ch, rx, ry, rz);
    }
}

/* ---------------------------------------------------------------- комнаты */
static void core_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L;
    fill_only(k, 1, 8, 0, 14, 8, 14, G);
    box(k, 0, 7, 0, 0, 7, 15, L); box(k, 15, 7, 0, 15, 7, 15, L); box(k, 1, 7, 0, 15, 7, 0, L); box(k, 1, 7, 15, 14, 7, 15, L);
    for (int y = 1; y <= 6; y++) {
        int bl = (y == 2 || y == 6) ? G : L;
        for (int x = 0; x <= 15; x += 15) { box(k, x, y, 0, x, y, 1, bl); box(k, x, y, 6, x, y, 9, bl); box(k, x, y, 14, x, y, 15, bl); }
        box(k, 1, y, 0, 1, y, 0, bl); box(k, 6, y, 0, 9, y, 0, bl); box(k, 14, y, 0, 14, y, 0, bl); box(k, 1, y, 15, 14, y, 15, bl);
    }
    box(k, 6, 3, 6, 9, 6, 9, k->D);
    box(k, 7, 4, 7, 8, 5, 8, k->GOLD);
    for (int y = 3; y <= 6; y += 3) for (int x = 6; x <= 9; x += 3) { put(k, k->S, x, y, 6); put(k, k->S, x, y, 9); }
    box(k, 5, 1, 6, 5, 2, 6, L); box(k, 5, 1, 9, 5, 2, 9, L); box(k, 10, 1, 6, 10, 2, 6, L); box(k, 10, 1, 9, 10, 2, 9, L);
    box(k, 6, 1, 5, 6, 2, 5, L); box(k, 9, 1, 5, 9, 2, 5, L); box(k, 6, 1, 10, 6, 2, 10, L); box(k, 9, 1, 10, 9, 2, 10, L);
    box(k, 5, 2, 5, 5, 6, 5, L); box(k, 5, 2, 10, 5, 6, 10, L); box(k, 10, 2, 5, 10, 6, 5, L); box(k, 10, 2, 10, 10, 6, 10, L);
    box(k, 5, 7, 1, 5, 7, 6, L); box(k, 10, 7, 1, 10, 7, 6, L); box(k, 5, 7, 9, 5, 7, 14, L); box(k, 10, 7, 9, 10, 7, 14, L);
    box(k, 1, 7, 5, 6, 7, 5, L); box(k, 1, 7, 10, 6, 7, 10, L); box(k, 9, 7, 5, 14, 7, 5, L); box(k, 9, 7, 10, 14, 7, 10, L);
    box(k, 2, 1, 2, 2, 1, 3, L); box(k, 3, 1, 2, 3, 1, 2, L); box(k, 13, 1, 2, 13, 1, 3, L); box(k, 12, 1, 2, 12, 1, 2, L);
    box(k, 2, 1, 12, 2, 1, 13, L); box(k, 3, 1, 13, 3, 1, 13, L); box(k, 13, 1, 12, 13, 1, 13, L); box(k, 12, 1, 13, 12, 1, 13, L);
}

static void dx_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *west = d->room, *east = west->conn[D_EAST];
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L, S = k->S;
    if (west->index / 25 > 0) { default_floor(k, 8, 0, east->open[D_DOWN]); default_floor(k, 0, 0, west->open[D_DOWN]); }
    if (!west->conn[D_UP]) fill_only(k, 1, 4, 1, 7, 4, 6, G);
    if (!east->conn[D_UP]) fill_only(k, 8, 4, 1, 14, 4, 6, G);
    box(k, 0, 3, 0, 0, 3, 7, L); box(k, 15, 3, 0, 15, 3, 7, L); box(k, 1, 3, 0, 15, 3, 0, L); box(k, 1, 3, 7, 14, 3, 7, L);
    box(k, 0, 2, 0, 0, 2, 7, G); box(k, 15, 2, 0, 15, 2, 7, G); box(k, 1, 2, 0, 15, 2, 0, G); box(k, 1, 2, 7, 14, 2, 7, G);
    box(k, 0, 1, 0, 0, 1, 7, L); box(k, 15, 1, 0, 15, 1, 7, L); box(k, 1, 1, 0, 15, 1, 0, L); box(k, 1, 1, 7, 14, 1, 7, L);
    box(k, 5, 1, 0, 10, 1, 4, L); box(k, 6, 2, 0, 9, 2, 3, G); box(k, 5, 3, 0, 10, 3, 4, L);
    put(k, S, 6, 2, 3); put(k, S, 9, 2, 3);
    if (west->open[D_SOUTH]) water(k, 3, 1, 0, 4, 2, 0);
    if (west->open[D_NORTH]) water(k, 3, 1, 7, 4, 2, 7);
    if (west->open[D_WEST]) water(k, 0, 1, 3, 0, 2, 4);
    if (east->open[D_SOUTH]) water(k, 11, 1, 0, 12, 2, 0);
    if (east->open[D_NORTH]) water(k, 11, 1, 7, 12, 2, 7);
    if (east->open[D_EAST]) water(k, 15, 1, 3, 15, 2, 4);
}

static void dxy_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *west = d->room, *east = west->conn[D_EAST];
    Room *wu = west->conn[D_UP], *eu = east->conn[D_UP];
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L, S = k->S;
    if (west->index / 25 > 0) { default_floor(k, 8, 0, east->open[D_DOWN]); default_floor(k, 0, 0, west->open[D_DOWN]); }
    if (!wu->conn[D_UP]) fill_only(k, 1, 8, 1, 7, 8, 6, G);
    if (!eu->conn[D_UP]) fill_only(k, 8, 8, 1, 14, 8, 6, G);
    for (int y = 1; y <= 7; y++) {
        int bl = (y == 2 || y == 6) ? G : L;
        box(k, 0, y, 0, 0, y, 7, bl); box(k, 15, y, 0, 15, y, 7, bl); box(k, 1, y, 0, 15, y, 0, bl); box(k, 1, y, 7, 14, y, 7, bl);
    }
    box(k, 2, 1, 3, 2, 7, 4, L); box(k, 3, 1, 2, 4, 7, 2, L); box(k, 3, 1, 5, 4, 7, 5, L);
    box(k, 13, 1, 3, 13, 7, 4, L); box(k, 11, 1, 2, 12, 7, 2, L); box(k, 11, 1, 5, 12, 7, 5, L);
    box(k, 5, 1, 3, 5, 3, 4, L); box(k, 10, 1, 3, 10, 3, 4, L);
    box(k, 5, 7, 2, 10, 7, 5, L);
    box(k, 5, 5, 2, 5, 7, 2, L); box(k, 10, 5, 2, 10, 7, 2, L); box(k, 5, 5, 5, 5, 7, 5, L); box(k, 10, 5, 5, 10, 7, 5, L);
    put(k, L, 6, 6, 2); put(k, L, 9, 6, 2); put(k, L, 6, 6, 5); put(k, L, 9, 6, 5);
    box(k, 5, 4, 3, 6, 4, 4, L); box(k, 9, 4, 3, 10, 4, 4, L);
    put(k, S, 5, 4, 2); put(k, S, 5, 4, 5); put(k, S, 10, 4, 2); put(k, S, 10, 4, 5);
    if (west->open[D_SOUTH]) water(k, 3, 1, 0, 4, 2, 0);
    if (west->open[D_NORTH]) water(k, 3, 1, 7, 4, 2, 7);
    if (west->open[D_WEST]) water(k, 0, 1, 3, 0, 2, 4);
    if (east->open[D_SOUTH]) water(k, 11, 1, 0, 12, 2, 0);
    if (east->open[D_NORTH]) water(k, 11, 1, 7, 12, 2, 7);
    if (east->open[D_EAST]) water(k, 15, 1, 3, 15, 2, 4);
    if (wu->open[D_SOUTH]) water(k, 3, 5, 0, 4, 6, 0);
    if (wu->open[D_NORTH]) water(k, 3, 5, 7, 4, 6, 7);
    if (wu->open[D_WEST]) water(k, 0, 5, 3, 0, 6, 4);
    if (eu->open[D_SOUTH]) water(k, 11, 5, 0, 12, 6, 0);
    if (eu->open[D_NORTH]) water(k, 11, 5, 7, 12, 6, 7);
    if (eu->open[D_EAST]) water(k, 15, 5, 3, 15, 6, 4);
}

static void dy_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *r = d->room;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L;
    if (r->index / 25 > 0) default_floor(k, 0, 0, r->open[D_DOWN]);
    Room *above = r->conn[D_UP];
    if (!above->conn[D_UP]) fill_only(k, 1, 8, 1, 6, 8, 6, G);
    box(k, 0, 4, 0, 0, 4, 7, L); box(k, 7, 4, 0, 7, 4, 7, L); box(k, 1, 4, 0, 6, 4, 0, L); box(k, 1, 4, 7, 6, 4, 7, L);
    box(k, 2, 4, 1, 2, 4, 2, L); box(k, 1, 4, 2, 1, 4, 2, L); box(k, 5, 4, 1, 5, 4, 2, L); box(k, 6, 4, 2, 6, 4, 2, L);
    box(k, 2, 4, 5, 2, 4, 6, L); box(k, 1, 4, 5, 1, 4, 5, L); box(k, 5, 4, 5, 5, 4, 6, L); box(k, 6, 4, 5, 6, 4, 5, L);
    Room *def = r;
    for (int y = 1; y <= 5; y += 4) {
        if (def->open[D_SOUTH]) { box(k, 2, y, 0, 2, y + 2, 0, L); box(k, 5, y, 0, 5, y + 2, 0, L); box(k, 3, y + 2, 0, 4, y + 2, 0, L); }
        else { box(k, 0, y, 0, 7, y + 2, 0, L); box(k, 0, y + 1, 0, 7, y + 1, 0, G); }
        if (def->open[D_NORTH]) { box(k, 2, y, 7, 2, y + 2, 7, L); box(k, 5, y, 7, 5, y + 2, 7, L); box(k, 3, y + 2, 7, 4, y + 2, 7, L); }
        else { box(k, 0, y, 7, 7, y + 2, 7, L); box(k, 0, y + 1, 7, 7, y + 1, 7, G); }
        if (def->open[D_WEST]) { box(k, 0, y, 2, 0, y + 2, 2, L); box(k, 0, y, 5, 0, y + 2, 5, L); box(k, 0, y + 2, 3, 0, y + 2, 4, L); }
        else { box(k, 0, y, 0, 0, y + 2, 7, L); box(k, 0, y + 1, 0, 0, y + 1, 7, G); }
        if (def->open[D_EAST]) { box(k, 7, y, 2, 7, y + 2, 2, L); box(k, 7, y, 5, 7, y + 2, 5, L); box(k, 7, y + 2, 3, 7, y + 2, 4, L); }
        else { box(k, 7, y, 0, 7, y + 2, 7, L); box(k, 7, y + 1, 0, 7, y + 1, 7, G); }
        def = above;
    }
}

static void dyz_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *south = d->room, *north = south->conn[D_NORTH];
    Room *nu = north->conn[D_UP], *su = south->conn[D_UP];
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L;
    if (south->index / 25 > 0) { default_floor(k, 0, 8, north->open[D_DOWN]); default_floor(k, 0, 0, south->open[D_DOWN]); }
    if (!su->conn[D_UP]) fill_only(k, 1, 8, 1, 6, 8, 7, G);
    if (!nu->conn[D_UP]) fill_only(k, 1, 8, 8, 6, 8, 14, G);
    for (int y = 1; y <= 7; y++) {
        int bl = (y == 2 || y == 6) ? G : L;
        box(k, 0, y, 0, 0, y, 15, bl); box(k, 7, y, 0, 7, y, 15, bl); box(k, 1, y, 0, 6, y, 0, bl); box(k, 1, y, 15, 6, y, 15, bl);
    }
    for (int y = 1; y <= 7; y++) {
        int bl = (y == 2 || y == 6) ? k->S : k->D;
        box(k, 3, y, 7, 4, y, 8, bl);
    }
    if (south->open[D_SOUTH]) water(k, 3, 1, 0, 4, 2, 0);
    if (south->open[D_EAST]) water(k, 7, 1, 3, 7, 2, 4);
    if (south->open[D_WEST]) water(k, 0, 1, 3, 0, 2, 4);
    if (north->open[D_NORTH]) water(k, 3, 1, 15, 4, 2, 15);
    if (north->open[D_WEST]) water(k, 0, 1, 11, 0, 2, 12);
    if (north->open[D_EAST]) water(k, 7, 1, 11, 7, 2, 12);
    if (su->open[D_SOUTH]) water(k, 3, 5, 0, 4, 6, 0);
    if (su->open[D_EAST]) { water(k, 7, 5, 3, 7, 6, 4); box(k, 5, 4, 2, 6, 4, 5, L); box(k, 6, 1, 2, 6, 3, 2, L); box(k, 6, 1, 5, 6, 3, 5, L); }
    if (su->open[D_WEST]) { water(k, 0, 5, 3, 0, 6, 4); box(k, 1, 4, 2, 2, 4, 5, L); box(k, 1, 1, 2, 1, 3, 2, L); box(k, 1, 1, 5, 1, 3, 5, L); }
    if (nu->open[D_NORTH]) water(k, 3, 5, 15, 4, 6, 15);
    if (nu->open[D_WEST]) { water(k, 0, 5, 11, 0, 6, 12); box(k, 1, 4, 10, 2, 4, 13, L); box(k, 1, 1, 10, 1, 3, 10, L); box(k, 1, 1, 13, 1, 3, 13, L); }
    if (nu->open[D_EAST]) { water(k, 7, 5, 11, 7, 6, 12); box(k, 5, 4, 10, 6, 4, 13, L); box(k, 6, 1, 10, 6, 3, 10, L); box(k, 6, 1, 13, 6, 3, 13, L); }
}

static void dz_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *south = d->room, *north = south->conn[D_NORTH];
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L, S = k->S;
    if (south->index / 25 > 0) { default_floor(k, 0, 8, north->open[D_DOWN]); default_floor(k, 0, 0, south->open[D_DOWN]); }
    if (!south->conn[D_UP]) fill_only(k, 1, 4, 1, 6, 4, 7, G);
    if (!north->conn[D_UP]) fill_only(k, 1, 4, 8, 6, 4, 14, G);
    box(k, 0, 3, 0, 0, 3, 15, L); box(k, 7, 3, 0, 7, 3, 15, L); box(k, 1, 3, 0, 7, 3, 0, L); box(k, 1, 3, 15, 6, 3, 15, L);
    box(k, 0, 2, 0, 0, 2, 15, G); box(k, 7, 2, 0, 7, 2, 15, G); box(k, 1, 2, 0, 7, 2, 0, G); box(k, 1, 2, 15, 6, 2, 15, G);
    box(k, 0, 1, 0, 0, 1, 15, L); box(k, 7, 1, 0, 7, 1, 15, L); box(k, 1, 1, 0, 7, 1, 0, L); box(k, 1, 1, 15, 6, 1, 15, L);
    box(k, 1, 1, 1, 1, 1, 2, L); box(k, 6, 1, 1, 6, 1, 2, L); box(k, 1, 3, 1, 1, 3, 2, L); box(k, 6, 3, 1, 6, 3, 2, L);
    box(k, 1, 1, 13, 1, 1, 14, L); box(k, 6, 1, 13, 6, 1, 14, L); box(k, 1, 3, 13, 1, 3, 14, L); box(k, 6, 3, 13, 6, 3, 14, L);
    box(k, 2, 1, 6, 2, 3, 6, L); box(k, 5, 1, 6, 5, 3, 6, L); box(k, 2, 1, 9, 2, 3, 9, L); box(k, 5, 1, 9, 5, 3, 9, L);
    box(k, 3, 2, 6, 4, 2, 6, L); box(k, 3, 2, 9, 4, 2, 9, L); box(k, 2, 2, 7, 2, 2, 8, L); box(k, 5, 2, 7, 5, 2, 8, L);
    put(k, S, 2, 2, 5); put(k, S, 5, 2, 5); put(k, S, 2, 2, 10); put(k, S, 5, 2, 10);
    put(k, L, 2, 3, 5); put(k, L, 5, 3, 5); put(k, L, 2, 3, 10); put(k, L, 5, 3, 10);
    if (south->open[D_SOUTH]) water(k, 3, 1, 0, 4, 2, 0);
    if (south->open[D_EAST]) water(k, 7, 1, 3, 7, 2, 4);
    if (south->open[D_WEST]) water(k, 0, 1, 3, 0, 2, 4);
    if (north->open[D_NORTH]) water(k, 3, 1, 15, 4, 2, 15);
    if (north->open[D_WEST]) water(k, 0, 1, 11, 0, 2, 12);
    if (north->open[D_EAST]) water(k, 7, 1, 11, 7, 2, 12);
}

static void entry_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *r = d->room;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int L = k->L;
    box(k, 0, 3, 0, 2, 3, 7, L); box(k, 5, 3, 0, 7, 3, 7, L); box(k, 0, 2, 0, 1, 2, 7, L); box(k, 6, 2, 0, 7, 2, 7, L);
    box(k, 0, 1, 0, 0, 1, 7, L); box(k, 7, 1, 0, 7, 1, 7, L); box(k, 0, 1, 7, 7, 3, 7, L); box(k, 1, 1, 0, 2, 3, 0, L); box(k, 5, 1, 0, 6, 3, 0, L);
    if (r->open[D_NORTH]) water(k, 3, 1, 7, 4, 2, 7);
    if (r->open[D_WEST]) water(k, 0, 1, 3, 1, 2, 4);
    if (r->open[D_EAST]) water(k, 6, 1, 3, 7, 2, 4);
}

static void pent_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L, D = k->D, S = k->S;
    box(k, 2, -1, 2, 11, -1, 11, L);
    box(k, 0, -1, 0, 1, -1, 11, G); box(k, 12, -1, 0, 13, -1, 11, G); box(k, 2, -1, 0, 11, -1, 1, G); box(k, 2, -1, 12, 11, -1, 13, G);
    box(k, 0, 0, 0, 0, 0, 13, L); box(k, 13, 0, 0, 13, 0, 13, L); box(k, 1, 0, 0, 12, 0, 0, L); box(k, 1, 0, 13, 12, 0, 13, L);
    for (int i = 2; i <= 11; i += 3) { put(k, S, 0, 0, i); put(k, S, 13, 0, i); put(k, S, i, 0, 0); }
    box(k, 2, 0, 3, 4, 0, 9, L); box(k, 9, 0, 3, 11, 0, 9, L); box(k, 4, 0, 9, 9, 0, 11, L);
    put(k, L, 5, 0, 8); put(k, L, 8, 0, 8); put(k, L, 10, 0, 10); put(k, L, 3, 0, 10);
    box(k, 3, 0, 3, 3, 0, 7, D); box(k, 10, 0, 3, 10, 0, 7, D); box(k, 6, 0, 10, 7, 0, 10, D);
    int x = 3;
    for (int i = 0; i < 2; i++) { for (int z = 2; z <= 8; z += 3) box(k, x, 0, z, x, 2, z, L); x = 10; }
    box(k, 5, 0, 10, 5, 2, 10, L); box(k, 8, 0, 10, 8, 2, 10, L);
    box(k, 6, -1, 7, 7, -1, 8, D);
    water(k, 6, -1, 3, 7, -1, 4);
    spawn_elder(k, 6, 1, 6);
}

static void simple_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *r = d->room;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L, D = k->D, S = k->S;
    if (r->index / 25 > 0) default_floor(k, 0, 0, r->open[D_DOWN]);
    if (!r->conn[D_UP]) fill_only(k, 1, 4, 1, 6, 4, 6, G);
    int pillar = d->design != 0 && rs_bool(c->rs) && !r->open[D_DOWN] && !r->open[D_UP] && room_count_openings(r) > 1;
    if (d->design == 0) {
        box(k, 0, 1, 0, 2, 1, 2, L); box(k, 0, 3, 0, 2, 3, 2, L); box(k, 0, 2, 0, 0, 2, 2, G); box(k, 1, 2, 0, 2, 2, 0, G); put(k, S, 1, 2, 1);
        box(k, 5, 1, 0, 7, 1, 2, L); box(k, 5, 3, 0, 7, 3, 2, L); box(k, 7, 2, 0, 7, 2, 2, G); box(k, 5, 2, 0, 6, 2, 0, G); put(k, S, 6, 2, 1);
        box(k, 0, 1, 5, 2, 1, 7, L); box(k, 0, 3, 5, 2, 3, 7, L); box(k, 0, 2, 5, 0, 2, 7, G); box(k, 1, 2, 7, 2, 2, 7, G); put(k, S, 1, 2, 6);
        box(k, 5, 1, 5, 7, 1, 7, L); box(k, 5, 3, 5, 7, 3, 7, L); box(k, 7, 2, 5, 7, 2, 7, G); box(k, 5, 2, 7, 6, 2, 7, G); put(k, S, 6, 2, 6);
        if (r->open[D_SOUTH]) box(k, 3, 3, 0, 4, 3, 0, L);
        else { box(k, 3, 3, 0, 4, 3, 1, L); box(k, 3, 2, 0, 4, 2, 0, G); box(k, 3, 1, 0, 4, 1, 1, L); }
        if (r->open[D_NORTH]) box(k, 3, 3, 7, 4, 3, 7, L);
        else { box(k, 3, 3, 6, 4, 3, 7, L); box(k, 3, 2, 7, 4, 2, 7, G); box(k, 3, 1, 6, 4, 1, 7, L); }
        if (r->open[D_WEST]) box(k, 0, 3, 3, 0, 3, 4, L);
        else { box(k, 0, 3, 3, 1, 3, 4, L); box(k, 0, 2, 3, 0, 2, 4, G); box(k, 0, 1, 3, 1, 1, 4, L); }
        if (r->open[D_EAST]) box(k, 7, 3, 3, 7, 3, 4, L);
        else { box(k, 6, 3, 3, 7, 3, 4, L); box(k, 7, 2, 3, 7, 2, 4, G); box(k, 6, 1, 3, 7, 1, 4, L); }
    } else if (d->design == 1) {
        box(k, 2, 1, 2, 2, 3, 2, L); box(k, 2, 1, 5, 2, 3, 5, L); box(k, 5, 1, 5, 5, 3, 5, L); box(k, 5, 1, 2, 5, 3, 2, L);
        put(k, S, 2, 2, 2); put(k, S, 2, 2, 5); put(k, S, 5, 2, 5); put(k, S, 5, 2, 2);
        box(k, 0, 1, 0, 1, 3, 0, L); box(k, 0, 1, 1, 0, 3, 1, L); box(k, 0, 1, 7, 1, 3, 7, L); box(k, 0, 1, 6, 0, 3, 6, L);
        box(k, 6, 1, 7, 7, 3, 7, L); box(k, 7, 1, 6, 7, 3, 6, L); box(k, 6, 1, 0, 7, 3, 0, L); box(k, 7, 1, 1, 7, 3, 1, L);
        put(k, G, 1, 2, 0); put(k, G, 0, 2, 1); put(k, G, 1, 2, 7); put(k, G, 0, 2, 6);
        put(k, G, 6, 2, 7); put(k, G, 7, 2, 6); put(k, G, 6, 2, 0); put(k, G, 7, 2, 1);
        if (!r->open[D_SOUTH]) { box(k, 1, 3, 0, 6, 3, 0, L); box(k, 1, 2, 0, 6, 2, 0, G); box(k, 1, 1, 0, 6, 1, 0, L); }
        if (!r->open[D_NORTH]) { box(k, 1, 3, 7, 6, 3, 7, L); box(k, 1, 2, 7, 6, 2, 7, G); box(k, 1, 1, 7, 6, 1, 7, L); }
        if (!r->open[D_WEST]) { box(k, 0, 3, 1, 0, 3, 6, L); box(k, 0, 2, 1, 0, 2, 6, G); box(k, 0, 1, 1, 0, 1, 6, L); }
        if (!r->open[D_EAST]) { box(k, 7, 3, 1, 7, 3, 6, L); box(k, 7, 2, 1, 7, 2, 6, G); box(k, 7, 1, 1, 7, 1, 6, L); }
    } else if (d->design == 2) {
        box(k, 0, 1, 0, 0, 1, 7, L); box(k, 7, 1, 0, 7, 1, 7, L); box(k, 1, 1, 0, 6, 1, 0, L); box(k, 1, 1, 7, 6, 1, 7, L);
        box(k, 0, 2, 0, 0, 2, 7, D); box(k, 7, 2, 0, 7, 2, 7, D); box(k, 1, 2, 0, 6, 2, 0, D); box(k, 1, 2, 7, 6, 2, 7, D);
        box(k, 0, 3, 0, 0, 3, 7, L); box(k, 7, 3, 0, 7, 3, 7, L); box(k, 1, 3, 0, 6, 3, 0, L); box(k, 1, 3, 7, 6, 3, 7, L);
        box(k, 0, 1, 3, 0, 2, 4, D); box(k, 7, 1, 3, 7, 2, 4, D); box(k, 3, 1, 0, 4, 2, 0, D); box(k, 3, 1, 7, 4, 2, 7, D);
        if (r->open[D_SOUTH]) water(k, 3, 1, 0, 4, 2, 0);
        if (r->open[D_NORTH]) water(k, 3, 1, 7, 4, 2, 7);
        if (r->open[D_WEST]) water(k, 0, 1, 3, 0, 2, 4);
        if (r->open[D_EAST]) water(k, 7, 1, 3, 7, 2, 4);
    }
    if (pillar) { box(k, 3, 1, 3, 4, 1, 4, L); box(k, 3, 2, 3, 4, 2, 4, G); box(k, 3, 3, 3, 4, 3, 4, L); }
}

static void stop_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data; Room *r = d->room;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int G = k->G, L = k->L, D = k->D;
    if (r->index / 25 > 0) default_floor(k, 0, 0, r->open[D_DOWN]);
    if (!r->conn[D_UP]) fill_only(k, 1, 4, 1, 6, 4, 6, G);
    for (int x = 1; x <= 6; x++) for (int z = 1; z <= 6; z++) {
        if (rs_bound(c->rs, 3) != 0) {
            int y0 = 2 + (rs_bound(c->rs, 4) == 0 ? 0 : 1);
            box(k, x, y0, z, x, 3, z, k->SPONGE);
        }
    }
    box(k, 0, 1, 0, 0, 1, 7, L); box(k, 7, 1, 0, 7, 1, 7, L); box(k, 1, 1, 0, 6, 1, 0, L); box(k, 1, 1, 7, 6, 1, 7, L);
    box(k, 0, 2, 0, 0, 2, 7, D); box(k, 7, 2, 0, 7, 2, 7, D); box(k, 1, 2, 0, 6, 2, 0, D); box(k, 1, 2, 7, 6, 2, 7, D);
    box(k, 0, 3, 0, 0, 3, 7, L); box(k, 7, 3, 0, 7, 3, 7, L); box(k, 1, 3, 0, 6, 3, 0, L); box(k, 1, 3, 7, 6, 3, 7, L);
    box(k, 0, 1, 3, 0, 2, 4, D); box(k, 7, 1, 3, 7, 2, 4, D); box(k, 3, 1, 0, 4, 2, 0, D); box(k, 3, 1, 7, 4, 2, 7, D);
    if (r->open[D_SOUTH]) water(k, 3, 1, 0, 4, 2, 0);
}

static void wing_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    OMChild *d = p->data;
    K kk; k_init(&kk, c, p); const K *k = &kk;
    int L = k->L, D = k->D, S = k->S;
    if (d->design == 0) {
        for (int i = 0; i < 4; i++) box(k, 10 - i, 3 - i, 20 - i, 12 + i, 3 - i, 20, L);
        box(k, 7, 0, 6, 15, 0, 16, L); box(k, 6, 0, 6, 6, 3, 20, L); box(k, 16, 0, 6, 16, 3, 20, L);
        box(k, 7, 1, 7, 7, 1, 20, L); box(k, 15, 1, 7, 15, 1, 20, L); box(k, 7, 1, 6, 9, 3, 6, L); box(k, 13, 1, 6, 15, 3, 6, L);
        box(k, 8, 1, 7, 9, 1, 7, L); box(k, 13, 1, 7, 14, 1, 7, L); box(k, 9, 0, 5, 13, 0, 5, L);
        box(k, 10, 0, 7, 12, 0, 7, D); box(k, 8, 0, 10, 8, 0, 12, D); box(k, 14, 0, 10, 14, 0, 12, D);
        for (int z = 18; z >= 7; z -= 3) { put(k, S, 6, 3, z); put(k, S, 16, 3, z); }
        put(k, S, 10, 0, 10); put(k, S, 12, 0, 10); put(k, S, 10, 0, 12); put(k, S, 12, 0, 12); put(k, S, 8, 3, 6); put(k, S, 14, 3, 6);
        put(k, L, 4, 2, 4); put(k, S, 4, 1, 4); put(k, L, 4, 0, 4);
        put(k, L, 18, 2, 4); put(k, S, 18, 1, 4); put(k, L, 18, 0, 4);
        put(k, L, 4, 2, 18); put(k, S, 4, 1, 18); put(k, L, 4, 0, 18);
        put(k, L, 18, 2, 18); put(k, S, 18, 1, 18); put(k, L, 18, 0, 18);
        put(k, L, 9, 7, 20); put(k, L, 13, 7, 20);
        box(k, 6, 0, 21, 7, 4, 21, L); box(k, 15, 0, 21, 16, 4, 21, L);
        spawn_elder(k, 11, 2, 16);
    } else if (d->design == 1) {
        box(k, 9, 3, 18, 13, 3, 20, L); box(k, 9, 0, 18, 9, 2, 18, L); box(k, 13, 0, 18, 13, 2, 18, L);
        int x = 9;
        for (int i = 0; i < 2; i++) { put(k, L, x, 6, 20); put(k, S, x, 5, 20); put(k, L, x, 4, 20); x = 13; }
        box(k, 7, 3, 7, 15, 3, 14, L);
        x = 10;
        for (int i = 0; i < 2; i++) {
            box(k, x, 0, 10, x, 6, 10, L); box(k, x, 0, 12, x, 6, 12, L);
            put(k, S, x, 0, 10); put(k, S, x, 0, 12); put(k, S, x, 4, 10); put(k, S, x, 4, 12);
            x = 12;
        }
        x = 8;
        for (int i = 0; i < 2; i++) { box(k, x, 0, 7, x, 2, 7, L); box(k, x, 0, 14, x, 2, 14, L); x = 14; }
        box(k, 8, 3, 8, 8, 3, 13, D); box(k, 14, 3, 8, 14, 3, 13, D);
        spawn_elder(k, 11, 5, 13);
    }
}

/* ---------------------------------------------------------------- построение (конструктор MonumentBuilding) */
static void omb_free(void *v) {
    OMB *b = v; if (!b) return;
    for (int i = 0; i < b->nch; i++) piece_free(b->ch[i]);
    free(b->ch); free(b);
}
static void child_free(void *v) { free(v); }

const PieceVT PIECE_OMB = { "minecraft:omb", omb_post, NULL, omb_free, NULL, NULL, NULL };
const PieceVT PIECE_OM_CHILD[11] = {
    [K_CORE]   = { "minecraft:omcr", core_post, NULL, child_free, NULL, NULL, NULL },
    [K_DX]     = { "minecraft:omdxr", dx_post, NULL, child_free, NULL, NULL, NULL },
    [K_DXY]    = { "minecraft:omdxyr", dxy_post, NULL, child_free, NULL, NULL, NULL },
    [K_DY]     = { "minecraft:omdyr", dy_post, NULL, child_free, NULL, NULL, NULL },
    [K_DYZ]    = { "minecraft:omdyzr", dyz_post, NULL, child_free, NULL, NULL, NULL },
    [K_DZ]     = { "minecraft:omdzr", dz_post, NULL, child_free, NULL, NULL, NULL },
    [K_ENTRY]  = { "minecraft:omentry", entry_post, NULL, child_free, NULL, NULL, NULL },
    [K_PENT]   = { "minecraft:ompenthouse", pent_post, NULL, child_free, NULL, NULL, NULL },
    [K_SIMPLE] = { "minecraft:omsimple", simple_post, NULL, child_free, NULL, NULL, NULL },
    [K_STOP]   = { "minecraft:omsimplet", stop_post, NULL, child_free, NULL, NULL, NULL },
    [K_WING]   = { "minecraft:omwr", wing_post, NULL, child_free, NULL, NULL, NULL },
};

static void add_child(OMB *b, StPiece *p) { b->ch = xrealloc(b->ch, (size_t)(b->nch + 1) * sizeof *b->ch); b->ch[b->nch++] = p; }

/* OceanMonumentPiece(type, 1, orientation, room, w, h, d): makeBoundingBox по индексу комнаты (без смещения здания) */
static StPiece *room_piece(int kind, int dir, Room *r, int w, int h, int d, int design) {
    int rx = r->index % 5, rz = r->index / 5 % 5, ry = r->index / 25;
    BB bb = sp_make_bb(0, 0, 0, dir, w * 8, h * 4, d * 8);
    switch (dir) {
    case DIR_NORTH: bb = bb_moved(bb, rx * 8, ry * 4, -(rz + d) * 8 + 1); break;
    case DIR_SOUTH: bb = bb_moved(bb, rx * 8, ry * 4, rz * 8); break;
    case DIR_WEST: bb = bb_moved(bb, -(rz + d) * 8 + 1, ry * 4, rx * 8); break;
    default: bb = bb_moved(bb, rz * 8, ry * 4, rx * 8); break;
    }
    OMChild *cd = xcalloc(1, sizeof *cd); cd->kind = kind; cd->room = r; cd->design = design;
    StPiece *p = piece_new(&PIECE_OM_CHILD[kind], bb, -1, 1, cd);
    sp_set_orientation(p, dir);
    return p;
}
static StPiece *box_piece(int kind, int dir, BB bb, int design) {
    OMChild *cd = xcalloc(1, sizeof *cd); cd->kind = kind; cd->design = design;
    StPiece *p = piece_new(&PIECE_OM_CHILD[kind], bb, -1, 1, cd);
    sp_set_orientation(p, dir);
    return p;
}

/* generateRoomGraph: возвращает список комнат (перемешанный; в конце крыша и крылья) в out[], число — результат */
static int gen_room_graph(OMB *b, RS *rs, Room **src_out, Room **core_out, Room **out) {
    Room *grid[75] = { 0 };
    for (int i = 0; i < 78; i++) memset(&b->rooms[i], 0, sizeof b->rooms[i]);
#define MK(x, y, z) do { int pos_ = room_index(x, y, z); b->rooms[pos_].index = pos_; grid[pos_] = &b->rooms[pos_]; b->exists[pos_] = 1; } while (0)
    for (int x = 0; x < 5; x++) for (int z = 0; z < 4; z++) MK(x, 0, z);
    for (int x = 0; x < 5; x++) for (int z = 0; z < 4; z++) MK(x, 1, z);
    for (int x = 1; x < 4; x++) for (int z = 0; z < 2; z++) MK(x, 2, z);
#undef MK
    Room *source = grid[room_index(2, 0, 0)];
    for (int x = 0; x < 5; x++) for (int z = 0; z < 5; z++) for (int y = 0; y < 3; y++) {
        int pos = room_index(x, y, z);
        if (!grid[pos]) continue;
        for (int d = 0; d < 6; d++) {
            int nx = x + DIR_DX[d], ny = y + DIR_DY[d], nz = z + DIR_DZ[d];
            if (nx < 0 || nx >= 5 || nz < 0 || nz >= 5 || ny < 0 || ny >= 3) continue;
            Room *n = grid[room_index(nx, ny, nz)];
            if (!n) continue;
            room_connect(grid[pos], nz == z ? d : (d ^ 1), n);
        }
    }
    Room *roof = &b->rooms[75], *lw = &b->rooms[76], *rw = &b->rooms[77];
    roof->index = 1003; lw->index = 1001; rw->index = 1002;
    room_connect(grid[room_index(2, 2, 0)], D_UP, roof);
    room_connect(grid[room_index(0, 1, 0)], D_SOUTH, lw);
    room_connect(grid[room_index(4, 1, 0)], D_SOUTH, rw);
    roof->claimed = lw->claimed = rw->claimed = 1;
    source->is_source = 1;
    Room *core = grid[room_index(rs_bound(rs, 4), 0, 2)];
    core->claimed = 1;
    core->conn[D_EAST]->claimed = 1;
    core->conn[D_NORTH]->claimed = 1;
    core->conn[D_EAST]->conn[D_NORTH]->claimed = 1;
    core->conn[D_UP]->claimed = 1;
    core->conn[D_EAST]->conn[D_UP]->claimed = 1;
    core->conn[D_NORTH]->conn[D_UP]->claimed = 1;
    core->conn[D_EAST]->conn[D_NORTH]->conn[D_UP]->claimed = 1;
    int n = 0;
    for (int i = 0; i < 75; i++) if (grid[i]) { room_update_openings(grid[i]); out[n++] = grid[i]; }
    room_update_openings(roof);
    for (int i = n; i > 1; i--) { int sw = rs_bound(rs, i); Room *t = out[i - 1]; out[i - 1] = out[sw]; out[sw] = t; }   /* Util.shuffle */
    int scan = 1;
    for (int i = 0; i < n; i++) {
        Room *def = out[i];
        int close = 0, attempt = 0;
        while (close < 2 && attempt < 5) {
            attempt++;
            int f = rs_bound(rs, 6);
            if (!def->open[f]) continue;
            int of = f ^ 1;
            def->open[f] = 0; def->conn[f]->open[of] = 0;
            if (room_find_source(def, scan++) && room_find_source(def->conn[f], scan++)) close++;
            else { def->open[f] = 1; def->conn[f]->open[of] = 1; }
        }
    }
    out[n++] = roof; out[n++] = lw; out[n++] = rw;
    *src_out = source; *core_out = core;
    return n;
}

static StPiece *omb_new(RS *rs, int west, int north, int dir) {
    OMB *b = xcalloc(1, sizeof *b);
    StPiece *p = piece_new(&PIECE_OMB, sp_make_bb(west, 39, north, dir, 58, 23, 58), -1, 0, b);
    sp_set_orientation(p, dir);
    Room *list[80], *source, *core;
    int n = gen_room_graph(b, rs, &source, &core, list);
    source->claimed = 1;
    add_child(b, room_piece(K_ENTRY, dir, source, 1, 1, 1, 0));
    add_child(b, room_piece(K_CORE, dir, core, 2, 2, 2, 0));
    for (int i = 0; i < n; i++) {
        Room *r = list[i];
        if (r->claimed || r->index >= 75) continue;
        Room *e = r->conn[D_EAST], *u = r->conn[D_UP], *no = r->conn[D_NORTH];
        if (r->open[D_EAST] && !e->claimed && r->open[D_UP] && !u->claimed && e->open[D_UP] && !e->conn[D_UP]->claimed) {        /* FitDoubleXYRoom */
            r->claimed = 1; e->claimed = 1; u->claimed = 1; e->conn[D_UP]->claimed = 1;
            add_child(b, room_piece(K_DXY, dir, r, 2, 2, 1, 0));
        } else if (r->open[D_NORTH] && !no->claimed && r->open[D_UP] && !u->claimed && no->open[D_UP] && !no->conn[D_UP]->claimed) {   /* FitDoubleYZRoom */
            r->claimed = 1; no->claimed = 1; u->claimed = 1; no->conn[D_UP]->claimed = 1;
            add_child(b, room_piece(K_DYZ, dir, r, 1, 2, 2, 0));
        } else if (r->open[D_NORTH] && !no->claimed) {                                                       /* FitDoubleZRoom */
            Room *s = r;
            if (!r->open[D_NORTH] || r->conn[D_NORTH]->claimed) s = r->conn[D_SOUTH];
            s->claimed = 1; s->conn[D_NORTH]->claimed = 1;
            add_child(b, room_piece(K_DZ, dir, s, 1, 1, 2, 0));
        } else if (r->open[D_EAST] && !e->claimed) {                                                         /* FitDoubleXRoom */
            r->claimed = 1; e->claimed = 1;
            add_child(b, room_piece(K_DX, dir, r, 2, 1, 1, 0));
        } else if (r->open[D_UP] && !u->claimed) {                                                           /* FitDoubleYRoom */
            r->claimed = 1; u->claimed = 1;
            add_child(b, room_piece(K_DY, dir, r, 1, 2, 1, 0));
        } else if (!r->open[D_WEST] && !r->open[D_EAST] && !r->open[D_NORTH] && !r->open[D_SOUTH] && !r->open[D_UP]) {   /* FitSimpleTopRoom */
            r->claimed = 1;
            add_child(b, room_piece(K_STOP, dir, r, 1, 1, 1, 0));
        } else {                                                                                             /* FitSimpleRoom */
            r->claimed = 1;
            add_child(b, room_piece(K_SIMPLE, dir, r, 1, 1, 1, rs_bound(rs, 3)));
        }
    }
    int ox = sp_wx(p, 9, 22), oy = sp_wy(p, 0), oz = sp_wz(p, 9, 22);
    for (int i = 0; i < b->nch; i++) b->ch[i]->bb = bb_moved(b->ch[i]->bb, ox, oy, oz);
#define WP(x, y, z) sp_wx(p, x, z), sp_wy(p, y), sp_wz(p, x, z)
    BB left = bb_from_corners(WP(1, 1, 1), WP(23, 8, 21));
    BB right = bb_from_corners(WP(34, 1, 1), WP(56, 8, 21));
    BB pent = bb_from_corners(WP(22, 13, 22), WP(35, 17, 35));
#undef WP
    i32 wing = rs_int(rs);
    add_child(b, box_piece(K_WING, dir, left, wing & 1)); wing = (i32)((u32)wing + 1u);
    add_child(b, box_piece(K_WING, dir, right, wing & 1));
    add_child(b, box_piece(K_PENT, dir, pent, 0));
    for (int i = 0; i < b->nch; i++) b->ch[i]->bb_init = b->ch[i]->bb;
    return p;
}

/* ---------------------------------------------------------------- структура */
typedef struct OMCfg { u8 *surround; int nb; } OMCfg;   /* #required_ocean_monument_surrounding */

static void tag_biomes(const McGen *g, const char *tag_id, u8 *mask, int depth) {
    if (depth > 8) return;
    const char *cpos = strchr(tag_id, ':'); const char *ns = cpos ? tag_id : "minecraft", *path = cpos ? cpos + 1 : tag_id; int nsl = cpos ? (int)(cpos - tag_id) : 9;
    char file[1024]; snprintf(file, sizeof file, "%s/data/%.*s/tags/worldgen/biome/%s.json", g->pack, nsl, ns, path);
    char err[128]; JsDoc *d = js_parse_file(file, err, sizeof err); if (!d) return;
    const Js *vals = js_get(js_root(d), "values");
    for (int i = 0; js_is_arr(vals) && i < vals->n; i++) {
        const Js *e = vals->items[i]; const char *s = js_is_str(e) ? e->s : js_str(js_get(e, "id"), NULL);
        if (!s) continue;
        if (s[0] == '#') tag_biomes(g, s + 1, mask, depth + 1);
        else { int bi = gen_biome_id(g, s); if (bi >= 0 && bi < g->nbiomes) mask[bi] = 1; }
    }
    js_free(d);
}
static void *om_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) {
    (void)cfg; (void)err; (void)errlen;
    OMCfg *k = xcalloc(1, sizeof *k);
    k->nb = sw->g->nbiomes;
    k->surround = xcalloc((size_t)(k->nb ? k->nb : 1), 1);
    tag_biomes(sw->g, "minecraft:required_ocean_monument_surrounding", k->surround, 0);
    return k;
}
static int om_find(GenCtx *c, const void *cfg, Stub *out) {
    const OMCfg *k = cfg;
    int x = c->cx * 16 + 9, z = c->cz * 16 + 9, y = c->w->sea_level, r = 29;
    int x0 = (x - r) >> 2, y0 = (y - r) >> 2, z0 = (z - r) >> 2, x1 = (x + r) >> 2, y1 = (y + r) >> 2, z1 = (z + r) >> 2;
    for (int qz = z0; qz <= z1; qz++) for (int qx = x0; qx <= x1; qx++) for (int qy = y0; qy <= y1; qy++) {
        int b = gen_biome_quart(c, qx, qy, qz);
        if (b < 0 || b >= k->nb || !k->surround[b]) return 0;
    }
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;          /* onTopOfChunkCenter(OCEAN_FLOOR_WG) */
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, HM_OCEAN_FLOOR_WG); out->state = NULL;
    return 1;
}
static int om_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    int dir = HD[rs_bound(&c->rs, 4)];                       /* Direction.Plane.HORIZONTAL.getRandomDirection */
    pvec_push(out, omb_new(&c->rs, c->cx * 16 - 29, c->cz * 16 - 29, dir));
    return 1;
}
static void om_free_cfg(void *v) { OMCfg *k = v; if (k) { free(k->surround); free(k); } }

const StructType STRUCT_OCEAN_MONUMENT = { "minecraft:ocean_monument", om_parse, om_find, om_build, NULL, om_free_cfg, NULL };

void structures_register_ocean_monument(void) {
    structure_register_type(&STRUCT_OCEAN_MONUMENT);
    piece_register_type(&PIECE_OMB);
    for (int i = 0; i < 11; i++) piece_register_type(&PIECE_OM_CHILD[i]);
}

/* structures/woodland_mansion.c — WoodlandMansionStructure / WoodlandMansionPieces (minecraft:woodland_mansion, часть minecraft:wmp).
 *
 * Старт: точка (чанк·16+7, чанк·16+7), случайный поворот, высота — самая низкая из угловых высот WORLD_SURFACE_WG в квадрате 5×5
 * (getLowestYIn5by5Box); ниже 60 — нет постройки. Части строятся по сетке 11×11 (MansionGrid): коридоры (recursiveCorridor),
 * сглаживание краёв (cleanEdges), разметка комнат 1×1/1×2/2×2 (identifyRooms, порядок — Util.shuffle), третий этаж от «лестничной»
 * комнаты 1×2 второго этажа. MansionPiecePlacer обходит наружные стены (traverseOuterWalls), кладёт крыши, коридоры, ковры, стены/двери
 * комнат и сами комнаты. Все части — шаблоны woodland_mansion/<имя> (TemplateStructurePiece, BlockIgnoreProcessor.STRUCTURE_BLOCK,
 * ignoreEntities, knownShape = false — после записи блоков форма обновляется по соседям, как StructureTemplate.placeInWorld).
 * Маркеры данных: Chest* — сундук (createChest), Mage/Warrior/Group of Allays — воздух на месте маркера (мобы не моделируются).
 * afterPlace: под частями колонны булыжника вниз до твёрдого блока. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const PieceVT PIECE_WOODLAND_MANSION;

/* ====================================================================== часть */
typedef struct WMData {
    const Template *t;
    char name[24];
    int tx, ty, tz;            /* templatePosition */
    int rot, mir;              /* placeSettings */
} WMData;

static void wm_move(StPiece *p, int dx, int dy, int dz) { WMData *d = p->data; d->tx += dx; d->ty += dy; d->tz += dz; }
static void wm_free(void *v) { free(v); }
static const char *ROTN[4] = { "NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90" };
static const char *MIRN[3] = { "NONE", "LEFT_RIGHT", "FRONT_BACK" };
static void wm_dump(const StPiece *p, StrBuf *o) {
    const WMData *d = p->data;
    sb_printf(o, ",\"tpl\":\"%s\",\"tpos\":[%d,%d,%d],\"trot\":\"%s\",\"tmir\":\"%s\"", d->name, d->tx, d->ty, d->tz, ROTN[d->rot & 3], MIRN[d->mir % 3]);
}

/* ---------------------------------------------------------------- классы блоков (кэш на таблицу состояний) */
typedef struct WMBlk {
    const BsTab *bs;
    int structure_block, chest, trapped_chest, barrel, dispenser, dropper, hopper, cobble;
} WMBlk;
static const WMBlk *wm_blk(const BsTab *bs) {
    static _Thread_local WMBlk k;
    if (k.bs != bs) {
        k.structure_block = bs_block_index(bs, "minecraft:structure_block"); k.chest = bs_block_index(bs, "minecraft:chest");
        k.trapped_chest = bs_block_index(bs, "minecraft:trapped_chest"); k.barrel = bs_block_index(bs, "minecraft:barrel");
        k.dispenser = bs_block_index(bs, "minecraft:dispenser"); k.dropper = bs_block_index(bs, "minecraft:dropper");
        k.hopper = bs_block_index(bs, "minecraft:hopper"); k.cobble = bs_block_index(bs, "minecraft:cobblestone");
        k.bs = bs;
    }
    return &k;
}

/* ---------------------------------------------------------------- knownShape = false: StructureTemplate.updateShapeAtEdge + updateFromNeighbourShapes */
static void wm_shape_update(StCtx *c, const TPal *pal, const WMData *d) { (void)c; (void)pal; (void)d; }

static void wm_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    WMData *d = p->data;
    McWorld *w = c->w; const BsTab *bs = c->sw->bs; const McGen *g = w->g;
    if (!d->t) return;
    const WMBlk *k = wm_blk(bs);
    TSettings s; tsettings_init(&s);
    const Proc *procs[1] = { proc_builtin(w, PB_STRUCTURE_BLOCK) };
    s.rot = d->rot; s.mir = d->mir; s.bounds = &c->chunk; s.procs = procs; s.nprocs = 1;
    const TPal *pal = tpl_palette_at(d->t, d->tx, d->ty, d->tz);
    if (!pal) return;
    if (!template_place(c->fc, w, d->t, d->tx, d->ty, d->tz, rx, ry, rz, &s, w->seeds.structures, 2)) return;
    /* placeInWorld: RandomizableContainer из шаблона получает LootTableSeed = random.nextLong(); формы блоков (knownShape = false) */
    for (int i = 0; i < pal->nb; i++) {
        const TInfo *b = &pal->b[i];
        if (!b->nbt) continue;
        int blk = g->state_block[b->state];
        if (blk != k->chest && blk != k->trapped_chest && blk != k->barrel && blk != k->dispenser && blk != k->dropper && blk != k->hopper) continue;
        int wx, wz; tpl_transform(b->x, b->z, d->mir, d->rot, 0, 0, &wx, &wz);
        if (bb_inside(&c->chunk, wx + d->tx, b->y + d->ty, wz + d->tz)) (void)rs_long(c->rs);
    }
    wm_shape_update(c, pal, d);
    /* маркеры данных (filterBlocks(STRUCTURE_BLOCK) в порядке палитры, только внутри chunkBB) */
    for (int i = 0; i < pal->nb; i++) {
        const TInfo *b = &pal->b[i];
        if (g->state_block[b->state] != k->structure_block || !b->nbt) continue;
        const char *mode = nbt_str(nbt_get(b->nbt, "mode"), "");
        if (strcmp(mode, "DATA") != 0) continue;
        int wx, wz; tpl_transform(b->x, b->z, d->mir, d->rot, 0, 0, &wx, &wz);
        wx += d->tx; wz += d->tz; int wy = b->y + d->ty;
        if (!bb_inside(&c->chunk, wx, wy, wz)) continue;
        const char *m = nbt_str(nbt_get(b->nbt, "metadata"), "");
        if (!strncmp(m, "Chest", 5)) {
            int chest = sp_st(c, "minecraft:chest");
            const char *f = NULL;
            if (!strcmp(m, "ChestWest")) f = "west"; else if (!strcmp(m, "ChestEast")) f = "east";
            else if (!strcmp(m, "ChestSouth")) f = "south"; else if (!strcmp(m, "ChestNorth")) f = "north";
            if (f) {
                int dir = dir_from_name(f);
                static const char *DN[6] = { "down", "up", "north", "south", "west", "east" };
                chest = sp_with(c, chest, "facing", DN[dir_rotate(dir, d->rot)]);
            }
            /* StructurePiece.createChest(level, chunkBB, random, pos, loot, state) */
            if (g->state_block[fc_get(c->fc, wx, wy, wz)] != k->chest) {
                fc_set(c->fc, wx, wy, wz, chest, 2);
                (void)rs_long(c->rs);
            }
        } else if (!strcmp(m, "Mage") || !strcmp(m, "Warrior") || !strcmp(m, "Group of Allays")) {
            if (!strcmp(m, "Group of Allays")) (void)rnd_next_int_bound(sp_region_random(c), 3);   /* level.getRandom().nextInt(3) + 1 */
            fc_set(c->fc, wx, wy, wz, g->st_air, 2);
        }
    }
}

const PieceVT PIECE_WOODLAND_MANSION = { "minecraft:wmp", wm_post, wm_move, wm_free, wm_dump, NULL, NULL };

/* ====================================================================== сетка особняка */
#define GW 11
typedef struct Grid { int v[GW][GW]; int out; } Grid;          /* SimpleGrid(11, 11, valueIfOutside) */
static void g_init(Grid *g, int out) { memset(g->v, 0, sizeof g->v); g->out = out; }
static int g_get(const Grid *g, int x, int y) { return x >= 0 && x < GW && y >= 0 && y < GW ? g->v[x][y] : g->out; }
static void g_set(Grid *g, int x, int y, int v) { if (x >= 0 && x < GW && y >= 0 && y < GW) g->v[x][y] = v; }
static void g_rect(Grid *g, int x0, int y0, int x1, int y1, int v) { for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) g_set(g, x, y, v); }
static void g_setif(Grid *g, int x, int y, int ifv, int v) { if (g_get(g, x, y) == ifv) g_set(g, x, y, v); }
static int g_edges(const Grid *g, int x, int y, int v) { return g_get(g, x - 1, y) == v || g_get(g, x + 1, y) == v || g_get(g, x, y + 1) == v || g_get(g, x, y - 1) == v; }
static int is_house(const Grid *g, int x, int y) { int v = g_get(g, x, y); return v == 1 || v == 2 || v == 3 || v == 4; }

enum { R1x1 = 65536, R1x2 = 131072, R2x2 = 262144, F_ORIGIN = 1048576, F_DOOR = 2097152, F_STAIRS = 4194304, F_CORR = 8388608,
       TYPE_MASK = 983040, ID_MASK = 65535 };
static const int HZ[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };     /* Direction.Plane.HORIZONTAL */
#define SX(d) DIR_DX[d]
#define SZ(d) DIR_DZ[d]

typedef struct MGrid { RS *r; Grid base, third, rooms[3]; int ex, ey; } MGrid;

static int is_room_id(const MGrid *m, int x, int y, int floor, int id) { return (g_get(&m->rooms[floor], x, y) & ID_MASK) == id; }
static int room_dir_1x2(const MGrid *m, int x, int y, int floor, int id) {
    for (int i = 0; i < 4; i++) if (is_room_id(m, x + SX(HZ[i]), y + SZ(HZ[i]), floor, id)) return HZ[i];
    return -1;
}

static void corridor(MGrid *m, Grid *g, int x, int y, int h, int depth) {
    if (depth <= 0) return;
    g_set(g, x, y, 1);
    g_setif(g, x + SX(h), y + SZ(h), 0, 1);
    for (int a = 0; a < 8; a++) {
        int nd = dir_from_2d(rs_bound(m->r, 4));
        if (nd != dir_opp(h) && (nd != DIR_EAST || !rs_bool(m->r))) {
            int nx = x + SX(h), ny = y + SZ(h);
            if (g_get(g, nx + SX(nd), ny + SZ(nd)) == 0 && g_get(g, nx + SX(nd) * 2, ny + SZ(nd) * 2) == 0) {
                corridor(m, g, x + SX(h) + SX(nd), y + SZ(h) + SZ(nd), nd, depth - 1);
                break;
            }
        }
    }
    int cw = dir_cw(h), ccw = dir_ccw(h);
    g_setif(g, x + SX(cw), y + SZ(cw), 0, 2);
    g_setif(g, x + SX(ccw), y + SZ(ccw), 0, 2);
    g_setif(g, x + SX(h) + SX(cw), y + SZ(h) + SZ(cw), 0, 2);
    g_setif(g, x + SX(h) + SX(ccw), y + SZ(h) + SZ(ccw), 0, 2);
    g_setif(g, x + SX(h) * 2, y + SZ(h) * 2, 0, 2);
    g_setif(g, x + SX(cw) * 2, y + SZ(cw) * 2, 0, 2);
    g_setif(g, x + SX(ccw) * 2, y + SZ(ccw) * 2, 0, 2);
}

static int clean_edges(Grid *g) {
    int touched = 0;
    for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
        if (g_get(g, x, y) != 0) continue;
        int dn = is_house(g, x + 1, y) + is_house(g, x - 1, y) + is_house(g, x, y + 1) + is_house(g, x, y - 1);
        if (dn >= 3) { g_set(g, x, y, 2); touched = 1; }
        else if (dn == 2) {
            int dg = is_house(g, x + 1, y + 1) + is_house(g, x - 1, y + 1) + is_house(g, x + 1, y - 1) + is_house(g, x - 1, y - 1);
            if (dg <= 1) { g_set(g, x, y, 2); touched = 1; }
        }
    }
    return touched;
}

static void identify_rooms(MGrid *m, const Grid *from, Grid *rg) {
    int pos[GW * GW][2], n = 0;
    for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) if (g_get(from, x, y) == 2) { pos[n][0] = x; pos[n][1] = y; n++; }
    for (int i = n; i > 1; i--) {                                    /* Util.shuffle */
        int j = rs_bound(m->r, i);
        int t0 = pos[i - 1][0], t1 = pos[i - 1][1]; pos[i - 1][0] = pos[j][0]; pos[i - 1][1] = pos[j][1]; pos[j][0] = t0; pos[j][1] = t1;
    }
    int id = 10;
    for (int i = 0; i < n; i++) {
        int x = pos[i][0], y = pos[i][1];
        if (g_get(rg, x, y) != 0) continue;
        int x0 = x, x1 = x, y0 = y, y1 = y, type = R1x1;
        if (g_get(rg, x + 1, y) == 0 && g_get(rg, x, y + 1) == 0 && g_get(rg, x + 1, y + 1) == 0
            && g_get(from, x + 1, y) == 2 && g_get(from, x, y + 1) == 2 && g_get(from, x + 1, y + 1) == 2) { x1++; y1++; type = R2x2; }
        else if (g_get(rg, x - 1, y) == 0 && g_get(rg, x, y + 1) == 0 && g_get(rg, x - 1, y + 1) == 0
            && g_get(from, x - 1, y) == 2 && g_get(from, x, y + 1) == 2 && g_get(from, x - 1, y + 1) == 2) { x0--; y1++; type = R2x2; }
        else if (g_get(rg, x - 1, y) == 0 && g_get(rg, x, y - 1) == 0 && g_get(rg, x - 1, y - 1) == 0
            && g_get(from, x - 1, y) == 2 && g_get(from, x, y - 1) == 2 && g_get(from, x - 1, y - 1) == 2) { x0--; y0--; type = R2x2; }
        else if (g_get(rg, x + 1, y) == 0 && g_get(from, x + 1, y) == 2) { x1++; type = R1x2; }
        else if (g_get(rg, x, y + 1) == 0 && g_get(from, x, y + 1) == 2) { y1++; type = R1x2; }
        else if (g_get(rg, x - 1, y) == 0 && g_get(from, x - 1, y) == 2) { x0--; type = R1x2; }
        else if (g_get(rg, x, y - 1) == 0 && g_get(from, x, y - 1) == 2) { y0--; type = R1x2; }
        int dx = rs_bool(m->r) ? x0 : x1;
        int dy = rs_bool(m->r) ? y0 : y1;
        int flag = F_DOOR;
        if (!g_edges(from, dx, dy, 1)) {
            dx = dx == x0 ? x1 : x0; dy = dy == y0 ? y1 : y0;
            if (!g_edges(from, dx, dy, 1)) {
                dy = dy == y0 ? y1 : y0;
                if (!g_edges(from, dx, dy, 1)) {
                    dx = dx == x0 ? x1 : x0; dy = dy == y0 ? y1 : y0;
                    if (!g_edges(from, dx, dy, 1)) { flag = 0; dx = x0; dy = y0; }
                }
            }
        }
        for (int ry = y0; ry <= y1; ry++) for (int rx = x0; rx <= x1; rx++)
            g_set(rg, rx, ry, (rx == dx && ry == dy) ? (F_ORIGIN | flag | type | id) : (type | id));
        id++;
    }
}

static void setup_third_floor(MGrid *m) {
    int pos[GW * GW][2], n = 0;
    Grid *floor = &m->rooms[1];
    for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
        int rd = g_get(floor, x, y);
        if ((rd & TYPE_MASK) == R1x2 && (rd & F_DOOR) == F_DOOR) { pos[n][0] = x; pos[n][1] = y; n++; }
    }
    if (n == 0) { g_rect(&m->third, 0, 0, GW, GW, 5); return; }
    int k = rs_bound(m->r, n);
    int px = pos[k][0], py = pos[k][1];
    int rd = g_get(floor, px, py);
    g_set(floor, px, py, rd | F_STAIRS);
    int rdir = room_dir_1x2(m, px, py, 1, rd & ID_MASK);
    if (rdir < 0) rdir = DIR_NORTH;                                   /* в игре невозможно (NPE) */
    int ex = px + SX(rdir), ey = py + SZ(rdir);
    for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
        if (!is_house(&m->base, x, y)) g_set(&m->third, x, y, 5);
        else if (x == px && y == py) g_set(&m->third, x, y, 3);
        else if (x == ex && y == ey) { g_set(&m->third, x, y, 3); g_set(&m->rooms[2], x, y, F_CORR); }
    }
    int cand[4], nc = 0;
    for (int i = 0; i < 4; i++) if (g_get(&m->third, ex + SX(HZ[i]), ey + SZ(HZ[i])) == 0) cand[nc++] = HZ[i];
    if (nc == 0) { g_rect(&m->third, 0, 0, GW, GW, 5); g_set(floor, px, py, rd); return; }
    int cd = cand[rs_bound(m->r, nc)];
    corridor(m, &m->third, ex + SX(cd), ey + SZ(cd), cd, 4);
    while (clean_edges(&m->third)) {}
}

static void mgrid_init(MGrid *m, RS *r) {
    memset(m, 0, sizeof *m);
    m->r = r; m->ex = 7; m->ey = 4;
    int ex = m->ex, ey = m->ey;
    Grid *b = &m->base; g_init(b, 5);
    g_rect(b, ex, ey, ex + 1, ey + 1, 3);
    g_rect(b, ex - 1, ey, ex - 1, ey + 1, 2);
    g_rect(b, ex + 2, ey - 2, ex + 3, ey + 3, 5);
    g_rect(b, ex + 1, ey - 2, ex + 1, ey - 1, 1);
    g_rect(b, ex + 1, ey + 2, ex + 1, ey + 3, 1);
    g_set(b, ex - 1, ey - 1, 1);
    g_set(b, ex - 1, ey + 2, 1);
    g_rect(b, 0, 0, 11, 1, 5);
    g_rect(b, 0, 9, 11, 11, 5);
    corridor(m, b, ex, ey - 2, DIR_WEST, 6);
    corridor(m, b, ex, ey + 3, DIR_WEST, 6);
    corridor(m, b, ex - 2, ey - 1, DIR_WEST, 3);
    corridor(m, b, ex - 2, ey + 2, DIR_WEST, 3);
    while (clean_edges(b)) {}
    for (int i = 0; i < 3; i++) g_init(&m->rooms[i], 5);
    identify_rooms(m, b, &m->rooms[0]);
    identify_rooms(m, b, &m->rooms[1]);
    g_rect(&m->rooms[0], ex + 1, ey, ex + 1, ey + 1, F_CORR);
    g_rect(&m->rooms[1], ex + 1, ey, ex + 1, ey + 1, F_CORR);
    g_init(&m->third, 5);
    setup_third_floor(m);
    identify_rooms(m, &m->third, &m->rooms[2]);
}

/* ====================================================================== MansionPiecePlacer */
typedef struct P3 { int x, y, z; } P3;
static P3 rel(P3 p, int dir, int n) { p.x += DIR_DX[dir] * n; p.y += DIR_DY[dir] * n; p.z += DIR_DZ[dir] * n; return p; }
static P3 up(P3 p, int n) { p.y += n; return p; }
static int rd(int rot, int dir) { return dir_rotate(dir, rot); }      /* Rotation.rotate(Direction) */
static int rot_add(int a, int b) { return (a + b) & 3; }              /* Rotation.getRotated */

typedef struct Placer { GenCtx *c; PieceVec *out; RS *r; int sx, sy; int fail; } Placer;
typedef struct PData { int rot; P3 pos; const char *wall; } PData;

static void add(Placer *pl, const char *name, P3 pos, int rot, int mir) {
    char id[64]; snprintf(id, sizeof id, "minecraft:woodland_mansion/%s", name);
    const Template *t = template_get(pl->c->w, id);
    if (!t) { pl->fail = 1; return; }
    WMData *d = xcalloc(1, sizeof *d);
    d->t = t; snprintf(d->name, sizeof d->name, "%s", name); d->tx = pos.x; d->ty = pos.y; d->tz = pos.z; d->rot = rot; d->mir = mir;
    StPiece *p = piece_new(&PIECE_WOODLAND_MANSION, tpl_bounding_box(t, pos.x, pos.y, pos.z, rot, mir, 0, 0), 2 /* NORTH */, 0, d);
    p->rot = rot; p->mir = MIR_NONE;
    pvec_push(pl->out, p);
}

/* наборы комнат этажа (FirstFloorRoomCollection / SecondFloorRoomCollection; третий = второй) */
static const char *nm(char *buf, const char *pre, int v) { snprintf(buf, 24, "%s%d", pre, v); return buf; }
static const char *r1x1(Placer *pl, int fl, char *b) { return nm(b, fl == 0 ? "1x1_a" : "1x1_b", rs_bound(pl->r, 5) + 1); }
static const char *r1x1s(Placer *pl, int fl, char *b) { (void)fl; return nm(b, "1x1_as", rs_bound(pl->r, 4) + 1); }
static const char *r1x2side(Placer *pl, int fl, int stairs, char *b) {
    if (fl == 0) return nm(b, "1x2_a", rs_bound(pl->r, 9) + 1);
    return stairs ? "1x2_c_stairs" : nm(b, "1x2_c", rs_bound(pl->r, 4) + 1);
}
static const char *r1x2front(Placer *pl, int fl, int stairs, char *b) {
    if (fl == 0) return nm(b, "1x2_b", rs_bound(pl->r, 5) + 1);
    return stairs ? "1x2_d_stairs" : nm(b, "1x2_d", rs_bound(pl->r, 5) + 1);
}
static const char *r1x2secret(Placer *pl, int fl, char *b) { return fl == 0 ? nm(b, "1x2_s", rs_bound(pl->r, 2) + 1) : nm(b, "1x2_se", rs_bound(pl->r, 1) + 1); }
static const char *r2x2(Placer *pl, int fl, char *b) { return fl == 0 ? nm(b, "2x2_a", rs_bound(pl->r, 4) + 1) : nm(b, "2x2_b", rs_bound(pl->r, 5) + 1); }

static void entrance(Placer *pl, PData *d) {
    add(pl, "entrance", rel(d->pos, rd(d->rot, DIR_WEST), 9), d->rot, MIR_NONE);
    d->pos = rel(d->pos, rd(d->rot, DIR_SOUTH), 16);
}
static void wall_piece(Placer *pl, PData *d) {
    add(pl, d->wall, rel(d->pos, rd(d->rot, DIR_EAST), 7), d->rot, MIR_NONE);
    d->pos = rel(d->pos, rd(d->rot, DIR_SOUTH), 8);
}
static void turn(Placer *pl, PData *d) {
    d->pos = rel(d->pos, rd(d->rot, DIR_SOUTH), -1);
    add(pl, "wall_corner", d->pos, d->rot, MIR_NONE);
    d->pos = rel(d->pos, rd(d->rot, DIR_SOUTH), -7);
    d->pos = rel(d->pos, rd(d->rot, DIR_WEST), -6);
    d->rot = rot_add(d->rot, ROT_CW90);
}
static void inner_turn(Placer *pl, PData *d) {
    (void)pl;
    d->pos = rel(d->pos, rd(d->rot, DIR_SOUTH), 6);
    d->pos = rel(d->pos, rd(d->rot, DIR_EAST), 8);
    d->rot = rot_add(d->rot, ROT_CCW90);
}
static void outer_walls(Placer *pl, PData *d, const Grid *g, int gd, int sx, int sy, int ex, int ey) {
    int x = sx, y = sy, sd = gd;
    int guard = 0;
    do {
        if (!is_house(g, x + SX(gd), y + SZ(gd))) {
            turn(pl, d);
            gd = dir_cw(gd);
            if (x != ex || y != ey || sd != gd) wall_piece(pl, d);
        } else if (is_house(g, x + SX(gd), y + SZ(gd)) && is_house(g, x + SX(gd) + SX(dir_ccw(gd)), y + SZ(gd) + SZ(dir_ccw(gd)))) {
            inner_turn(pl, d);
            x += SX(gd); y += SZ(gd);
            gd = dir_ccw(gd);
        } else {
            x += SX(gd); y += SZ(gd);
            if (x != ex || y != ey || sd != gd) wall_piece(pl, d);
        }
        if (++guard > 100000) { pl->fail = 1; break; }
    } while (x != ex || y != ey || sd != gd);
}

static void create_roof(Placer *pl, P3 origin, int rot, const Grid *g, const Grid *ag) {
    for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
        P3 pos = rel(rel(origin, rd(rot, DIR_SOUTH), 8 + (y - pl->sy) * 8), rd(rot, DIR_EAST), (x - pl->sx) * 8);
        int above = ag && is_house(ag, x, y);
        if (!is_house(g, x, y) || above) continue;
        add(pl, "roof", up(pos, 3), rot, MIR_NONE);
        if (!is_house(g, x + 1, y)) add(pl, "roof_front", rel(pos, rd(rot, DIR_EAST), 6), rot, MIR_NONE);
        if (!is_house(g, x - 1, y)) add(pl, "roof_front", rel(rel(pos, rd(rot, DIR_EAST), 0), rd(rot, DIR_SOUTH), 7), rot_add(rot, ROT_CW180), MIR_NONE);
        if (!is_house(g, x, y - 1)) add(pl, "roof_front", rel(pos, rd(rot, DIR_WEST), 1), rot_add(rot, ROT_CCW90), MIR_NONE);
        if (!is_house(g, x, y + 1)) add(pl, "roof_front", rel(rel(pos, rd(rot, DIR_EAST), 6), rd(rot, DIR_SOUTH), 6), rot_add(rot, ROT_CW90), MIR_NONE);
    }
    if (ag) {
        for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
            P3 pos = rel(rel(origin, rd(rot, DIR_SOUTH), 8 + (y - pl->sy) * 8), rd(rot, DIR_EAST), (x - pl->sx) * 8);
            if (!(is_house(g, x, y) && is_house(ag, x, y))) continue;
            if (!is_house(g, x + 1, y)) add(pl, "small_wall", rel(pos, rd(rot, DIR_EAST), 7), rot, MIR_NONE);
            if (!is_house(g, x - 1, y)) add(pl, "small_wall", rel(rel(pos, rd(rot, DIR_WEST), 1), rd(rot, DIR_SOUTH), 6), rot_add(rot, ROT_CW180), MIR_NONE);
            if (!is_house(g, x, y - 1)) add(pl, "small_wall", rel(rel(pos, rd(rot, DIR_WEST), 0), rd(rot, DIR_NORTH), 1), rot_add(rot, ROT_CCW90), MIR_NONE);
            if (!is_house(g, x, y + 1)) add(pl, "small_wall", rel(rel(pos, rd(rot, DIR_EAST), 6), rd(rot, DIR_SOUTH), 7), rot_add(rot, ROT_CW90), MIR_NONE);
            if (!is_house(g, x + 1, y)) {
                if (!is_house(g, x, y - 1)) add(pl, "small_wall_corner", rel(rel(pos, rd(rot, DIR_EAST), 7), rd(rot, DIR_NORTH), 2), rot, MIR_NONE);
                if (!is_house(g, x, y + 1)) add(pl, "small_wall_corner", rel(rel(pos, rd(rot, DIR_EAST), 8), rd(rot, DIR_SOUTH), 7), rot_add(rot, ROT_CW90), MIR_NONE);
            }
            if (!is_house(g, x - 1, y)) {
                if (!is_house(g, x, y - 1)) add(pl, "small_wall_corner", rel(rel(pos, rd(rot, DIR_WEST), 2), rd(rot, DIR_NORTH), 1), rot_add(rot, ROT_CCW90), MIR_NONE);
                if (!is_house(g, x, y + 1)) add(pl, "small_wall_corner", rel(rel(pos, rd(rot, DIR_WEST), 1), rd(rot, DIR_SOUTH), 8), rot_add(rot, ROT_CW180), MIR_NONE);
            }
        }
    }
    for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
        P3 pos = rel(rel(origin, rd(rot, DIR_SOUTH), 8 + (y - pl->sy) * 8), rd(rot, DIR_EAST), (x - pl->sx) * 8);
        int above = ag && is_house(ag, x, y);
        if (!is_house(g, x, y) || above) continue;
        if (!is_house(g, x + 1, y)) {
            P3 p2 = rel(pos, rd(rot, DIR_EAST), 6);
            if (!is_house(g, x, y + 1)) add(pl, "roof_corner", rel(p2, rd(rot, DIR_SOUTH), 6), rot, MIR_NONE);
            else if (is_house(g, x + 1, y + 1)) add(pl, "roof_inner_corner", rel(p2, rd(rot, DIR_SOUTH), 5), rot, MIR_NONE);
            if (!is_house(g, x, y - 1)) add(pl, "roof_corner", p2, rot_add(rot, ROT_CCW90), MIR_NONE);
            else if (is_house(g, x + 1, y - 1)) add(pl, "roof_inner_corner", rel(rel(pos, rd(rot, DIR_EAST), 9), rd(rot, DIR_NORTH), 2), rot_add(rot, ROT_CW90), MIR_NONE);
        }
        if (!is_house(g, x - 1, y)) {
            P3 p2 = rel(rel(pos, rd(rot, DIR_EAST), 0), rd(rot, DIR_SOUTH), 0);
            if (!is_house(g, x, y + 1)) add(pl, "roof_corner", rel(p2, rd(rot, DIR_SOUTH), 6), rot_add(rot, ROT_CW90), MIR_NONE);
            else if (is_house(g, x - 1, y + 1)) add(pl, "roof_inner_corner", rel(rel(p2, rd(rot, DIR_SOUTH), 8), rd(rot, DIR_WEST), 3), rot_add(rot, ROT_CCW90), MIR_NONE);
            if (!is_house(g, x, y - 1)) add(pl, "roof_corner", p2, rot_add(rot, ROT_CW180), MIR_NONE);
            else if (is_house(g, x - 1, y - 1)) add(pl, "roof_inner_corner", rel(p2, rd(rot, DIR_SOUTH), 1), rot_add(rot, ROT_CW180), MIR_NONE);
        }
    }
}

/* StructureTemplate.getZeroPositionWithTransform(pos, Mirror.NONE, rot, 7, 7) */
static P3 zero_pos(P3 z, int rot) {
    const int sx = 6, sz = 6;
    switch (rot) {
    case ROT_CCW90: z.z += sx; break;
    case ROT_CW90: z.x += sz; break;
    case ROT_CW180: z.x += sx; z.z += sz; break;
    default: break;
    }
    return z;
}
static P3 pos_rotate(P3 p, int rot) {                                 /* BlockPos.rotate */
    P3 r = p;
    switch (rot) {
    case ROT_CW90: r.x = -p.z; r.z = p.x; break;
    case ROT_CW180: r.x = -p.x; r.z = -p.z; break;
    case ROT_CCW90: r.x = p.z; r.z = -p.x; break;
    default: break;
    }
    return r;
}

static void room_1x1(Placer *pl, P3 rp, int rot, int door, int fl) {
    char b[24];
    int prot = ROT_NONE;
    const char *name = r1x1(pl, fl, b);
    if (door != DIR_EAST) {
        if (door == DIR_NORTH) prot = rot_add(prot, ROT_CCW90);
        else if (door == DIR_WEST) prot = rot_add(prot, ROT_CW180);
        else if (door == DIR_SOUTH) prot = rot_add(prot, ROT_CW90);
        else name = r1x1s(pl, fl, b);
    }
    P3 o = zero_pos((P3){ 1, 0, 0 }, prot);
    prot = rot_add(prot, rot);
    o = pos_rotate(o, rot);
    P3 pos = { rp.x + o.x, rp.y, rp.z + o.z };
    add(pl, name, pos, prot, MIR_NONE);
}

static void room_1x2(Placer *pl, P3 rp, int rot, int rdir, int door, int fl, int stairs) {
    char b[24];
    const int E = rd(rot, DIR_EAST), S = rd(rot, DIR_SOUTH), N = rd(rot, DIR_NORTH), W = rd(rot, DIR_WEST);
    if (door == DIR_EAST && rdir == DIR_SOUTH) add(pl, r1x2side(pl, fl, stairs, b), rel(rp, E, 1), rot, MIR_NONE);
    else if (door == DIR_EAST && rdir == DIR_NORTH) { P3 p = rel(rel(rp, E, 1), S, 6); add(pl, r1x2side(pl, fl, stairs, b), p, rot, MIR_LEFT_RIGHT); }
    else if (door == DIR_WEST && rdir == DIR_NORTH) { P3 p = rel(rel(rp, E, 7), S, 6); add(pl, r1x2side(pl, fl, stairs, b), p, rot_add(rot, ROT_CW180), MIR_NONE); }
    else if (door == DIR_WEST && rdir == DIR_SOUTH) add(pl, r1x2side(pl, fl, stairs, b), rel(rp, E, 7), rot, MIR_FRONT_BACK);
    else if (door == DIR_SOUTH && rdir == DIR_EAST) add(pl, r1x2side(pl, fl, stairs, b), rel(rp, E, 1), rot_add(rot, ROT_CW90), MIR_LEFT_RIGHT);
    else if (door == DIR_SOUTH && rdir == DIR_WEST) add(pl, r1x2side(pl, fl, stairs, b), rel(rp, E, 7), rot_add(rot, ROT_CW90), MIR_NONE);
    else if (door == DIR_NORTH && rdir == DIR_WEST) { P3 p = rel(rel(rp, E, 7), S, 6); add(pl, r1x2side(pl, fl, stairs, b), p, rot_add(rot, ROT_CW90), MIR_FRONT_BACK); }
    else if (door == DIR_NORTH && rdir == DIR_EAST) { P3 p = rel(rel(rp, E, 1), S, 6); add(pl, r1x2side(pl, fl, stairs, b), p, rot_add(rot, ROT_CCW90), MIR_NONE); }
    else if (door == DIR_SOUTH && rdir == DIR_NORTH) { P3 p = rel(rel(rp, E, 1), N, 8); add(pl, r1x2front(pl, fl, stairs, b), p, rot, MIR_NONE); }
    else if (door == DIR_NORTH && rdir == DIR_SOUTH) { P3 p = rel(rel(rp, E, 7), S, 14); add(pl, r1x2front(pl, fl, stairs, b), p, rot_add(rot, ROT_CW180), MIR_NONE); }
    else if (door == DIR_WEST && rdir == DIR_EAST) add(pl, r1x2front(pl, fl, stairs, b), rel(rp, E, 15), rot_add(rot, ROT_CW90), MIR_NONE);
    else if (door == DIR_EAST && rdir == DIR_WEST) { P3 p = rel(rel(rp, W, 7), S, 6); add(pl, r1x2front(pl, fl, stairs, b), p, rot_add(rot, ROT_CCW90), MIR_NONE); }
    else if (door == DIR_UP && rdir == DIR_EAST) add(pl, r1x2secret(pl, fl, b), rel(rp, E, 15), rot_add(rot, ROT_CW90), MIR_NONE);
    else if (door == DIR_UP && rdir == DIR_SOUTH) { P3 p = rel(rel(rp, E, 1), N, 0); add(pl, r1x2secret(pl, fl, b), p, rot, MIR_NONE); }
}

static void room_2x2(Placer *pl, P3 rp, int rot, int rdir, int door, int fl) {
    char b[24];
    int east = 0, south = 0, r = rot, mir = MIR_NONE;
    if (door == DIR_EAST && rdir == DIR_SOUTH) east = -7;
    else if (door == DIR_EAST && rdir == DIR_NORTH) { east = -7; south = 6; mir = MIR_LEFT_RIGHT; }
    else if (door == DIR_NORTH && rdir == DIR_EAST) { east = 1; south = 14; r = rot_add(rot, ROT_CCW90); }
    else if (door == DIR_NORTH && rdir == DIR_WEST) { east = 7; south = 14; r = rot_add(rot, ROT_CCW90); mir = MIR_LEFT_RIGHT; }
    else if (door == DIR_SOUTH && rdir == DIR_WEST) { east = 7; south = -8; r = rot_add(rot, ROT_CW90); }
    else if (door == DIR_SOUTH && rdir == DIR_EAST) { east = 1; south = -8; r = rot_add(rot, ROT_CW90); mir = MIR_LEFT_RIGHT; }
    else if (door == DIR_WEST && rdir == DIR_NORTH) { east = 15; south = 6; r = rot_add(rot, ROT_CW180); }
    else if (door == DIR_WEST && rdir == DIR_SOUTH) { east = 15; mir = MIR_FRONT_BACK; }
    P3 pos = rel(rel(rp, rd(rot, DIR_EAST), east), rd(rot, DIR_SOUTH), south);
    add(pl, r2x2(pl, fl, b), pos, r, mir);
}

static void create_mansion(Placer *pl, P3 origin, int rot, MGrid *m) {
    PData data = { rot, origin, "wall_flat" };
    PData second;
    entrance(pl, &data);
    second.pos = up(data.pos, 8); second.rot = data.rot; second.wall = "wall_window";
    const Grid *base = &m->base, *third = &m->third;
    pl->sx = m->ex + 1; pl->sy = m->ey + 1;
    int endX = m->ex + 1, endY = m->ey;
    outer_walls(pl, &data, base, DIR_SOUTH, pl->sx, pl->sy, endX, endY);
    outer_walls(pl, &second, base, DIR_SOUTH, pl->sx, pl->sy, endX, endY);
    PData td; td.pos = up(data.pos, 19); td.rot = data.rot; td.wall = "wall_window";
    int done = 0;
    for (int y = 0; y < GW && !done; y++) for (int x = GW - 1; x >= 0 && !done; x--) {
        if (is_house(third, x, y)) {
            td.pos = rel(td.pos, rd(rot, DIR_SOUTH), 8 + (y - pl->sy) * 8);
            td.pos = rel(td.pos, rd(rot, DIR_EAST), (x - pl->sx) * 8);
            wall_piece(pl, &td);
            outer_walls(pl, &td, third, DIR_SOUTH, x, y, x, y);
            done = 1;
        }
    }
    create_roof(pl, up(origin, 16), rot, base, third);
    create_roof(pl, up(origin, 27), rot, third, NULL);
    for (int fl = 0; fl < 3; fl++) {
        P3 fo = up(origin, 8 * fl + (fl == 2 ? 3 : 0));
        const Grid *rooms = &m->rooms[fl];
        const Grid *g = fl == 2 ? third : base;
        const char *south = fl == 0 ? "carpet_south_1" : "carpet_south_2";
        const char *west = fl == 0 ? "carpet_west_1" : "carpet_west_2";
        for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
            if (g_get(g, x, y) != 1) continue;
            P3 pos = rel(rel(fo, rd(rot, DIR_SOUTH), 8 + (y - pl->sy) * 8), rd(rot, DIR_EAST), (x - pl->sx) * 8);
            add(pl, "corridor_floor", pos, rot, MIR_NONE);
            if (g_get(g, x, y - 1) == 1 || (g_get(rooms, x, y - 1) & F_CORR) == F_CORR) add(pl, "carpet_north", up(rel(pos, rd(rot, DIR_EAST), 1), 1), rot, MIR_NONE);
            if (g_get(g, x + 1, y) == 1 || (g_get(rooms, x + 1, y) & F_CORR) == F_CORR)
                add(pl, "carpet_east", up(rel(rel(pos, rd(rot, DIR_SOUTH), 1), rd(rot, DIR_EAST), 5), 1), rot, MIR_NONE);
            if (g_get(g, x, y + 1) == 1 || (g_get(rooms, x, y + 1) & F_CORR) == F_CORR)
                add(pl, south, rel(rel(pos, rd(rot, DIR_SOUTH), 5), rd(rot, DIR_WEST), 1), rot, MIR_NONE);
            if (g_get(g, x - 1, y) == 1 || (g_get(rooms, x - 1, y) & F_CORR) == F_CORR)
                add(pl, west, rel(rel(pos, rd(rot, DIR_WEST), 1), rd(rot, DIR_NORTH), 1), rot, MIR_NONE);
        }
        const char *wallp = fl == 0 ? "indoors_wall_1" : "indoors_wall_2";
        const char *doorp = fl == 0 ? "indoors_door_1" : "indoors_door_2";
        for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) {
            int tfs = fl == 2 && g_get(g, x, y) == 3;
            if (!(g_get(g, x, y) == 2 || tfs)) continue;
            int rdata = g_get(rooms, x, y);
            int rtype = rdata & TYPE_MASK, rid = rdata & ID_MASK;
            tfs = tfs && (rdata & F_CORR) == F_CORR;
            int dd[4], nd = 0;
            if ((rdata & F_DOOR) == F_DOOR) for (int i = 0; i < 4; i++) if (g_get(g, x + SX(HZ[i]), y + SZ(HZ[i])) == 1) dd[nd++] = HZ[i];
            int door = -1;
            if (nd) door = dd[rs_bound(pl->r, nd)];
            else if ((rdata & F_ORIGIN) == F_ORIGIN) door = DIR_UP;
            P3 rp = rel(rel(fo, rd(rot, DIR_SOUTH), 8 + (y - pl->sy) * 8), rd(rot, DIR_EAST), -1 + (x - pl->sx) * 8);
            if (is_house(g, x - 1, y) && !is_room_id(m, x - 1, y, fl, rid)) add(pl, door == DIR_WEST ? doorp : wallp, rp, rot, MIR_NONE);
            if (g_get(g, x + 1, y) == 1 && !tfs) add(pl, door == DIR_EAST ? doorp : wallp, rel(rp, rd(rot, DIR_EAST), 8), rot, MIR_NONE);
            if (is_house(g, x, y + 1) && !is_room_id(m, x, y + 1, fl, rid)) {
                P3 p = rel(rel(rp, rd(rot, DIR_SOUTH), 7), rd(rot, DIR_EAST), 7);
                add(pl, door == DIR_SOUTH ? doorp : wallp, p, rot_add(rot, ROT_CW90), MIR_NONE);
            }
            if (g_get(g, x, y - 1) == 1 && !tfs) {
                P3 p = rel(rel(rp, rd(rot, DIR_NORTH), 1), rd(rot, DIR_EAST), 7);
                add(pl, door == DIR_NORTH ? doorp : wallp, p, rot_add(rot, ROT_CW90), MIR_NONE);
            }
            if (rtype == R1x1) room_1x1(pl, rp, rot, door, fl);
            else if (rtype == R1x2 && door >= 0) {
                int rdir = room_dir_1x2(m, x, y, fl, rid);
                room_1x2(pl, rp, rot, rdir, door, fl, (rdata & F_STAIRS) == F_STAIRS);
            } else if (rtype == R2x2 && door >= 0 && door != DIR_UP) {
                int rdir = dir_cw(door);
                if (!is_room_id(m, x + SX(rdir), y + SZ(rdir), fl, rid)) rdir = dir_opp(rdir);
                room_2x2(pl, rp, rot, rdir, door, fl);
            } else if (rtype == R2x2 && door == DIR_UP) {
                add(pl, "2x2_s1", rel(rp, rd(rot, DIR_EAST), 1), rot, MIR_NONE);
            }
        }
    }
}

/* ====================================================================== afterPlace: булыжник под особняком */
static void wm_after(StCtx *c, const StStart *s) {
    const BsTab *bs = c->sw->bs; const McGen *g = c->w->g;
    BB bb = s->bb;
    int y0 = bb.y0, miny = c->w->min_y;
    int cobble = sp_st(c, "minecraft:cobblestone");
    for (int x = c->chunk.x0; x <= c->chunk.x1; x++) for (int z = c->chunk.z0; z <= c->chunk.z1; z++) {
        if (gen_is_air(g, fc_get(c->fc, x, y0, z)) || !bb_inside(&bb, x, y0, z)) continue;
        int in = 0;
        for (int i = 0; i < s->n && !in; i++) if (bb_inside(&s->pieces[i]->bb, x, y0, z)) in = 1;
        if (!in) continue;
        for (int y = y0 - 1; y > miny; y--) {
            int st = fc_get(c->fc, x, y, z);
            if (!gen_is_air(g, st) && !(bs->flags[st] & BSF_LIQUID)) break;
            fc_set(c->fc, x, y, z, cobble, 2);
        }
    }
}

/* ====================================================================== структура */
static void *wm_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int wm_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    int bx = c->cx * 16 + 7, bz = c->cz * 16 + 7;
    if (!gen_could_exist_in_column(c, bx, bz, c->min_y - 1, c->min_y + c->height - 1)) return 0;
    int rot = rs_bound(&c->rs, 4);                                    /* Rotation.getRandom */
    int ox = 5, oz = 5;                                               /* getLowestYIn5by5Box */
    if (rot == ROT_CW90) ox = -5;
    else if (rot == ROT_CW180) { ox = -5; oz = -5; }
    else if (rot == ROT_CCW90) oz = -5;
    int y = gen_lowest_y(c, bx, bz, ox, oz);
    if (y < 60) return 0;
    out->x = bx; out->y = y; out->z = bz; out->state = (void *)(intptr_t)(rot + 1);
    return 1;
}
static int wm_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg;
    int rot = (int)(intptr_t)stub->state - 1;
    MGrid m; mgrid_init(&m, &c->rs);
    Placer pl; memset(&pl, 0, sizeof pl); pl.c = c; pl.out = out; pl.r = &c->rs;
    create_mansion(&pl, (P3){ stub->x, stub->y, stub->z }, rot, &m);
    return out->n > 0;
}
static void wm_free_cfg(void *p) { free(p); }
static void wm_free_stub(Stub *s) { s->state = NULL; }
const StructType STRUCT_WOODLAND_MANSION = { "minecraft:woodland_mansion", wm_parse, wm_find, wm_build, wm_after, wm_free_cfg, wm_free_stub };

void structures_register_woodland_mansion(void) { structure_register_type(&STRUCT_WOODLAND_MANSION); piece_register_type(&PIECE_WOODLAND_MANSION); }

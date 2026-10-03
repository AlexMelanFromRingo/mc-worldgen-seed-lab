/* structures/mineshaft.c — MineshaftStructure/MineshaftPieces (Java-кодированная постройка): типы minecraft:mineshaft (normal) и
 * minecraft:mineshaft_mesa (mesa); части MSRoom/MSCorridor/MSCrossing/MSStairs (minecraft:msroom/mscorridor/mscrossing/msstairs).
 *
 * Особенности игры, которые здесь важны:
 *  - все части строятся уже в findGenerationPoint (до проверки биома): рекурсия addChildren с проверкой столкновений, затем
 *    moveBelowSeaLevel (normal) или сдвиг центра к случайной высоте между уровнем моря и поверхностью (mesa); точка проверки биома —
 *    (середина чанка по X, 50 + dy, мин. Z чанка). Готовые части хранятся в Stub.state до build.
 *  - у комнаты и перекрёстка ориентация null: локальные координаты = мировые (sp_* с orient = −1).
 *  - canBeReplaced: доски/брёвна/забор своего типа и железная цепь не заменяются (placeBlock пропускает их).
 *  - ГСЧ при рисовании: generateMaybeBox/maybeGenerateBlock тратят числа для всех позиций; опоры (nextInt(4), факелы) — только если
 *    потолок опоры в чанке; сундук-вагонетка — nextBoolean (форма рельса) + nextLong (таблица добычи) только при успешной установке;
 *    спаунер пещерного паука (setEntityId при пустом spawnPotentials) ГСЧ не тратит; nextInt(3) для его позиции тратится в каждой секции,
 *    пока hasPlacedSpider == false (флаг общий для всех чанков: состояние части, сбрасывается reset перед прогоном региона).
 */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

enum { MS_ROOM = 0, MS_CORRIDOR, MS_CROSSING, MS_STAIRS };
enum { MST_NORMAL = 0, MST_MESA = 1 };

extern const PieceVT PIECE_MS_ROOM, PIECE_MS_CORRIDOR, PIECE_MS_CROSSING, PIECE_MS_STAIRS;

typedef struct MSData {
    int kind, type;
    /* коридор */
    int has_rails, spider, placed_spider, num;
    /* перекрёсток */
    int dir, two_floored;
    /* комната: childEntranceBoxes */
    BB *ent; int nent, cent;
} MSData;

static void ms_free(void *v) { MSData *d = v; if (d) { free(d->ent); free(d); } }
static void ms_reset(StPiece *p) { MSData *d = p->data; d->placed_spider = 0; }
static void ms_room_move(StPiece *p, int dx, int dy, int dz) { MSData *d = p->data; for (int i = 0; i < d->nent; i++) d->ent[i] = bb_moved(d->ent[i], dx, dy, dz); }
static void ms_dump(const StPiece *p, StrBuf *o) {
    const MSData *d = p->data;
    sb_printf(o, ",\"mst\":%d", d->type);
    if (d->kind == MS_CORRIDOR) sb_printf(o, ",\"hr\":%d,\"sc\":%d,\"num\":%d", d->has_rails, d->spider, d->num);
    else if (d->kind == MS_CROSSING) sb_printf(o, ",\"tf\":%d,\"d\":%d", d->two_floored, dir_to_2d(d->dir));
    else if (d->kind == MS_ROOM) {
        sb_puts(o, ",\"ent\":[");
        for (int i = 0; i < d->nent; i++) sb_printf(o, "%s[%d,%d,%d,%d,%d,%d]", i ? "," : "", d->ent[i].x0, d->ent[i].y0, d->ent[i].z0, d->ent[i].x1, d->ent[i].y1, d->ent[i].z1);
        sb_puts(o, "]");
    }
}

static MSData *ms_data(int kind, int type) { MSData *d = xcalloc(1, sizeof *d); d->kind = kind; d->type = type; d->dir = -1; return d; }

/* ====================================================================== генерация частей (StructurePiecesBuilder) */
typedef struct MSGen { PieceVec *pv; RS *r; int type; } MSGen;

static StPiece *ms_gen_add(MSGen *g, const StPiece *start, int fx, int fy, int fz, int dir, int depth);
static void ms_add_children(MSGen *g, StPiece *p, const StPiece *start);

static StPiece *ms_create_random(MSGen *g, int fx, int fy, int fz, int dir, int gd) {
    RS *r = g->r;
    int sel = rs_bound(r, 100);
    if (sel >= 80) {                                           /* MineShaftCrossing.findCrossing */
        int y1 = rs_bound(r, 4) == 0 ? 6 : 2;
        BB b;
        switch (dir) {
        case DIR_SOUTH: b = bb_make(-1, 0, 0, 3, y1, 4); break;
        case DIR_WEST: b = bb_make(-4, 0, -1, 0, y1, 3); break;
        case DIR_EAST: b = bb_make(0, 0, -1, 4, y1, 3); break;
        default: b = bb_make(-1, 0, -4, 3, y1, 0); break;
        }
        b = bb_moved(b, fx, fy, fz);
        if (pvec_collision(g->pv, &b)) return NULL;
        MSData *d = ms_data(MS_CROSSING, g->type); d->dir = dir; d->two_floored = bb_yspan(&b) > 3;
        return piece_new(&PIECE_MS_CROSSING, b, -1, gd, d);
    } else if (sel >= 70) {                                    /* MineShaftStairs.findStairs */
        BB b;
        switch (dir) {
        case DIR_SOUTH: b = bb_make(0, -5, 0, 2, 2, 8); break;
        case DIR_WEST: b = bb_make(-8, -5, 0, 0, 2, 2); break;
        case DIR_EAST: b = bb_make(0, -5, 0, 8, 2, 2); break;
        default: b = bb_make(0, -5, -8, 2, 2, 0); break;
        }
        b = bb_moved(b, fx, fy, fz);
        if (pvec_collision(g->pv, &b)) return NULL;
        StPiece *p = piece_new(&PIECE_MS_STAIRS, b, -1, gd, ms_data(MS_STAIRS, g->type));
        sp_set_orientation(p, dir);
        return p;
    } else {                                                   /* MineShaftCorridor.findCorridorSize */
        int found = 0; BB b = bb_empty();
        for (int len = rs_bound(r, 3) + 2; len > 0; len--) {
            int bl = len * 5;
            switch (dir) {
            case DIR_SOUTH: b = bb_make(0, 0, 0, 2, 2, bl - 1); break;
            case DIR_WEST: b = bb_make(-(bl - 1), 0, 0, 0, 2, 2); break;
            case DIR_EAST: b = bb_make(0, 0, 0, bl - 1, 2, 2); break;
            default: b = bb_make(0, 0, -(bl - 1), 2, 2, 0); break;
            }
            b = bb_moved(b, fx, fy, fz);
            if (!pvec_collision(g->pv, &b)) { found = 1; break; }
        }
        if (!found) return NULL;
        MSData *d = ms_data(MS_CORRIDOR, g->type);
        StPiece *p = piece_new(&PIECE_MS_CORRIDOR, b, -1, gd, d);
        sp_set_orientation(p, dir);
        d->has_rails = rs_bound(r, 3) == 0;
        d->spider = !d->has_rails && rs_bound(r, 23) == 0;
        d->num = (dir == DIR_NORTH || dir == DIR_SOUTH) ? bb_zspan(&b) / 5 : bb_xspan(&b) / 5;
        return p;
    }
}

/* MineshaftPieces.generateAndAddPiece */
static StPiece *ms_gen_add(MSGen *g, const StPiece *start, int fx, int fy, int fz, int dir, int depth) {
    if (depth > 8) return NULL;
    if (abs(fx - start->bb.x0) > 80 || abs(fz - start->bb.z0) > 80) return NULL;
    StPiece *p = ms_create_random(g, fx, fy, fz, dir, depth + 1);
    if (p) { pvec_push(g->pv, p); ms_add_children(g, p, start); }
    return p;
}

static void room_add_entrance(MSData *d, BB b) {
    if (d->nent == d->cent) { d->cent = d->cent ? d->cent * 2 : 8; d->ent = xrealloc(d->ent, (size_t)d->cent * sizeof *d->ent); }
    d->ent[d->nent++] = b;
}

static void ms_add_children(MSGen *g, StPiece *p, const StPiece *start) {
    MSData *d = p->data; RS *r = g->r;
    const int depth = p->depth;
    if (d->kind == MS_ROOM) {
        int hs = bb_yspan(&p->bb) - 3 - 1; if (hs <= 0) hs = 1;
        int xs = bb_xspan(&p->bb), zs = bb_zspan(&p->bb);
        for (int pos = 0; pos < xs; pos += 4) {
            pos += rs_bound(r, xs); if (pos + 3 > xs) break;
            int fy = p->bb.y0 + rs_bound(r, hs) + 1;
            StPiece *ch = ms_gen_add(g, start, p->bb.x0 + pos, fy, p->bb.z0 - 1, DIR_NORTH, depth);
            if (ch) room_add_entrance(d, bb_make(ch->bb.x0, ch->bb.y0, p->bb.z0, ch->bb.x1, ch->bb.y1, p->bb.z0 + 1));
        }
        for (int pos = 0; pos < xs; pos += 4) {
            pos += rs_bound(r, xs); if (pos + 3 > xs) break;
            int fy = p->bb.y0 + rs_bound(r, hs) + 1;
            StPiece *ch = ms_gen_add(g, start, p->bb.x0 + pos, fy, p->bb.z1 + 1, DIR_SOUTH, depth);
            if (ch) room_add_entrance(d, bb_make(ch->bb.x0, ch->bb.y0, p->bb.z1 - 1, ch->bb.x1, ch->bb.y1, p->bb.z1));
        }
        for (int pos = 0; pos < zs; pos += 4) {
            pos += rs_bound(r, zs); if (pos + 3 > zs) break;
            int fy = p->bb.y0 + rs_bound(r, hs) + 1;
            StPiece *ch = ms_gen_add(g, start, p->bb.x0 - 1, fy, p->bb.z0 + pos, DIR_WEST, depth);
            if (ch) room_add_entrance(d, bb_make(p->bb.x0, ch->bb.y0, ch->bb.z0, p->bb.x0 + 1, ch->bb.y1, ch->bb.z1));
        }
        for (int pos = 0; pos < zs; pos += 4) {
            pos += rs_bound(r, zs); if (pos + 3 > zs) break;
            int fy = p->bb.y0 + rs_bound(r, hs) + 1;
            StPiece *ch = ms_gen_add(g, start, p->bb.x1 + 1, fy, p->bb.z0 + pos, DIR_EAST, depth);
            if (ch) room_add_entrance(d, bb_make(p->bb.x1 - 1, ch->bb.y0, ch->bb.z0, p->bb.x1, ch->bb.y1, ch->bb.z1));
        }
        return;
    }
    if (d->kind == MS_CORRIDOR) {
        int o = sp_orientation_dir(p);
        int end = rs_bound(r, 4);
        const BB b = p->bb;
        switch (o) {
        case DIR_SOUTH:
            if (end <= 1) ms_gen_add(g, start, b.x0, b.y0 - 1 + rs_bound(r, 3), b.z1 + 1, o, depth);
            else if (end == 2) ms_gen_add(g, start, b.x0 - 1, b.y0 - 1 + rs_bound(r, 3), b.z1 - 3, DIR_WEST, depth);
            else ms_gen_add(g, start, b.x1 + 1, b.y0 - 1 + rs_bound(r, 3), b.z1 - 3, DIR_EAST, depth);
            break;
        case DIR_WEST:
            if (end <= 1) ms_gen_add(g, start, b.x0 - 1, b.y0 - 1 + rs_bound(r, 3), b.z0, o, depth);
            else if (end == 2) ms_gen_add(g, start, b.x0, b.y0 - 1 + rs_bound(r, 3), b.z0 - 1, DIR_NORTH, depth);
            else ms_gen_add(g, start, b.x0, b.y0 - 1 + rs_bound(r, 3), b.z1 + 1, DIR_SOUTH, depth);
            break;
        case DIR_EAST:
            if (end <= 1) ms_gen_add(g, start, b.x1 + 1, b.y0 - 1 + rs_bound(r, 3), b.z0, o, depth);
            else if (end == 2) ms_gen_add(g, start, b.x1 - 3, b.y0 - 1 + rs_bound(r, 3), b.z0 - 1, DIR_NORTH, depth);
            else ms_gen_add(g, start, b.x1 - 3, b.y0 - 1 + rs_bound(r, 3), b.z1 + 1, DIR_SOUTH, depth);
            break;
        default:   /* NORTH */
            if (end <= 1) ms_gen_add(g, start, b.x0, b.y0 - 1 + rs_bound(r, 3), b.z0 - 1, o, depth);
            else if (end == 2) ms_gen_add(g, start, b.x0 - 1, b.y0 - 1 + rs_bound(r, 3), b.z0, DIR_WEST, depth);
            else ms_gen_add(g, start, b.x1 + 1, b.y0 - 1 + rs_bound(r, 3), b.z0, DIR_EAST, depth);
            break;
        }
        if (depth < 8) {
            if (o != DIR_NORTH && o != DIR_SOUTH) {
                for (int x = b.x0 + 3; x + 3 <= b.x1; x += 5) {
                    int s = rs_bound(r, 5);
                    if (s == 0) ms_gen_add(g, start, x, b.y0, b.z0 - 1, DIR_NORTH, depth + 1);
                    else if (s == 1) ms_gen_add(g, start, x, b.y0, b.z1 + 1, DIR_SOUTH, depth + 1);
                }
            } else {
                for (int z = b.z0 + 3; z + 3 <= b.z1; z += 5) {
                    int s = rs_bound(r, 5);
                    if (s == 0) ms_gen_add(g, start, b.x0 - 1, b.y0, z, DIR_WEST, depth + 1);
                    else if (s == 1) ms_gen_add(g, start, b.x1 + 1, b.y0, z, DIR_EAST, depth + 1);
                }
            }
        }
        return;
    }
    if (d->kind == MS_CROSSING) {
        const BB b = p->bb;
        switch (d->dir) {
        case DIR_SOUTH:
            ms_gen_add(g, start, b.x0 + 1, b.y0, b.z1 + 1, DIR_SOUTH, depth);
            ms_gen_add(g, start, b.x0 - 1, b.y0, b.z0 + 1, DIR_WEST, depth);
            ms_gen_add(g, start, b.x1 + 1, b.y0, b.z0 + 1, DIR_EAST, depth);
            break;
        case DIR_WEST:
            ms_gen_add(g, start, b.x0 + 1, b.y0, b.z0 - 1, DIR_NORTH, depth);
            ms_gen_add(g, start, b.x0 + 1, b.y0, b.z1 + 1, DIR_SOUTH, depth);
            ms_gen_add(g, start, b.x0 - 1, b.y0, b.z0 + 1, DIR_WEST, depth);
            break;
        case DIR_EAST:
            ms_gen_add(g, start, b.x0 + 1, b.y0, b.z0 - 1, DIR_NORTH, depth);
            ms_gen_add(g, start, b.x0 + 1, b.y0, b.z1 + 1, DIR_SOUTH, depth);
            ms_gen_add(g, start, b.x1 + 1, b.y0, b.z0 + 1, DIR_EAST, depth);
            break;
        default:   /* NORTH */
            ms_gen_add(g, start, b.x0 + 1, b.y0, b.z0 - 1, DIR_NORTH, depth);
            ms_gen_add(g, start, b.x0 - 1, b.y0, b.z0 + 1, DIR_WEST, depth);
            ms_gen_add(g, start, b.x1 + 1, b.y0, b.z0 + 1, DIR_EAST, depth);
            break;
        }
        if (d->two_floored) {
            if (rs_bool(r)) ms_gen_add(g, start, b.x0 + 1, b.y0 + 3 + 1, b.z0 - 1, DIR_NORTH, depth);
            if (rs_bool(r)) ms_gen_add(g, start, b.x0 - 1, b.y0 + 3 + 1, b.z0 + 1, DIR_WEST, depth);
            if (rs_bool(r)) ms_gen_add(g, start, b.x1 + 1, b.y0 + 3 + 1, b.z0 + 1, DIR_EAST, depth);
            if (rs_bool(r)) ms_gen_add(g, start, b.x0 + 1, b.y0 + 3 + 1, b.z1 + 1, DIR_SOUTH, depth);
        }
        return;
    }
    if (d->kind == MS_STAIRS) {
        const BB b = p->bb;
        switch (sp_orientation_dir(p)) {
        case DIR_SOUTH: ms_gen_add(g, start, b.x0, b.y0, b.z1 + 1, DIR_SOUTH, depth); break;
        case DIR_WEST: ms_gen_add(g, start, b.x0 - 1, b.y0, b.z0, DIR_WEST, depth); break;
        case DIR_EAST: ms_gen_add(g, start, b.x1 + 1, b.y0, b.z0, DIR_EAST, depth); break;
        default: ms_gen_add(g, start, b.x0, b.y0, b.z0 - 1, DIR_NORTH, depth); break;
        }
    }
}

/* ====================================================================== рисование */
typedef struct MSBlk {
    const StructWorld *sw;
    int cave_air, cobweb, spawner, chain, lava_b, chain_b;
    int planks[2], wood[2], fence[2], fence_w[2], fence_e[2];       /* состояния по типу */
    int planks_b[2], wood_b[2], fence_b[2];                          /* индексы блоков */
    int rail_ns, rail_ew, torch_s, torch_n;
    const u8 *fences_tag, *walls_tag, *unstable;
    u8 *blocking;                                                    /* биомы minecraft:mineshaft_blocking */
} MSBlk;

static void collect_biome_tag(const McGen *g, const char *tag_id, u8 *mask, int depth) {
    if (depth > 8) return;
    const char *c = strchr(tag_id, ':'); const char *ns = c ? tag_id : "minecraft", *path = c ? c + 1 : tag_id; int nsl = c ? (int)(c - tag_id) : 9;
    char file[1024]; snprintf(file, sizeof file, "%s/data/%.*s/tags/worldgen/biome/%s.json", g->pack, nsl, ns, path);
    char err[128]; JsDoc *d = js_parse_file(file, err, sizeof err); if (!d) return;
    const Js *vals = js_get(js_root(d), "values");
    for (int i = 0; js_is_arr(vals) && i < vals->n; i++) {
        const Js *e = vals->items[i]; const char *s = js_is_str(e) ? e->s : js_str(js_get(e, "id"), NULL);
        if (!s) continue;
        if (s[0] == '#') collect_biome_tag(g, s + 1, mask, depth + 1);
        else { int b = gen_biome_id(g, s); if (b >= 0 && b < g->nbiomes) mask[b] = 1; }
    }
    js_free(d);
}

static const MSBlk *ms_blk(StCtx *c) {
    static _Thread_local MSBlk k; static _Thread_local int ready;
    if (ready && k.sw == c->sw) return &k;
    const McGen *g = c->w->g;
    free(k.blocking); memset(&k, 0, sizeof k);
    k.sw = c->sw;
    k.cave_air = sp_st(c, "minecraft:cave_air"); k.cobweb = sp_st(c, "minecraft:cobweb"); k.spawner = sp_st(c, "minecraft:spawner");
    k.chain = sp_st(c, "minecraft:iron_chain"); k.chain_b = k.chain >= 0 ? g->state_block[k.chain] : -1;
    k.lava_b = g->state_block[sp_st(c, "minecraft:lava")];
    static const char *PL[2] = { "minecraft:oak_planks", "minecraft:dark_oak_planks" }, *WD[2] = { "minecraft:oak_log", "minecraft:dark_oak_log" },
                      *FN[2] = { "minecraft:oak_fence", "minecraft:dark_oak_fence" };
    for (int t = 0; t < 2; t++) {
        k.planks[t] = sp_st(c, PL[t]); k.wood[t] = sp_st(c, WD[t]); k.fence[t] = sp_st(c, FN[t]);
        k.fence_w[t] = sp_with(c, k.fence[t], "west", "true"); k.fence_e[t] = sp_with(c, k.fence[t], "east", "true");
        k.planks_b[t] = g->state_block[k.planks[t]]; k.wood_b[t] = g->state_block[k.wood[t]]; k.fence_b[t] = g->state_block[k.fence[t]];
    }
    int rail = sp_st(c, "minecraft:rail");
    k.rail_ns = sp_with(c, rail, "shape", "north_south"); k.rail_ew = sp_with(c, rail, "shape", "east_west");
    int wt = sp_st(c, "minecraft:wall_torch");
    k.torch_s = sp_with(c, wt, "facing", "south"); k.torch_n = sp_with(c, wt, "facing", "north");
    k.fences_tag = gen_block_tag(g, "minecraft:fences"); k.walls_tag = gen_block_tag(g, "minecraft:walls"); k.unstable = gen_block_tag(g, "minecraft:unstable_bottom_center");
    k.blocking = xcalloc((size_t)(g->nbiomes ? g->nbiomes : 1), 1);
    collect_biome_tag(g, "minecraft:mineshaft_blocking", k.blocking, 0);
    ready = 1;
    return &k;
}

static inline int st_block(StCtx *c, int st) { return c->w->g->state_block[st]; }
static inline int is_air(StCtx *c, int st) { return gen_is_air(c->w->g, st); }
static inline int sturdy(StCtx *c, int st, int dir) { const BsTab *bs = c->sw->bs; return bs->sturdy ? (bs->sturdy[st] >> dir) & 1 : (bs->flags[st] & BSF_SOLID_RENDER) != 0; }

/* MineShaftPiece.canBeReplaced */
static int ms_can_replace(StCtx *c, const StPiece *p, int x, int y, int z) {
    const MSBlk *k = ms_blk(c); const MSData *d = p->data;
    int b = st_block(c, sp_get(c, p, x, y, z));
    return b != k->planks_b[d->type] && b != k->wood_b[d->type] && b != k->fence_b[d->type] && b != k->chain_b;
}

/* MineShaftPiece.isInInvalidLocation */
static int ms_invalid(StCtx *c, const StPiece *p) {
    const MSBlk *k = ms_blk(c); const BsTab *bs = c->sw->bs;
    const BB *b = &p->bb, *cb = &c->chunk;
    int x0 = b->x0 - 1 > cb->x0 ? b->x0 - 1 : cb->x0, y0 = b->y0 - 1 > cb->y0 ? b->y0 - 1 : cb->y0, z0 = b->z0 - 1 > cb->z0 ? b->z0 - 1 : cb->z0;
    int x1 = b->x1 + 1 < cb->x1 ? b->x1 + 1 : cb->x1, y1 = b->y1 + 1 < cb->y1 ? b->y1 + 1 : cb->y1, z1 = b->z1 + 1 < cb->z1 ? b->z1 + 1 : cb->z1;
    int bio = fc_biome(c->fc, (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
    if (bio >= 0 && bio < c->w->g->nbiomes && k->blocking[bio]) return 1;
    #define LIQ(X, Y, Z) ((bs->flags[sp_get_world(c, X, Y, Z)] & BSF_LIQUID) != 0)
    for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) if (LIQ(x, y0, z) || LIQ(x, y1, z)) return 1;
    for (int x = x0; x <= x1; x++) for (int y = y0; y <= y1; y++) if (LIQ(x, y, z0) || LIQ(x, y, z1)) return 1;
    for (int z = z0; z <= z1; z++) for (int y = y0; y <= y1; y++) if (LIQ(x0, y, z) || LIQ(x1, y, z)) return 1;
    #undef LIQ
    return 0;
}

/* MineShaftPiece.setPlanksBlock: level.setBlock напрямую (без canBeReplaced/пометок) */
static void ms_set_planks(StCtx *c, const StPiece *p, int planks, int x, int y, int z) {
    if (!sp_is_interior(c, p, x, y, z)) return;
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y), wz = sp_wz(p, x, z);
    if (!sturdy(c, sp_get_world(c, wx, wy, wz), DIR_UP)) sp_set_world(c, wx, wy, wz, planks);
}

/* isSupportingBox: потолок над опорой (вне чанка — воздух) */
static int ms_supporting_box(StCtx *c, const StPiece *p, int x0, int x1, int y1, int z) {
    for (int x = x0; x <= x1; x++) if (is_air(c, sp_get(c, p, x, y1 + 1, z))) return 0;
    return 1;
}

static int is_replaceable(StCtx *c, int st) { return sp_replaceable_by_structures(c, st); }

/* Block.canSupportCenter(level, pos, DOWN) && !(FallingBlock) */
static int can_hang_chain_below(StCtx *c, const MSBlk *k, int st) {
    const BsTab *bs = c->sw->bs;
    int b = st_block(c, st);
    if (k->unstable && k->unstable[b]) return 0;
    int ok = sturdy(c, st, DIR_DOWN);
    if (!ok) {
        if ((k->fences_tag && k->fences_tag[b]) || (k->walls_tag && k->walls_tag[b]) || bs_is_a(bs, st, "IronBarsBlock")) ok = 1;
        else if (b == k->chain_b) { const char *ax = "y"; bs_get_prop(bs, st, "axis", &ax); ok = !strcmp(ax, "y"); }
    }
    if (!ok) return 0;
    return !bs_is_a(bs, st, "FallingBlock");
}

/* MineShaftCorridor.fillPillarDownOrChainUp */
static void ms_pillar_or_chain(StCtx *c, const StPiece *p, const MSBlk *k, int pillar, int x, int y, int z) {
    int wx = sp_wx(p, x, z), wy0 = sp_wy(p, y), wz = sp_wz(p, x, z);
    if (!sp_inside(c, wx, wy0, wz)) return;
    const MSData *d = p->data;
    int min_y = c->w->min_y, max_y = c->w->min_y + c->w->height - 1;
    int below = 1, above = 1;
    for (int dist = 1; below || above; dist++) {
        if (below) {
            int by = wy0 - dist, bst = sp_get_world(c, wx, by, wz);
            int empty = is_replaceable(c, bst) && st_block(c, bst) != k->lava_b;
            if (!empty && sturdy(c, bst, DIR_UP)) {
                for (int py = wy0 - dist + 1; py < wy0; py++) sp_set_world(c, wx, py, wz, pillar);
                return;
            }
            below = dist <= 20 && empty && by > min_y + 1;
        }
        if (above) {
            int ay = wy0 + dist, ast = sp_get_world(c, wx, ay, wz);
            int empty = is_replaceable(c, ast);
            if (!empty && can_hang_chain_below(c, k, ast)) {
                sp_set_world(c, wx, wy0 + 1, wz, k->fence[d->type]);
                for (int py = wy0 + 2; py < wy0 + dist; py++) sp_set_world(c, wx, py, wz, k->chain);
                return;
            }
            above = dist <= 50 && empty && ay < max_y;
        }
    }
}

static void ms_double_support(StCtx *c, const StPiece *p, const MSBlk *k, int x, int y, int z) {
    const MSData *d = p->data;
    if (st_block(c, sp_get(c, p, x, y, z)) == k->planks_b[d->type]) ms_pillar_or_chain(c, p, k, k->wood[d->type], x, y, z);
    if (st_block(c, sp_get(c, p, x + 2, y, z)) == k->planks_b[d->type]) ms_pillar_or_chain(c, p, k, k->wood[d->type], x + 2, y, z);
}

/* MineShaftCorridor.placeSupport */
static void ms_support(StCtx *c, const StPiece *p, const MSBlk *k, int x0, int y0, int z, int y1, int x1) {
    const MSData *d = p->data;
    if (!ms_supporting_box(c, p, x0, x1, y1, z)) return;
    int planks = k->planks[d->type], CA = k->cave_air;
    sp_box(c, p, x0, y0, z, x0, y1 - 1, z, k->fence_w[d->type], CA, 0);
    sp_box(c, p, x1, y0, z, x1, y1 - 1, z, k->fence_e[d->type], CA, 0);
    if (rs_bound(c->rs, 4) == 0) {
        sp_box(c, p, x0, y1, z, x0, y1, z, planks, CA, 0);
        sp_box(c, p, x1, y1, z, x1, y1, z, planks, CA, 0);
    } else {
        sp_box(c, p, x0, y1, z, x1, y1, z, planks, CA, 0);
        sp_maybe_block(c, p, 0.05f, x0 + 1, y1, z - 1, k->torch_s);
        sp_maybe_block(c, p, 0.05f, x0 + 1, y1, z + 1, k->torch_n);
    }
}

/* hasSturdyNeighbours */
static int ms_sturdy_neighbours(StCtx *c, const StPiece *p, int x, int y, int z, int count) {
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y), wz = sp_wz(p, x, z), n = 0;
    for (int dir = 0; dir < 6; dir++) {          /* Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST */
        int nx = wx + DIR_DX[dir], ny = wy + DIR_DY[dir], nz = wz + DIR_DZ[dir];
        if (sp_inside(c, nx, ny, nz) && sturdy(c, sp_get_world(c, nx, ny, nz), dir_opp(dir))) { if (++n >= count) return 1; }
    }
    return 0;
}

static void ms_maybe_cobweb(StCtx *c, const StPiece *p, const MSBlk *k, float prob, int x, int y, int z) {
    if (sp_is_interior(c, p, x, y, z) && rs_float(c->rs) < prob && ms_sturdy_neighbours(c, p, x, y, z, 2)) sp_place(c, p, k->cobweb, x, y, z);
}

/* MineShaftCorridor.createChest: рельс + вагонетка с сундуком (сущность не ставим) */
static void ms_chest(StCtx *c, const StPiece *p, const MSBlk *k, int x, int y, int z) {
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y), wz = sp_wz(p, x, z);
    if (!sp_inside(c, wx, wy, wz) || !is_air(c, sp_get_world(c, wx, wy, wz)) || is_air(c, sp_get_world(c, wx, wy - 1, wz))) return;
    int st = rs_bool(c->rs) ? k->rail_ns : k->rail_ew;
    sp_place(c, p, st, x, y, z);
    (void)rs_long(c->rs);                     /* MinecartChest.setLootTable(table, random.nextLong()) */
}

static void corridor_post(StCtx *c, StPiece *p, const MSBlk *k) {
    MSData *d = p->data; RS *rs = c->rs; const BsTab *bs = c->sw->bs;
    if (ms_invalid(c, p)) return;
    int length = d->num * 5 - 1, CA = k->cave_air, planks = k->planks[d->type];
    sp_box(c, p, 0, 0, 0, 2, 1, length, CA, CA, 0);
    sp_maybe_box(c, p, 0.8f, 0, 2, 0, 2, 2, length, CA, CA, 0, 0);
    if (d->spider) sp_maybe_box(c, p, 0.6f, 0, 0, 0, 2, 1, length, k->cobweb, CA, 0, 1);
    for (int sec = 0; sec < d->num; sec++) {
        int z = 2 + sec * 5;
        ms_support(c, p, k, 0, 0, z, 2, 2);
        ms_maybe_cobweb(c, p, k, 0.1f, 0, 2, z - 1);
        ms_maybe_cobweb(c, p, k, 0.1f, 2, 2, z - 1);
        ms_maybe_cobweb(c, p, k, 0.1f, 0, 2, z + 1);
        ms_maybe_cobweb(c, p, k, 0.1f, 2, 2, z + 1);
        ms_maybe_cobweb(c, p, k, 0.05f, 0, 2, z - 2);
        ms_maybe_cobweb(c, p, k, 0.05f, 2, 2, z - 2);
        ms_maybe_cobweb(c, p, k, 0.05f, 0, 2, z + 2);
        ms_maybe_cobweb(c, p, k, 0.05f, 2, 2, z + 2);
        if (rs_bound(rs, 100) == 0) ms_chest(c, p, k, 2, 0, z - 1);
        if (rs_bound(rs, 100) == 0) ms_chest(c, p, k, 0, 0, z + 1);
        if (d->spider && !d->placed_spider) {
            int nz = z - 1 + rs_bound(rs, 3);
            int wx = sp_wx(p, 1, nz), wy = sp_wy(p, 0), wz = sp_wz(p, 1, nz);
            if (sp_inside(c, wx, wy, wz) && sp_is_interior(c, p, 1, 0, nz)) {
                d->placed_spider = 1;
                sp_set_world(c, wx, wy, wz, k->spawner);   /* setEntityId(CAVE_SPIDER, random): spawnPotentials пуст — ГСЧ не тратится */
            }
        }
    }
    for (int x = 0; x <= 2; x++) for (int z = 0; z <= length; z++) ms_set_planks(c, p, planks, x, -1, z);
    ms_double_support(c, p, k, 0, -1, 2);
    if (d->num > 1) ms_double_support(c, p, k, 0, -1, length - 2);
    if (d->has_rails) {
        for (int z = 0; z <= length; z++) {
            int fl = sp_get(c, p, 1, -1, z);
            if (!is_air(c, fl) && (bs->flags[fl] & BSF_SOLID_RENDER)) {
                float prob = sp_is_interior(c, p, 1, 0, z) ? 0.7f : 0.9f;
                sp_maybe_block(c, p, prob, 1, 0, z, k->rail_ns);
            }
        }
    }
}

static void crossing_support_pillar(StCtx *c, const StPiece *p, const MSBlk *k, int x, int y0, int z, int y1) {
    const MSData *d = p->data;
    if (!is_air(c, sp_get(c, p, x, y1 + 1, z))) sp_box(c, p, x, y0, z, x, y1, z, k->planks[d->type], k->cave_air, 0);
}

static void crossing_post(StCtx *c, StPiece *p, const MSBlk *k) {
    MSData *d = p->data;
    if (ms_invalid(c, p)) return;
    const BB b = p->bb; int CA = k->cave_air;
    if (d->two_floored) {
        sp_box(c, p, b.x0 + 1, b.y0, b.z0, b.x1 - 1, b.y0 + 3 - 1, b.z1, CA, CA, 0);
        sp_box(c, p, b.x0, b.y0, b.z0 + 1, b.x1, b.y0 + 3 - 1, b.z1 - 1, CA, CA, 0);
        sp_box(c, p, b.x0 + 1, b.y1 - 2, b.z0, b.x1 - 1, b.y1, b.z1, CA, CA, 0);
        sp_box(c, p, b.x0, b.y1 - 2, b.z0 + 1, b.x1, b.y1, b.z1 - 1, CA, CA, 0);
        sp_box(c, p, b.x0 + 1, b.y0 + 3, b.z0 + 1, b.x1 - 1, b.y0 + 3, b.z1 - 1, CA, CA, 0);
    } else {
        sp_box(c, p, b.x0 + 1, b.y0, b.z0, b.x1 - 1, b.y1, b.z1, CA, CA, 0);
        sp_box(c, p, b.x0, b.y0, b.z0 + 1, b.x1, b.y1, b.z1 - 1, CA, CA, 0);
    }
    crossing_support_pillar(c, p, k, b.x0 + 1, b.y0, b.z0 + 1, b.y1);
    crossing_support_pillar(c, p, k, b.x0 + 1, b.y0, b.z1 - 1, b.y1);
    crossing_support_pillar(c, p, k, b.x1 - 1, b.y0, b.z0 + 1, b.y1);
    crossing_support_pillar(c, p, k, b.x1 - 1, b.y0, b.z1 - 1, b.y1);
    int y = b.y0 - 1;
    for (int x = b.x0; x <= b.x1; x++) for (int z = b.z0; z <= b.z1; z++) ms_set_planks(c, p, k->planks[d->type], x, y, z);
}

static void room_post(StCtx *c, StPiece *p, const MSBlk *k) {
    MSData *d = p->data;
    if (ms_invalid(c, p)) return;
    const BB b = p->bb; int CA = k->cave_air;
    sp_box(c, p, b.x0, b.y0 + 1, b.z0, b.x1, b.y0 + 3 < b.y1 ? b.y0 + 3 : b.y1, b.z1, CA, CA, 0);
    for (int i = 0; i < d->nent; i++) {
        const BB *e = &d->ent[i];
        sp_box(c, p, e->x0, e->y1 - 2, e->z0, e->x1, e->y1, e->z1, CA, CA, 0);
    }
    sp_upper_half_sphere(c, p, b.x0, b.y0 + 4, b.z0, b.x1, b.y1, b.z1, CA, 0);
}

static void stairs_post(StCtx *c, StPiece *p, const MSBlk *k) {
    if (ms_invalid(c, p)) return;
    int CA = k->cave_air;
    sp_box(c, p, 0, 5, 0, 2, 7, 1, CA, CA, 0);
    sp_box(c, p, 0, 0, 7, 2, 2, 8, CA, CA, 0);
    for (int i = 0; i < 5; i++) sp_box(c, p, 0, 5 - i - (i < 4 ? 1 : 0), 2 + i, 2, 7 - i, 2 + i, CA, CA, 0);
}

static void ms_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    (void)rx; (void)ry; (void)rz;
    const MSBlk *k = ms_blk(c); MSData *d = p->data;
    switch (d->kind) {
    case MS_ROOM: room_post(c, p, k); break;
    case MS_CORRIDOR: corridor_post(c, p, k); break;
    case MS_CROSSING: crossing_post(c, p, k); break;
    default: stairs_post(c, p, k); break;
    }
}

const PieceVT PIECE_MS_ROOM = { "minecraft:msroom", ms_post, ms_room_move, ms_free, ms_dump, NULL, ms_can_replace };
const PieceVT PIECE_MS_CORRIDOR = { "minecraft:mscorridor", ms_post, NULL, ms_free, ms_dump, ms_reset, ms_can_replace };
const PieceVT PIECE_MS_CROSSING = { "minecraft:mscrossing", ms_post, NULL, ms_free, ms_dump, NULL, ms_can_replace };
const PieceVT PIECE_MS_STAIRS = { "minecraft:msstairs", ms_post, NULL, ms_free, ms_dump, NULL, ms_can_replace };

/* ====================================================================== структура */
typedef struct MSCfg { int type; } MSCfg;

static void *ms_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) {
    (void)sw;
    const char *t = js_str(js_get(cfg, "mineshaft_type"), "normal");
    MSCfg *c = xcalloc(1, sizeof *c);
    if (!strcmp(t, "mesa")) c->type = MST_MESA;
    else if (!strcmp(t, "normal")) c->type = MST_NORMAL;
    else { snprintf(err, errlen, "mineshaft: неизвестный mineshaft_type %s", t); free(c); return NULL; }
    return c;
}

static void pv_free_all(PieceVec *pv) { if (!pv) return; for (int i = 0; i < pv->n; i++) piece_free(pv->v[i]); free(pv->v); free(pv); }

/* findGenerationPoint: части строятся сразу (generatePiecesAndAdjust) */
static int ms_find(GenCtx *c, const void *cfgp, Stub *out) {
    const MSCfg *cfg = cfgp;
    RS *r = &c->rs;
    (void)rs_double(r);
    PieceVec *pv = xcalloc(1, sizeof *pv);
    MSGen g = { pv, r, cfg->type };
    int west = c->cx * 16 + 2, north = c->cz * 16 + 2;
    int x1 = west + 7 + rs_bound(r, 6), y1 = 54 + rs_bound(r, 6), z1 = north + 7 + rs_bound(r, 6);
    StPiece *room = piece_new(&PIECE_MS_ROOM, bb_make(west, 50, north, x1, y1, z1), -1, 0, ms_data(MS_ROOM, cfg->type));
    pvec_push(pv, room);
    ms_add_children(&g, room, room);
    int sea = c->w->sea_level, dy;
    if (cfg->type == MST_MESA) {
        BB b = pvec_bb(pv);
        int cx = bb_cx(&b), cy = bb_cy(&b), cz = bb_cz(&b);
        int surf = gen_first_free_height(c, cx, cz, HM_WORLD_SURFACE_WG);
        int target = surf <= sea ? sea : rs_between(r, sea, surf);
        dy = target - cy;
        pvec_offset_vertically(pv, dy);
    } else {
        dy = pvec_move_below_sea_level(pv, sea, c->w->ns->min_y, r, 10);
    }
    out->x = c->cx * 16 + 8; out->y = 50 + dy; out->z = c->cz * 16; out->state = pv;
    return 1;
}
static int ms_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)c; (void)cfg;
    PieceVec *pv = stub->state; stub->state = NULL;
    if (!pv) return 0;
    for (int i = 0; i < pv->n; i++) pvec_push(out, pv->v[i]);
    free(pv->v); free(pv);
    return 1;
}
static void ms_free_stub(Stub *stub) { pv_free_all(stub->state); stub->state = NULL; }
static void ms_free_cfg(void *p) { free(p); }

const StructType STRUCT_MINESHAFT = { "minecraft:mineshaft", ms_parse, ms_find, ms_build, NULL, ms_free_cfg, ms_free_stub };

void structures_register_mineshaft(void) {
    structure_register_type(&STRUCT_MINESHAFT);
    piece_register_type(&PIECE_MS_ROOM); piece_register_type(&PIECE_MS_CORRIDOR);
    piece_register_type(&PIECE_MS_CROSSING); piece_register_type(&PIECE_MS_STAIRS);
}

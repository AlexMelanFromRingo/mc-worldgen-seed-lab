/* structures/end_city.c — EndCityStructure/EndCityPieces (minecraft:end_city; часть minecraft:ecp).
 * Дерево частей из шаблонов end_city/*: дом-башня (HOUSE_TOWER), башня (TOWER), мост (TOWER_BRIDGE, корабль — один на город),
 * толстая башня (FAT_TOWER); глубина ≤ 8; потомки уровня получают общий genDepth = random.nextInt() и отбрасываются целиком при
 * пересечении с частью другого genDepth. Позиция дочерней части — calculateConnectedPosition(родитель, смещение) (опорная точка 0). */
#include "shipwreck_tpl.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_END_CITY;

typedef struct ECData { TplPiece tp; int ow; } ECData;

static void ec_free(void *v) { free(v); }
static void ec_move(StPiece *p, int dx, int dy, int dz) { ECData *d = p->data; tplp_move(&d->tp, dx, dy, dz); }
static void ec_dump(const StPiece *p, StrBuf *o) { const ECData *d = p->data; tplp_dump(&d->tp, o); sb_printf(o, ",\"ow\":%d", d->ow); }
static void ec_marker(StCtx *c, StPiece *p, void *ud, const char *m, int x, int y, int z, const BB *bounds) {
    (void)p; (void)ud;
    if (!strncmp(m, "Chest", 5)) { if (bb_inside(bounds, x, y - 1, z)) tplp_loot(c, x, y - 1, z); }
    /* Sentry (шалкер), Elytra (рамка) — сущности */
}
static void ec_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    ECData *d = p->data;
    tplp_post(c, p, &d->tp, &c->chunk, rx, ry, rz, ec_marker, NULL);
}
const PieceVT PIECE_END_CITY = { "minecraft:ecp", ec_post, ec_move, ec_free, ec_dump, NULL, NULL };

/* ---------------------------------------------------------------- построение */
typedef struct ECGen { GenCtx *c; int ship_created; } ECGen;
static TplPiece *TP(StPiece *p) { return &((ECData *)p->data)->tp; }

static StPiece *new_piece(GenCtx *c, const char *name, int x, int y, int z, int rot, int ow) {
    char loc[128]; snprintf(loc, sizeof loc, "minecraft:end_city/%s", name);
    ECData *d = xcalloc(1, sizeof *d);
    tplp_init(c->w, &d->tp, loc, name, x, y, z, rot, MIR_NONE, 0, 0);
    tplp_add_proc(&d->tp, proc_builtin(c->w, ow ? PB_STRUCTURE_BLOCK : PB_STRUCTURE_AND_AIR));
    d->ow = ow;
    StPiece *p = piece_new(&PIECE_END_CITY, tplp_bb(&d->tp), -1, 0, d);
    sp_set_orientation(p, DIR_NORTH);
    return p;
}
/* EndCityPieces.addPiece(parent, offset, name, rotation, overwrite) */
static StPiece *add_piece(GenCtx *c, StPiece *parent, int ox, int oy, int oz, const char *name, int rot, int ow) {
    const TplPiece *pt = TP(parent);
    StPiece *ch = new_piece(c, name, pt->x, pt->y, pt->z, rot, ow);
    int dx, dy, dz; tplp_connected(pt, ox, oy, oz, TP(ch), &dx, &dy, &dz);
    piece_move(ch, dx, dy, dz);
    return ch;
}
static StPiece *add(PieceVec *v, StPiece *p) { pvec_push(v, p); return p; }

typedef int (*SecGen)(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces);
static int gen_house_tower(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces);
static int gen_tower(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces);
static int gen_tower_bridge(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces);
static int gen_fat_tower(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces);

static int recursive_children(ECGen *g, SecGen gen, int depth, StPiece *parent, const int *off, PieceVec *pieces) {
    if (depth > 8) return 0;
    PieceVec child; memset(&child, 0, sizeof child);
    if (gen(g, depth, parent, off, &child)) {
        int collision = 0;
        int tag = rs_int(&g->c->rs);
        for (int i = 0; i < child.n; i++) {
            child.v[i]->depth = tag;
            StPiece *cp = pvec_collision(pieces, &child.v[i]->bb);
            if (cp && cp->depth != parent->depth) { collision = 1; break; }
        }
        if (!collision) {
            for (int i = 0; i < child.n; i++) pvec_push(pieces, child.v[i]);
            free(child.v);
            return 1;
        }
    }
    for (int i = 0; i < child.n; i++) piece_free(child.v[i]);
    free(child.v);
    return 0;
}

static int gen_house_tower(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces) {
    GenCtx *c = g->c;
    if (depth > 8) return 0;
    int rot = TP(parent)->rot;
    StPiece *last = add(pieces, add_piece(c, parent, off[0], off[1], off[2], "base_floor", rot, 1));
    int floors = rs_bound(&c->rs, 3);
    if (floors == 0) {
        add(pieces, add_piece(c, last, -1, 4, -1, "base_roof", rot, 1));
    } else if (floors == 1) {
        last = add(pieces, add_piece(c, last, -1, 0, -1, "second_floor_2", rot, 0));
        last = add(pieces, add_piece(c, last, -1, 8, -1, "second_roof", rot, 0));
        recursive_children(g, gen_tower, depth + 1, last, NULL, pieces);
    } else {
        last = add(pieces, add_piece(c, last, -1, 0, -1, "second_floor_2", rot, 0));
        last = add(pieces, add_piece(c, last, -1, 4, -1, "third_floor_2", rot, 0));
        last = add(pieces, add_piece(c, last, -1, 8, -1, "third_roof", rot, 1));
        recursive_children(g, gen_tower, depth + 1, last, NULL, pieces);
    }
    return 1;
}

static const int TOWER_BRIDGES[4][4] = { { ROT_NONE, 1, -1, 0 }, { ROT_CW90, 6, -1, 1 }, { ROT_CCW90, 0, -1, 5 }, { ROT_CW180, 5, -1, 6 } };
static const int FAT_TOWER_BRIDGES[4][4] = { { ROT_NONE, 4, -1, 0 }, { ROT_CW90, 12, -1, 4 }, { ROT_CCW90, 0, -1, 8 }, { ROT_CW180, 8, -1, 12 } };

static int gen_tower(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces) {
    (void)off;
    GenCtx *c = g->c;
    int rot = TP(parent)->rot;
    StPiece *last = parent;
    int bx = 3 + rs_bound(&c->rs, 2); int bz = 3 + rs_bound(&c->rs, 2);
    last = add(pieces, add_piece(c, last, bx, -3, bz, "tower_base", rot, 1));
    last = add(pieces, add_piece(c, last, 0, 7, 0, "tower_piece", rot, 1));
    StPiece *bridge = rs_bound(&c->rs, 3) == 0 ? last : NULL;
    int height = 1 + rs_bound(&c->rs, 3);
    for (int i = 0; i < height; i++) {
        last = add(pieces, add_piece(c, last, 0, 4, 0, "tower_piece", rot, 1));
        if (i < height - 1 && rs_bool(&c->rs)) bridge = last;
    }
    if (bridge) {
        for (int b = 0; b < 4; b++) {
            if (rs_bool(&c->rs)) {
                StPiece *bs = add(pieces, add_piece(c, bridge, TOWER_BRIDGES[b][1], TOWER_BRIDGES[b][2], TOWER_BRIDGES[b][3], "bridge_end", (rot + TOWER_BRIDGES[b][0]) & 3, 1));
                recursive_children(g, gen_tower_bridge, depth + 1, bs, NULL, pieces);
            }
        }
        add(pieces, add_piece(c, last, -1, 4, -1, "tower_top", rot, 1));
    } else {
        if (depth != 7) return recursive_children(g, gen_fat_tower, depth + 1, last, NULL, pieces);
        add(pieces, add_piece(c, last, -1, 4, -1, "tower_top", rot, 1));
    }
    return 1;
}

static int gen_tower_bridge(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces) {
    (void)off;
    GenCtx *c = g->c;
    int rot = TP(parent)->rot;
    int len = rs_bound(&c->rs, 4) + 1;
    StPiece *last = add(pieces, add_piece(c, parent, 0, 0, -4, "bridge_piece", rot, 1));
    last->depth = -1;
    int ny = 0;
    for (int i = 0; i < len; i++) {
        if (rs_bool(&c->rs)) { last = add(pieces, add_piece(c, last, 0, ny, -4, "bridge_piece", rot, 1)); ny = 0; }
        else {
            if (rs_bool(&c->rs)) last = add(pieces, add_piece(c, last, 0, ny, -4, "bridge_steep_stairs", rot, 1));
            else last = add(pieces, add_piece(c, last, 0, ny, -8, "bridge_gentle_stairs", rot, 1));
            ny = 4;
        }
    }
    if (!g->ship_created && rs_bound(&c->rs, 10 - depth) == 0) {
        int sx = -8 + rs_bound(&c->rs, 8); int sz = -70 + rs_bound(&c->rs, 10);
        add(pieces, add_piece(c, last, sx, ny, sz, "ship", rot, 1));
        g->ship_created = 1;
    } else {
        int o[3] = { -3, ny + 1, -11 };
        if (!recursive_children(g, gen_house_tower, depth + 1, last, o, pieces)) return 0;
    }
    last = add(pieces, add_piece(c, last, 4, ny, 0, "bridge_end", (rot + ROT_CW180) & 3, 1));
    last->depth = -1;
    return 1;
}

static int gen_fat_tower(ECGen *g, int depth, StPiece *parent, const int *off, PieceVec *pieces) {
    (void)off;
    GenCtx *c = g->c;
    int rot = TP(parent)->rot;
    StPiece *last = add(pieces, add_piece(c, parent, -3, 4, -3, "fat_tower_base", rot, 1));
    last = add(pieces, add_piece(c, last, 0, 4, 0, "fat_tower_middle", rot, 1));
    for (int i = 0; i < 2 && rs_bound(&c->rs, 3) != 0; i++) {
        last = add(pieces, add_piece(c, last, 0, 8, 0, "fat_tower_middle", rot, 1));
        for (int b = 0; b < 4; b++) {
            if (rs_bool(&c->rs)) {
                StPiece *bs = add(pieces, add_piece(c, last, FAT_TOWER_BRIDGES[b][1], FAT_TOWER_BRIDGES[b][2], FAT_TOWER_BRIDGES[b][3], "bridge_end", (rot + FAT_TOWER_BRIDGES[b][0]) & 3, 1));
                recursive_children(g, gen_tower_bridge, depth + 1, bs, NULL, pieces);
            }
        }
    }
    add(pieces, add_piece(c, last, -2, 8, -2, "fat_tower_top", rot, 1));
    return 1;
}

/* ---------------------------------------------------------------- структура */
static void *ec_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int ec_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    int bx = c->cx * 16 + 7, bz = c->cz * 16 + 7;
    if (!gen_could_exist_in_column(c, bx, bz, c->min_y - 1, c->min_y + c->height - 1)) return 0;
    int rot = rs_bound(&c->rs, 4);
    int ox = 5, oz = 5;                                               /* getLowestYIn5by5Box */
    if (rot == ROT_CW90) ox = -5; else if (rot == ROT_CW180) { ox = -5; oz = -5; } else if (rot == ROT_CCW90) oz = -5;
    int y = gen_lowest_y(c, bx, bz, ox, oz);
    if (y < 60) return 0;
    out->x = bx; out->y = y; out->z = bz;
    out->state = (void *)(intptr_t)(rot + 1);
    return 1;
}
static int ec_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg;
    int rot = (int)(intptr_t)stub->state - 1;
    ECGen g = { c, 0 };
    StPiece *last = add(out, new_piece(c, "base_floor", stub->x, stub->y, stub->z, rot, 1));
    last = add(out, add_piece(c, last, -1, 0, -1, "second_floor_1", rot, 0));
    last = add(out, add_piece(c, last, -1, 4, -1, "third_floor_1", rot, 0));
    last = add(out, add_piece(c, last, -1, 8, -1, "third_roof", rot, 1));
    recursive_children(&g, gen_tower, 1, last, NULL, out);
    return 1;
}
static void ec_free_cfg(void *p) { free(p); }
const StructType STRUCT_END_CITY = { "minecraft:end_city", ec_parse, ec_find, ec_build, NULL, ec_free_cfg, NULL };

void structures_register_end_city(void) { structure_register_type(&STRUCT_END_CITY); piece_register_type(&PIECE_END_CITY); }

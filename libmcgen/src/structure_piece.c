/* structure_piece.c — вспомогательный слой StructurePiece для Java-кодированных построек (см. structure_piece.h). */
#include "structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- состояния по имени */
static int cached_state(StructWorld *sw, const char *name) {
    mutex_lock(sw->lock);
    intptr_t v = (intptr_t)sm_get(&sw->st_cache, name);
    mutex_unlock(sw->lock);
    if (v) return (int)v - 1;
    int st = gen_state_id(sw->g, name);
    mutex_lock(sw->lock);
    sm_put(&sw->st_cache, name, (void *)(intptr_t)(st + 1));
    mutex_unlock(sw->lock);
    return st;
}
int sp_st(StCtx *c, const char *name) { return cached_state(c->sw, name); }
int sp_st_gen(GenCtx *c, const char *name) { return cached_state(c->sw, name); }
int sp_with(StCtx *c, int state, const char *prop, const char *value) { int r = bs_with(c->sw->bs, state, prop, value); return r < 0 ? state : r; }
int sp_air(StCtx *c) { return c->w->g->st_air; }

/* ---------------------------------------------------------------- ориентация и координаты */
void sp_set_orientation(StPiece *p, int dir) {
    p->orient = dir < 0 ? -1 : dir_to_2d(dir);
    switch (dir) {
    case DIR_SOUTH: p->mir = MIR_LEFT_RIGHT; p->rot = ROT_NONE; break;
    case DIR_WEST: p->mir = MIR_LEFT_RIGHT; p->rot = ROT_CW90; break;
    case DIR_EAST: p->mir = MIR_NONE; p->rot = ROT_CW90; break;
    default: p->mir = MIR_NONE; p->rot = ROT_NONE; break;
    }
}
int sp_orientation_dir(const StPiece *p) { return p->orient < 0 ? -1 : dir_from_2d(p->orient); }
BB sp_make_bb(int x, int y, int z, int dir, int width, int height, int depth) {
    return (dir == DIR_NORTH || dir == DIR_SOUTH) ? bb_make(x, y, z, x + width - 1, y + height - 1, z + depth - 1) : bb_make(x, y, z, x + depth - 1, y + height - 1, z + width - 1);
}
int sp_wx(const StPiece *p, int x, int z) {
    if (p->orient < 0) return x;
    switch (dir_from_2d(p->orient)) { case DIR_NORTH: case DIR_SOUTH: return p->bb.x0 + x; case DIR_WEST: return p->bb.x1 - z; case DIR_EAST: return p->bb.x0 + z; default: return x; }
}
int sp_wy(const StPiece *p, int y) { return p->orient < 0 ? y : y + p->bb.y0; }
int sp_wz(const StPiece *p, int x, int z) {
    if (p->orient < 0) return z;
    switch (dir_from_2d(p->orient)) { case DIR_NORTH: return p->bb.z1 - z; case DIR_SOUTH: return p->bb.z0 + z; case DIR_WEST: case DIR_EAST: return p->bb.z0 + x; default: return z; }
}

/* ---------------------------------------------------------------- чтение/запись */
int sp_inside(StCtx *c, int wx, int wy, int wz) { return bb_inside(&c->chunk, wx, wy, wz); }
int sp_get_world(StCtx *c, int wx, int wy, int wz) { return fc_get(c->fc, wx, wy, wz); }
int sp_get(StCtx *c, const StPiece *p, int x, int y, int z) {
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y), wz = sp_wz(p, x, z);
    return bb_inside(&c->chunk, wx, wy, wz) ? fc_get(c->fc, wx, wy, wz) : c->w->g->st_air;
}
static void ensure_shape_chk(StructWorld *sw) {
    if (sw->shape_chk) return;
    const BsTab *bs = sw->bs;
    u8 *m = xcalloc((size_t)(bs->nblocks ? bs->nblocks : 1), 1);
    static const char *N[] = { "minecraft:nether_brick_fence", "minecraft:torch", "minecraft:wall_torch", "minecraft:oak_fence", "minecraft:spruce_fence", "minecraft:dark_oak_fence",
                               "minecraft:pale_oak_fence", "minecraft:acacia_fence", "minecraft:birch_fence", "minecraft:jungle_fence", "minecraft:ladder", "minecraft:iron_bars", NULL };
    for (int i = 0; N[i]; i++) { int b = bs_block_index(bs, N[i]); if (b >= 0) m[b] = 1; }
    mutex_lock(sw->lock);
    if (!sw->shape_chk) sw->shape_chk = m; else free(m);
    mutex_unlock(sw->lock);
}
void sp_set_world(StCtx *c, int wx, int wy, int wz, int state) {
    if (!bb_inside(&c->chunk, wx, wy, wz)) return;
    fc_set(c->fc, wx, wy, wz, state, 2);
}
void sp_place(StCtx *c, const StPiece *p, int state, int x, int y, int z) {
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y), wz = sp_wz(p, x, z);
    if (!bb_inside(&c->chunk, wx, wy, wz)) return;
    if (p->vt && p->vt->can_replace && !p->vt->can_replace(c, p, x, y, z)) return;
    if (p->mir != MIR_NONE) state = bsx_mirror(c->sw->bs, state, p->mir);
    if (p->rot != ROT_NONE) state = bsx_rotate(c->sw->bs, state, p->rot);
    fc_set(c->fc, wx, wy, wz, state, 2);
    ensure_shape_chk(c->sw);
    if (c->sw->shape_chk[c->w->g->state_block[state]]) fc_mark(c->fc, wx, wy, wz);
}
void sp_air_box(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1) {
    int air = c->w->g->st_air;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) sp_place(c, p, air, x, y, z);
}
void sp_box(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1, int edge, int fill, int skip_air) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) {
        if (skip_air && gen_is_air(c->w->g, sp_get(c, p, x, y, z))) continue;
        if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1) sp_place(c, p, fill, x, y, z); else sp_place(c, p, edge, x, y, z);
    }
}
void sp_box_sel(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1, int skip_air, SPSel *sel) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) {
        if (skip_air && gen_is_air(c->w->g, sp_get(c, p, x, y, z))) continue;
        int st = sel->next(sel, c->rs, x, y, z, y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1);
        sp_place(c, p, st, x, y, z);
    }
}
void sp_maybe_box(StCtx *c, const StPiece *p, float prob, int x0, int y0, int z0, int x1, int y1, int z1, int edge, int fill, int skip_air, int has_to_be_inside) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) for (int z = z0; z <= z1; z++) {
        if (rs_float(c->rs) > prob) continue;
        if (skip_air && gen_is_air(c->w->g, sp_get(c, p, x, y, z))) continue;
        if (has_to_be_inside && !sp_is_interior(c, p, x, y, z)) continue;
        if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1) sp_place(c, p, fill, x, y, z); else sp_place(c, p, edge, x, y, z);
    }
}
void sp_maybe_block(StCtx *c, const StPiece *p, float prob, int x, int y, int z, int state) { if (rs_float(c->rs) < prob) sp_place(c, p, state, x, y, z); }
void sp_upper_half_sphere(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1, int fill, int skip_air) {
    float dx = (float)(x1 - x0 + 1), dy = (float)(y1 - y0 + 1), dz = (float)(z1 - z0 + 1);
    float cx = (float)x0 + dx / 2.0f, cz = (float)z0 + dz / 2.0f;
    for (int y = y0; y <= y1; y++) {
        float ny = (float)(y - y0) / dy;
        for (int x = x0; x <= x1; x++) {
            float nx = ((float)x - cx) / (dx * 0.5f);
            for (int z = z0; z <= z1; z++) {
                float nz = ((float)z - cz) / (dz * 0.5f);
                if (skip_air && gen_is_air(c->w->g, sp_get(c, p, x, y, z))) continue;
                float d = nx * nx + ny * ny + nz * nz;
                if (d <= 1.05f) sp_place(c, p, fill, x, y, z);
            }
        }
    }
}
int sp_replaceable_by_structures(StCtx *c, int state) {
    const BsTab *bs = c->sw->bs; const McGen *g = c->w->g;
    if (gen_is_air(g, state) || (bs->flags[state] & BSF_LIQUID)) return 1;
    int b = g->state_block[state];
    static _Thread_local const BsTab *kb; static _Thread_local int lichen, sg, tsg;
    if (kb != bs) { lichen = bs_block_index(bs, "minecraft:glow_lichen"); sg = bs_block_index(bs, "minecraft:seagrass"); tsg = bs_block_index(bs, "minecraft:tall_seagrass"); kb = bs; }
    return b == lichen || b == sg || b == tsg;
}
void sp_fill_column_down(StCtx *c, const StPiece *p, int state, int x, int start_y, int z) {
    int wx = sp_wx(p, x, z), wy = sp_wy(p, start_y), wz = sp_wz(p, x, z);
    if (!bb_inside(&c->chunk, wx, wy, wz)) return;
    while (sp_replaceable_by_structures(c, fc_get(c->fc, wx, wy, wz)) && wy > c->w->min_y + 1) { fc_set(c->fc, wx, wy, wz, state, 2); wy--; }
}
int sp_height(StCtx *c, int hm_type, int wx, int wz) {
    return structure_height(c->fc, hm_type, wx, wz);
}
int sp_is_interior(StCtx *c, const StPiece *p, int x, int y, int z) {
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y + 1), wz = sp_wz(p, x, z);
    if (!bb_inside(&c->chunk, wx, wy, wz)) return 0;
    return wy < sp_height(c, HM_OCEAN_FLOOR_WG, wx, wz);
}
int sp_reorient(StCtx *c, int wx, int wy, int wz, int chest_state) {
    const BsTab *bs = c->sw->bs; const McGen *g = c->w->g;
    static const int HD[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
    int chest = bs_block_index(bs, "minecraft:chest");
    int solid = -1, many = 0;
    for (int i = 0; i < 4; i++) {
        int st = fc_get(c->fc, wx + DIR_DX[HD[i]], wy, wz + DIR_DZ[HD[i]]);
        if (g->state_block[st] == chest) return chest_state;
        if (bs->flags[st] & BSF_SOLID_RENDER) { if (solid >= 0) { solid = -1; many = 1; break; } solid = HD[i]; }
    }
    (void)many;
    static const char *DN[6] = { "down", "up", "north", "south", "west", "east" };
    if (solid >= 0) return sp_with(c, chest_state, "facing", DN[dir_opp(solid)]);
    const char *fv = "north"; bs_get_prop(bs, chest_state, "facing", &fv);
    int lock = DIR_NORTH; for (int i = 0; i < 6; i++) if (!strcmp(DN[i], fv)) lock = i;
    #define SOLID_AT(d) ((bs->flags[fc_get(c->fc, wx + DIR_DX[d], wy, wz + DIR_DZ[d])] & BSF_SOLID_RENDER) != 0)
    if (SOLID_AT(lock)) lock = dir_opp(lock);
    if (SOLID_AT(lock)) lock = dir_cw(lock);
    if (SOLID_AT(lock)) lock = dir_opp(lock);
    #undef SOLID_AT
    return sp_with(c, chest_state, "facing", DN[lock]);
}
int sp_create_chest(StCtx *c, const StPiece *p, int x, int y, int z, int state) {
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y), wz = sp_wz(p, x, z);
    int chest = sp_st(c, "minecraft:chest");
    if (!bb_inside(&c->chunk, wx, wy, wz) || c->w->g->state_block[fc_get(c->fc, wx, wy, wz)] == c->w->g->state_block[chest]) return 0;
    if (state < 0) state = sp_reorient(c, wx, wy, wz, chest);
    fc_set(c->fc, wx, wy, wz, state, 2);
    (void)rs_long(c->rs);                    /* ChestBlockEntity.setLootTable(table, random.nextLong()) */
    return 1;
}
int sp_create_dispenser(StCtx *c, const StPiece *p, int x, int y, int z, int facing) {
    static const char *DN[6] = { "down", "up", "north", "south", "west", "east" };
    int wx = sp_wx(p, x, z), wy = sp_wy(p, y), wz = sp_wz(p, x, z);
    int disp = sp_st(c, "minecraft:dispenser");
    if (!bb_inside(&c->chunk, wx, wy, wz) || c->w->g->state_block[fc_get(c->fc, wx, wy, wz)] == c->w->g->state_block[disp]) return 0;
    sp_place(c, p, sp_with(c, disp, "facing", DN[facing]), x, y, z);
    (void)rs_long(c->rs);
    return 1;
}

/* ---------------------------------------------------------------- ScatteredFeaturePiece */
int sp_update_avg_ground(StCtx *c, StPiece *p, int *hpos, int offset) {
    if (*hpos >= 0) return 1;
    int total = 0, count = 0;
    for (int z = p->bb.z0; z <= p->bb.z1; z++) for (int x = p->bb.x0; x <= p->bb.x1; x++) {
        if (bb_inside(&c->chunk, x, 64, z)) { total += sp_height(c, HM_MOTION_BLOCKING_NO_LEAVES, x, z); count++; }
    }
    if (count == 0) return 0;
    *hpos = total / count;
    p->bb = bb_moved(p->bb, 0, *hpos - p->bb.y0 + offset, 0);
    return 1;
}
int sp_update_lowest_ground(StCtx *c, StPiece *p, int *hpos, int offset) {
    if (*hpos >= 0) return 1;
    int lowest = c->w->min_y + c->w->height, found = 0;      /* getMaxY() + 1 */
    for (int z = p->bb.z0; z <= p->bb.z1; z++) for (int x = p->bb.x0; x <= p->bb.x1; x++) {
        int h = sp_height(c, HM_MOTION_BLOCKING_NO_LEAVES, x, z);
        if (h < lowest) lowest = h;
        found = 1;
    }
    if (!found) return 0;
    *hpos = lowest;
    p->bb = bb_moved(p->bb, 0, *hpos - p->bb.y0 + offset, 0);
    return 1;
}

/* ---------------------------------------------------------------- ГСЧ */
Rnd *sp_region_random(StCtx *c) {
    static _Thread_local struct { McWorld *w; int cx, cz; long run; Rnd r; int ok; } t;
    long run = c->w->struct_run;
    if (!t.ok || t.w != c->w || t.cx != c->cx || t.cz != c->cz || t.run != run) {
        Rnd r = pos_from_hash(&c->w->pos_terrain, "minecraft:worldgen_region_random");
        PosRnd f = rnd_fork_positional(&r);
        t.r = pos_at(&f, c->cx * 16, 0, c->cz * 16); t.w = c->w; t.cx = c->cx; t.cz = c->cz; t.run = run; t.ok = 1;
    }
    return &t.r;
}
RS sp_seed_positional(StCtx *c, int x, int y, int z) {
    RS base; rs_seed_lcg(&base, c->w->seeds.structures);
    i64 fseed = rs_long(&base);
    RS r; rs_seed_lcg(&r, mth_get_seed(x, y, z) ^ fseed);
    return r;
}

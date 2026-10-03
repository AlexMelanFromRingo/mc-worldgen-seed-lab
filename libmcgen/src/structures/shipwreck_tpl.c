/* structures/shipwreck_tpl.c — общая часть шаблонных частей (TemplateStructurePiece): см. shipwreck_tpl.h.
 * Запись блоков повторяет StructureTemplate.placeInWorld (template.c: template_place), но дополнительно:
 *   — random.nextLong() на каждый записанный блок с NBT, чья блок-сущность — RandomizableContainer (LootTableSeed);
 *   — заливка «toFill» как в игре (в т. ч. для блоков, поставленных в воздух рядом с источником);
 *   — knownShape == false: updateShapeAtEdge по граням записанного объёма и Block.updateFromNeighbourShapes каждого записанного блока;
 *   — маркеры данных и jigsaw-блоки (TemplateStructurePiece.postProcess). */
#include "shipwreck_tpl.h"
#include "../blockstate.h"
#include <stdio.h>
#include <stdlib.h>

int tplp_init(McWorld *w, TplPiece *tp, const char *loc, const char *name, int x, int y, int z, int rot, int mir, int px, int pz) {
    memset(tp, 0, sizeof *tp);
    tp->t = template_get(w, loc);
    snprintf(tp->name, sizeof tp->name, "%s", name);
    tp->x = x; tp->y = y; tp->z = z; tp->rot = rot; tp->mir = mir; tp->px = px; tp->pz = pz;
    tp->waterlog = 1; tp->known_shape = 0;
    return tp->t != NULL;
}
void tplp_add_proc(TplPiece *tp, const Proc *p) { if (p && tp->nprocs < 8) tp->procs[tp->nprocs++] = p; }

BB tplp_bb(const TplPiece *tp) {
    if (!tp->t) return bb_make(tp->x, tp->y, tp->z, tp->x, tp->y, tp->z);
    return tpl_bounding_box(tp->t, tp->x, tp->y, tp->z, tp->rot, tp->mir, tp->px, tp->pz);
}

void tplp_connected(const TplPiece *parent, int ox, int oy, int oz, const TplPiece *child, int *dx, int *dy, int *dz) {
    int ax, az, bx, bz;
    tpl_transform(ox, oz, parent->mir, parent->rot, parent->px, parent->pz, &ax, &az);
    tpl_transform(0, 0, child->mir, child->rot, child->px, child->pz, &bx, &bz);
    *dx = ax - bx; *dy = oy; *dz = az - bz;
}

int tplp_is_container(StCtx *c, int st) {
    const BsTab *bs = c->sw->bs;
    return bs_is_a(bs, st, "ChestBlock") || bs_is_a(bs, st, "BarrelBlock") || bs_is_a(bs, st, "DispenserBlock") || bs_is_a(bs, st, "HopperBlock")
        || bs_is_a(bs, st, "ShulkerBoxBlock") || bs_is_a(bs, st, "DecoratedPotBlock") || bs_is_a(bs, st, "CrafterBlock");
}
void tplp_loot(StCtx *c, int x, int y, int z) {
    if (!fc_chunk(c->fc, x, z) || fc_outside(c->fc, y)) return;
    if (tplp_is_container(c, fc_get(c->fc, x, y, z))) (void)rs_long(c->rs);
}

/* ---------------------------------------------------------------- обновление форм (knownShape == false) */
static int has_wl(const BsTab *bs, int st) { return bs_has_prop(bs, st, "waterlogged"); }
static int facing_of(const BsTab *bs, int st) {
    const char *v; if (!bs_get_prop(bs, st, "facing", &v)) return -1;
    return dir_from_name(v);
}

/* StairBlock.getStairsShape */
static int is_stairs(const BsTab *bs, int st) { return bs_is_a(bs, st, "StairBlock"); }
static const char *half_of(const BsTab *bs, int st) { const char *v = ""; bs_get_prop(bs, st, "half", &v); return v; }
static int can_take_shape(StCtx *c, int st, int x, int y, int z, int dir) {
    const BsTab *bs = c->sw->bs;
    int n = fc_get(c->fc, x + DIR_DX[dir], y, z + DIR_DZ[dir]);
    return !is_stairs(bs, n) || facing_of(bs, n) != facing_of(bs, st) || strcmp(half_of(bs, n), half_of(bs, st)) != 0;
}
static const char *stairs_shape(StCtx *c, int st, int x, int y, int z) {
    const BsTab *bs = c->sw->bs;
    int facing = facing_of(bs, st);
    int behind = fc_get(c->fc, x + DIR_DX[facing], y, z + DIR_DZ[facing]);
    if (is_stairs(bs, behind) && !strcmp(half_of(bs, st), half_of(bs, behind))) {
        int bf = facing_of(bs, behind);
        int ax_b = (bf == DIR_EAST || bf == DIR_WEST), ax_f = (facing == DIR_EAST || facing == DIR_WEST);
        if (ax_b != ax_f && can_take_shape(c, st, x, y, z, dir_opp(bf))) return bf == dir_ccw(facing) ? "outer_left" : "outer_right";
    }
    int front = fc_get(c->fc, x - DIR_DX[facing], y, z - DIR_DZ[facing]);
    if (is_stairs(bs, front) && !strcmp(half_of(bs, st), half_of(bs, front))) {
        int ff = facing_of(bs, front);
        int ax_b = (ff == DIR_EAST || ff == DIR_WEST), ax_f = (facing == DIR_EAST || facing == DIR_WEST);
        if (ax_b != ax_f && can_take_shape(c, st, x, y, z, ff)) return ff == dir_ccw(facing) ? "inner_left" : "inner_right";
    }
    return "straight";
}

/* BlockState.updateShape(level, pos, dir, neighbourPos, neighbourState) для классов, у которых результат меняется при генерации;
 * остальные — без изменений */
static int update_shape(StCtx *c, int st, int x, int y, int z, int dir, int nst) {
    const BsTab *bs = c->sw->bs; const McGen *g = bs->g;
    if (is_stairs(bs, st)) {
        if (dir == DIR_UP || dir == DIR_DOWN) return st;
        int r = bs_with(bs, st, "shape", stairs_shape(c, st, x, y, z));
        return r < 0 ? st : r;
    }
    if (bs_is_a(bs, st, "DoorBlock")) {
        const char *half = ""; bs_get_prop(bs, st, "half", &half);
        int lower = !strcmp(half, "lower");
        if ((dir == DIR_UP && lower) || (dir == DIR_DOWN && !lower)) {
            if (g->state_block[nst] == g->state_block[st]) {
                const char *nh = ""; bs_get_prop(bs, nst, "half", &nh);
                if (strcmp(nh, half) != 0) {
                    static const char *P[4] = { "facing", "open", "hinge", "powered" };
                    int r = st; for (int i = 0; i < 4; i++) { const char *v; if (bs_get_prop(bs, nst, P[i], &v)) { int q = bs_with(bs, r, P[i], v); if (q >= 0) r = q; } }
                    return r;
                }
            }
            return g->st_air;
        }
        if (dir == DIR_DOWN && lower && !(bs->sturdy[nst] >> DIR_UP & 1)) return g->st_air;   /* canSurvive: опора снизу */
        return st;
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
    return st;
}
/* Block.updateFromNeighbourShapes: порядок UPDATE_SHAPE_ORDER = WEST, EAST, NORTH, SOUTH, DOWN, UP */
static int update_from_neighbours(StCtx *c, int st, int x, int y, int z) {
    static const int ORD[6] = { DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH, DIR_DOWN, DIR_UP };
    int cur = st;
    for (int i = 0; i < 6; i++) {
        int d = ORD[i];
        int n = fc_get(c->fc, x + DIR_DX[d], y + DIR_DY[d], z + DIR_DZ[d]);
        cur = update_shape(c, cur, x, y, z, d, n);
    }
    return cur;
}

typedef struct PSet { int *k; int cap, n; } PSet;      /* множество позиций (открытая адресация) */
static u64 pkey(int x, int y, int z) { return ((u64)(u32)(x + 0x2000000) << 38) ^ ((u64)(u32)(z + 0x2000000) << 12) ^ (u64)(u32)(y + 2048); }
static int pset_has(const PSet *s, int x, int y, int z, const int *pos) {
    if (!s->cap) return 0;
    u64 h = pkey(x, y, z) * 0x9E3779B97F4A7C15ULL; int i = (int)(h >> 40) & (s->cap - 1);
    while (s->k[i] >= 0) { const int *p = &pos[s->k[i] * 3]; if (p[0] == x && p[1] == y && p[2] == z) return 1; i = (i + 1) & (s->cap - 1); }
    return 0;
}
static void pset_build(PSet *s, const int *pos, int n) {
    s->cap = 16; while (s->cap < n * 2 + 2) s->cap *= 2;
    s->k = xmalloc((size_t)s->cap * sizeof(int)); for (int i = 0; i < s->cap; i++) s->k[i] = -1;
    for (int j = 0; j < n; j++) {
        u64 h = pkey(pos[j * 3], pos[j * 3 + 1], pos[j * 3 + 2]) * 0x9E3779B97F4A7C15ULL; int i = (int)(h >> 40) & (s->cap - 1);
        while (s->k[i] >= 0) i = (i + 1) & (s->cap - 1);
        s->k[i] = j;
    }
}

/* StructureTemplate.updateShapeAtEdge: для каждой грани объёма записанных блоков, за которой нет записанного блока —
 * state.updateShape к соседу и сосед.updateShape обратно (флаги 0: без пометок пост-обработки) */
static void shape_at_edge(StCtx *c, const int *pos, int n) {
    if (n == 0) return;
    PSet s; pset_build(&s, pos, n);
    int x0 = pos[0], y0 = pos[1], z0 = pos[2], x1 = x0, y1 = y0, z1 = z0;
    for (int i = 1; i < n; i++) {
        const int *p = &pos[i * 3];
        if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1]; if (p[2] < z0) z0 = p[2]; if (p[2] > z1) z1 = p[2];
    }
    /* DiscreteVoxelShape.forAllFaces: оси в порядке AxisCycle NONE (X), FORWARD (Y), BACKWARD (Z); внутри оси — обход (u, v, w) с w по оси,
     * грань «назад» (−ось) перед «вперёд» (+ось) */
    static const int AX[3][3] = { {DIR_WEST, DIR_EAST, 0}, {DIR_DOWN, DIR_UP, 1}, {DIR_NORTH, DIR_SOUTH, 2} };
    for (int a = 0; a < 3; a++) {
        /* AxisCycle: NONE → (x,y,z), FORWARD → (y,z,x), BACKWARD → (z,x,y); внешние циклы по двум другим осям, внутренний — по оси грани */
        int lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
        int o1 = (a + 1) % 3, o2 = (a + 2) % 3;
        for (int u = lo[o1]; u <= hi[o1]; u++) for (int v = lo[o2]; v <= hi[o2]; v++) {
            for (int wv = lo[a]; wv <= hi[a] + 1; wv++) {
                int q[3], r[3]; q[a] = wv - 1; q[o1] = u; q[o2] = v; r[a] = wv; r[o1] = u; r[o2] = v;
                int inq = q[a] >= lo[a] && pset_has(&s, q[0], q[1], q[2], pos);
                int inr = r[a] <= hi[a] && pset_has(&s, r[0], r[1], r[2], pos);
                if (inq == inr) continue;
                int px, py, pz, dir;
                if (inq) { px = q[0]; py = q[1]; pz = q[2]; dir = AX[a][1]; } else { px = r[0]; py = r[1]; pz = r[2]; dir = AX[a][0]; }
                int nx = px + DIR_DX[dir], ny = py + DIR_DY[dir], nz = pz + DIR_DZ[dir];
                int st = fc_get(c->fc, px, py, pz), ns = fc_get(c->fc, nx, ny, nz);
                int st2 = update_shape(c, st, px, py, pz, dir, ns);
                if (st2 != st) fc_set(c->fc, px, py, pz, st2, 0);
                int ns2 = update_shape(c, ns, nx, ny, nz, dir_opp(dir), st2);
                if (ns2 != ns) fc_set(c->fc, nx, ny, nz, ns2, 0);
            }
        }
    }
    free(s.k);
}

int tplp_post(StCtx *c, StPiece *p, TplPiece *tp, const BB *bounds, int rx, int ry, int rz, TplMarkerFn marker, void *ud) {
    McWorld *w = c->w; const BsTab *bs = c->sw->bs; FCtx *fc = c->fc; const McGen *g = bs->g;
    const Template *t = tp->t;
    p->bb = tplp_bb(tp);
    if (!t) return 0;
    const TPal *pal = tpl_palette_at(t, tp->x, tp->y, tp->z);
    if (!pal || pal->nb == 0 || t->sx < 1 || t->sy < 1 || t->sz < 1) return 0;
    TSettings s; tsettings_init(&s);
    s.rot = tp->rot; s.mir = tp->mir; s.px = tp->px; s.pz = tp->pz; s.bounds = bounds; s.waterlog = tp->waterlog;
    s.procs = tp->procs; s.nprocs = tp->nprocs;
    TInfo *list = NULL;
    int n = template_process(fc, w, t, tp->x, tp->y, tp->z, rx, ry, rz, &s, w->seeds.structures, &list, NULL);
    int *placed = xmalloc((size_t)(n ? n : 1) * 3 * sizeof(int)); int np = 0;
    int *tofill = xmalloc((size_t)(n ? n : 1) * 3 * sizeof(int)); int nfill = 0;
    int *locked = xmalloc((size_t)(n ? n : 1) * 3 * sizeof(int)); int nlocked = 0;
    for (int i = 0; i < n; i++) {
        const TInfo *b = &list[i];
        if (bounds && !bb_inside(bounds, b->x, b->y, b->z)) continue;
        int prev = s.waterlog ? bs->fluid[fc_get(fc, b->x, b->y, b->z)] : 0;
        int st = bsx_rotate(bs, bsx_mirror(bs, b->state, tp->mir), tp->rot);
        if (!fc_set(fc, b->x, b->y, b->z, st, 2)) continue;
        placed[np * 3] = b->x; placed[np * 3 + 1] = b->y; placed[np * 3 + 2] = b->z; np++;
        if (b->nbt && !fc_outside(fc, b->y) && tplp_is_container(c, st)) (void)rs_long(c->rs);     /* blockInfo.nbt.putLong("LootTableSeed", random.nextLong()) */
        if (s.waterlog) {
            int nf = bs->fluid[st];
            if (BS_FL_TYPE(nf) != FL_NONE && BS_FL_SOURCE(nf)) { locked[nlocked * 3] = b->x; locked[nlocked * 3 + 1] = b->y; locked[nlocked * 3 + 2] = b->z; nlocked++; }
            else if (has_wl(bs, st)) {
                const char *wv;
                /* LiquidBlockContainer.placeLiquid(level, pos, state, previousFluidState): вода-источник заливает */
                if (bs_get_prop(bs, st, "waterlogged", &wv) && !strcmp(wv, "false") && BS_FL_TYPE(prev) == FL_WATER && BS_FL_SOURCE(prev)) {
                    int ns = bs_with(bs, st, "waterlogged", "true"); if (ns >= 0) fc_set(fc, b->x, b->y, b->z, ns, 3);
                }
                if (!(BS_FL_TYPE(prev) != FL_NONE && BS_FL_SOURCE(prev))) { tofill[nfill * 3] = b->x; tofill[nfill * 3 + 1] = b->y; tofill[nfill * 3 + 2] = b->z; nfill++; }
            }
        }
    }
    /* заливка: соседи UP, NORTH, EAST, SOUTH, WEST; первый найденный источник (не «запертый») */
    {
        static const int D[5][3] = { {0,1,0}, {0,0,-1}, {1,0,0}, {0,0,1}, {-1,0,0} };
        int filled = 1;
        while (filled && nfill > 0) {
            filled = 0;
            int k = 0;
            for (int i = 0; i < nfill; i++) {
                int px = tofill[i * 3], py = tofill[i * 3 + 1], pz = tofill[i * 3 + 2];
                int fl = bs->fluid[fc_get(fc, px, py, pz)];
                int src = BS_FL_TYPE(fl) != FL_NONE && BS_FL_SOURCE(fl);
                for (int d = 0; d < 5 && !src; d++) {
                    int nx = px + D[d][0], ny = py + D[d][1], nz = pz + D[d][2];
                    int nfl = bs->fluid[fc_get(fc, nx, ny, nz)];
                    if (BS_FL_TYPE(nfl) != FL_NONE && BS_FL_SOURCE(nfl)) {
                        int lk = 0; for (int q = 0; q < nlocked; q++) if (locked[q * 3] == nx && locked[q * 3 + 1] == ny && locked[q * 3 + 2] == nz) { lk = 1; break; }
                        if (!lk) { fl = nfl; src = 1; }
                    }
                }
                if (src) {
                    int st = fc_get(fc, px, py, pz); const char *wv;
                    if (has_wl(bs, st)) {
                        if (BS_FL_TYPE(fl) == FL_WATER && bs_get_prop(bs, st, "waterlogged", &wv) && !strcmp(wv, "false")) { int ns = bs_with(bs, st, "waterlogged", "true"); if (ns >= 0) fc_set(fc, px, py, pz, ns, 3); }
                        filled = 1;
                        continue;                                  /* iterator.remove() */
                    }
                }
                tofill[k * 3] = px; tofill[k * 3 + 1] = py; tofill[k * 3 + 2] = pz; k++;
            }
            nfill = k;
        }
    }
    if (np > 0 && !tp->known_shape) {
        shape_at_edge(c, placed, np);
        for (int i = 0; i < np; i++) {
            int x = placed[i * 3], y = placed[i * 3 + 1], z = placed[i * 3 + 2];
            int st = fc_get(fc, x, y, z);
            int ns = update_from_neighbours(c, st, x, y, z);
            if (ns != st) fc_set(fc, x, y, z, ns, 16);
        }
    }
    free(placed); free(tofill); free(locked); free(list);
    /* маркеры данных: filterBlocks(templatePosition, settings, STRUCTURE_BLOCK) */
    int sblk = bs_block_index(bs, "minecraft:structure_block"), jblk = bs_block_index(bs, "minecraft:jigsaw");
    for (int i = 0; i < pal->nb; i++) {
        const TInfo *b = &pal->b[i];
        if (g->state_block[b->state] != sblk || !b->nbt) continue;
        int wx, wz; tpl_transform(b->x, b->z, tp->mir, tp->rot, tp->px, tp->pz, &wx, &wz);
        wx += tp->x; wz += tp->z; int wy = b->y + tp->y;
        if (bounds && !bb_inside(bounds, wx, wy, wz)) continue;
        const char *mode = nbt_str(nbt_get(b->nbt, "mode"), "");
        if (strcmp(mode, "DATA") != 0) continue;
        if (marker) marker(c, p, ud, nbt_str(nbt_get(b->nbt, "metadata"), ""), wx, wy, wz, bounds);
    }
    /* jigsaw-блоки: setBlockAndUpdate(pos, final_state) */
    for (int i = 0; i < pal->nb; i++) {
        const TInfo *b = &pal->b[i];
        if (g->state_block[b->state] != jblk || !b->nbt) continue;
        int wx, wz; tpl_transform(b->x, b->z, tp->mir, tp->rot, tp->px, tp->pz, &wx, &wz);
        wx += tp->x; wz += tp->z; int wy = b->y + tp->y;
        if (bounds && !bb_inside(bounds, wx, wy, wz)) continue;
        int st = gen_state_id(g, nbt_str(nbt_get(b->nbt, "final_state"), "minecraft:air"));
        fc_set(fc, wx, wy, wz, st >= 0 ? st : g->st_air, 3);
    }
    return 1;
}

void tplp_dump(const TplPiece *tp, StrBuf *o) {
    sb_printf(o, ",\"tpl\":\"%s\",\"tp\":[%d,%d,%d],\"trot\":\"%s\",\"tmir\":%d", tp->name, tp->x, tp->y, tp->z, TPLP_ROTN[tp->rot & 3], tp->mir);
}

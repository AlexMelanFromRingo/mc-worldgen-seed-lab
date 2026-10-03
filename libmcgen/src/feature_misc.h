/* feature_misc.h — общие мелочи для групп фич «подземные/ледяные/особые/Nether/End» (поток W12): итераторы BlockPos и пр.
 * Порядок обхода — как в игре: он важен, когда обход меняет блоки, от которых зависят следующие шаги. */
#ifndef MCGEN_FEATURE_MISC_H
#define MCGEN_FEATURE_MISC_H
#include "feature.h"

static inline int iabs_(int v) { return v < 0 ? -v : v; }
static inline int imin_(int a, int b) { return a < b ? a : b; }
static inline int imax_(int a, int b) { return a > b ? a : b; }
static inline int dist_manhattan(int ax, int ay, int az, int bx, int by, int bz) { return iabs_(ax - bx) + iabs_(ay - by) + iabs_(az - bz); }

/* BlockPos.manhattanOrdered(origin, reachX, reachY, reachZ, maxDepth): withinManhattan / withinClippedManhattan / withinBoxByManhattanDistance */
typedef struct ManIt { int ox, oy, oz, rx, ry, rz, max_depth, depth, max_x, max_y, x, y, zmirror, cx, cy, cz; } ManIt;
static inline void man_init(ManIt *m, int ox, int oy, int oz, int rx, int ry, int rz, int max_depth) {
    memset(m, 0, sizeof *m); m->ox = ox; m->oy = oy; m->oz = oz; m->rx = rx; m->ry = ry; m->rz = rz; m->max_depth = max_depth;
}
static inline int man_next(ManIt *m, int *px, int *py, int *pz) {
    if (m->zmirror) {
        m->zmirror = 0; m->cz = m->oz - (m->cz - m->oz);
        *px = m->cx; *py = m->cy; *pz = m->cz; return 1;
    }
    for (int found = 0; !found; m->y++) {
        if (m->y > m->max_y) {
            m->x++;
            if (m->x > m->max_x) {
                m->depth++;
                if (m->depth > m->max_depth) return 0;
                m->max_x = imin_(m->rx, m->depth);
                m->x = -m->max_x;
            }
            m->max_y = imin_(m->ry, m->depth - iabs_(m->x));
            m->y = -m->max_y;
        }
        int xx = m->x, yy = m->y, zz = m->depth - iabs_(xx) - iabs_(yy);
        if (zz <= m->rz) {
            m->zmirror = zz != 0;
            m->cx = m->ox + xx; m->cy = m->oy + yy; m->cz = m->oz + zz;
            found = 1;
        }
    }
    *px = m->cx; *py = m->cy; *pz = m->cz;
    return 1;
}

/* BlockPos.betweenClosed(minX…maxZ): x быстрее всего, затем y, затем z */
typedef struct BcIt { int minx, miny, minz, w, h, end, idx; } BcIt;
static inline void bc_init(BcIt *b, int minx, int miny, int minz, int maxx, int maxy, int maxz) {
    b->minx = minx; b->miny = miny; b->minz = minz; b->w = maxx - minx + 1; b->h = maxy - miny + 1;
    b->end = b->w * b->h * (maxz - minz + 1); b->idx = 0;
}
static inline int bc_next(BcIt *b, int *px, int *py, int *pz) {
    if (b->idx >= b->end) return 0;
    int x = b->idx % b->w, slice = b->idx / b->w, y = slice % b->h, z = slice / b->h;
    b->idx++;
    *px = b->minx + x; *py = b->miny + y; *pz = b->minz + z;
    return 1;
}

static inline int fc_max_y(const FCtx *c) { return c->min_y + c->height - 1; }
/* BlockState.is(Block) по индексу блока */
static inline int fc_is_block(const FCtx *c, int st, int blk) { return c->g->state_block[st] == blk; }
/* BlockState.is(HolderSet) по u8[nblocks] */
static inline int fc_in_set(const FCtx *c, int st, const u8 *set) { return set[c->g->state_block[st]] != 0; }
static inline int fc_state_is_water(const FCtx *c, int st) { return c->g->state_block[st] == c->g->blk_water; }
static inline int fc_liquid(const FCtx *c, int st) { return (c->bs->flags[st] & BSF_LIQUID) != 0; }
static inline int fc_solid(const FCtx *c, int st) { return (c->bs->flags[st] & BSF_SOLID) != 0; }
/* список placed_feature из JSON: массив (ссылки/встроенные) или одна ссылка */
Placed **fpm_placed_list(FParse *p, const Js *v, int *n);

#endif

/* feature_tree.c — TreeFeature.place (поток W11): двухфазность (блоки стволов/корней/листвы + украшатели + updateLeaves + updateShapeAtEdge),
 * разбор конфигурации дерева, java.util.HashSet<BlockPos> (порядок обхода влияет на ГСЧ украшателей), fallen_tree, регистрация типов.
 * Подробно — docs/blender/features-trees.md. */
#include "feature_tree.h"
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== java.util.HashSet<BlockPos> (по алгоритму HashMap, JDK 21) */
static inline int jhash(int x, int y, int z) {
    unsigned h = (unsigned)(((unsigned)y + (unsigned)z * 31u) * 31u + (unsigned)x);
    return (int)(h ^ (h >> 16));      /* HashMap.hash(): h ^ (h >>> 16) от BlockPos.hashCode() */
}
void jset_init(JSet *s) { memset(s, 0, sizeof *s); s->free = -1; jset_clear(s); }
void jset_clear(JSet *s) {
    if (!s->tab) { s->tcap = 16; s->tab = xmalloc(sizeof(int) * 16); }
    s->tsize = 16; s->thr = 12; s->size = 0; s->nn = 0; s->free = -1; s->lo = 16;
    for (int i = 0; i < 16; i++) s->tab[i] = -1;
}
void jset_free(JSet *s) { free(s->nodes); free(s->tab); memset(s, 0, sizeof *s); }
static int jnew(JSet *s, int x, int y, int z, int hash) {
    int i;
    if (s->free >= 0) { i = s->free; s->free = s->nodes[i].next; }
    else {
        if (s->nn == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->nodes = realloc(s->nodes, sizeof(JNode) * (size_t)s->cap); if (!s->nodes) abort(); }
        i = s->nn++;
    }
    JNode *n = &s->nodes[i]; n->x = x; n->y = y; n->z = z; n->hash = hash; n->next = -1;
    return i;
}
static void jresize(JSet *s) {
    int oc = s->tsize, nc = oc * 2;
    if (nc > s->tcap) { s->tcap = nc; s->tab = realloc(s->tab, sizeof(int) * (size_t)nc); if (!s->tab) abort(); }
    for (int j = 0; j < oc; j++) {
        int lh = -1, lt = -1, hh = -1, ht = -1;
        for (int e = s->tab[j]; e >= 0;) {
            int nx = s->nodes[e].next; s->nodes[e].next = -1;
            if ((s->nodes[e].hash & oc) == 0) { if (lt < 0) lh = e; else s->nodes[lt].next = e; lt = e; }
            else { if (ht < 0) hh = e; else s->nodes[ht].next = e; ht = e; }
            e = nx;
        }
        s->tab[j] = lh; s->tab[j + oc] = hh;
    }
    s->tsize = nc; s->thr *= 2; s->lo = 0;
}
int jset_add(JSet *s, int x, int y, int z) {
    int hash = jhash(x, y, z), idx = hash & (s->tsize - 1);
    int p = s->tab[idx];
    if (p < 0) { int n = jnew(s, x, y, z, hash); s->tab[idx] = n; }
    else {
        int len = 0;
        for (int e = p;; e = s->nodes[e].next) {
            const JNode *n = &s->nodes[e];
            if (n->hash == hash && n->x == x && n->y == y && n->z == z) return 0;
            len++;
            if (n->next < 0) { p = e; break; }
        }
        int nn = jnew(s, x, y, z, hash);
        s->nodes[p].next = nn;
        if (len >= 8) {                    /* binCount >= TREEIFY_THRESHOLD − 1 → treeifyBin */
            if (s->tsize < 64) jresize(s);
            else s->treeified++;
        }
    }
    if (idx < s->lo) s->lo = idx;
    if (++s->size > s->thr) jresize(s);
    return 1;
}
int jset_contains(const JSet *s, int x, int y, int z) {
    int hash = jhash(x, y, z);
    for (int e = s->tab[hash & (s->tsize - 1)]; e >= 0; e = s->nodes[e].next) {
        const JNode *n = &s->nodes[e];
        if (n->hash == hash && n->x == x && n->y == y && n->z == z) return 1;
    }
    return 0;
}
int jset_pop_first(JSet *s, BPos *out) {
    if (s->size == 0) return 0;
    int b = s->lo;
    while (b < s->tsize && s->tab[b] < 0) b++;
    if (b >= s->tsize) { s->size = 0; return 0; }
    s->lo = b;
    int e = s->tab[b]; JNode *n = &s->nodes[e];
    out->x = n->x; out->y = n->y; out->z = n->z;
    s->tab[b] = n->next; n->next = s->free; s->free = e; s->size--;
    return 1;
}
void blist_reset(BList *l) { l->n = 0; }
void blist_push(BList *l, int x, int y, int z) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->a = realloc(l->a, sizeof(BPos) * (size_t)l->cap); if (!l->a) abort(); }
    l->a[l->n].x = x; l->a[l->n].y = y; l->a[l->n].z = z; l->n++;
}
void blist_free(BList *l) { free(l->a); memset(l, 0, sizeof *l); }
void jset_to_list(const JSet *s, BList *out) {
    blist_reset(out);
    for (int b = 0; b < s->tsize; b++) for (int e = s->tab[b]; e >= 0; e = s->nodes[e].next) blist_push(out, s->nodes[e].x, s->nodes[e].y, s->nodes[e].z);
}
void blist_sort_by_y(BList *l) {            /* стабильная сортировка слиянием снизу вверх */
    int n = l->n; if (n < 2) return;
    BPos *tmp = xmalloc(sizeof(BPos) * (size_t)n), *a = l->a, *b = tmp;
    for (int w = 1; w < n; w *= 2) {
        for (int i = 0; i < n; i += 2 * w) {
            int m = i + w < n ? i + w : n, h = i + 2 * w < n ? i + 2 * w : n, p = i, q = m, k = i;
            while (p < m && q < h) b[k++] = a[q].y < a[p].y ? a[q++] : a[p++];
            while (p < m) b[k++] = a[p++];
            while (q < h) b[k++] = a[q++];
        }
        BPos *t = a; a = b; b = t;
    }
    if (a != l->a) memcpy(l->a, a, sizeof(BPos) * (size_t)n);
    free(tmp);
}

/* ====================================================================== тэги / вспомогательные */
const TreeTags *tree_tags(FParse *p) {
    TreeTags *t = fp_alloc(p, sizeof *t);
    t->replaceable_by_trees = gen_block_tag(p->g, "minecraft:replaceable_by_trees");
    t->logs = gen_block_tag(p->g, "minecraft:logs");
    t->leaves = gen_block_tag(p->g, "minecraft:leaves");
    t->replaceable_by_mushrooms = gen_block_tag(p->g, "minecraft:replaceable_by_mushrooms");
    t->prevents_leaf_decay = gen_block_tag(p->g, "minecraft:prevents_nearby_leaf_decay");
    t->vine = bs_block_index(p->bs, "minecraft:vine");
    return t;
}
static inline int blk_of(const FCtx *c, int st) { return c->g->state_block[st]; }

int tr_valid_pos(const TreeRun *tr, int x, int y, int z) {
    int st = fc_get(tr->c, x, y, z);
    return fc_is_air(tr->c, st) || tr->tags->replaceable_by_trees[blk_of(tr->c, st)];
}
int tr_is_air_or_leaves(const TreeRun *tr, int x, int y, int z) {
    int st = fc_get(tr->c, x, y, z);
    return fc_is_air(tr->c, st) || tr->tags->leaves[blk_of(tr->c, st)];
}
int tree_state_axis(const BsTab *bs, int st, int axis) {
    static const char *N[3] = { "x", "y", "z" };
    return bs_try_with(bs, st, "axis", N[axis]);
}
void tr_set_trunk(TreeRun *tr, int x, int y, int z, int st) { jset_add(tr->trunks, x, y, z); fc_set(tr->c, x, y, z, st, 19); }
void tr_set_root(TreeRun *tr, int x, int y, int z, int st) { jset_add(tr->roots, x, y, z); fc_set(tr->c, x, y, z, st, 19); }
void tr_set_foliage(TreeRun *tr, int x, int y, int z, int st) { jset_add(tr->foliage, x, y, z); fc_set(tr->c, x, y, z, st, 19); }
void tr_set_decor(TreeRun *tr, int x, int y, int z, int st) { if (tr->decor) jset_add(tr->decor, x, y, z); fc_set(tr->c, x, y, z, st, 19); }

void att_init(AttList *l) { l->a = l->inl; l->n = 0; l->cap = 48; }
void att_push(AttList *l, int x, int y, int z, int radius_off, int height_off, int sx, int sz) {
    if (l->n == l->cap) {
        int nc = l->cap * 2;
        FoliageAtt *na = xmalloc(sizeof(FoliageAtt) * (size_t)nc);
        memcpy(na, l->a, sizeof(FoliageAtt) * (size_t)l->n);
        if (l->a != l->inl) free(l->a);
        l->a = na; l->cap = nc;
    }
    FoliageAtt *a = &l->a[l->n++]; a->x = x; a->y = y; a->z = z; a->radius_off = radius_off; a->height_off = height_off; a->size_x = sx; a->size_z = sz;
}
void att_free(AttList *l) { if (l->a != l->inl) free(l->a); l->a = l->inl; l->n = 0; l->cap = 48; }

/* ====================================================================== временные структуры (на поток) */
typedef struct TreeScratch {
    JSet roots, trunks, foliage, decor, check[7];
    BList logs, leaves, rootl, tmp;
    u8 *shape; size_t shape_cap;
    int busy; struct TreeScratch *next;
} TreeScratch;
static _Thread_local TreeScratch *g_ts_free;
static TreeScratch *ts_acquire(void) {
    TreeScratch *t = g_ts_free;
    if (t) { g_ts_free = t->next; }
    else {
        t = xcalloc(1, sizeof *t);
        jset_init(&t->roots); jset_init(&t->trunks); jset_init(&t->foliage); jset_init(&t->decor);
        for (int i = 0; i < 7; i++) jset_init(&t->check[i]);
    }
    return t;
}
static void ts_release(TreeScratch *t) { t->next = g_ts_free; g_ts_free = t; }

/* ====================================================================== updateLeaves (BFS по расстоянию до ближайшего ствола) */
static inline int leaf_distance_prop(const FCtx *c, int st) {     /* значение distance (1..7) или 0, если свойства нет */
    const char *v = NULL;
    if (!bs_get_prop(c->bs, st, "distance", &v) || !v) return 0;
    return atoi(v);
}
static void update_leaves(TreeRun *tr, TreeScratch *ts, int minx, int miny, int minz, int sx, int sy, int sz) {
    FCtx *c = tr->c; const BsTab *bs = c->bs;
    size_t nshape = (size_t)sx * sy * sz;
    if (nshape > ts->shape_cap) { ts->shape = realloc(ts->shape, nshape); ts->shape_cap = nshape; if (!ts->shape) abort(); }
    u8 *shape = ts->shape; memset(shape, 0, nshape);
    #define SIDX(x, y, z) (((size_t)((x) - minx) * sy + (size_t)((y) - miny)) * sz + (size_t)((z) - minz))
    #define INSIDE(x, y, z) ((x) >= minx && (x) < minx + sx && (y) >= miny && (y) < miny + sy && (z) >= minz && (z) < minz + sz)
    BList *l = &ts->tmp;
    jset_to_list(tr->decor, l); for (int i = 0; i < l->n; i++) if (INSIDE(l->a[i].x, l->a[i].y, l->a[i].z)) shape[SIDX(l->a[i].x, l->a[i].y, l->a[i].z)] = 1;
    jset_to_list(tr->roots, l); for (int i = 0; i < l->n; i++) if (INSIDE(l->a[i].x, l->a[i].y, l->a[i].z)) shape[SIDX(l->a[i].x, l->a[i].y, l->a[i].z)] = 1;
    for (int i = 0; i < 7; i++) jset_clear(&ts->check[i]);
    jset_to_list(tr->trunks, l);
    for (int i = 0; i < l->n; i++) jset_add(&ts->check[0], l->a[i].x, l->a[i].y, l->a[i].z);
    int smallest = 0;
    for (;;) {
        while (smallest >= 7 || ts->check[smallest].size > 0) {
            if (smallest >= 7) goto done;
            BPos pos;
            if (!jset_pop_first(&ts->check[smallest], &pos)) break;
            if (INSIDE(pos.x, pos.y, pos.z)) {
                if (smallest != 0) {
                    int st = fc_get(c, pos.x, pos.y, pos.z);
                    char vb[4]; vb[0] = (char)('0' + smallest); vb[1] = 0;
                    int ns = bs_with(bs, st, "distance", vb);
                    if (ns >= 0) st = ns;               /* BlockState.setValue(DISTANCE, d) */
                    fc_set(c, pos.x, pos.y, pos.z, st, 19);
                }
                shape[SIDX(pos.x, pos.y, pos.z)] = 1;
                for (int d = 0; d < 6; d++) {
                    int nx = pos.x + DIR_DX[d], ny = pos.y + DIR_DY[d], nz = pos.z + DIR_DZ[d];
                    if (!INSIDE(nx, ny, nz) || shape[SIDX(nx, ny, nz)]) continue;
                    int cur = fc_get(c, nx, ny, nz);
                    int dist;
                    if (tr->tags->prevents_leaf_decay[blk_of(c, cur)]) dist = 0;
                    else { dist = leaf_distance_prop(c, cur); if (!dist) continue; }
                    int nd = dist < smallest + 1 ? dist : smallest + 1;
                    if (nd < 7) { jset_add(&ts->check[nd], nx, ny, nz); if (nd < smallest) smallest = nd; }
                }
            }
        }
        smallest++;
    }
done:
    tree_update_shape_at_edge(c, minx, miny, minz, sx, sy, sz, shape);
    #undef SIDX
    #undef INSIDE
}

static int tree_trace(void) { static int v = -1; if (v < 0) v = getenv("MCGEN_TREE_TRACE") != NULL; return v; }

/* ====================================================================== TreeFeature.place */
static int tree_max_free_height(TreeRun *tr, int max_h, int tx, int ty, int tz) {
    const TreeCfg *t = tr->t; FCtx *c = tr->c;
    for (int y = 0; y <= max_h + 1; y++) {
        int r = featsize_at(&t->size, max_h, y);
        for (int x = -r; x <= r; x++) for (int z = -r; z <= r; z++) {
            int px = tx + x, py = ty + y, pz = tz + z;
            if (!tr_is_free(tr, px, py, pz)) return y - 2;
            if (!t->ignore_vines && blk_of(c, fc_get(c, px, py, pz)) == t->tags->vine) return y - 2;
        }
    }
    return max_h;
}
static _Thread_local int g_why;
static int tree_do_place(TreeRun *tr, int ox, int oy, int oz) {
    g_why = 0;
    const TreeCfg *t = tr->t; FCtx *c = tr->c; FRnd *r = tr->r;
    int tree_h = tp_tree_height(&t->tp, r);
    int fol_h = fp_foliage_height(&t->fp, r, tree_h);
    int trunk_h = tree_h - fol_h;
    int leaf_r = fp_foliage_radius(&t->fp, r, trunk_h);
    int tox = ox, toy = oy, toz = oz;
    if (t->root) toy = oy + intprov_sample(t->root->trunk_offset_y, r);
    int miny = oy < toy ? oy : toy, maxy = (oy > toy ? oy : toy) + tree_h + 1;
    if (!(miny >= c->min_y + 1 && maxy <= c->min_y + c->height)) { g_why = 1; return 0; }
    int clipped = tree_max_free_height(tr, tree_h, tox, toy, toz);
    if (!(clipped >= tree_h || (t->size.min_clipped >= 0 && clipped >= t->size.min_clipped))) { g_why = 2; return 0; }
    if (t->root && !root_place(tr, ox, oy, oz, tox, toy, toz)) { g_why = 3; return 0; }
    AttList al; att_init(&al);
    tp_place_trunk(tr, clipped, tox, toy, toz, &al);
    for (int i = 0; i < al.n; i++) fp_create_foliage(tr, clipped, &al.a[i], fol_h, leaf_r);
    att_free(&al);
    return 1;
}
static int tree_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const TreeCfg *t = cfg;
    TreeScratch *ts = ts_acquire();
    TreeRun tr = { c, c->rnd, t, t->tags, &ts->roots, &ts->trunks, &ts->foliage, &ts->decor };
    jset_clear(&ts->roots); jset_clear(&ts->trunks); jset_clear(&ts->foliage); jset_clear(&ts->decor);
    int res = 0;
    int ok = tree_do_place(&tr, ox, oy, oz);
    if (ok && (ts->trunks.size > 0 || ts->foliage.size > 0)) {
        if (t->ndeco) {
            TreeCtx x; memset(&x, 0, sizeof x);
            x.c = c; x.r = c->rnd; x.tags = t->tags; x.tr = &tr;
            x.logs = ts->logs; x.leaves = ts->leaves; x.roots = ts->rootl;       /* буферы потока (передаются по значению и возвращаются) */
            jset_to_list(&ts->trunks, &x.logs); jset_to_list(&ts->foliage, &x.leaves); jset_to_list(&ts->roots, &x.roots);
            blist_sort_by_y(&x.logs); blist_sort_by_y(&x.leaves); blist_sort_by_y(&x.roots);
            for (int i = 0; i < t->ndeco; i++) deco_run(&x, &t->deco[i]);
            ts->logs = x.logs; ts->leaves = x.leaves; ts->rootl = x.roots;
        }
        /* BoundingBox.encapsulatingPositions(roots ∪ trunks ∪ foliage ∪ decorations) */
        int mnx = 1 << 30, mny = 1 << 30, mnz = 1 << 30, mxx = -(1 << 30), mxy = -(1 << 30), mxz = -(1 << 30), any = 0;
        const JSet *sets[4] = { &ts->roots, &ts->trunks, &ts->foliage, &ts->decor };
        for (int k = 0; k < 4; k++) {
            const JSet *s = sets[k];
            for (int b = 0; b < s->tsize; b++) for (int e = s->tab[b]; e >= 0; e = s->nodes[e].next) {
                const JNode *n = &s->nodes[e]; any = 1;
                if (n->x < mnx) mnx = n->x; if (n->x > mxx) mxx = n->x;
                if (n->y < mny) mny = n->y; if (n->y > mxy) mxy = n->y;
                if (n->z < mnz) mnz = n->z; if (n->z > mxz) mxz = n->z;
            }
        }
        if (any) { update_leaves(&tr, ts, mnx, mny, mnz, mxx - mnx + 1, mxy - mny + 1, mxz - mnz + 1); res = 1; }
    }
    if (tree_trace()) fprintf(stderr, "TREE chunk(%d,%d) at (%d,%d,%d) ok=%d res=%d trunks=%d foliage=%d decor=%d why=%d rnd=%llx\n", c->ccx, c->ccz, ox, oy, oz, ok, res, ts->trunks.size,
                              ts->foliage.size, ts->decor.size, g_why, (unsigned long long)c->rnd->x.lo);
    ts_release(ts);
    return res;
}

/* ====================================================================== разбор конфигурации */
static int parse_featsize(FParse *p, const Js *v, FeatSize *fs) {
    const char *t = js_str(js_get(v, "type"), "minecraft:two_layers_feature_size");
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    memset(fs, 0, sizeof *fs); fs->min_clipped = -1;
    Js *mc = js_get(v, "min_clipped_height"); if (mc) fs->min_clipped = js_int(mc, -1);
    if (!strcmp(t, "two_layers_feature_size")) {
        fs->type = FS_TWO; fs->limit = js_int(js_get(v, "limit"), 1); fs->lower = js_int(js_get(v, "lower_size"), 0); fs->upper = js_int(js_get(v, "upper_size"), 1);
    } else if (!strcmp(t, "three_layers_feature_size")) {
        fs->type = FS_THREE; fs->limit = js_int(js_get(v, "limit"), 1); fs->upper_limit = js_int(js_get(v, "upper_limit"), 1);
        fs->lower = js_int(js_get(v, "lower_size"), 0); fs->middle = js_int(js_get(v, "middle_size"), 1); fs->upper = js_int(js_get(v, "upper_size"), 1);
    } else return fp_fail(p, "feature_size: неизвестный тип %s", t);
    return 1;
}
int featsize_at(const FeatSize *fs, int tree_height, int yo) {
    if (fs->type == FS_TWO) return yo < fs->limit ? fs->lower : fs->upper;
    if (yo < fs->limit) return fs->lower;
    return yo >= tree_height - fs->upper_limit ? fs->upper : fs->middle;
}

static void *tree_parse(FParse *p, const Js *cfg) {
    TreeCfg *t = fp_alloc(p, sizeof *t);
    if (!(t->trunk_prov = fp_bsprov(p, js_get(cfg, "trunk_provider")))) return NULL;
    if (!(t->foliage_prov = fp_bsprov(p, js_get(cfg, "foliage_provider")))) return NULL;
    Js *bt = js_get(cfg, "below_trunk_provider");
    if (bt) { if (!(t->below_prov = fp_bsprov(p, bt))) return NULL; }
    else if (!(t->below_prov = fp_bsprov(p, js_get(cfg, "dirt_provider")))) return NULL;
    t->ignore_vines = js_bool(js_get(cfg, "ignore_vines"), 0);
    if (!parse_featsize(p, js_get(cfg, "minimum_size"), &t->size)) return NULL;
    if (!tree_parse_trunk(p, js_get(cfg, "trunk_placer"), &t->tp)) return NULL;
    if (!tree_parse_foliage(p, js_get(cfg, "foliage_placer"), &t->fp)) return NULL;
    if (js_get(cfg, "root_placer")) { t->root = tree_parse_root(p, js_get(cfg, "root_placer")); if (!t->root) return NULL; }
    if (!tree_parse_decorators(p, js_get(cfg, "decorators"), &t->deco, &t->ndeco)) return NULL;
    t->tags = tree_tags(p);
    if (t->tp.type == TP_UPWARDS && !t->tp.can_grow_through) return NULL;
    return t;
}

/* ====================================================================== TrunkPlacer.isFree / validTreePos (виртуальные: у upwards_branching свой can_grow_through) */
int tp_valid_pos(const TreeRun *tr, int x, int y, int z) {
    if (tr_valid_pos(tr, x, y, z)) return 1;
    if (tr->t->tp.type == TP_UPWARDS) return tr->t->tp.can_grow_through[blk_of(tr->c, fc_get(tr->c, x, y, z))] != 0;
    return 0;
}
int tr_is_free(const TreeRun *tr, int x, int y, int z) {
    if (tp_valid_pos(tr, x, y, z)) return 1;
    return tr->tags->logs[blk_of(tr->c, fc_get(tr->c, x, y, z))] != 0;
}

/* ====================================================================== fallen_tree */
static void *fallen_parse(FParse *p, const Js *cfg) {
    FallenCfg *f = fp_alloc(p, sizeof *f);
    if (!(f->trunk_prov = fp_bsprov(p, js_get(cfg, "trunk_provider")))) return NULL;
    if (!(f->log_length = fp_intprov(p, js_get(cfg, "log_length")))) return NULL;
    if (!tree_parse_decorators(p, js_get(cfg, "stump_decorators"), &f->stump, &f->nstump)) return NULL;
    if (!tree_parse_decorators(p, js_get(cfg, "log_decorators"), &f->log, &f->nlog)) return NULL;
    f->tags = tree_tags(p);
    return f;
}
static int fallen_may_place_on(FCtx *c, const FallenCfg *f, int x, int y, int z);
static int fallen_over_solid(FCtx *c, int x, int y, int z) {
    int below = fc_get(c, x, y - 1, z);
    return (c->bs->sturdy[below] >> DIR_UP) & 1;            /* isFaceSturdy(level, pos, UP) */
}
static int fallen_valid_pos(const FCtx *c, const FallenCfg *f, int x, int y, int z) {
    int st = fc_get(c, x, y, z);
    return fc_is_air(c, st) || f->tags->replaceable_by_trees[c->g->state_block[st]];
}
static int fallen_may_place_on(FCtx *c, const FallenCfg *f, int x, int y, int z) { return fallen_valid_pos(c, f, x, y, z) && fallen_over_solid(c, x, y, z); }
static void fallen_place_log(FCtx *c, const FallenCfg *f, int x, int y, int z, int axis) {
    int st = bsprov_state(c, f->trunk_prov, x, y, z);
    if (axis >= 0) st = tree_state_axis(c->bs, st, axis);
    fc_set(c, x, y, z, st, 3);                              /* setBlockAndUpdate */
    fc_mark_above(c, x, y, z);
}
static void fallen_decorate(FCtx *c, const FallenCfg *f, const BList *logs_in, const TreeDeco *d, int nd) {
    if (!nd) return;
    TreeCtx x; memset(&x, 0, sizeof x);
    x.c = c; x.r = c->rnd; x.tags = f->tags; x.tr = NULL;
    for (int i = 0; i < logs_in->n; i++) blist_push(&x.logs, logs_in->a[i].x, logs_in->a[i].y, logs_in->a[i].z);
    blist_sort_by_y(&x.logs);
    for (int i = 0; i < nd; i++) deco_run(&x, &d[i]);
    blist_free(&x.logs); blist_free(&x.leaves); blist_free(&x.roots);
}
static int fallen_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const FallenCfg *f = cfg; FRnd *r = c->rnd;
    /* placeStump: log без изменения оси + украшатели пня (множество из одного блока) */
    fallen_place_log(c, f, ox, oy, oz, -1);
    BList one = {0}; blist_push(&one, ox, oy, oz);
    fallen_decorate(c, f, &one, f->stump, f->nstump);
    blist_free(&one);
    static const int HX[4] = { 0, 1, 0, -1 }, HZ[4] = { -1, 0, 1, 0 };       /* Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST */
    int dir = frnd_int_bound(r, 4);
    int log_len = intprov_sample(f->log_length, r) - 2;
    int dist = 2 + frnd_int_bound(r, 2);
    int sx = ox + HX[dir] * dist, sy = oy, sz = oz + HZ[dir] * dist;
    /* setGroundHeightForFallenLogStartPos */
    sy += 1;
    for (int i = 0; i < 6; i++) { if (fallen_may_place_on(c, f, sx, sy, sz)) break; sy--; }
    /* canPlaceEntireFallenLog */
    int gap = 0, ok = 1, px = sx, pz = sz;
    for (int i = 0; i < log_len; i++) {
        if (!fallen_valid_pos(c, f, px, sy, pz)) { ok = 0; break; }
        if (!fallen_over_solid(c, px, sy, pz)) { if (++gap > 2) { ok = 0; break; } } else gap = 0;
        px += HX[dir]; pz += HZ[dir];
    }
    if (ok) {
        BList logs = {0};
        int axis = HX[dir] != 0 ? 0 : 2;
        px = sx; pz = sz;
        for (int i = 0; i < log_len; i++) { fallen_place_log(c, f, px, sy, pz, axis); blist_push(&logs, px, sy, pz); px += HX[dir]; pz += HZ[dir]; }
        /* HashSet<BlockPos> fallenLog — порядок обхода как у HashSet */
        JSet hs; jset_init(&hs);
        for (int i = 0; i < logs.n; i++) jset_add(&hs, logs.a[i].x, logs.a[i].y, logs.a[i].z);
        BList ordered = {0}; jset_to_list(&hs, &ordered);
        fallen_decorate(c, f, &ordered, f->log, f->nlog);
        jset_free(&hs); blist_free(&ordered); blist_free(&logs);
    }
    return 1;
}

static const FeatType T_TREE = { "minecraft:tree", tree_parse, tree_place };
static const FeatType T_FALLEN = { "minecraft:fallen_tree", fallen_parse, fallen_place };
void feature_register_trees(void) { feature_register_type(&T_TREE); feature_register_type(&T_FALLEN); }

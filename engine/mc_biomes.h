/* mc_biomes.h — таблица климатических параметров биомов и поиск ближайшего (порт Climate.RTree).
 *
 * Порядок точек и структура дерева воспроизводят Climate.RTree.create/build (26.1–26.3, идентичны):
 *   childrenPerNode = 19, 7 измерений (temperature, humidity, continentalness, erosion, depth, weirdness, offset).
 * Поиск: ParameterPoint.fitness / Node.distance, порядок обхода как в Climate.RTree.SubTree.search
 * (с candidate = null; Java хранит lastResult в ThreadLocal — это влияет ТОЛЬКО на ничьи fitness).
 */
#ifndef MC_BIOMES_H
#define MC_BIOMES_H
#include "mc_climate.h"

#define MC_RT_DIM 7
typedef struct { i64 lo[MC_RT_DIM], hi[MC_RT_DIM]; } McParamBox;

typedef struct {
    McParamBox box;
    i32 first;        /* subtree: индекс первого потомка в массиве child_idx; leaf: -1 */
    i32 count;        /* число потомков; leaf: 0 */
    i32 biome;        /* leaf: id биома; subtree: -1 */
} McRNode;

#define MC_RT_MAX_NODES 12000     /* 7595 листьев + внутренние узлы */
typedef struct {
    int n_nodes;
    int root;
    McRNode node[MC_RT_MAX_NODES];
    int n_child;
    i32 child_idx[MC_RT_MAX_NODES];
} McBiomeTree;

MC_HD MC_INLINE i64 mc_param_dist(i64 lo, i64 hi, i64 t) {
    i64 above = t - hi, below = lo - t;
    return above > 0 ? above : (below > 0 ? below : 0);
}
MC_HD MC_INLINE i64 mc_rnode_distance(const McRNode *n, const i64 target[MC_RT_DIM]) {
    i64 d = 0;
    for (int i = 0; i < MC_RT_DIM; i++) { i64 x = mc_param_dist(n->box.lo[i], n->box.hi[i], target[i]); d += x * x; }
    return d;
}

/* Поиск: возвращает индекс листа */
MC_HD static int mc_rt_search_node(const McBiomeTree *T, int ni, const i64 target[MC_RT_DIM], int candidate) {
    const McRNode *n = &T->node[ni];
    if (n->biome >= 0 && n->count == 0) return ni;          /* leaf */
    i64 minD = candidate < 0 ? 0x7fffffffffffffffLL : mc_rnode_distance(&T->node[candidate], target);
    int closest = candidate;
    for (int c = 0; c < n->count; c++) {
        int ci = T->child_idx[n->first + c];
        i64 cd = mc_rnode_distance(&T->node[ci], target);
        if (minD > cd) {
            int leaf = mc_rt_search_node(T, ci, target, closest);
            i64 ld = (ci == leaf) ? cd : mc_rnode_distance(&T->node[leaf], target);
            if (minD > ld) { minD = ld; closest = leaf; }
        }
    }
    return closest;
}
MC_HD MC_INLINE int mc_biome_find(const McBiomeTree *T, const McTarget *t) {
    i64 target[MC_RT_DIM] = {t->t, t->h, t->c, t->e, t->d, t->w, 0};
    int leaf = mc_rt_search_node(T, T->root, target, -1);
    return T->node[leaf].biome;
}

#ifndef __CUDA_ARCH__
#include <stdlib.h>
/* ------------------------------ построение дерева (хост) ------------------------------ */
typedef struct { const McBiomeTree *T; int dim; int absolute; } McCmpCtx;
static i64 mc_center(const McParamBox *b, int d) { return (b->lo[d] + b->hi[d]) / 2; }   /* деление к нулю как в Java */
static i64 mc_labs(i64 v) { return v < 0 ? -v : v; }

/* stable merge sort индексов по компаратору */
typedef int (*mc_cmp_fn)(const void *ctx, int a, int b);
static void mc_stable_sort(int *idx, int n, mc_cmp_fn cmp, const void *ctx) {
    if (n < 2) return;
    int *tmp = (int *)malloc(sizeof(int) * (size_t)n);
    for (int width = 1; width < n; width *= 2) {
        for (int i = 0; i < n; i += 2 * width) {
            int lo = i, mid = i + width < n ? i + width : n, hi = i + 2 * width < n ? i + 2 * width : n;
            int a = lo, b = mid, k = lo;
            while (a < mid && b < hi) tmp[k++] = (cmp(ctx, idx[b], idx[a]) < 0) ? idx[b++] : idx[a++];
            while (a < mid) tmp[k++] = idx[a++];
            while (b < hi) tmp[k++] = idx[b++];
        }
        memcpy(idx, tmp, sizeof(int) * (size_t)n);
    }
    free(tmp);
}
static int mc_cmp_dim(const void *ctxp, int a, int b) {
    const McCmpCtx *c = (const McCmpCtx *)ctxp;
    for (int k = 0; k < MC_RT_DIM; k++) {
        int d = (c->dim + k) % MC_RT_DIM;
        i64 ka = mc_center(&c->T->node[a].box, d), kb = mc_center(&c->T->node[b].box, d);
        if (c->absolute) { ka = mc_labs(ka); kb = mc_labs(kb); }
        if (ka != kb) return ka < kb ? -1 : 1;
    }
    return 0;
}
static int mc_cmp_magnitude(const void *ctxp, int a, int b) {
    const McBiomeTree *T = (const McBiomeTree *)ctxp;
    i64 ta = 0, tb = 0;
    for (int d = 0; d < MC_RT_DIM; d++) { ta += mc_labs(mc_center(&T->node[a].box, d)); tb += mc_labs(mc_center(&T->node[b].box, d)); }
    return ta < tb ? -1 : (ta > tb ? 1 : 0);
}
typedef struct { const McBiomeTree *T; const int *bn; int dim; } McBucketCtx;
static int mc_cmp_bucket_abs(const void *ctxp, int a, int b) {
    const McBucketCtx *c = (const McBucketCtx *)ctxp;
    for (int k = 0; k < MC_RT_DIM; k++) {
        int d = (c->dim + k) % MC_RT_DIM;
        i64 ka = mc_labs(mc_center(&c->T->node[c->bn[a]].box, d)), kb = mc_labs(mc_center(&c->T->node[c->bn[b]].box, d));
        if (ka != kb) return ka < kb ? -1 : 1;
    }
    return 0;
}

static int mc_rt_new_node(McBiomeTree *T) { return T->n_nodes++; }
static void mc_rt_make_subtree(McBiomeTree *T, int node, const int *ch, int n) {
    McRNode *r = &T->node[node];
    r->first = T->n_child; r->count = n; r->biome = -1;
    for (int i = 0; i < n; i++) T->child_idx[T->n_child++] = ch[i];
    for (int d = 0; d < MC_RT_DIM; d++) {
        i64 lo = T->node[ch[0]].box.lo[d], hi = T->node[ch[0]].box.hi[d];
        for (int i = 1; i < n; i++) { if (T->node[ch[i]].box.lo[d] < lo) lo = T->node[ch[i]].box.lo[d]; if (T->node[ch[i]].box.hi[d] > hi) hi = T->node[ch[i]].box.hi[d]; }
        r->box.lo[d] = lo; r->box.hi[d] = hi;
    }
}
static i64 mc_box_cost(const McParamBox *b) { i64 s = 0; for (int d = 0; d < MC_RT_DIM; d++) s += mc_labs(b->hi[d] - b->lo[d]); return s; }

/* build(children[0..n)) -> индекс узла. `ch` может переупорядочиваться. */
static int mc_rt_build(McBiomeTree *T, int *ch, int n, int cpn) {
    if (n == 1) return ch[0];
    if (n <= cpn) {
        mc_stable_sort(ch, n, mc_cmp_magnitude, T);
        int nd = mc_rt_new_node(T); mc_rt_make_subtree(T, nd, ch, n); return nd;
    }
    i64 minCost = 0x7fffffffffffffffLL; int minDim = -1;
    int *bestBuckets = NULL; int *bestBucketStart = NULL; int bestNB = 0; int *bestOrder = NULL;
    int *order = (int *)malloc(sizeof(int) * (size_t)n); memcpy(order, ch, sizeof(int) * (size_t)n);
    int expected = (int)pow((double)cpn, floor(log((double)n - 0.01) / log((double)cpn)));
    for (int d = 0; d < MC_RT_DIM; d++) {
        McCmpCtx c = {T, d, 0};
        mc_stable_sort(order, n, mc_cmp_dim, &c);           /* сортировка «на месте» — следующая итерация продолжает с этого порядка */
        /* bucketize */
        int nb = 0; int *starts = (int *)malloc(sizeof(int) * (size_t)(n / expected + 2));
        for (int i = 0; i < n; i += expected) starts[nb++] = i;
        starts[nb] = n;
        i64 total = 0;
        for (int b = 0; b < nb; b++) {
            int s = starts[b], e = starts[b + 1];
            McParamBox box; McRNode tmp; (void)tmp;
            for (int k = 0; k < MC_RT_DIM; k++) {
                i64 lo = T->node[order[s]].box.lo[k], hi = T->node[order[s]].box.hi[k];
                for (int i = s + 1; i < e; i++) { if (T->node[order[i]].box.lo[k] < lo) lo = T->node[order[i]].box.lo[k]; if (T->node[order[i]].box.hi[k] > hi) hi = T->node[order[i]].box.hi[k]; }
                box.lo[k] = lo; box.hi[k] = hi;
            }
            total += mc_box_cost(&box);
        }
        if (minCost > total) {
            minCost = total; minDim = d;
            free(bestOrder); free(bestBucketStart);
            bestOrder = (int *)malloc(sizeof(int) * (size_t)n); memcpy(bestOrder, order, sizeof(int) * (size_t)n);
            bestBucketStart = starts; bestNB = nb;
        } else free(starts);
    }
    (void)bestBuckets;
    /* создаём SubTree-«корзины» (для сортировки по абсолютному центру), потом строим детей */
    int *bnode = (int *)malloc(sizeof(int) * (size_t)bestNB);
    for (int b = 0; b < bestNB; b++) {
        /* временный узел только с боксом (potomki не нужны — перестроим ниже) */
        int nd = mc_rt_new_node(T);
        int s = bestBucketStart[b], e = bestBucketStart[b + 1];
        McRNode *r = &T->node[nd]; r->first = 0; r->count = 0; r->biome = -1;
        for (int k = 0; k < MC_RT_DIM; k++) {
            i64 lo = T->node[bestOrder[s]].box.lo[k], hi = T->node[bestOrder[s]].box.hi[k];
            for (int i = s + 1; i < e; i++) { if (T->node[bestOrder[i]].box.lo[k] < lo) lo = T->node[bestOrder[i]].box.lo[k]; if (T->node[bestOrder[i]].box.hi[k] > hi) hi = T->node[bestOrder[i]].box.hi[k]; }
            r->box.lo[k] = lo; r->box.hi[k] = hi;
        }
        bnode[b] = nd;
    }
    int *border = (int *)malloc(sizeof(int) * (size_t)bestNB);
    for (int b = 0; b < bestNB; b++) border[b] = b;
    /* сортировка корзин по абсолютному центру (absolute=true) начиная с minDim */
    {
        McBucketCtx c2 = {T, bnode, minDim};
        mc_stable_sort(border, bestNB, mc_cmp_bucket_abs, &c2);
    }
    int *kids = (int *)malloc(sizeof(int) * (size_t)bestNB);
    for (int bi = 0; bi < bestNB; bi++) {
        int b = border[bi];
        int s = bestBucketStart[b], e = bestBucketStart[b + 1];
        int cnt = e - s;
        int *sub = (int *)malloc(sizeof(int) * (size_t)cnt);
        memcpy(sub, bestOrder + s, sizeof(int) * (size_t)cnt);
        kids[bi] = mc_rt_build(T, sub, cnt, cpn);
        free(sub);
    }
    int nd = mc_rt_new_node(T); mc_rt_make_subtree(T, nd, kids, bestNB);
    free(kids); free(border); free(bnode); free(bestOrder); free(bestBucketStart); free(order);
    return nd;
}
/* Построить дерево по списку из n точек (в порядке parameterList) */
static McBiomeTree *mc_rt_create(const McParamBox *boxes, const int *biomes, int n, int cpn) {
    McBiomeTree *T = (McBiomeTree *)calloc(1, sizeof(McBiomeTree));
    int *leaves = (int *)malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        int nd = mc_rt_new_node(T);
        T->node[nd].box = boxes[i]; T->node[nd].first = -1; T->node[nd].count = 0; T->node[nd].biome = biomes[i];
        leaves[i] = nd;
    }
    T->root = mc_rt_build(T, leaves, n, cpn);
    free(leaves);
    return T;
}
#endif /* !__CUDA_ARCH__ */

#endif /* MC_BIOMES_H */

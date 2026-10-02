/* biome.c — источники биомов: мультишумовой (Overworld/Nether: климат из роутера → R-дерево), End (TheEndBiomeSource),
 * фиксированный. Климат считается density-функциями роутера мира (с учётом тонких настроек), как Climate.Sampler. */
#include "mcgen_internal.h"
#include "df_old.h"
#include "mc_biomes.h"
#include "mcgen_test.h"
#include <stdlib.h>

static _Thread_local SCtx *tls_uncached;   /* SamplerContext.EMPTY_UNCACHED: кэши выключены, буферы — свои у потока */
static SCtx *uncached_ctx(void) { if (!tls_uncached) tls_uncached = sctx_new(NULL, 0); return tls_uncached; }

/* Climate.RTree.search: *last — лист предыдущего поиска (ThreadLocal lastResult игры), −1 — нет. При равных расстояниях
 * («ничья») побеждает кандидат lastResult, поэтому результат зависит от порядка запросов: внутри чанка он
 * детерминирован (ChunkAccess.fillBiomesFromNoise), первый запрос чанка в игре наследует лист от чужого чанка потока. */
static int find_biome_last(const McWorld *w, const float v[6], int *last) {
    i64 target[MC_RT_DIM] = { (i64)(v[0] * 10000.0f), (i64)(v[1] * 10000.0f), (i64)(v[2] * 10000.0f),
                              (i64)(v[3] * 10000.0f), (i64)(v[4] * 10000.0f), (i64)(v[5] * 10000.0f), 0 };
    const McBiomeTree *T = w->preset->biome_source == BS_MULTI_NETHER ? w->g->nether_tree : w->g->ow_tree;
    int leaf = mc_rt_search_node(T, T->root, target, last ? *last : -1);
    if (last) *last = leaf;
    return T->node[leaf].biome;
}
static int find_biome(const McWorld *w, const float v[6]) { return find_biome_last(w, v, NULL); }
int world_biome_from_climate(const McWorld *w, const float v[6]) { return find_biome(w, v); }

static int end_biome(const McWorld *w, int qx, int qy, int qz) {
    const McGen *g = w->g;
    int bx = qx * 4, by = qy * 4, bz = qz * 4;
    int cx = bx >> 4, cz = bz >> 4;
    static _Thread_local const McGen *cg; static _Thread_local int ids[5];
    if (cg != g) {
        ids[0] = gen_biome_id(g, "minecraft:the_end"); ids[1] = gen_biome_id(g, "minecraft:end_highlands");
        ids[2] = gen_biome_id(g, "minecraft:end_midlands"); ids[3] = gen_biome_id(g, "minecraft:small_end_islands");
        ids[4] = gen_biome_id(g, "minecraft:end_barrens"); cg = g;
    }
    if ((i64)cx * cx + (i64)cz * cz <= 4096LL) return ids[0];
    int wx = (cx * 2 + 1) * 8, wz = (cz * 2 + 1) * 8;
    double h;
    if (g->newf) h = (double)s_value(uncached_ctx(), w->s_rf[RF_EROSION], wx, by, wz);
    else h = old_router_point(w->old, RF_EROSION, wx, by, wz);
    if (h > 0.25) return ids[1];
    if (h >= -0.0625) return ids[2];
    return h < -0.21875 ? ids[3] : ids[4];
}

/* Climate.Sampler.sample в точке (блоки) */
static void climate_point(const McWorld *w, int bx, int by, int bz, float v[6]) {
    static const int F[6] = { RF_TEMPERATURE, RF_VEGETATION, RF_CONTINENTS, RF_EROSION, RF_DEPTH, RF_RIDGES };
    if (w->g->newf) { SCtx *x = uncached_ctx(); for (int k = 0; k < 6; k++) v[k] = s_value(x, w->s_rf[F[k]], bx, by, bz); }
    else for (int k = 0; k < 6; k++) v[k] = (float)old_router_point(w->old, F[k], bx, by, bz);
}

int mcgen_x_climate(const McWorld *w, int qx, int qy, int qz, float out[6]) { climate_point(w, qx * 4, qy * 4, qz * 4, out); return 0; }

int world_biome_noise(const McWorld *w, int qx, int qy, int qz) {
    switch (w->preset->biome_source) {
    case BS_FIXED: return w->preset->fixed_biome;
    case BS_THE_END: return end_biome(w, qx, qy, qz);
    default: { float v[6]; climate_point(w, qx * 4, qy * 4, qz * 4, v); return find_biome(w, v); }
    }
}

/* ---- биом клетки «как хранит чанк» (с учётом ничьих R-дерева) ----
 * Climate.RTree.search с кандидатом C возвращает C, если расстояние до C равно минимальному d*, иначе — первый лист
 * с расстоянием d* в порядке обхода (= поиск без кандидата, L0). Поэтому история нужна только в клетках с ничьей:
 * идём назад по порядку заполнения чанка, пока не встретим клетку без ничьей (или первую клетку чанка), и
 * разворачиваем цепочку вперёд. */
static int rt_count_min(const McBiomeTree *T, int ni, const i64 *tg, i64 d) {
    const McRNode *n = &T->node[ni];
    if (n->biome >= 0 && n->count == 0) return mc_rnode_distance(n, tg) == d;
    int c = 0;
    for (int k = 0; k < n->count && c < 2; k++) {
        int ci = T->child_idx[n->first + k];
        if (mc_rnode_distance(&T->node[ci], tg) <= d) c += rt_count_min(T, ci, tg, d);
    }
    return c;
}
typedef struct { i64 tg[MC_RT_DIM]; i64 d; int l0; } TieCell;
int world_biome_cell(const McWorld *w, int qx, int qy, int qz) {
    int bs = w->preset->biome_source;
    int qmin = w->min_y >> 2, qh = w->height >> 2;
    if (qy < qmin) qy = qmin;                       /* ChunkAccess.getNoiseBiome: y зажимается в пределы чанка */
    if (qy > qmin + qh - 1) qy = qmin + qh - 1;
    if (bs != BS_MULTI_OVERWORLD && bs != BS_MULTI_NETHER) return world_biome_noise(w, qx, qy, qz);
    const McBiomeTree *T = bs == BS_MULTI_NETHER ? w->g->nether_tree : w->g->ow_tree;
    int bx = qx & ~3, bz = qz & ~3, ly = qy - qmin;
    int idx = (ly >> 2) * 64 + (qx & 3) * 16 + (ly & 3) * 4 + (qz & 3);   /* порядок LevelChunkSection.fillBiomesFromNoise */
    TieCell local[32], *st = local; int cap = 32, n = 0, base = -1;
    for (;;) {
        int s = idx >> 6, r = idx & 63;
        int cx = bx + (r >> 4), cy = qmin + s * 4 + ((r >> 2) & 3), cz = bz + (r & 3);
        float v[6]; climate_point(w, cx * 4, cy * 4, cz * 4, v);
        TieCell c;
        for (int k = 0; k < 6; k++) c.tg[k] = (i64)(v[k] * 10000.0f);
        c.tg[6] = 0;
        c.l0 = mc_rt_search_node(T, T->root, c.tg, -1);
        c.d = mc_rnode_distance(&T->node[c.l0], c.tg);
        if (idx == 0 || rt_count_min(T, T->root, c.tg, c.d) < 2) { base = c.l0; break; }
        if (n == cap) { TieCell *nb = xmalloc(sizeof(TieCell) * (size_t)cap * 2); memcpy(nb, st, sizeof(TieCell) * (size_t)n); if (st != local) free(st); st = nb; cap *= 2; }
        st[n++] = c; idx--;
    }
    int leaf = base;
    for (int i = n - 1; i >= 0; i--) leaf = mc_rnode_distance(&T->node[leaf], st[i].tg) == st[i].d ? leaf : st[i].l0;
    if (st != local) free(st);
    return T->node[leaf].biome;
}

/* Биомы чанка, как ChunkGenerator.doCreateBiomes: 26.3+ мультишумовой — пакетом через sampleVolume (кэширующий контекст),
 * объём 4×(H/4)×4 кварт с шагом 4 блока; иначе — по точкам. out: [qy][qz][qx]. */
void world_chunk_biomes(const McWorld *w, SCtx *x, int cx, int cz, uint8_t *out) {
    int qh = w->height / 4, qy0 = w->min_y >> 2;
    int qx0 = cx * 4, qz0 = cz * 4;
    if (w->g->newf && (w->preset->biome_source == BS_MULTI_OVERWORLD || w->preset->biome_source == BS_MULTI_NETHER) && x) {
        static const int F[6] = { RF_TEMPERATURE, RF_VEGETATION, RF_CONTINENTS, RF_EROSION, RF_DEPTH, RF_RIDGES };
        Vol v = { 4, qh, 4, qx0 * 4, qy0 * 4, qz0 * 4, 4, 4, 4 };
        int n = vol_size(&v);
        float *b[6];
        for (int k = 0; k < 6; k++) { b[k] = xmalloc(sizeof(float) * (size_t)n); s_volume(x, w->s_rf[F[k]], b[k], &v); }
        int last = -1;   /* порядок запросов — как LevelChunkSection.fillBiomesFromNoise: секции снизу вверх, x, y, z */
        for (int sy = 0; sy < qh; sy += 4) for (int qx = 0; qx < 4; qx++) for (int qy = sy; qy < sy + 4 && qy < qh; qy++) for (int qz = 0; qz < 4; qz++) {
            int i = vol_idx(&v, qx, qy, qz);
            float c[6] = { b[0][i], b[1][i], b[2][i], b[3][i], b[4][i], b[5][i] };
            out[(qy * 4 + qz) * 4 + qx] = (uint8_t)find_biome_last(w, c, &last);
        }
        for (int k = 0; k < 6; k++) free(b[k]);
        return;
    }
    if (w->preset->biome_source == BS_MULTI_OVERWORLD || w->preset->biome_source == BS_MULTI_NETHER) {
        int last = -1;
        for (int sy = 0; sy < qh; sy += 4) for (int qx = 0; qx < 4; qx++) for (int qy = sy; qy < sy + 4 && qy < qh; qy++) for (int qz = 0; qz < 4; qz++) {
            float c[6]; climate_point(w, (qx0 + qx) * 4, (qy0 + qy) * 4, (qz0 + qz) * 4, c);
            out[(qy * 4 + qz) * 4 + qx] = (uint8_t)find_biome_last(w, c, &last);
        }
        return;
    }
    for (int qy = 0; qy < qh; qy++) for (int qz = 0; qz < 4; qz++) for (int qx = 0; qx < 4; qx++)
        out[(qy * 4 + qz) * 4 + qx] = (uint8_t)world_biome_noise(w, qx0 + qx, qy0 + qy, qz0 + qz);
}

/* fitness (Climate.ParameterPoint.fitness): сумма квадратов расстояний до интервалов; 26.4 — исключающая верхняя граница */
static i64 pdist(i64 lo, i64 hi, i64 t, int excl) { i64 above = t - hi + (excl ? 1 : 0), below = lo - t; return above > 0 ? above : (below > 0 ? below : 0); }
int mcgen_x_biome_tie(const McWorld *w, int qx, int qy, int qz, int a, int b) {
    if (w->preset->biome_source != BS_MULTI_OVERWORLD && w->preset->biome_source != BS_MULTI_NETHER) return 0;
    float v[6]; climate_point(w, qx * 4, qy * 4, qz * 4, v);
    i64 t[7] = { (i64)(v[0] * 10000.0f), (i64)(v[1] * 10000.0f), (i64)(v[2] * 10000.0f), (i64)(v[3] * 10000.0f), (i64)(v[4] * 10000.0f), (i64)(v[5] * 10000.0f), 0 };
    const ClimPoint *P = w->preset->biome_source == BS_MULTI_NETHER ? w->g->nether_points : w->g->ow_points;
    int n = w->preset->biome_source == BS_MULTI_NETHER ? w->g->n_nether : w->g->n_ow, excl = w->g->version >= V26_4;
    i64 best_a = INT64_MAX, best_b = INT64_MAX, best = INT64_MAX;
    for (int i = 0; i < n; i++) {
        i64 d = 0; for (int k = 0; k < 7; k++) { i64 x = pdist(P[i].lo[k], P[i].hi[k], t[k], excl); d += x * x; }
        if (d < best) best = d;
        if (P[i].biome == a && d < best_a) best_a = d;
        if (P[i].biome == b && d < best_b) best_b = d;
    }
    return best_a == best && best_b == best;
}

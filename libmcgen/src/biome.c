/* biome.c — источники биомов: мультишумовой (Overworld/Nether: климат из роутера → R-дерево), End (TheEndBiomeSource),
 * фиксированный. Климат считается density-функциями роутера мира (с учётом тонких настроек), как Climate.Sampler. */
#include "mcgen_internal.h"
#include "df_old.h"
#include "mc_biomes.h"
#include "mcgen_test.h"
#include <stdlib.h>

static _Thread_local SCtx *tls_uncached;   /* SamplerContext.EMPTY_UNCACHED: кэши выключены, буферы — свои у потока */
static SCtx *uncached_ctx(void) { if (!tls_uncached) tls_uncached = sctx_new(NULL, 0); return tls_uncached; }

static int find_biome(const McWorld *w, const float v[6]) {
    McTarget t;
    t.t = (i64)(v[0] * 10000.0f); t.h = (i64)(v[1] * 10000.0f); t.c = (i64)(v[2] * 10000.0f);
    t.e = (i64)(v[3] * 10000.0f); t.d = (i64)(v[4] * 10000.0f); t.w = (i64)(v[5] * 10000.0f);
    const McBiomeTree *T = w->preset->biome_source == BS_MULTI_NETHER ? w->g->nether_tree : w->g->ow_tree;
    return mc_biome_find(T, &t);
}
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
        for (int qy = 0; qy < qh; qy++) for (int qz = 0; qz < 4; qz++) for (int qx = 0; qx < 4; qx++) {
            int i = vol_idx(&v, qx, qy, qz);
            float c[6] = { b[0][i], b[1][i], b[2][i], b[3][i], b[4][i], b[5][i] };
            out[(qy * 4 + qz) * 4 + qx] = (uint8_t)find_biome(w, c);
        }
        for (int k = 0; k < 6; k++) free(b[k]);
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

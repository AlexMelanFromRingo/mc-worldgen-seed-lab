/* biome_params.c — таблицы климата мультишумовых источников биомов (в игре — код, не данные):
 *   Overworld: OverworldBiomeBuilder (порядок точек важен: на нём строится R-дерево и разрешаются ничьи);
 *   Nether:    MultiNoiseBiomeSourceParameterList.Preset.NETHER (5 точек).
 * Различия версий: 26.2 +sulfur_caves; 26.3 dappled_forest (MIDDLE_BIOMES_VARIANT[1][0]);
 * 26.4-snapshot-2: у dripstone_caves и sulfur_caves влажность [-1, 0.7] + исключающая верхняя граница расстояния.
 * Проверка: tests/biome_params_check.py сравнивает с дампом игры (data/params/<V>/overworld.tsv).
 */
#include "mcgen_internal.h"
#include "mc_biomes.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { i64 lo, hi; } Par;
static i64 qz(float v) { return (i64)(v * 10000.0f); }                 /* Climate.quantizeCoord */
static Par span(float a, float b) { Par p = { qz(a), qz(b) }; return p; }
static Par spanp(Par a, Par b) { Par p = { a.lo, b.hi }; return p; }
static Par point(float v) { return span(v, v); }

typedef struct {
    McGen *g; int v;
    ClimPoint *pts; int n, cap;
    int fail; char miss[64];
    Par FULL, T[5], H[5], E[7], FROZEN, UNFROZEN;
    Par mushroom, deep_ocean, ocean, coast, inland, near_inland, mid_inland, far_inland;
} B;

static int bid(B *b, const char *name) {
    if (!name) return -1;
    int id = gen_biome_id(b->g, name);
    if (id < 0 && !b->fail) { b->fail = 1; snprintf(b->miss, sizeof b->miss, "%s", name); }
    return id;
}
static void emit(B *b, Par t, Par h, Par c, Par e, Par d, Par w, float off, const char *biome) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 8192; b->pts = xrealloc(b->pts, sizeof(ClimPoint) * (size_t)b->cap); }
    ClimPoint *p = &b->pts[b->n++];
    Par a[6] = { t, h, c, e, d, w };
    for (int i = 0; i < 6; i++) { p->lo[i] = a[i].lo; p->hi[i] = a[i].hi; }
    p->lo[6] = p->hi[6] = qz(off);
    p->biome = bid(b, biome);
}
/* addSurfaceBiome: глубина 0 и 1; addUndergroundBiome: [0.2, 0.9]; addBottomBiome: 1.1 */
static void surface(B *b, Par t, Par h, Par c, Par e, Par w, const char *bi) { emit(b, t, h, c, e, point(0.0f), w, 0.0f, bi); emit(b, t, h, c, e, point(1.0f), w, 0.0f, bi); }

/* ---- таблицы (порядок индексов — temperature × humidity) ---- */
static const char *OCEANS[2][5] = {
    {"deep_frozen_ocean", "deep_cold_ocean", "deep_ocean", "deep_lukewarm_ocean", "warm_ocean"},
    {"frozen_ocean", "cold_ocean", "ocean", "lukewarm_ocean", "warm_ocean"}};
static const char *MIDDLE[5][5] = {
    {"snowy_plains", "snowy_plains", "snowy_plains", "snowy_taiga", "taiga"},
    {"plains", "plains", "forest", "taiga", "old_growth_spruce_taiga"},
    {"flower_forest", "plains", "forest", "birch_forest", "dark_forest"},
    {"savanna", "savanna", "forest", "jungle", "jungle"},
    {"desert", "desert", "desert", "desert", "desert"}};
static const char *MIDDLE_VAR[5][5] = {
    {"ice_spikes", NULL, "snowy_taiga", NULL, NULL},
    {NULL /* 26.3+: dappled_forest */, NULL, NULL, NULL, "old_growth_pine_taiga"},
    {"sunflower_plains", NULL, NULL, "old_growth_birch_forest", NULL},
    {NULL, NULL, "plains", "sparse_jungle", "bamboo_jungle"},
    {NULL, NULL, NULL, NULL, NULL}};
static const char *PLATEAU[5][5] = {
    {"snowy_plains", "snowy_plains", "snowy_plains", "snowy_taiga", "snowy_taiga"},
    {"meadow", "meadow", "forest", "taiga", "old_growth_spruce_taiga"},
    {"meadow", "meadow", "meadow", "meadow", "pale_garden"},
    {"savanna_plateau", "savanna_plateau", "forest", "forest", "jungle"},
    {"badlands", "badlands", "badlands", "wooded_badlands", "wooded_badlands"}};
static const char *PLATEAU_VAR[5][5] = {
    {"ice_spikes", NULL, NULL, NULL, NULL},
    {"cherry_grove", NULL, "meadow", "meadow", "old_growth_pine_taiga"},
    {"cherry_grove", "cherry_grove", "forest", "birch_forest", NULL},
    {NULL, NULL, NULL, NULL, NULL},
    {"eroded_badlands", "eroded_badlands", NULL, NULL, NULL}};
static const char *SHATTERED[5][5] = {
    {"windswept_gravelly_hills", "windswept_gravelly_hills", "windswept_hills", "windswept_forest", "windswept_forest"},
    {"windswept_gravelly_hills", "windswept_gravelly_hills", "windswept_hills", "windswept_forest", "windswept_forest"},
    {"windswept_hills", "windswept_hills", "windswept_hills", "windswept_forest", "windswept_forest"},
    {NULL, NULL, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL}};

/* ---- выбор биома (pick*) ---- */
static int neg(Par w) { return w.hi < 0; }   /* weirdness.max() < 0 */
static const char *middle(B *b, int t, int h, Par w) {
    if (neg(w)) return MIDDLE[t][h];
    const char *v = MIDDLE_VAR[t][h];
    if (t == 1 && h == 0 && b->v >= V26_3) v = "dappled_forest";
    return v ? v : MIDDLE[t][h];
}
static const char *badlands(int h, Par w) { return h < 2 ? (neg(w) ? "badlands" : "eroded_badlands") : (h < 3 ? "badlands" : "wooded_badlands"); }
static const char *middle_or_badlands(B *b, int t, int h, Par w) { return t == 4 ? badlands(h, w) : middle(b, t, h, w); }
static const char *plateau(int t, int h, Par w) { if (!neg(w) && PLATEAU_VAR[t][h]) return PLATEAU_VAR[t][h]; return PLATEAU[t][h]; }
static const char *slope(int t, int h, Par w) { return t >= 3 ? plateau(t, h, w) : (h <= 1 ? "snowy_slopes" : "grove"); }
static const char *middle_or_badlands_or_slope(B *b, int t, int h, Par w) { return t == 0 ? slope(t, h, w) : middle_or_badlands(b, t, h, w); }
static const char *windswept_savanna(int t, int h, Par w, const char *under) { return (t > 1 && h < 4 && !neg(w)) ? "windswept_savanna" : under; }
static const char *beach(int t) { return t == 0 ? "snowy_beach" : (t == 4 ? "desert" : "beach"); }
static const char *shattered_coast(B *b, int t, int h, Par w) { return windswept_savanna(t, h, w, !neg(w) ? middle(b, t, h, w) : beach(t)); }
static const char *peak(int t, int h, Par w) { return t <= 2 ? (neg(w) ? "jagged_peaks" : "frozen_peaks") : (t == 3 ? "stony_peaks" : badlands(h, w)); }
static const char *shattered(B *b, int t, int h, Par w) { return SHATTERED[t][h] ? SHATTERED[t][h] : middle(b, t, h, w); }

static void off_coast(B *b) {
    surface(b, b->FULL, b->FULL, b->mushroom, b->FULL, b->FULL, "mushroom_fields");
    for (int t = 0; t < 5; t++) {
        surface(b, b->T[t], b->FULL, b->deep_ocean, b->FULL, b->FULL, OCEANS[0][t]);
        surface(b, b->T[t], b->FULL, b->ocean, b->FULL, b->FULL, OCEANS[1][t]);
    }
}
static void peaks(B *b, Par w) {
    for (int t = 0; t < 5; t++) for (int h = 0; h < 5; h++) {
        Par T = b->T[t], H = b->H[h];
        const char *mid = middle(b, t, h, w), *mob = middle_or_badlands(b, t, h, w), *mobs = middle_or_badlands_or_slope(b, t, h, w);
        const char *pl = plateau(t, h, w), *sh = shattered(b, t, h, w), *shs = windswept_savanna(t, h, w, sh), *pk = peak(t, h, w);
        surface(b, T, H, spanp(b->coast, b->far_inland), b->E[0], w, pk);
        surface(b, T, H, spanp(b->coast, b->near_inland), b->E[1], w, mobs);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[1], w, pk);
        surface(b, T, H, spanp(b->coast, b->near_inland), spanp(b->E[2], b->E[3]), w, mid);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[2], w, pl);
        surface(b, T, H, b->mid_inland, b->E[3], w, mob);
        surface(b, T, H, b->far_inland, b->E[3], w, pl);
        surface(b, T, H, spanp(b->coast, b->far_inland), b->E[4], w, mid);
        surface(b, T, H, spanp(b->coast, b->near_inland), b->E[5], w, shs);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[5], w, sh);
        surface(b, T, H, spanp(b->coast, b->far_inland), b->E[6], w, mid);
    }
}
static void high_slice(B *b, Par w) {
    for (int t = 0; t < 5; t++) for (int h = 0; h < 5; h++) {
        Par T = b->T[t], H = b->H[h];
        const char *mid = middle(b, t, h, w), *mob = middle_or_badlands(b, t, h, w), *mobs = middle_or_badlands_or_slope(b, t, h, w);
        const char *pl = plateau(t, h, w), *sh = shattered(b, t, h, w), *mws = windswept_savanna(t, h, w, mid);
        const char *sl = slope(t, h, w), *pk = peak(t, h, w);
        surface(b, T, H, b->coast, spanp(b->E[0], b->E[1]), w, mid);
        surface(b, T, H, b->near_inland, b->E[0], w, sl);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[0], w, pk);
        surface(b, T, H, b->near_inland, b->E[1], w, mobs);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[1], w, sl);
        surface(b, T, H, spanp(b->coast, b->near_inland), spanp(b->E[2], b->E[3]), w, mid);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[2], w, pl);
        surface(b, T, H, b->mid_inland, b->E[3], w, mob);
        surface(b, T, H, b->far_inland, b->E[3], w, pl);
        surface(b, T, H, spanp(b->coast, b->far_inland), b->E[4], w, mid);
        surface(b, T, H, spanp(b->coast, b->near_inland), b->E[5], w, mws);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[5], w, sh);
        surface(b, T, H, spanp(b->coast, b->far_inland), b->E[6], w, mid);
    }
}
static void swamps(B *b, Par w, Par cont) {
    surface(b, spanp(b->T[1], b->T[2]), b->FULL, cont, b->E[6], w, "swamp");
    surface(b, spanp(b->T[3], b->T[4]), b->FULL, cont, b->E[6], w, "mangrove_swamp");
}
static void mid_slice(B *b, Par w) {
    surface(b, b->FULL, b->FULL, b->coast, spanp(b->E[0], b->E[2]), w, "stony_shore");
    swamps(b, w, spanp(b->near_inland, b->far_inland));
    for (int t = 0; t < 5; t++) for (int h = 0; h < 5; h++) {
        Par T = b->T[t], H = b->H[h];
        const char *mid = middle(b, t, h, w), *mob = middle_or_badlands(b, t, h, w), *mobs = middle_or_badlands_or_slope(b, t, h, w);
        const char *sh = shattered(b, t, h, w), *pl = plateau(t, h, w), *be = beach(t), *mws = windswept_savanna(t, h, w, mid);
        const char *shc = shattered_coast(b, t, h, w), *sl = slope(t, h, w);
        surface(b, T, H, spanp(b->near_inland, b->far_inland), b->E[0], w, sl);
        surface(b, T, H, spanp(b->near_inland, b->mid_inland), b->E[1], w, mobs);
        surface(b, T, H, b->far_inland, b->E[1], w, t == 0 ? sl : pl);
        surface(b, T, H, b->near_inland, b->E[2], w, mid);
        surface(b, T, H, b->mid_inland, b->E[2], w, mob);
        surface(b, T, H, b->far_inland, b->E[2], w, pl);
        surface(b, T, H, spanp(b->coast, b->near_inland), b->E[3], w, mid);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[3], w, mob);
        if (neg(w)) {
            surface(b, T, H, b->coast, b->E[4], w, be);
            surface(b, T, H, spanp(b->near_inland, b->far_inland), b->E[4], w, mid);
        } else surface(b, T, H, spanp(b->coast, b->far_inland), b->E[4], w, mid);
        surface(b, T, H, b->coast, b->E[5], w, shc);
        surface(b, T, H, b->near_inland, b->E[5], w, mws);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[5], w, sh);
        surface(b, T, H, b->coast, b->E[6], w, neg(w) ? be : mid);
        if (t == 0) surface(b, T, H, spanp(b->near_inland, b->far_inland), b->E[6], w, mid);
    }
}
static void low_slice(B *b, Par w) {
    surface(b, b->FULL, b->FULL, b->coast, spanp(b->E[0], b->E[2]), w, "stony_shore");
    swamps(b, w, spanp(b->near_inland, b->far_inland));
    for (int t = 0; t < 5; t++) for (int h = 0; h < 5; h++) {
        Par T = b->T[t], H = b->H[h];
        const char *mid = middle(b, t, h, w), *mob = middle_or_badlands(b, t, h, w), *mobs = middle_or_badlands_or_slope(b, t, h, w);
        const char *be = beach(t), *mws = windswept_savanna(t, h, w, mid), *shc = shattered_coast(b, t, h, w);
        surface(b, T, H, b->near_inland, spanp(b->E[0], b->E[1]), w, mob);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), spanp(b->E[0], b->E[1]), w, mobs);
        surface(b, T, H, b->near_inland, spanp(b->E[2], b->E[3]), w, mid);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), spanp(b->E[2], b->E[3]), w, mob);
        surface(b, T, H, b->coast, spanp(b->E[3], b->E[4]), w, be);
        surface(b, T, H, spanp(b->near_inland, b->far_inland), b->E[4], w, mid);
        surface(b, T, H, b->coast, b->E[5], w, shc);
        surface(b, T, H, b->near_inland, b->E[5], w, mws);
        surface(b, T, H, spanp(b->mid_inland, b->far_inland), b->E[5], w, mid);
        surface(b, T, H, b->coast, b->E[6], w, be);
        if (t == 0) surface(b, T, H, spanp(b->near_inland, b->far_inland), b->E[6], w, mid);
    }
}
static void valleys(B *b, Par w) {
    Par e01 = spanp(b->E[0], b->E[1]);
    surface(b, b->FROZEN, b->FULL, b->coast, e01, w, neg(w) ? "stony_shore" : "frozen_river");
    surface(b, b->UNFROZEN, b->FULL, b->coast, e01, w, neg(w) ? "stony_shore" : "river");
    surface(b, b->FROZEN, b->FULL, b->near_inland, e01, w, "frozen_river");
    surface(b, b->UNFROZEN, b->FULL, b->near_inland, e01, w, "river");
    surface(b, b->FROZEN, b->FULL, spanp(b->coast, b->far_inland), spanp(b->E[2], b->E[5]), w, "frozen_river");
    surface(b, b->UNFROZEN, b->FULL, spanp(b->coast, b->far_inland), spanp(b->E[2], b->E[5]), w, "river");
    surface(b, b->FROZEN, b->FULL, b->coast, b->E[6], w, "frozen_river");
    surface(b, b->UNFROZEN, b->FULL, b->coast, b->E[6], w, "river");
    swamps(b, w, spanp(b->inland, b->far_inland));
    surface(b, b->FROZEN, b->FULL, spanp(b->inland, b->far_inland), b->E[6], w, "frozen_river");
    for (int t = 0; t < 5; t++) for (int h = 0; h < 5; h++)
        surface(b, b->T[t], b->H[h], spanp(b->mid_inland, b->far_inland), e01, w, middle_or_badlands(b, t, h, w));
}
static void inland(B *b) {
    mid_slice(b, span(-1.0f, -0.93333334f));
    high_slice(b, span(-0.93333334f, -0.7666667f));
    peaks(b, span(-0.7666667f, -0.56666666f));
    high_slice(b, span(-0.56666666f, -0.4f));
    mid_slice(b, span(-0.4f, -0.26666668f));
    low_slice(b, span(-0.26666668f, -0.05f));
    valleys(b, span(-0.05f, 0.05f));
    low_slice(b, span(0.05f, 0.26666668f));
    mid_slice(b, span(0.26666668f, 0.4f));
    high_slice(b, span(0.4f, 0.56666666f));
    peaks(b, span(0.56666666f, 0.7666667f));
    high_slice(b, span(0.7666667f, 0.93333334f));
    mid_slice(b, span(0.93333334f, 1.0f));
}
static void underground(B *b) {
    Par und = span(0.2f, 0.9f);
    Par hum_low = b->v >= V26_4 ? span(-1.0f, 0.7f) : b->FULL;
    emit(b, b->FULL, hum_low, span(0.8f, 1.0f), b->FULL, und, b->FULL, 0.0f, "dripstone_caves");
    emit(b, b->FULL, span(0.7f, 1.0f), b->FULL, b->FULL, und, b->FULL, 0.0f, "lush_caves");
    if (b->v >= V26_2)
        emit(b, b->FULL, hum_low, spanp(b->coast, b->inland), spanp(b->E[5], b->E[6]), und, span(-1.1f, -0.85f), 0.0f, "sulfur_caves");
    emit(b, b->FULL, b->FULL, b->FULL, spanp(b->E[0], b->E[1]), point(1.1f), b->FULL, 0.0f, "deep_dark");
}

static void init_ranges(B *b) {
    b->FULL = span(-1.0f, 1.0f);
    b->T[0] = span(-1.0f, -0.45f); b->T[1] = span(-0.45f, -0.15f); b->T[2] = span(-0.15f, 0.2f); b->T[3] = span(0.2f, 0.55f); b->T[4] = span(0.55f, 1.0f);
    b->H[0] = span(-1.0f, -0.35f); b->H[1] = span(-0.35f, -0.1f); b->H[2] = span(-0.1f, 0.1f); b->H[3] = span(0.1f, 0.3f); b->H[4] = span(0.3f, 1.0f);
    b->E[0] = span(-1.0f, -0.78f); b->E[1] = span(-0.78f, -0.375f); b->E[2] = span(-0.375f, -0.2225f); b->E[3] = span(-0.2225f, 0.05f);
    b->E[4] = span(0.05f, 0.45f); b->E[5] = span(0.45f, 0.55f); b->E[6] = span(0.55f, 1.0f);
    b->FROZEN = b->T[0]; b->UNFROZEN = spanp(b->T[1], b->T[4]);
    b->mushroom = span(-1.2f, -1.05f); b->deep_ocean = span(-1.05f, -0.455f); b->ocean = span(-0.455f, -0.19f);
    b->coast = span(-0.19f, -0.11f); b->inland = span(-0.11f, 0.55f); b->near_inland = span(-0.11f, 0.03f);
    b->mid_inland = span(0.03f, 0.3f); b->far_inland = span(0.3f, 1.0f);
}

static void build_tree(McGen *g, const ClimPoint *pts, int n, void **tree, int **leaf_biome) {
    McParamBox *boxes = xcalloc((size_t)n, sizeof(McParamBox));
    int *ids = xcalloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) { for (int d = 0; d < 7; d++) { boxes[i].lo[d] = pts[i].lo[d]; boxes[i].hi[d] = pts[i].hi[d]; } ids[i] = pts[i].biome; }
    /* Climate.RTree: 6 потомков на узел (26.1/26.2), 19 (26.3+) */
    McBiomeTree *T = mc_rt_create(boxes, ids, n, g->version >= V26_3 ? 19 : 6);
    /* 26.4-snapshot-2: Parameter.distance с исключающей верхней границей ≡ hi−1 во всех узлах */
    if (g->version >= V26_4) for (int i = 0; i < T->n_nodes; i++) for (int d = 0; d < MC_RT_DIM; d++) T->node[i].box.hi[d] -= 1;
    *tree = T; *leaf_biome = NULL;
    free(boxes); free(ids);
}

int biome_params_build(McGen *g, char *err, size_t errlen) {
    B b; memset(&b, 0, sizeof b); b.g = g; b.v = g->version;
    init_ranges(&b);
    off_coast(&b); inland(&b); underground(&b);
    if (b.fail) { free(b.pts); set_err(err, errlen, "нет биома %s в датапаке", b.miss); return MCGEN_E_DATA; }
    g->ow_points = b.pts; g->n_ow = b.n;
    /* Nether */
    static const struct { float t, h, off; const char *bi; } N[5] = {
        {0.0f, 0.0f, 0.0f, "nether_wastes"}, {0.0f, -0.5f, 0.0f, "soul_sand_valley"}, {0.4f, 0.0f, 0.0f, "crimson_forest"},
        {0.0f, 0.5f, 0.375f, "warped_forest"}, {-0.5f, 0.0f, 0.175f, "basalt_deltas"}};
    B nb; memset(&nb, 0, sizeof nb); nb.g = g; nb.v = g->version;
    for (int i = 0; i < 5; i++) emit(&nb, point(N[i].t), point(N[i].h), point(0), point(0), point(0), point(0), N[i].off, N[i].bi);
    if (nb.fail) { free(nb.pts); set_err(err, errlen, "нет биома %s", nb.miss); return MCGEN_E_DATA; }
    memcpy(g->nether_points, nb.pts, sizeof(ClimPoint) * 5); g->n_nether = 5; free(nb.pts);
    build_tree(g, g->ow_points, g->n_ow, &g->ow_tree, &g->ow_leaf_biome);
    build_tree(g, g->nether_points, g->n_nether, &g->nether_tree, &g->nether_leaf_biome);
    return 0;
}
void biome_params_free(McGen *g) {
    free(g->ow_points); g->ow_points = NULL;
    free(g->ow_tree); free(g->nether_tree); g->ow_tree = g->nether_tree = NULL;
}

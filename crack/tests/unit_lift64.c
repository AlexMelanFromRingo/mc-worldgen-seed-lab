/* Юнит-тесты crack-lift64: SHA-256 (obfuscateSeed) на известных векторах oracle, random_world_seeds (NextLongReverser-аналог),
 * тай-брейки R-дерева (accept любой из равных биомов), согласованность дерева и перебора по листьям.
 * Сборка: gcc -O2 -std=gnu11 -ffp-contract=off -fopenmp -I../engine -o /tmp/unit_lift64 unit_lift64.c -lm  (make unit) */
#define main lift64_main
#include "../src/crack_lift64.c"
#undef main

static u64 rs = 0x123456789ABCDEFULL;
static u64 rnd64(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static long fails = 0;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } } while (0)

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "..";
    setenv("MCGEN_ROOT", root, 1);
    /* 1. obfuscateSeed: значения из oracle.hashed (реальный BiomeManager.obfuscateSeed) */
    CHECK(obfuscate_seed(12345ULL) == 293737985876514017ULL, "sha(12345)");
    CHECK(obfuscate_seed(0ULL) == 8794265229978523055ULL, "sha(0)");
    /* 1b. SHA-NI против скалярной реализации (если CPU поддерживает) */
    if (have_shani()) { long bad = 0; u64 x = 0x9E3779B97F4A7C15ULL; for (long i = 0; i < 500000; i++) { x = x * 6364136223846793005ULL + 1442695040888963407ULL; if (obfuscate_seed_shani(x) != obfuscate_seed_scalar(x)) bad++; }
                        CHECK(bad == 0, "SHA-NI расходится со скалярным на %ld значениях", bad); printf("unit_lift64: SHA-NI == скалярный SHA-256 на 500000 значениях\n"); }
    else printf("unit_lift64: SHA-NI недоступен, проверен только скалярный путь\n");
    /* 2. random_world_seeds: W = nextLong() при Random(unique) — выводится из нижних 48 бит */
    for (int t = 0; t < 2000; t++) {
        u64 st = rnd64() & MASK48;                                      /* состояние после scramble */
        u64 s1 = (st * 0x5DEECE66DULL + 0xBULL) & MASK48, s2 = (s1 * 0x5DEECE66DULL + 0xBULL) & MASK48;
        i32 a = (i32)(s1 >> 16), b = (i32)(s2 >> 16);
        u64 W = (u64)(((i64)a << 32) + (i64)b);
        u64 out[64]; int n = random_world_seeds(W & MASK48, out, 64); int hit = 0;
        for (int i = 0; i < n; i++) hit |= (out[i] == W);
        CHECK(hit && n >= 1 && n <= 8, "random_world_seeds t=%d n=%d hit=%d", t, n, hit);
    }
    /* 3. тай-брейки: дерево против полного перебора листьев; допускаются равные по fitness */
    McVersionData V; int ver = MC_26_3;
    if (mc_load_version(&V, ver)) { fprintf(stderr, "не загрузились данные\n"); return 2; }
    McBiomeTree *T = mc_load_biome_tree(ver, "overworld");
    int *leaf_of[MC_BIOME_COUNT]; int n_leaf[MC_BIOME_COUNT]; index_leaves(T, leaf_of, n_leaf);
    int leaves[9000], nl = 0; for (int i = 0; i < T->n_nodes; i++) if (T->node[i].biome >= 0 && T->node[i].count == 0) leaves[nl++] = i;
    long ties = 0, tie_multi_biome = 0, total = 40000;
    for (long t = 0; t < total; t++) {
        McTarget tg; i64 v[6];
        for (int d = 0; d < 6; d++) v[d] = (i64)(rnd64() % 24000) - 12000 + (d == 4 ? 4000 : 0);
        tg.t = v[0]; tg.h = v[1]; tg.c = v[2]; tg.e = v[3]; tg.d = v[4]; tg.w = v[5];
        i64 target[7] = {tg.t, tg.h, tg.c, tg.e, tg.d, tg.w, 0};
        i64 best = 0x7fffffffffffffffLL; for (int k = 0; k < nl; k++) { i64 d = mc_rnode_distance(&T->node[leaves[k]], target); if (d < best) best = d; }
        int seen[MC_BIOME_COUNT] = {0}, nb = 0;
        for (int k = 0; k < nl; k++) if (mc_rnode_distance(&T->node[leaves[k]], target) == best) { int b = T->node[leaves[k]].biome; if (!seen[b]) { seen[b] = 1; nb++; } }
        if (nb > 1) { tie_multi_biome++; }
        ties += (nb > 1);
        /* каждый из равных биомов должен приниматься; биом с большим расстоянием — нет */
        for (int b = 0; b < MC_BIOME_COUNT; b++) {
            int want_ok = seen[b];
            if (nb > 1 || t % 50 == 0) CHECK(tree_biome_matches(T, leaf_of, n_leaf, &tg, b) == want_ok, "tie-handling biome=%d want=%d", b, want_ok);
        }
    }
    printf("unit_lift64: тай-брейки: из %ld случайных точек климата у %ld есть равные по fitness листья РАЗНЫХ биомов (все принимаются)\n", total, tie_multi_biome);
    printf(fails ? "unit_lift64: ПРОВАЛ (%ld)\n" : "unit_lift64: OK (%ld ошибок)\n", fails);
    return fails ? 1 : 0;
}

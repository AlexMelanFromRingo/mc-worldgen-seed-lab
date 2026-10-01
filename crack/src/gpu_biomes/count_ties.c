/* count_ties — как часто в точке минимум fitness R-дерева достигается листьями РАЗНЫХ биомов (тай-брейк в игре недетерминирован:
 * Climate.RTree.search стартует с lastResult потока). Такие наблюдения могут не пройти — тогда перечислите оба биома через '|'.
 *   count_ties <26.1|26.2|26.3> <preset> <N точек> [rng]      (MCGEN_ROOT=корень проекта)
 */
#include "../../../engine/mc_data.h"
static u64 rs = 0x243F6A8885A308D3ULL;
static u64 rnd64(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs * 0x2545F4914F6CDD1DULL; }
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage\n"); return 2; }
    int ver = mc_version_from_name(argv[1]); const char *pr = argv[2]; long N = atol(argv[3]); if (argc > 4) rs ^= (u64)atol(argv[4]) * 0x9E3779B97F4A7C15ULL;
    static McVersionData V; if (mc_load_version(&V, ver)) return 1;
    McClimateSpec *S = mc_preset(&V, !strcmp(pr, "normal") ? "overworld" : pr);
    McBiomeTree *T = mc_load_biome_tree(ver, "overworld");
    int nleaf = 0; int *leaf = malloc(sizeof(int) * T->n_nodes);
    for (int i = 0; i < T->n_nodes; i++) if (T->node[i].count == 0 && T->node[i].biome >= 0) leaf[nleaf++] = i;
    static McClimate cl; long ties = 0, ties_diff = 0, pts = 0;
    long nseeds = N / 64 + 1;
    for (long si = 0; si < nseeds; si++) {
        i64 seed = (i64)rnd64(); mc_climate_init_overworld(&cl, S, seed);
        for (int p = 0; p < 64; p++) {
            int R = (p & 1) ? 100000 : 2000;
            int qx = (int)(rnd64() % (2 * R + 1)) - R, qz = (int)(rnd64() % (2 * R + 1)) - R, qy = (int)(rnd64() % 97) - 16;
            float raw[6]; mc_climate_overworld_raw(&cl, S, qx * 4, qy * 4, qz * 4, raw);
            McTarget t = mc_target_from_raw(raw);
            i64 tg[MC_RT_DIM] = {t.t, t.h, t.c, t.e, t.d, t.w, 0};
            i64 best = 0x7fffffffffffffffLL; int nb = 0; int b0 = -1, diff = 0;
            for (int k = 0; k < nleaf; k++) {
                i64 d = mc_rnode_distance(&T->node[leaf[k]], tg);
                if (d < best) { best = d; nb = 1; b0 = T->node[leaf[k]].biome; diff = 0; }
                else if (d == best) { nb++; if (T->node[leaf[k]].biome != b0) diff = 1; }
            }
            pts++; if (nb > 1) ties++; if (diff) ties_diff++;
        }
    }
    printf("%s %s: точек %ld, тай по fitness (>=2 листа с минимумом) %ld (%.4f%%), из них разных биомов %ld (%.5f%%)\n", argv[1], pr, pts, ties, 100.0 * ties / pts, ties_diff, 100.0 * ties_diff / pts);
    return 0;
}

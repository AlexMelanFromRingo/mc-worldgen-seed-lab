/* biome_cells — согласованность двух путей «биом клетки, как его хранит чанк»:
 *   world_chunk_biomes (генерация чанка, порядок запросов LevelChunkSection.fillBiomesFromNoise, lastResult R-дерева)
 *   world_biome_cell   (одна клетка: откат по цепочке ничьих; им пользуется mcgen_biome_at)
 * Должны совпадать во всех клетках. Печатает число клеток с ничьей, разрешённой историей (отличие от поиска без кандидата).
 *   biome_cells <pack> <version> <dim> <seed> <cx0> <cz0> <n>
 */
#include "mcgen_internal.h"
#include "mcgen_test.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 8) { fprintf(stderr, "biome_cells <pack> <version> <dim> <seed> <cx0> <cz0> <n>\n"); return 2; }
    char err[512]; McGen *g; McWorld *w; McRegion *r;
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    long long seed = atoll(argv[4]);
    McSeeds s = { seed, seed, seed, seed };
    if (mcgen_world_new(g, argv[3], "normal", &s, NULL, 0, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    int cx0 = atoi(argv[5]), cz0 = atoi(argv[6]), n = atoi(argv[7]);
    if (mcgen_generate_region(w, cx0, cz0, n, n, MC_STAGE_BIOMES, 0, NULL, NULL, &r, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    int qh = mcgen_world_height(w) / 4, qmin = mcgen_world_min_y(w) >> 2;
    long cells = 0, bad = 0, hist = 0;
    for (int cz = cz0; cz < cz0 + n; cz++) for (int cx = cx0; cx < cx0 + n; cx++) {
        const uint8_t *b = mcgen_region_biomes(r, cx, cz);
        for (int qy = 0; qy < qh; qy++) for (int qz = 0; qz < 4; qz++) for (int qx = 0; qx < 4; qx++) {
            int a = b[(qy * 4 + qz) * 4 + qx];
            int c = world_biome_cell(w, cx * 4 + qx, qmin + qy, cz * 4 + qz);
            int u = world_biome_noise(w, cx * 4 + qx, qmin + qy, cz * 4 + qz);
            cells++; bad += a != c; hist += a != u;
            if (a != c && bad <= 5) fprintf(stderr, "расхождение: клетка (%d,%d,%d) чанк %s, клетка %s\n", cx * 4 + qx, qmin + qy, cz * 4 + qz,
                                           mcgen_biome_name(g, a), mcgen_biome_name(g, c));
        }
    }
    printf("%s %s seed %lld: клеток %ld, расхождений чанк/клетка %ld, ничьих, разрешённых историей %ld\n", argv[2], argv[3], seed, cells, bad, hist);
    mcgen_region_free(r); mcgen_world_free(w); mcgen_close(g);
    return bad != 0;
}

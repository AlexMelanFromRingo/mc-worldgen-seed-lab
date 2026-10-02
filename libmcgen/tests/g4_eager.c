/* g4_eager.c — самопроверка стадии CARVERS: два пути применения маски должны давать одинаковый результат там, где нет травы.
 *   путь A (26.3+): маска целиком, затем applyCarvingMask (отложенное применение);
 *   путь B (26.1/26.2): блоки меняются сразу при каждом эллипсоиде (WorldCarver.carveBlock) с проверкой «уже вырезано» по маске.
 * Без поверхности (стадии BIOMES|TERRAIN|CARVERS, нет травы/мицелия), без растекания жидкостей (порядок пометок у путей разный) блоки обязаны совпасть.
 * Использование: g4_eager <pack> <version> <dim> <seed> <cx0> <cz0> <n>   (код возврата 0 — совпало)
 */
#include "mcgen_internal.h"
#include "carver.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 8) { fprintf(stderr, "g4_eager <pack> <version> <dim> <seed> <cx0> <cz0> <n>\n"); return 2; }
    McGen *g; char err[256]; McWorld *w;
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; }
    McSeeds s = mcgen_seeds_unified(strtoll(argv[4], NULL, 10));
    McTweakValue tw[1] = { { "fluid_flow", 0.0 } };
    if (mcgen_world_new(g, argv[3], "normal", &s, tw, 1, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; }
    int cx0 = atoi(argv[5]), cz0 = atoi(argv[6]), n = atoi(argv[7]);
    McRegion *ra = NULL, *rb = NULL;
    carvers_x_set_eager(0);
    if (mcgen_generate_region(w, cx0, cz0, n, n, MC_STAGE_BIOMES | MC_STAGE_TERRAIN | MC_STAGE_CARVERS, 0, NULL, NULL, &ra, err, sizeof err)) { fprintf(stderr, "A: %s\n", err); return 2; }
    carvers_x_set_eager(1);
    if (mcgen_generate_region(w, cx0, cz0, n, n, MC_STAGE_BIOMES | MC_STAGE_TERRAIN | MC_STAGE_CARVERS, 0, NULL, NULL, &rb, err, sizeof err)) { fprintf(stderr, "B: %s\n", err); return 2; }
    McRegionInfo ri; mcgen_region_info(ra, &ri);
    long diff = 0, tot = 0; size_t per = (size_t)ri.height * 256;
    for (int cz = cz0; cz < cz0 + n; cz++) for (int cx = cx0; cx < cx0 + n; cx++) {
        const uint16_t *a = mcgen_region_blocks(ra, cx, cz), *b = mcgen_region_blocks(rb, cx, cz);
        for (size_t i = 0; i < per; i++) { tot++; if (a[i] != b[i]) { if (diff < 5) printf("расхождение: чанк (%d,%d) индекс %zu: %s | %s\n", cx, cz, i, mcgen_block_state_name(g, a[i]), mcgen_block_state_name(g, b[i])); diff++; } }
    }
    printf("%s %s seed %s: блоков %ld, расхождений %ld\n", argv[2], argv[3], argv[4], tot, diff);
    mcgen_region_free(ra); mcgen_region_free(rb); mcgen_world_free(w); mcgen_close(g);
    return diff ? 1 : 0;
}

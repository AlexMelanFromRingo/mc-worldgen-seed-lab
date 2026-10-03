/* g3_bench.c — скорость стадии SURFACE (поток W2): разность времени регионов 0x3 (BIOMES|TERRAIN) и 0x7 (+SURFACE), один поток,
 * растекание жидкостей выключено, минимум из N повторов.
 *   g3_bench <pack> <версия> <измерение> [seed] [чанков по стороне] [повторов]
 * Без аргументов (make test) ничего не делает. */
#include "../include/mcgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }

int main(int argc, char **argv) {
    if (argc < 4) { printf("g3_bench <pack> <версия> <измерение> [seed] [N] [повторов] — пропуск\n"); return 0; }
    long seed = argc > 4 ? atol(argv[4]) : 12345;
    int n = argc > 5 ? atoi(argv[5]) : 16, reps = argc > 6 ? atoi(argv[6]) : 5;
    char err[512] = {0};
    McGen *g; McWorld *w;
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    McSeeds sd = mcgen_seeds_unified(seed);
    McTweakValue tw[1] = { { "fluid_flow", 0.0 } };
    if (mcgen_world_new(g, argv[3], "normal", &sd, tw, 1, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    double best[2] = { 1e9, 1e9 };
    uint32_t st[2] = { MC_STAGE_BIOMES | MC_STAGE_TERRAIN, MC_STAGE_BIOMES | MC_STAGE_TERRAIN | MC_STAGE_SURFACE };
    for (int r = 0; r < reps; r++) for (int k = 0; k < 2; k++) {
        McRegion *reg; double t0 = now();
        if (mcgen_generate_region(w, 300, 300, n, n, st[k], 1, NULL, NULL, &reg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
        double dt = now() - t0; mcgen_region_free(reg);
        if (dt < best[k]) best[k] = dt;
    }
    double chunks = (double)n * n;
    printf("%s %s: TERRAIN+BIOMES %.2f мс/чанк (%.0f ч/с), +SURFACE %.2f мс/чанк (%.0f ч/с); стадия SURFACE ≈ %.2f мс/чанк\n", argv[2], argv[3],
           1e3 * best[0] / chunks, chunks / best[0], 1e3 * best[1] / chunks, chunks / best[1], 1e3 * (best[1] - best[0]) / chunks);
    mcgen_world_free(w); mcgen_close(g);
    return 0;
}

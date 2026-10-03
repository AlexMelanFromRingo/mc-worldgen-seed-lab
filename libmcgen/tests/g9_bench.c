/* g9_bench.c — ворота G9 (часть 3): скорость CPU против GPU.
 *
 *   g9_bench --run ../run --lib build/gpu/libmcgen_cuda.so --version 26.3 [--dim minecraft:overworld] [--preset normal] [--seed 12345]
 *            [--biome N] [--step 4] [--terrain N] [--reps 3] [--threads 0]
 *
 *  --biome N    сетка биомов N×N (шаг --step блоков): mcgen_biome_grid_cpu против mcgen_gpu_biome_grid;
 *  --terrain N  стадия TERRAIN области N×N чанков: режим CPU против GPU (mcgen_generate_region; режим задаёт mcgen_gpu_set_compute).
 * Печатает минимум из --reps повторов и ускорение. Без аргументов (make test) ничего не делает. */
#include "../include/mcgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }

int main(int argc, char **argv) {
    const char *run = NULL, *lib = NULL, *ver = "26.3", *dim = "minecraft:overworld", *preset = "normal";
    long long seed = 12345; int nbiome = 0, nterr = 0, step = 4, reps = 3, threads = 0;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(k, "--run") && v) { run = v; i++; }
        else if (!strcmp(k, "--lib") && v) { lib = v; i++; }
        else if (!strcmp(k, "--version") && v) { ver = v; i++; }
        else if (!strcmp(k, "--dim") && v) { dim = v; i++; }
        else if (!strcmp(k, "--preset") && v) { preset = v; i++; }
        else if (!strcmp(k, "--seed") && v) { seed = atoll(v); i++; }
        else if (!strcmp(k, "--biome") && v) { nbiome = atoi(v); i++; }
        else if (!strcmp(k, "--terrain") && v) { nterr = atoi(v); i++; }
        else if (!strcmp(k, "--step") && v) { step = atoi(v); i++; }
        else if (!strcmp(k, "--reps") && v) { reps = atoi(v); i++; }
        else if (!strcmp(k, "--threads") && v) { threads = atoi(v); i++; }
    }
    if (!run) { printf("g9_bench --run <каталог run> --lib <libmcgen_cuda.so> … — пропуск\n"); return 0; }
    char pack[512], err[512];
    snprintf(pack, sizeof pack, "%s/pack-%s", run, ver);
    if (lib) mcgen_gpu_set_library_path(lib);
    McGen *g; McWorld *w;
    if (mcgen_open(pack, ver, &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    McSeeds sd = mcgen_seeds_unified(seed);
    McTweakValue tw[1] = { { "fluid_flow", 0.0 } };
    if (mcgen_world_new(g, dim, preset, &sd, tw, nterr ? 1 : 0, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    char st[1500]; mcgen_gpu_set_compute(MCGEN_COMPUTE_GPU, -1);
    int have = mcgen_gpu_status(st, sizeof st);
    printf("%s %s/%s seed %lld: GPU %s\n", ver, dim, preset, seed, have ? "доступно" : "НЕТ");
    if (nbiome > 0) {
        size_t n = (size_t)nbiome * nbiome;
        uint8_t *a = malloc(n), *b = malloc(n);
        double tc = 1e9, tg = 1e9;
        for (int r = 0; r < reps; r++) { double t0 = now(); mcgen_biome_grid_cpu(w, -nbiome * step / 2, -nbiome * step / 2, nbiome, nbiome, step, 64, a); double dt = now() - t0; if (dt < tc) tc = dt; }
        int rc = 0;
        for (int r = 0; r < reps + 1 && have; r++) { double t0 = now(); rc = mcgen_gpu_biome_grid(w, -nbiome * step / 2, -nbiome * step / 2, nbiome, nbiome, step, 64, b); double dt = now() - t0; if (r > 0 && dt < tg) tg = dt; if (rc) break; }
        long diff = 0; if (have && !rc) for (size_t i = 0; i < n; i++) diff += a[i] != b[i];
        printf("  карта биомов %d×%d (шаг %d): CPU %.3f с, GPU %.3f с, ускорение %.1f×, расхождений %ld%s\n", nbiome, nbiome, step, tc, tg, tc / tg, diff, rc ? " (GPU отказал)" : "");
        free(a); free(b);
    }
    if (nterr > 0) {
        double tc = 1e9, tg = 1e9;
        for (int mode = 0; mode < 2; mode++) {
            mcgen_gpu_set_compute(mode == 0 ? MCGEN_COMPUTE_CPU : MCGEN_COMPUTE_GPU, -1);
            for (int r = 0; r < reps + (mode ? 1 : 0); r++) {
                McRegion *reg; double t0 = now();
                if (mcgen_generate_region(w, 0, 0, nterr, nterr, MC_STAGE_BIOMES | MC_STAGE_TERRAIN, threads, NULL, NULL, &reg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
                double dt = now() - t0; mcgen_region_free(reg);
                if (mode == 0 && dt < tc) tc = dt;
                if (mode == 1 && r > 0 && dt < tg) tg = dt;
            }
        }
        printf("  TERRAIN %d×%d чанков: CPU %.3f с (%.0f чанков/с), GPU %.3f с (%.0f чанков/с), ускорение %.2f×\n", nterr, nterr, tc, nterr * nterr / tc, tg, nterr * nterr / tg, tc / tg);
    }
    mcgen_world_free(w); mcgen_close(g);
    return 0;
}

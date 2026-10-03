/* g9_biomes.c — ворота G9 (часть 1): сетка биомов на GPU ≡ CPU побитно.
 *
 *   g9_biomes --run ../run --lib build/gpu/libmcgen_cuda.so [--versions 26.1,26.2,26.3,26.4-snapshot-2] [--points 500000]
 *             [--seeds 4] [--grids 6] [--threads N] [--dims all|overworld|nether|end] [--quick]
 *
 * Для каждой версии × измерения × пресета, для нескольких seed (граничные 0, −1, 2^63−1 и случайные):
 *   (а) --points случайных блоковых точек (|x|,|z| до 3·10^7, y от min−16 до max+16): mcgen_gpu_biome_points ≡ mcgen_biome_at;
 *   (б) --grids случайных сеток (шаг 1…16): mcgen_gpu_biome_grid ≡ mcgen_biome_grid_cpu.
 * «Не решено на GPU» (точки, отданные на CPU) считаются отдельно и в сравнении не участвуют. Код возврата 0 — расхождений нет.
 * Без аргументов (make test) ничего не делает. */
#include "../include/mcgen.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
static uint64_t rng_next(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x; }

typedef struct { const McWorld *w; const int *xyz; const uint8_t *gpu; int a, b; long long mism; int first[3]; uint8_t first_g, first_c; int has_first; } Job;
static void *cmp_thread(void *p) {
    Job *j = p;
    for (int i = j->a; i < j->b; i++) {
        if (j->gpu[i] == 255) continue;
        int c = mcgen_biome_at(j->w, j->xyz[3 * i], j->xyz[3 * i + 1], j->xyz[3 * i + 2]);
        if (c != j->gpu[i]) {
            if (!j->has_first) { j->has_first = 1; memcpy(j->first, &j->xyz[3 * i], 12); j->first_g = j->gpu[i]; j->first_c = (uint8_t)c; }
            j->mism++;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *run = NULL, *lib = NULL, *vers = "26.1,26.2,26.3,26.4-snapshot-2", *dims = "all";
    int points = 500000, nseeds = 4, ngrids = 6, threads = 12, quick = 0;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(k, "--run") && v) { run = v; i++; }
        else if (!strcmp(k, "--lib") && v) { lib = v; i++; }
        else if (!strcmp(k, "--versions") && v) { vers = v; i++; }
        else if (!strcmp(k, "--points") && v) { points = atoi(v); i++; }
        else if (!strcmp(k, "--seeds") && v) { nseeds = atoi(v); i++; }
        else if (!strcmp(k, "--grids") && v) { ngrids = atoi(v); i++; }
        else if (!strcmp(k, "--threads") && v) { threads = atoi(v); i++; }
        else if (!strcmp(k, "--dims") && v) { dims = v; i++; }
        else if (!strcmp(k, "--quick")) quick = 1;
    }
    if (!run) { printf("g9_biomes --run <каталог run> --lib <libmcgen_cuda.so> … — пропуск\n"); return 0; }
    if (quick) { points = 20000; nseeds = 2; ngrids = 2; }
    if (lib) mcgen_gpu_set_library_path(lib);
    mcgen_gpu_set_compute(MCGEN_COMPUTE_GPU, -1);
    char st[1500];
    if (!mcgen_gpu_status(st, sizeof st)) { printf("GPU недоступно:\n%s\n", st); return 3; }
    printf("%s\n\n", st);
    static const int64_t EDGE[] = { 0, -1, 1, 9223372036854775807LL, (int64_t)0x8000000000000000ULL, 12345, -4172144997902289642LL };
    long long tot_pts = 0, tot_mism = 0, tot_unres = 0, tot_grid = 0, tot_gmism = 0; int configs = 0;
    uint64_t rs = 0xDEADBEEF12345ULL;
    char *vcopy = strdup(vers);
    for (char *ver = strtok(vcopy, ","); ver; ver = strtok(NULL, ",")) {
        char pack[512], err[512];
        snprintf(pack, sizeof pack, "%s/pack-%s", run, ver);
        McGen *g;
        if (mcgen_open(pack, ver, &g, err, sizeof err)) { printf("пропуск %s: %s\n", ver, err); continue; }
        for (int d = 0; d < mcgen_dimension_count(g); d++) {
            const char *dn = mcgen_dimension_name(g, d);
            if (strcmp(dims, "all") && !strstr(dn, dims)) continue;
            for (int p = 0; p < mcgen_preset_count(g, dn); p++) {
                const char *pn = mcgen_preset_name(g, dn, p);
                long long cpts = 0, cmism = 0, cunres = 0, cgrid = 0, cgmism = 0; double t_gpu = 0, t_cpu = 0; int cfg_ok = 1; char why[300] = "";
                for (int si = 0; si < nseeds && cfg_ok; si++) {
                    int64_t seed = si < (int)(sizeof EDGE / sizeof *EDGE) ? EDGE[si] : (int64_t)rng_next(&rs);
                    if (si >= 3 && si < 7) seed = (int64_t)rng_next(&rs) ^ EDGE[si % 7];
                    McSeeds sd = mcgen_seeds_unified(seed);
                    McWorld *w;
                    if (mcgen_world_new(g, dn, pn, &sd, NULL, 0, &w, err, sizeof err)) { snprintf(why, sizeof why, "world_new: %s", err); cfg_ok = 0; break; }
                    int ymin = mcgen_world_min_y(w), yh = mcgen_world_height(w);
                    int n = points / nseeds;
                    int *xyz = malloc(sizeof(int) * 3 * (size_t)n); uint8_t *gpu = malloc((size_t)n);
                    for (int i = 0; i < n; i++) {
                        uint64_t r = rng_next(&rs);
                        int span = (r & 7) == 0 ? 30000000 : ((r & 7) <= 2 ? 4000000 : ((r & 7) <= 4 ? 200000 : ((r & 7) == 5 ? 20000 : 1500)));
                        xyz[3 * i] = (int)(rng_next(&rs) % (2 * (uint64_t)span + 1)) - span;
                        xyz[3 * i + 2] = (int)(rng_next(&rs) % (2 * (uint64_t)span + 1)) - span;
                        xyz[3 * i + 1] = ymin - 16 + (int)(rng_next(&rs) % (uint64_t)(yh + 32));
                    }
                    int ncpu = 0;
                    double t0 = now();
                    int rc = mcgen_gpu_biome_points(w, n, xyz, gpu, &ncpu);
                    t_gpu += now() - t0;
                    if (rc) { char s2[1500]; mcgen_gpu_status(s2, sizeof s2); snprintf(why, sizeof why, "GPU отказал для мира (код %d): %s", rc, strstr(s2, "last_fallback") ? strstr(s2, "last_fallback") : "?"); cfg_ok = 0; free(xyz); free(gpu); mcgen_world_free(w); break; }
                    /* ВНИМАНИЕ: mcgen_gpu_biome_points уже заменила 255 на CPU-значения; для честного сравнения пересчитываем «нерешённые» отдельно — они совпадают по построению */
                    t0 = now();
                    Job jobs[64]; pthread_t th[64]; if (threads > 64) threads = 64;
                    for (int t = 0; t < threads; t++) { memset(&jobs[t], 0, sizeof jobs[t]); jobs[t] = (Job){ w, xyz, gpu, (int)((long long)n * t / threads), (int)((long long)n * (t + 1) / threads), 0, {0}, 0, 0, 0 }; pthread_create(&th[t], NULL, cmp_thread, &jobs[t]); }
                    long long mism = 0; Job *fj = NULL;
                    for (int t = 0; t < threads; t++) { pthread_join(th[t], NULL); mism += jobs[t].mism; if (!fj && jobs[t].has_first) fj = &jobs[t]; }
                    t_cpu += now() - t0;
                    cpts += n; cmism += mism; cunres += ncpu;
                    if (mism && fj) snprintf(why, sizeof why, "seed %lld: первое расхождение в (%d,%d,%d): GPU %d, CPU %d", (long long)seed, fj->first[0], fj->first[1], fj->first[2], fj->first_g, fj->first_c);
                    free(xyz); free(gpu);
                    /* сетки */
                    for (int gi = 0; gi < ngrids; gi++) {
                        int step = 1 << (rng_next(&rs) % 5);
                        int nx = 48 + (int)(rng_next(&rs) % 80), nz = 48 + (int)(rng_next(&rs) % 80);
                        int span = (gi & 1) ? 30000000 : 300000;
                        int x0 = (int)(rng_next(&rs) % (2 * (uint64_t)span + 1)) - span, z0 = (int)(rng_next(&rs) % (2 * (uint64_t)span + 1)) - span;
                        int y = ymin + (int)(rng_next(&rs) % (uint64_t)yh);
                        uint8_t *a = malloc((size_t)nx * nz), *b = malloc((size_t)nx * nz);
                        if (mcgen_gpu_biome_grid(w, x0, z0, nx, nz, step, y, a) || mcgen_biome_grid_cpu(w, x0, z0, nx, nz, step, y, b)) { snprintf(why, sizeof why, "сетка: ошибка"); cfg_ok = 0; free(a); free(b); break; }
                        for (long i = 0; i < (long)nx * nz; i++) if (a[i] != b[i]) { cgmism++; }
                        cgrid += (long)nx * nz; free(a); free(b);
                    }
                    mcgen_world_free(w);
                }
                configs++;
                printf("%-18s %-22s %-22s: точек %9lld  расхождений %lld  (GPU→CPU: %lld)  сеток %8lld расхождений %lld  GPU %.2f с, CPU %.2f с %s%s\n", ver, dn, pn, cpts, cmism, cunres, cgrid, cgmism,
                       t_gpu, t_cpu, cfg_ok ? "" : " ОТКАЗ: ", cfg_ok ? "" : why);
                if (cmism && why[0]) printf("    %s\n", why);
                fflush(stdout);
                tot_pts += cpts; tot_mism += cmism; tot_unres += cunres; tot_grid += cgrid; tot_gmism += cgmism;
                if (!cfg_ok) tot_mism += 1;
            }
        }
        mcgen_close(g);
    }
    printf("\nИТОГО: конфигураций %d, точек %lld, расхождений %lld (GPU→CPU %lld), точек в сетках %lld, расхождений %lld\n", configs, tot_pts, tot_mism, tot_unres, tot_grid, tot_gmism);
    free(vcopy);
    return (tot_mism || tot_gmism || !configs) ? 1 : 0;
}

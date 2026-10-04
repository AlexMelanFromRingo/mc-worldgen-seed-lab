/* g9_terrain.c — ворота G9 (часть 2): плотность final_density и жилы руд на GPU ≡ CPU побитно (уровень объёмов).
 *
 *   g9_terrain --run ../run --lib build/gpu/libmcgen_cuda.so [--versions 26.3,26.4-snapshot-2] [--chunks 256] [--seeds 3]
 *              [--dims all|overworld|nether|end] [--presets] [--structures]
 * Для каждой версии (26.3+) × измерения × seed: GPU считает плотность (и заплатки жил, и ячейки кэшей) для --chunks случайных
 * чанков; CPU — s_volume(final_density) в «чистом» контексте и veins_apply_new на сплошном камне; сравнение побитово.
 * Без аргументов (make test) ничего не делает. */
#include "mcgen_internal.h"
#include "gpu_bridge.h"
#include "mcgen_tweaks_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


void veins_apply_new(McWorld *w, SCtx *x, int cx, int cz, int y0, int ny, uint16_t *blocks);
typedef struct Beard Beard;
Beard *beard_for_chunk(McWorld *w, int cx, int cz);
void beard_free(Beard *b);
float beard_value(const Beard *b, int x, int y, int z);
void beard_volume(const Beard *b, float *out, const Vol *v);
static float bcb_v(void *ud, int x, int y, int z) { return beard_value(ud, x, y, z); }
static void bcb_vol(void *ud, float *o, const Vol *v) { beard_volume(ud, o, v); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
static uint64_t rng_next(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x; }

int main(int argc, char **argv) {
    const char *run = NULL, *lib = NULL, *vers = "26.3,26.4-snapshot-2", *dims = "all";
    int nchunks = 256, nseeds = 3, presets = 0, structures = 0, tweaks = 0;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(k, "--run") && v) { run = v; i++; }
        else if (!strcmp(k, "--lib") && v) { lib = v; i++; }
        else if (!strcmp(k, "--versions") && v) { vers = v; i++; }
        else if (!strcmp(k, "--chunks") && v) { nchunks = atoi(v); i++; }
        else if (!strcmp(k, "--seeds") && v) { nseeds = atoi(v); i++; }
        else if (!strcmp(k, "--dims") && v) { dims = v; i++; }
        else if (!strcmp(k, "--presets")) presets = 1;
        else if (!strcmp(k, "--structures")) structures = 1;
        else if (!strcmp(k, "--tweaks")) tweaks = 1;
    }
    if (!run) { printf("g9_terrain --run <run> --lib <libmcgen_cuda.so> … — пропуск\n"); return 0; }
    if (lib) mcgen_gpu_set_library_path(lib);
    mcgen_gpu_set_compute(MCGEN_COMPUTE_GPU, -1);
    char st[1500];
    if (!mcgen_gpu_status(st, sizeof st)) { printf("GPU недоступно:\n%s\n", st); return 3; }
    static const int64_t SEEDS[] = { 12345, -4172144997902289642LL, 0, 987654321987LL, 7 };
    long long tot_chunks = 0, tot_bad_d = 0, tot_bad_v = 0, tot_bad_c = 0, tot_elems = 0, tot_vchunks = 0, tot_patches = 0, tot_cells = 0; int configs = 0, fails = 0;
    char *vcopy = strdup(vers); uint64_t rs = 0x1234ABCDULL;
    for (char *ver = strtok(vcopy, ","); ver; ver = strtok(NULL, ",")) {
        char pack[512], err[512]; McGen *g;
        snprintf(pack, sizeof pack, "%s/pack-%s", run, ver);
        if (mcgen_open(pack, ver, &g, err, sizeof err)) { printf("пропуск %s: %s\n", ver, err); continue; }
        if (!g->newf) { printf("%s: ветка 26.1/26.2 — рельеф на GPU не реализован, пропуск\n", ver); mcgen_close(g); continue; }
        for (int d = 0; d < mcgen_dimension_count(g); d++) {
            const char *dn = mcgen_dimension_name(g, d);
            if (strcmp(dims, "all") && !strstr(dn, dims)) continue;
            for (int p = 0; p < mcgen_preset_count(g, dn); p++) {
                const char *pn = mcgen_preset_name(g, dn, p);
                if (!presets && strcmp(pn, "normal")) continue;
                for (int si = 0; si < nseeds; si++) {
                    int64_t seed = SEEDS[si % 5];
                    McSeeds sd = mcgen_seeds_unified(seed);
                    McWorld *w; char e2[512];
                    McTweakValue tv[6] = { { "terrain_amplitude", 1.6 }, { "terrain_steepness", 2.0 }, { "climate_scale_xz", 1.7 }, { "climate_scale_y", 0.8 }, { "cave_density", 0.6 }, { "sea_level_offset", 7 } };
                    if (mcgen_world_new(g, dn, pn, &sd, tweaks ? tv : NULL, tweaks ? 6 : 0, &w, e2, sizeof e2)) continue;
                    w->struct_on = structures;
                    int nmin, nh, bmax, veins_on = 0; char why[300] = {0};
                    if (gpu_terrain_dims(w, &nmin, &nh, &bmax, &veins_on)) { printf("%-16s %-22s %-14s seed %-20lld: рельеф на GPU недоступен\n", ver, dn, pn, (long long)seed); fails++; mcgen_world_free(w); continue; }
                    size_t nd = (size_t)16 * nh * 16;
                    int *cx = malloc(sizeof(int) * nchunks), *cz = malloc(sizeof(int) * nchunks);
                    for (int i = 0; i < nchunks; i++) {
                        uint64_t r = rng_next(&rs);
                        int span = (r & 3) == 0 ? 3000 : ((r & 3) == 1 ? 300 : ((r & 3) == 2 ? 40 : 1900000));
                        cx[i] = (int)(rng_next(&rs) % (2 * (uint64_t)span + 1)) - span; cz[i] = (int)(rng_next(&rs) % (2 * (uint64_t)span + 1)) - span;
                    }
                    float *dens = malloc(sizeof(float) * nd * nchunks); uint16_t *vein = malloc(sizeof(uint16_t) * nd * nchunks);
                    int ncell = 0; size_t cf = 0; signed char *which = NULL;
                    double t0 = now();
                    int rc = gpu_terrain_batch_raw(w, 1, cx, cz, dens, vein, NULL, NULL, why, sizeof why);       /* прогрев и создание мира */
                    float *cells = NULL;
                    gpu_terrain_cells_info(w, &ncell, &cf);
                    if (cf) cells = malloc(sizeof(float) * cf * nchunks);
                    which = malloc((size_t)(ncell ? ncell : 1) * nchunks);
                    t0 = now();
                    if (!rc) rc = gpu_terrain_batch_raw(w, nchunks, cx, cz, dens, vein, cells, which, why, sizeof why);
                    double tg = now() - t0;
                    if (rc) { printf("%-16s %-22s %-14s seed %-20lld: ОТКАЗ GPU: %s\n", ver, dn, pn, (long long)seed, why); fails++; mcgen_world_free(w); free(cx); free(cz); free(dens); free(vein); free(cells); continue; }
                    long long n_beard = 0, n_patch = 0, n_vchunks = 0, bad_d = 0, bad_v = 0, bad_c = 0, n_inj = 0, n_noinj = 0, n_unknown = 0; double tcpu = 0;
                    SCtx *x = sctx_new(w->nc, 1);
                    float *cd = malloc(sizeof(float) * nd);
                    uint16_t *bc = malloc(sizeof(uint16_t) * (size_t)w->height * 256), *bg = malloc(sizeof(uint16_t) * (size_t)w->height * 256);
                    float *surf = malloc(sizeof(float) * 11 * 11);
                    for (int i = 0; i < nchunks; i++) {
                        double t1 = now();
                        sctx_reset_caches(x);
                        Vol q = { 11, 1, 11, cx[i] * 16 - 16, 0, cz[i] * 16 - 16, 4, 1, 4 };
                        int has_aq = w->ns->has_aquifers && w->s_aq[AQ_SURFACE_LEVEL];
                        if (has_aq) s_volume(x, w->s_aq[AQ_SURFACE_LEVEL], surf, &q);
                        Vol v = { 16, nh, 16, cx[i] * 16, nmin, cz[i] * 16, 1, 1, 1 };

                        Beard *bd = structures ? beard_for_chunk(w, cx[i], cz[i]) : NULL;     /* как terrain_fill_chunk */
                        SBeard sbd = { bd, bcb_v, bcb_vol };
                        n_beard += bd != NULL;
                        sctx_set_beardifier(x, bd ? &sbd : NULL);
                        s_volume(x, w->s_rf[RF_FINAL_DENSITY], cd, &v);
                        sctx_set_beardifier(x, NULL); beard_free(bd);
                        tcpu += now() - t1;
                        if (memcmp(cd, dens + (size_t)i * nd, sizeof(float) * nd)) { for (size_t k = 0; k < nd; k++) if (memcmp(&cd[k], &dens[(size_t)i * nd + k], 4)) bad_d++; }
                        /* ячейки кэшей: каждая, которую GPU возвращает (which ≥ 0), должна совпасть с ячейкой CPU; -2 — «неизвестно» (считать нельзя) */
                        for (int k = 0; k < ncell; k++) {
                            int wh = which[(size_t)i * ncell + k];
                            if (wh == -2) { n_unknown++; continue; }
                            if (wh < 0) { n_noinj++; continue; }
                            int cid, vol9[9], xr, zr, cnt; size_t off;
                            gpu_terrain_cell_cand(w, k, wh, &cid, vol9, &xr, &zr, &cnt, &off);
                            Vol cv; const float *cb;
                            int has = gpu_sctx_get_cell(x, cid, &cv, &cb);
                            Vol ev = { vol9[0], vol9[1], vol9[2], xr ? cx[i] * 16 + vol9[3] : vol9[3], vol9[4], zr ? cz[i] * 16 + vol9[5] : vol9[5], vol9[6], vol9[7], vol9[8] };
                            if (!has || memcmp(&cv, &ev, sizeof(Vol)) || memcmp(cb, cells + (size_t)i * cf + off, sizeof(float) * cnt)) {
                                if (bad_c < 4) printf("   чанк %d (cid %d): ячейка CPU %s, объём CPU {%d,%d,%d|%d,%d,%d|%d,%d,%d}, ожидался {%d,%d,%d|%d,%d,%d|%d,%d,%d}\n", i, cid, has ? "есть" : "НЕТ", has ? cv.sx : 0, has ? cv.sy : 0, has ? cv.sz : 0, has ? cv.x0 : 0, has ? cv.y0 : 0, has ? cv.z0 : 0, has ? cv.dx : 0, has ? cv.dy : 0, has ? cv.dz : 0, ev.sx, ev.sy, ev.sz, ev.x0, ev.y0, ev.z0, ev.dx, ev.dy, ev.dz);
                                bad_c++;
                            } else n_inj++;
                        }
                        /* жилы: сплошной камень, CPU-правила против заплаток GPU */
                        if (veins_on && w->veins && ((int *)w->veins)[0] > 0 && w->tweak[MCGEN_TWEAK_ORE_VEINS] != 0.0) {
                            n_vchunks++;
                            for (size_t kk = 0; kk < nd; kk++) n_patch += vein[(size_t)i * nd + kk] != 0xFFFF;
                            for (size_t k = 0; k < (size_t)w->height * 256; k++) bc[k] = bg[k] = (uint16_t)w->def_block;
                            veins_apply_new(w, x, cx[i], cz[i], nmin - w->min_y, nh, bc);
                            GpuChunk gc; memset(&gc, 0, sizeof gc); gc.veins = vein + (size_t)i * nd;
                            gpu_terrain_apply_veins(w, &gc, bg);
                            if (memcmp(bc, bg, sizeof(uint16_t) * (size_t)w->height * 256)) bad_v++;
                        }
                    }
                    tot_chunks += nchunks; tot_elems += (long long)nd * nchunks; tot_bad_d += bad_d; tot_bad_v += bad_v; tot_bad_c += bad_c; configs++; tot_vchunks += n_vchunks; tot_patches += n_patch; tot_cells += n_inj;
                    printf("%-16s %-22s %-14s seed %-20lld: чанков %d, элементов плотности %lld, расхождений плотности %lld, жилы: чанков %lld (позиций с жилой %lld), расхождений %lld, ячейки кэшей: совпало %lld, расхождений %lld, без вставки %lld, неизвестно %lld; чанков с Beardifier %lld; GPU %.3f с (%.0f ч/с), CPU(объёмы) %.3f с\n",
                           ver, dn, pn, (long long)seed, nchunks, (long long)nd * nchunks, bad_d, n_vchunks, n_patch, bad_v, n_inj, bad_c, n_noinj, n_unknown, n_beard, tg, nchunks / tg, tcpu);
                    fflush(stdout);
                    sctx_free(x); free(cd); free(bc); free(bg); free(surf);
                    free(cx); free(cz); free(dens); free(vein); free(cells); free(which);
                    mcgen_world_free(w);
                }
            }
        }
        mcgen_close(g);
    }
    printf("\nИТОГО: конфигураций %d, чанков %lld, элементов плотности %lld; РАСХОЖДЕНИЙ плотности %lld; жилы: чанков %lld, позиций с жилой %lld, РАСХОЖДЕНИЙ (чанков) %lld; ячейки кэшей: совпало %lld, РАСХОЖДЕНИЙ %lld; отказов %d\n", configs, tot_chunks, tot_elems, tot_bad_d, tot_vchunks, tot_patches, tot_bad_v, tot_cells, tot_bad_c, fails);
    free(vcopy);
    return (tot_bad_d || tot_bad_v || tot_bad_c || fails || !configs) ? 1 : 0;
}

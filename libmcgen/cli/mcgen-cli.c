/* mcgen-cli — командная строка libmcgen.
 *
 *   mcgen-cli --pack run/pack-26.3 --version 26.3 --dim minecraft:overworld --preset normal --seed 12345 \
 *             [--seeds climate,terrain,structures,features] [--tweak id=value …] \
 *             --cx0 0 --cz0 0 --nx 8 --nz 8 --stages 0x3f --threads 0 --out region.mcr
 *             [--pp-margin K]  растекание жидкостей только в чанках на расстоянии >= K от края (эмуляция загруженной
 *                              игрой области: внешние кольца FULL/proto-чанков postProcessGeneration не проходят)
 *   mcgen-cli info  --pack … --version …                        измерения, пресеты, число состояний/биомов, настройки
 *   mcgen-cli df    --pack … --version … --dim … --preset … --seed S --id <df_id>   точки «x y z» из stdin →
 *                   «value hexbits» (float-биты для 26.3+, double-биты для 26.1/26.2)  [тест G1]
 *   mcgen-cli biome --pack … --dim … --seed S --x0 X --z0 Z --nx N --nz N --step K --y Y   сетка биомов (имена)
 *   mcgen-cli bench --pack … --dim … --seed S --nx N --nz N --stages 2 --threads T          скорость генерации
 */
#include "mcgen.h"
#include "mcgen_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

typedef struct {
    const char *cmd, *pack, *version, *dim, *preset, *out, *id;
    int64_t seed; int has_seeds; McSeeds seeds;
    McTweakValue tw[64]; int ntw;
    int cx0, cz0, nx, nz, threads, pp_margin; unsigned stages;
    int x0, z0, step, y;
} Args;

static int64_t parse_seed(const char *s) {
    if (!strncmp(s, "s:", 2)) { uint32_t h = 0; for (const char *p = s + 2; *p; p++) h = 31u * h + (uint8_t)*p; return (int64_t)(int32_t)h; }
    char *e; long long v = strtoll(s, &e, 0);
    if (*e) { uint32_t h = 0; for (const char *p = s; *p; p++) h = 31u * h + (uint8_t)*p; return (int64_t)(int32_t)h; }   /* как игра для нечисловых */
    return (int64_t)v;
}

static void usage(void) {
    fprintf(stderr, "использование: mcgen-cli [info|df|biome|bench] --pack DIR --version V [--dim D] [--preset P] [--seed S]\n"
                    "  регион: --cx0 --cz0 --nx --nz --stages 0x3f --threads 0 --out file.mcr  (см. libmcgen/README.md)\n");
}

static int parse_args(int argc, char **argv, Args *a) {
    memset(a, 0, sizeof *a);
    a->version = "26.3"; a->dim = "minecraft:overworld"; a->preset = "normal"; a->nx = a->nz = 1; a->stages = 1u; a->step = 4; a->y = 64; a->pp_margin = -1;
    int i = 1;
    if (argc > 1 && argv[1][0] != '-') a->cmd = argv[i++];
    for (; i < argc; i++) {
        const char *k = argv[i]; const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        #define NEXT() do { if (!v) { fprintf(stderr, "нет значения для %s\n", k); return -1; } i++; } while (0)
        if (!strcmp(k, "--pack")) { NEXT(); a->pack = v; }
        else if (!strcmp(k, "--version")) { NEXT(); a->version = v; }
        else if (!strcmp(k, "--dim")) { NEXT(); a->dim = v; }
        else if (!strcmp(k, "--preset")) { NEXT(); a->preset = v; }
        else if (!strcmp(k, "--seed")) { NEXT(); a->seed = parse_seed(v); }
        else if (!strcmp(k, "--seeds")) {
            NEXT(); int64_t s[4]; char *dup = strdup(v), *p = dup;
            for (int j = 0; j < 4; j++) { char *c = strchr(p, ','); if (c) *c = 0; s[j] = parse_seed(p); p = c ? c + 1 : p; }
            free(dup);
            a->has_seeds = 1; a->seeds.climate = s[0]; a->seeds.terrain = s[1]; a->seeds.structures = s[2]; a->seeds.features = s[3];
        }
        else if (!strcmp(k, "--tweak")) {
            NEXT(); const char *eq = strchr(v, '=');
            if (!eq || a->ntw >= 64) { fprintf(stderr, "--tweak id=value\n"); return -1; }
            char *id = malloc((size_t)(eq - v) + 1); memcpy(id, v, (size_t)(eq - v)); id[eq - v] = 0;
            a->tw[a->ntw].id = id; a->tw[a->ntw].value = strtod(eq + 1, NULL); a->ntw++;
        }
        else if (!strcmp(k, "--cx0")) { NEXT(); a->cx0 = atoi(v); }
        else if (!strcmp(k, "--cz0")) { NEXT(); a->cz0 = atoi(v); }
        else if (!strcmp(k, "--nx")) { NEXT(); a->nx = atoi(v); }
        else if (!strcmp(k, "--nz")) { NEXT(); a->nz = atoi(v); }
        else if (!strcmp(k, "--stages")) { NEXT(); a->stages = (unsigned)strtoul(v, NULL, 0); }
        else if (!strcmp(k, "--threads")) { NEXT(); a->threads = atoi(v); }
        else if (!strcmp(k, "--pp-margin")) { NEXT(); a->pp_margin = atoi(v); }
        else if (!strcmp(k, "--out")) { NEXT(); a->out = v; }
        else if (!strcmp(k, "--id")) { NEXT(); a->id = v; }
        else if (!strcmp(k, "--x0")) { NEXT(); a->x0 = atoi(v); }
        else if (!strcmp(k, "--z0")) { NEXT(); a->z0 = atoi(v); }
        else if (!strcmp(k, "--step")) { NEXT(); a->step = atoi(v); }
        else if (!strcmp(k, "--y")) { NEXT(); a->y = atoi(v); }
        else { fprintf(stderr, "неизвестный аргумент %s\n", k); return -1; }
        #undef NEXT
    }
    if (!a->has_seeds) a->seeds = mcgen_seeds_unified(a->seed);
    if (!a->pack) { usage(); return -1; }
    return 0;
}

static int progress(void *ud, double f, const char *what) { (void)ud; fprintf(stderr, "\r%5.1f%% %s   ", f * 100.0, what ? what : ""); return 0; }

int main(int argc, char **argv) {
    Args a;
    if (parse_args(argc, argv, &a)) return 2;
    char err[1024] = {0};
    McGen *g;
    if (mcgen_open(a.pack, a.version, &g, err, sizeof err)) { fprintf(stderr, "mcgen_open: %s\n", err); return 1; }
    int rc = 0;
    if (a.cmd && !strcmp(a.cmd, "info")) {
        printf("%s\nсостояний блоков: %d, биомов: %d\n", mcgen_version(), mcgen_block_state_count(g), mcgen_biome_count(g));
        for (int d = 0; d < mcgen_dimension_count(g); d++) {
            const char *dn = mcgen_dimension_name(g, d);
            printf("%s:", dn);
            for (int p = 0; p < mcgen_preset_count(g, dn); p++) printf(" %s", mcgen_preset_name(g, dn, p));
            printf("\n");
        }
        for (int t = 0; t < mcgen_tweak_count(g); t++) { const McTweakInfo *ti = mcgen_tweak_info(g, t); printf("tweak %s = %g [%g, %g]\n", ti->id, ti->def, ti->min, ti->max); }
        mcgen_close(g); return 0;
    }
    McWorld *w;
    if (mcgen_world_new(g, a.dim, a.preset, &a.seeds, a.tw, a.ntw, &w, err, sizeof err)) { fprintf(stderr, "mcgen_world_new: %s\n", err); mcgen_close(g); return 1; }
    if (a.cmd && !strcmp(a.cmd, "df")) {
        /* --id a,b,c — несколько функций за один запуск: вывод по функциям подряд */
        if (!a.id) { fprintf(stderr, "df: нужен --id\n"); return 2; }
        int cap = 1 << 16, n = 0; int *xyz = malloc(sizeof(int) * 3 * (size_t)cap);
        int x, y, z;
        while (scanf("%d %d %d", &x, &y, &z) == 3) {
            if (n == cap) { cap *= 2; xyz = realloc(xyz, sizeof(int) * 3 * (size_t)cap); }
            xyz[3 * n] = x; xyz[3 * n + 1] = y; xyz[3 * n + 2] = z; n++;
        }
        double *out = malloc(sizeof(double) * (size_t)(n ? n : 1));
        int newf = mcgen_x_is_float(w);
        char *ids = strdup(a.id), *save = NULL;
        for (char *id = strtok_r(ids, ",", &save); id && !rc; id = strtok_r(NULL, ",", &save)) {
            if (mcgen_x_df_eval(w, id, n, xyz, out, err, sizeof err)) { fprintf(stderr, "df %s: %s\n", id, err); rc = 1; break; }
            for (int i = 0; i < n; i++) {
                if (newf) { float f = (float)out[i]; uint32_t b; memcpy(&b, &f, 4); printf("%.9g %08" PRIx32 "\n", f, b); }
                else { uint64_t b; memcpy(&b, &out[i], 8); printf("%.17g %016" PRIx64 "\n", out[i], b); }
            }
        }
        free(ids); free(xyz); free(out);
    } else if (a.cmd && !strcmp(a.cmd, "biome")) {
        uint8_t *grid = malloc((size_t)a.nx * (size_t)a.nz);
        if (mcgen_biome_grid(w, a.x0, a.z0, a.nx, a.nz, a.step, a.y, grid)) { fprintf(stderr, "biome_grid: ошибка\n"); rc = 1; }
        else for (int i = 0; i < a.nx * a.nz; i++) printf("%s\n", mcgen_biome_name(g, grid[i]));
        free(grid);
    } else if (a.cmd && !strcmp(a.cmd, "qbiome")) {
        /* «сырые» биомы по квартам: --x0/--z0 — кварты, --y — кварта */
        uint8_t *grid = malloc((size_t)a.nx * (size_t)a.nz);
        mcgen_x_noise_biomes(w, a.x0, a.z0, a.nx, a.nz, a.y, grid);
        for (int i = 0; i < a.nx * a.nz; i++) printf("%s\n", mcgen_biome_name(g, grid[i]));
        free(grid);
    } else if (a.cmd && !strcmp(a.cmd, "climate")) {
        /* квантованный климат по квартам (как oracle climategrid q) */
        for (int iz = 0; iz < a.nz; iz++) for (int ix = 0; ix < a.nx; ix++) {
            float v[6]; mcgen_x_climate(w, a.x0 + ix, a.y, a.z0 + iz, v);
            for (int k = 0; k < 6; k++) printf("%lld%c", (long long)(v[k] * 10000.0f), k == 5 ? '\n' : ' ');
        }
    } else if (a.cmd && !strcmp(a.cmd, "chunkbiomes")) {
        /* биомы стадии BIOMES региона (как генерация чанка): по чанкам cz-major, [qy][qz][qx] */
        McRegion *r;
        if (mcgen_generate_region(w, a.cx0, a.cz0, a.nx, a.nz, MC_STAGE_BIOMES, a.threads, NULL, NULL, &r, err, sizeof err)) { fprintf(stderr, "%s\n", err); rc = 1; }
        else {
            McRegionInfo ri; mcgen_region_info(r, &ri);
            for (int cz = 0; cz < a.nz; cz++) for (int cx = 0; cx < a.nx; cx++) {
                uint8_t *b = mcgen_region_biomes(r, a.cx0 + cx, a.cz0 + cz);
                for (int i = 0; i < ri.height / 4 * 16; i++) printf("%s\n", mcgen_biome_name(g, b[i]));
            }
            mcgen_region_free(r);
        }
    } else if (a.cmd && !strcmp(a.cmd, "biometie")) {
        /* stdin: «qx qy qz биомA биомB» → 1 (ничья), 0 */
        int qx, qy, qz; char ba[128], bb[128];
        while (scanf("%d %d %d %127s %127s", &qx, &qy, &qz, ba, bb) == 5) {
            int ia = -1, ib = -1;
            for (int i = 0; i < mcgen_biome_count(g); i++) { if (!strcmp(mcgen_biome_name(g, i), ba)) ia = i; if (!strcmp(mcgen_biome_name(g, i), bb)) ib = i; }
            printf("%d\n", mcgen_x_biome_tie(w, qx, qy, qz, ia, ib));
        }
    } else if (a.cmd && !strcmp(a.cmd, "fillraw")) {
        /* «сырое» заполнение (без пост-обработки жидкостей) одного чанка --cx0/--cz0: id состояний построчно y,z,x */
        uint16_t *b = malloc(sizeof(uint16_t) * (size_t)mcgen_world_height(w) * 256);
        if (mcgen_x_fill_chunk(w, a.cx0, a.cz0, b, err, sizeof err)) { fprintf(stderr, "%s\n", err); rc = 1; }
        else { fwrite(b, sizeof(uint16_t), (size_t)mcgen_world_height(w) * 256, stdout); }
        free(b);
    } else if (a.cmd && !strcmp(a.cmd, "bench")) {
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        McRegion *r;
        if (mcgen_generate_region(w, a.cx0, a.cz0, a.nx, a.nz, a.stages, a.threads, NULL, NULL, &r, err, sizeof err)) { fprintf(stderr, "%s\n", err); rc = 1; }
        else {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
            printf("%d чанков за %.3f с: %.2f чанков/с (потоков %d)\n", a.nx * a.nz, dt, a.nx * a.nz / dt, a.threads);
            mcgen_region_free(r);
        }
    } else if (!a.cmd) {
        McRegion *r;
        int grc = a.pp_margin >= 0 ? mcgen_x_generate_region_pp(w, a.cx0, a.cz0, a.nx, a.nz, a.stages, a.threads, a.pp_margin, &r, err, sizeof err)
                                   : mcgen_generate_region(w, a.cx0, a.cz0, a.nx, a.nz, a.stages, a.threads, a.out ? progress : NULL, NULL, &r, err, sizeof err);
        if (grc) {
            fprintf(stderr, "\nmcgen_generate_region: %s\n", err); rc = 1;
        } else {
            if (a.out) fprintf(stderr, "\n");
            if (a.out && mcgen_region_write_mcr(r, g, a.out, err, sizeof err)) { fprintf(stderr, "write_mcr: %s\n", err); rc = 1; }
            mcgen_region_free(r);
        }
    } else { usage(); rc = 2; }
    mcgen_world_free(w);
    mcgen_close(g);
    for (int i = 0; i < a.ntw; i++) free((void *)a.tw[i].id);
    return rc;
}

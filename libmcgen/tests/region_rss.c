/* region_rss.c — физическая память региона после генерации и сверка содержимого (освобождение нулевых страниц: region.c, MCGEN_NO_TRIM=1 отключает).
 *   region_rss <pack> <версия> <seed> <cx0> <cz0> <N> <стадии> — печатает RSS до/после и контрольную сумму блоков (FNV-1a), число нулевых страниц.
 * Без аргументов (make test) ничего не делает. Только Linux (читает /proc/self/statm). */
#include "../include/mcgen.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static double rss_mb(void) { long pages = 0, res = 0; FILE *f = fopen("/proc/self/statm", "r"); if (!f) return -1; if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0; fclose(f); return (double)res * (double)sysconf(_SC_PAGESIZE) / 1048576.0; }

int main(int argc, char **argv) {
    if (argc < 8) { printf("region_rss <pack> <версия> <seed> <cx0> <cz0> <N> <стадии> — пропуск\n"); return 0; }
    char err[512] = {0};
    McGen *g; McWorld *w; McRegion *reg;
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    McSeeds sd = mcgen_seeds_unified(atol(argv[3]));
    if (mcgen_world_new(g, "minecraft:overworld", "normal", &sd, NULL, 0, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    int cx0 = atoi(argv[4]), cz0 = atoi(argv[5]), n = atoi(argv[6]);
    double r0 = rss_mb();
    if (mcgen_generate_region(w, cx0, cz0, n, n, (uint32_t)strtoul(argv[7], NULL, 0), 0, NULL, NULL, &reg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    double r1 = rss_mb();
    McRegionInfo ri; mcgen_region_info(reg, &ri);
    uint64_t h = 1469598103934665603ULL; long zero_pages = 0, pages = 0;
    for (int cz = 0; cz < n; cz++) for (int cx = 0; cx < n; cx++) {
        const uint16_t *b = mcgen_region_blocks(reg, cx0 + cx, cz0 + cz);
        size_t cnt = (size_t)ri.height * 256;
        for (size_t i = 0; i < cnt; i++) { h ^= b[i]; h *= 1099511628211ULL; }
        for (size_t p = 0; p + 2048 <= cnt; p += 2048) { size_t k = 0; while (k < 2048 && !b[p + k]) k++; pages++; if (k == 2048) zero_pages++; }
    }
    double r2 = rss_mb();
    printf("rss до %.0f МБ, после генерации %.0f МБ (регион %d×%d), после чтения всех блоков %.0f МБ; нулевых страниц по 4 КБ: %ld из %ld; fnv=%016llx\n", r0, r1, n, n, r2, zero_pages, pages, (unsigned long long)h);
    mcgen_region_free(reg); mcgen_world_free(w); mcgen_close(g);
    return 0;
}

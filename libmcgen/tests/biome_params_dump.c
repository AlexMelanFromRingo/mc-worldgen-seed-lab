/* biome_params_dump — печатает таблицу климата Overworld/Nether libmcgen в формате data/params/<V>/*.tsv
 * (13 чисел: 6 пар lo hi, offset; затем имя биома) — для сверки с дампом игры (tests/biome_params_check.sh). */
#include "mcgen_internal.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "biome_params_dump <pack> <version> overworld|nether\n"); return 2; }
    char err[512]; McGen *g;
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    int ow = !strcmp(argv[3], "overworld");
    const ClimPoint *p = ow ? g->ow_points : g->nether_points; int n = ow ? g->n_ow : g->n_nether;
    for (int i = 0; i < n; i++) {
        for (int d = 0; d < 6; d++) printf("%lld\t%lld\t", (long long)p[i].lo[d], (long long)p[i].hi[d]);
        printf("%lld\t%s\n", (long long)p[i].lo[6], g->biome_names[p[i].biome]);
    }
    mcgen_close(g);
    return 0;
}

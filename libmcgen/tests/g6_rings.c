/* g6_rings.c — позиции колец strongholds libmcgen (JSON [[cx,cz],…]) для сверки с oracle `stronghold <seed>`.
 *   g6_rings <pack> <версия> <seed> */
#include "structure.h"
#include <stdio.h>
#include <stdlib.h>
int structures_ring_force_all(StructWorld *sw, StructSet *s);
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    McGen *g; char err[512];
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    McSeeds sd = mcgen_seeds_unified(strtoll(argv[3], NULL, 10)); McWorld *w;
    if (mcgen_world_new(g, "minecraft:overworld", "normal", &sd, NULL, 0, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    StructWorld *sw = structures_world_get(w);
    double t0 = now_sec();
    for (int i = 0; i < sw->nsets; i++) if (sw->sets[i].ptype == 1) {
        structures_ring_force_all(sw, &sw->sets[i]);
        printf("[");
        for (int k = 0; k < sw->sets[i].nring; k++) printf("%s[%d,%d]", k ? "," : "", sw->sets[i].ring_x[k], sw->sets[i].ring_z[k]);
        printf("]\n");
    }
    fprintf(stderr, "rings: %.2f s\n", now_sec() - t0);
    return 0;
}

/* g9_dump.c — отладка W7: гистограмма видов узлов скомпилированных программ (поля роутера мира) для выгрузки на GPU.
 *   g9_dump <pack> <версия> <измерение> <пресет> [seed] */
#include "mcgen_internal.h"
#include "gpu_bridge.h"
#include <stdio.h>
#include <stdlib.h>
static const char *KN[] = { "CONST","NOISE","NOISE_XZ","NOISE_XYZ","SHIFT_B","END","DIST","GRAD_CLAMP","GRAD_REPEAT","GRAD_MIRROR","CTX_ALPHA","CTX_OFFSET","CTX_BEARD","ABS","SQUARE","CUBE","SQRT","LEAKY","RECIP","NEG","SQUEEZE","LOG","SIGN","ROUND_INT","ROUND","ADD","CADD","CSUB","SUB","MUL","CMUL","DIV","CDIV","MIN","CMIN","MAX","CMAX","POW_CB","POW_CE","POW","SPLINE","LERP","LERP_CF","LERP_CS","CLAMP","RANGE_C","RANGE","ISEL1","ISEL","CACHE","BLEND_DENSITY","INTERP","SLICE_X","SLICE_Y","SLICE_Z","SLICE_XZ","FTS" };
int main(int argc, char **argv) {
    if (argc < 5) return 0;
    char err[512]; McGen *g; McWorld *w;
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    McSeeds sd = mcgen_seeds_unified(argc > 5 ? atoll(argv[5]) : 12345);
    if (mcgen_world_new(g, argv[3], argv[4], &sd, NULL, 0, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    if (!g->newf) { printf("старая ветка\n"); return 0; }
    static const char *RN[] = { "temperature","vegetation","continents","erosion","depth","ridges","final_density","chunk_surface_level","prelim","barrier","flood","spread","lava","vein_toggle","vein_ridged","vein_gap" };
    for (int k = 0; k < RF__COUNT; k++) {
        if (!w->s_rf[k]) continue;
        McgProg p; const S *r[1] = { w->s_rf[k] };
        gpu_export_new(r, 1, &p);
        int h[64] = {0}; for (int i = 0; i < p.nnodes; i++) h[p.nodes[i].k]++;
        printf("%-20s %4d узлов, %3d шумов %4d октав %3d сплайнов;", RN[k], p.nnodes, p.nnoise, p.noct, p.nsp);
        for (int i = 0; i < 64; i++) if (h[i]) printf(" %s×%d", KN[i], h[i]);
        printf("\n");
        gpu_prog_free(&p);
    }
    return 0;
}

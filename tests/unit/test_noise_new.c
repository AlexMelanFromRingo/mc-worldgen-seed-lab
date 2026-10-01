/* Сверка NormalNoise (режим FLOAT, 26.3) с эталоном Java: ref263.txt */
#include "../../engine/mc_noise.h"
#include <stdio.h>
#include <stdlib.h>
typedef struct { const char *name; int first; int n; double base; double a[12]; } NP;
static const NP NOISES[] = {
    {"temperature", -10, 6, 1.2453007926713473, {1.5,0,1,0,0,0}},
    {"vegetation", -8, 6, 0.9494731054427978, {1,1,0,0,0,0}},
    {"continentalness", -9, 9, 0.8880832896205223, {1,1,2,2,2,1,1,1,1}},
    {"erosion", -9, 5, 1.063180125160734, {1,1,0,1,1}},
    {"ridge", -7, 6, 0.9147152149950137, {1,2,1,0,0,0}},
    {"offset", -3, 4, 0.9381732587751005, {1,1,1,0}},
};
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "r"); if (!f) { perror("ref"); return 2; }
    char nm[64]; long long seed, xb, yb, zb; unsigned vb;
    long long cur_seed = 0; static char keep[64]; static McNormal nn; McNoiseSpec spec;
    long total = 0, bad = 0;
    while (fscanf(f, "%lld %63s %lld %lld %lld %x", &seed, nm, &xb, &yb, &zb, &vb) == 6) {
        if (seed != cur_seed || strcmp(nm, keep) != 0 || total == 0) {
            cur_seed = seed; strcpy(keep, nm);
            const NP *P = NULL; for (int i = 0; i < 6; i++) if (!strcmp(NOISES[i].name, nm)) P = &NOISES[i];
            McNoiseParams np; memset(&np, 0, sizeof np); np.new_format = 1; np.first_octave = P->first; np.n_amp = P->n;
            np.base_amplitude = P->base; np.normalize = 1; np.has_mod = 1;
            for (int i = 0; i < P->n; i++) np.amp[i] = P->a[i];
            mc_spec_new(&spec, &np, 0);
            McXoro root = xoro_from_long_seed(seed);
            McXoroPos pf = xoro_fork_positional(&root);
            char full[96]; snprintf(full, sizeof full, "minecraft:%s", nm);
            u64 hl, hh; mc_md5_seed128(full, &hl, &hh);
            McXoro r = xoro_from_hash(&pf, hl, hh);
            normal_init_xoro(&nn, &spec, &r);
        }
        double x, y, z; memcpy(&x, &xb, 8); memcpy(&y, &yb, 8); memcpy(&z, &zb, 8);
        float v = normal_get_f(&nn, &spec, x, y, z);
        unsigned got; memcpy(&got, &v, 4);
        total++;
        if (got != vb) { if (bad < 5) { float e; memcpy(&e, &vb, 4); printf("MISMATCH seed=%lld %s: got %.9g exp %.9g\n", seed, nm, v, e); } bad++; }
    }
    printf("test_noise_new: %ld values, %ld mismatches\n", total, bad);
    return bad != 0;
}

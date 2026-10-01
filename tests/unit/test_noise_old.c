/* Сверка NormalNoise (режим DOUBLE, 26.1/26.2) с эталоном Java: ref262.txt */
#include "../../engine/mc_noise.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { const char *name; int first; int n; double a[12]; } NP;
static const NP NOISES[] = {
    {"temperature", -10, 6, {1.5,0,1,0,0,0}},
    {"vegetation", -8, 6, {1,1,0,0,0,0}},
    {"continentalness", -9, 9, {1,1,2,2,2,1,1,1,1}},
    {"erosion", -9, 5, {1,1,0,1,1}},
    {"ridge", -7, 6, {1,2,1,0,0,0}},
    {"offset", -3, 4, {1,1,1,0}},
};

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "r"); if (!f) { perror("ref"); return 2; }
    char nm[64]; long long seed, xb, yb, zb; unsigned long long vb;
    long long cur_seed = 0; const char *cur_name = ""; static McNormal nn; McNoiseSpec spec;
    long total = 0, bad = 0;
    while (fscanf(f, "%lld %63s %lld %lld %lld %llx", &seed, nm, &xb, &yb, &zb, &vb) == 6) {
        if (seed != cur_seed || strcmp(nm, cur_name) != 0 || total == 0) {
            cur_seed = seed;
            static char keep[64]; strcpy(keep, nm); cur_name = keep;
            const NP *P = NULL; for (int i = 0; i < 6; i++) if (!strcmp(NOISES[i].name, nm)) P = &NOISES[i];
            McNoiseParams np; memset(&np, 0, sizeof np); np.first_octave = P->first; np.n_amp = P->n;
            for (int i = 0; i < P->n; i++) np.amp[i] = P->a[i];
            mc_spec_old(&spec, &np, 0);
            McXoro root = xoro_from_long_seed(seed);
            McXoroPos pf = xoro_fork_positional(&root);
            char full[96]; snprintf(full, sizeof full, "minecraft:%s", nm);
            u64 hl, hh; mc_md5_seed128(full, &hl, &hh);
            McXoro r = xoro_from_hash(&pf, hl, hh);
            normal_init_xoro(&nn, &spec, &r);
        }
        double x, y, z; memcpy(&x, &xb, 8); memcpy(&y, &yb, 8); memcpy(&z, &zb, 8);
        double v = normal_get_d(&nn, &spec, x, y, z);
        unsigned long long got; memcpy(&got, &v, 8);
        total++;
        if (got != vb) { if (bad < 5) { double e; memcpy(&e, &vb, 8); printf("MISMATCH seed=%lld %s: got %.17g exp %.17g\n", seed, nm, v, e); } bad++; }
    }
    printf("test_noise_old: %ld values, %ld mismatches\n", total, bad);
    return bad != 0;
}

// Сравнение реальных климатических параметров/биомов игры (дамп ClimateDump.java)
// с cubiomes. Использование: ./cmp_climate <dump.txt> <mc-enum-name> [large]
//   mc: MC_1_21_3 | MC_1_21_WD | MC_1_20 | MC_1_19 | MC_1_18
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage\n"); return 1; }
    int mc = MC_1_21_WD;
    if (!strcmp(argv[2], "MC_1_21_3")) mc = MC_1_21_3;
    else if (!strcmp(argv[2], "MC_1_20")) mc = MC_1_20;
    else if (!strcmp(argv[2], "MC_1_19")) mc = MC_1_19;
    else if (!strcmp(argv[2], "MC_1_18")) mc = MC_1_18;
    int large = argc > 3 && !strcmp(argv[3], "large");
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("open"); return 1; }
    char line[512];
    Generator g;
    setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);
    int64_t curseed = 0; int have = 0;
    long n = 0, nbio = 0;
    long npdiff[6] = {0}; long npdiff_gt1[6] = {0}; long maxd[6] = {0};
    long unknown_java = 0;
    // счётчик расхождений по парам биомов
    enum { MAXP = 512 };
    static char pj[MAXP][40], pc[MAXP][40]; static long pn[MAXP]; int np_pairs = 0;
    long dat_diff = 0; // расхождения при использовании dat-кэша (как в игре последовательно)
    uint64_t dat = 0;
    while (fgets(line, sizeof line, f))
    {
        if (line[0] == '#') continue;
        long long seed; int qx, qy, qz; long long j[6]; char name[64];
        if (sscanf(line, "%lld %d %d %d %lld %lld %lld %lld %lld %lld %63s", &seed, &qx, &qy, &qz,
                &j[0], &j[1], &j[2], &j[3], &j[4], &j[5], name) != 11) continue;
        if (!have || seed != curseed)
        {
            applySeed(&g, DIM_OVERWORLD, (uint64_t)seed);
            curseed = seed; have = 1; dat = 0;
        }
        int64_t np[6];
        int id = sampleBiomeNoise(&g.bn, np, qx, qy, qz, NULL, 0);
        const char *cn = biome2str(mc, id);
        n++;
        for (int k = 0; k < 6; k++)
        {
            long d = labs((long)(np[k] - j[k]));
            if (d) npdiff[k]++;
            if (d > 1) npdiff_gt1[k]++;
            if (d > maxd[k]) maxd[k] = d;
        }
        if (!cn) cn = "?";
        if (strcmp(cn, name) && getenv("SHOW_OTHER") && strcmp(name, "dappled_forest") && strcmp(name, "sulfur_caves") && strcmp(name, "pale_garden"))
            printf("OTHER seed=%lld q=(%d,%d,%d) game=%s(np %lld %lld %lld %lld %lld %lld) cubiomes=%s(np %lld %lld %lld %lld %lld %lld)\n",
                seed, qx, qy, qz, name, j[0], j[1], j[2], j[3], j[4], j[5], cn,
                (long long)np[0], (long long)np[1], (long long)np[2], (long long)np[3], (long long)np[4], (long long)np[5]);
        if (strcmp(cn, name))
        {
            nbio++;
            int k;
            for (k = 0; k < np_pairs; k++)
                if (!strcmp(pj[k], name) && !strcmp(pc[k], cn)) break;
            if (k == np_pairs && np_pairs < MAXP) { strncpy(pj[k], name, 39); strncpy(pc[k], cn, 39); pn[k] = 0; np_pairs++; }
            if (k < MAXP) pn[k]++;
        }
    }
    printf("points=%ld  biome mismatches=%ld (%.4f%%)\n", n, nbio, 100.0 * nbio / (n ? n : 1));
    const char *nm[] = {"temperature", "humidity", "continentalness", "erosion", "depth", "weirdness"};
    for (int k = 0; k < 6; k++)
        printf("  np[%d] %-16s: differ in %ld (%.3f%%), |diff|>1 in %ld, max|diff|=%ld\n", k, nm[k],
            npdiff[k], 100.0 * npdiff[k] / (n ? n : 1), npdiff_gt1[k], maxd[k]);
    // сортировка пар
    for (int a = 0; a < np_pairs; a++)
        for (int b = a + 1; b < np_pairs; b++)
            if (pn[b] > pn[a]) {
                long t = pn[a]; pn[a] = pn[b]; pn[b] = t;
                char tmp[40]; strcpy(tmp, pj[a]); strcpy(pj[a], pj[b]); strcpy(pj[b], tmp);
                strcpy(tmp, pc[a]); strcpy(pc[a], pc[b]); strcpy(pc[b], tmp);
            }
    for (int k = 0; k < np_pairs && k < 25; k++)
        printf("  game=%-28s cubiomes=%-28s x%ld\n", pj[k], pc[k], pn[k]);
    (void)unknown_java; (void)dat_diff;
    return 0;
}

/* selftest.c — сверка mcrand.h с эталоном из реального кода игры (vectors/gameref-<V>.txt). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../mcrand.h"

typedef struct { const char *name; int spacing, sep, salt, type; } Sp;
static const Sp SETS[] = {
    {"desert_pyramid", 32, 8, 14357617, 0}, {"igloo", 32, 8, 14357618, 0}, {"jungle_pyramid", 32, 8, 14357619, 0},
    {"swamp_hut", 32, 8, 14357620, 0}, {"shipwreck", 24, 4, 165745295, 0}, {"village", 34, 8, 10387312, 0},
    {"trial_chambers", 34, 12, 94251327, 0}, {"ancient_city", 24, 8, 20083232, 0}, {"monument", 32, 5, 10387313, 1},
    {"mansion", 80, 20, 10387319, 1}, {"end_city", 20, 11, 10387313, 1}, {"ruined_portal", 40, 15, 34222645, 0},
    {"nether_complex", 27, 4, 30084232, 0}};

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "vectors/gameref-26.3.txt";
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    char line[1024];
    long ok[8] = {0}, bad[8] = {0};
    const char *cat[] = {"struct", "slime", "pillar", "pillarseed", "bedrock", "stronghold", "mineshaft", "hash"};
    while (fgets(line, sizeof line, f)) {
        char kind[64]; sscanf(line, "%63s", kind);
        int k = -1; for (int i = 0; i < 8; i++) if (!strcmp(kind, cat[i])) k = i;
        if (k < 0) continue;
        int good = 0;
        if (k == 0) {
            char nm[64]; long long seed; int rx, rz, cx, cz;
            sscanf(line, "%*s %63s %lld %d %d %d %d", nm, &seed, &rx, &rz, &cx, &cz);
            const Sp *sp = NULL; for (unsigned i = 0; i < sizeof SETS / sizeof *SETS; i++) if (!strcmp(SETS[i].name, nm)) sp = &SETS[i];
            i32 ox, oz; spread_offset((u64)seed, rx, rz, sp->spacing, sp->sep, sp->salt, sp->type, &ox, &oz);
            good = (rx * sp->spacing + ox == cx) && (rz * sp->spacing + oz == cz);
        } else if (k == 1) {
            long long seed; int cx, cz, v; sscanf(line, "%*s %lld %d %d %d", &seed, &cx, &cz, &v);
            good = is_slime_chunk((u64)seed, cx, cz) == v;
        } else if (k == 2) {
            long long seed; unsigned key; char seq[64]; sscanf(line, "%*s %lld %u %63s", &seed, &key, seq);
            int sz[10]; pillar_sizes(pillar_seed_of((u64)seed), sz);
            char buf[64]; int n = 0; for (int i = 0; i < 10; i++) n += sprintf(buf + n, i ? ",%d" : "%d", sz[i]);
            good = pillar_seed_of((u64)seed) == key && !strcmp(buf, seq);
        } else if (k == 3) {
            unsigned ps; char seq[64]; sscanf(line, "%*s %u %63s", &ps, seq);
            int sz[10]; pillar_sizes(ps, sz);
            char buf[64]; int n = 0; for (int i = 0; i < 10; i++) n += sprintf(buf + n, i ? ",%d" : "%d", sz[i]);
            good = !strcmp(buf, seq);
        } else if (k == 4) {
            long long seed; char nm[64]; int x, y, z; int bits;
            sscanf(line, "%*s %lld %63s %d %d %d %d", &seed, nm, &x, &y, &z, &bits);
            int h = strstr(nm, "roof") ? HASH_BEDROCK_ROOF : HASH_BEDROCK_FLOOR;
            u64 F = nether_bedrock_factory((u64)seed, h);
            float v = bedrock_float(F, x, y, z);
            int mine; memcpy(&mine, &v, 4);
            good = mine == bits;
        } else if (k == 5) {
            long long seed; int p[6]; sscanf(line, "%*s %lld %d %d %d %d %d %d", &seed, p, p+1, p+2, p+3, p+4, p+5);
            u64 s = lcg_scramble((u64)seed);
            double angle = lcg_nextDouble(&s) * M_PI * 2.0; good = 1;
            for (int i = 0; i < 3; i++) {
                double dist = 4 * 32 + (lcg_nextDouble(&s) - 0.5) * (32 * 2.5);
                int ix = (int)floor(cos(angle) * dist + 0.5), iz = (int)floor(sin(angle) * dist + 0.5);
                lcg_nextLong(&s); /* random.fork() */
                if (ix != p[2*i] || iz != p[2*i+1]) good = 0;
                angle += M_PI * 2 / 3;
            }
        } else if (k == 6) {
            long long seed; int cx, cz; long long dbits; int pass; sscanf(line, "%*s %lld %d %d %lld %d", &seed, &cx, &cz, &dbits, &pass);
            good = mineshaft_start((u64)seed, cx, cz) == pass;
        } else if (k == 7) {
            int a, b; sscanf(line, "%*s %d %d", &a, &b); good = a == HASH_BEDROCK_ROOF && b == HASH_BEDROCK_FLOOR;
        }
        if (good) ok[k]++; else { bad[k]++; if (bad[k] <= 3) printf("MISMATCH: %s", line); }
    }
    long tot_bad = 0;
    for (int i = 0; i < 8; i++) { printf("%-11s ok=%ld bad=%ld\n", cat[i], ok[i], bad[i]); tot_bad += bad[i]; }
    printf(tot_bad ? "SELFTEST FAILED\n" : "SELFTEST OK (%s)\n", path);
    return tot_bad != 0;
}

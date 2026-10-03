/* g4_seeds.c — домен seed карверов: маска вырезания зависит от McSeeds.terrain и не зависит от climate/structures/features.
 *   маска(climate=A, terrain=B) == маска(climate=B, terrain=B)  и  != маска(climate=A, terrain=A)
 * Использование: g4_seeds <pack> <version> <dim> <seedA> <seedB> <cx0> <cz0> <n>   (0 — домен верный)
 */
#include "mcgen_internal.h"
#include "carver.h"
#include <stdio.h>
#include <stdlib.h>

static McWorld *mk(McGen *g, const char *dim, int64_t climate, int64_t terrain) {
    McSeeds s = { climate, terrain, climate ^ 0x1234, terrain ^ 0x4321 };
    McWorld *w; char err[256];
    if (mcgen_world_new(g, dim, "normal", &s, NULL, 0, &w, err, sizeof err)) { fprintf(stderr, "%s\n", err); exit(2); }
    return w;
}

int main(int argc, char **argv) {
    if (argc < 9) { fprintf(stderr, "g4_seeds <pack> <version> <dim> <seedA> <seedB> <cx0> <cz0> <n>\n"); return 2; }
    McGen *g; char err[256];
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; }
    int64_t A = strtoll(argv[4], NULL, 10), B = strtoll(argv[5], NULL, 10);
    McWorld *w1 = mk(g, argv[3], A, B), *w2 = mk(g, argv[3], B, B), *w3 = mk(g, argv[3], A, A);
    int cx0 = atoi(argv[6]), cz0 = atoi(argv[7]), n = atoi(argv[8]);
    static uint8_t m1[512 * 256], m2[512 * 256], m3[512 * 256];
    long same12 = 0, tot = 0, set = 0, diff13 = 0; int miny, h;
    for (int cz = cz0; cz < cz0 + n; cz++) for (int cx = cx0; cx < cx0 + n; cx++) {
        int a = carvers_chunk_mask(w1, cx, cz, m1, sizeof m1, &miny, &h);
        int b = carvers_chunk_mask(w2, cx, cz, m2, sizeof m2, &miny, &h);
        int c = carvers_chunk_mask(w3, cx, cz, m3, sizeof m3, &miny, &h);
        if (a < 0 || b < 0 || c < 0) { printf("маска недоступна (%d %d %d)\n", a, b, c); return 2; }
        size_t sz = (size_t)h * 256;
        tot++; set += a;
        if (!memcmp(m1, m2, sz)) same12++;
        if (memcmp(m1, m3, sz)) diff13++;
    }
    printf("%s %s: чанков %ld, отмечено блоков %ld; маска(climate=A, terrain=B) == маска(B, B): %ld/%ld; отличается от маски(A, A): %ld/%ld\n",
           argv[2], argv[3], tot, set, same12, tot, diff13, tot);
    int ok = same12 == tot && (set == 0 || diff13 > 0);
    printf("%s\n", ok ? "домен seed: terrain — OK" : "домен seed: ОШИБКА");
    return ok ? 0 : 1;
}

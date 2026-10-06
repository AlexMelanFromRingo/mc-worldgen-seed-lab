/* Тест JSet против java.util.HashSet: ту же последовательность операций (Random Java) воспроизводит JSetRef.java. Здесь — копия java.util.Random (LCG 48 бит). */
#include "../../src/feature_tree.h"
#include <stdio.h>
static uint64_t seed;
static int next(int bits) { seed = (seed * 0x5DEECE66Dull + 0xBull) & ((1ull << 48) - 1); return (int)(int64_t)(seed >> (48 - bits)); }
static int nextInt(int bound) {
    int r = next(31); int m = bound - 1;
    if ((bound & m) == 0) return (int)(((long long)bound * (long long)r) >> 31);
    for (int u = r; u - (r = u % bound) + m < 0; u = next(31));
    return r;
}
int main(int argc, char **argv) {
    long long sd = atoll(argv[1]); int n = atoi(argv[2]); int mode = argc > 3 ? atoi(argv[3]) : 0;
    seed = ((uint64_t)sd ^ 0x5DEECE66Dull) & ((1ull << 48) - 1);
    JSet s; jset_init(&s);
    for (int i = 0; i < n; i++) {
        int op = nextInt(10);
        if (op < (mode == 0 ? 7 : mode == 1 ? 6 : mode == 3 ? (i % 20 < 13 ? 10 : 0) : mode >= 4 ? 6 : 5 + (i & 1)) || s.size == 0) {
            int x, y, z;
            if (mode == 0) { x = nextInt(40) - 20 + 300; y = nextInt(30) + 60; z = nextInt(40) - 20 - 500; }
            else if (mode == 1) { x = nextInt(12) - 3000; y = nextInt(4) + 70; z = nextInt(12) + 1000; }
            else if (mode == 2) { x = nextInt(16) + 17; y = nextInt(8) - 5; z = nextInt(16) - 3000; }
            else if (mode == 3) { x = nextInt(24) + 5000; y = 70; z = nextInt(24) - 100; }
            else if (mode == 4) { x = nextInt(28) - 777; y = nextInt(2) + 12; z = nextInt(28) + 40; }
            else {
                int c = mode == 5 ? 17 : 5 + 20 * nextInt(3);
                y = 60; z = nextInt(8) + 200;
                int base = ((c - 31 * y - z) % 64 + 64) % 64;
                x = base + 64 * nextInt(8);
            }
            jset_add(&s, x, y, z);
        }
        else { BPos p; jset_pop_first(&s, &p); printf("%d,%d,%d\n", p.x, p.y, p.z); }
    }
    fprintf(stderr, "treeified=%ld\n", s.treeified);
    printf("ITER\n");
    BList l = {0}; jset_to_list(&s, &l);
    for (int i = 0; i < l.n; i++) printf("%d,%d,%d\n", l.a[i].x, l.a[i].y, l.a[i].z);
    return 0;
}

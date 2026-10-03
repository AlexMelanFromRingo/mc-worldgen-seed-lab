/* Проверка feature_veg_hash.c: порядок обхода HashSet<BlockPos>. Читает наборы как HashOrder.java, печатает порядок индексов вставки. */
#include "../src/feature_veg.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    int n;
    while (scanf("%d", &n) == 1) {
        VPos *a = malloc(sizeof(VPos) * (size_t)n); int *idx = malloc(sizeof(int) * (size_t)n);
        for (int i = 0; i < n; i++) { if (scanf("%d %d %d", &a[i].x, &a[i].y, &a[i].z) != 3) return 1; }
        VPos *b = malloc(sizeof(VPos) * (size_t)n); memcpy(b, a, sizeof(VPos) * (size_t)n);
        veg_hashset_order(b, n);
        for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) if (a[j].x == b[i].x && a[j].y == b[i].y && a[j].z == b[i].z) { printf("%d ", j); break; } }
        printf("\n"); free(a); free(b); free(idx);
    }
    return 0;
}

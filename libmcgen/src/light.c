/* light.c — небесный свет полностью освещённого мира (см. light.h). */
#include "light.h"
#include <stdlib.h>
#include <string.h>

#define LR 14                       /* радиус поиска: уровень 15 − стоимость ≥ 1 ⇒ стоимость ≤ 14, шаг ≥ 1 */
#define LD (2 * LR + 1)
#define LN (LD * LD * LD)

typedef struct LightScratch {
    u8 g[LN];                       /* стоимость пути до клетки (255 — не посещена) */
    int *pool_i, *pool_n, pool_cap, pool_used;      /* узлы ведёрок: клетка и ссылка на следующий */
    int head[LR + 1];
    int low[LD * LD];               /* lowestSourceY колонок (INT_MIN — не вычислена) */
} LightScratch;

static _Thread_local LightScratch *g_ls;

static inline int damp_of(const BsTab *bs, int st) { return bs->damp[st]; }

static void push(LightScratch *s, int cost, int idx) {
    if (s->pool_used == s->pool_cap) {
        s->pool_cap = s->pool_cap ? s->pool_cap * 2 : 1 << 15;
        s->pool_i = realloc(s->pool_i, sizeof(int) * (size_t)s->pool_cap); s->pool_n = realloc(s->pool_n, sizeof(int) * (size_t)s->pool_cap);
    }
    int n = s->pool_used++;
    s->pool_i[n] = idx; s->pool_n[n] = s->head[cost]; s->head[cost] = n;
}

static int light_search(const BsTab *bs, LightGetFn get, void *ud, int min_y, int height, int x, int y, int z, int block) {
    int max_y = min_y + height - 1;
    if (y < min_y || y > max_y) return block ? 0 : 15;
    LightScratch *s = g_ls;
    if (!s) { s = g_ls = calloc(1, sizeof *s); }
    int best = 0;
    memset(s->g, 255, sizeof s->g);
    for (int i = 0; i <= LR; i++) s->head[i] = -1;
    for (int i = 0; i < LD * LD; i++) s->low[i] = 0x7fffffff;
    s->pool_used = 0;
    /* индекс клетки в окне [−LR, LR]³ вокруг (x, y, z): ((dy+LR)·LD + (dz+LR))·LD + (dx+LR) */
    int c0 = (LR * LD + LR) * LD + LR;
    s->g[c0] = 0; push(s, 0, c0);
    static const int DX[6] = { 1, -1, 0, 0, 0, 0 }, DY[6] = { 0, 0, 1, -1, 0, 0 }, DZ[6] = { 0, 0, 0, 0, 1, -1 };
    for (int cost = 0; cost <= LR; cost++) {
        while (s->head[cost] >= 0) {
            int n = s->head[cost]; s->head[cost] = s->pool_n[n];
            int idx = s->pool_i[n];
            if (s->g[idx] != cost) continue;                         /* устаревшая запись (нашли путь дешевле) */
            int dx = idx % LD - LR, dz = (idx / LD) % LD - LR, dy = idx / (LD * LD) - LR;
            int px = x + dx, py = y + dy, pz = z + dz;
            int st = get(ud, px, py, pz);
            if (block) {                                              /* блочный свет: источник — блок со свечением e; уровень = e − стоимость, берём максимум */
                int e = BSF_LIGHT(bs->flags[st]);
                if (e - cost > best) best = e - cost;
                if (best >= 15 - cost) return best;                   /* дальше стоимость выше, а свечение ≤ 15: лучше не найти */
            } else {
                /* источник: y ≥ lowestSourceY колонки (самый высокий блок с затуханием ≠ 0, плюс 1; нет такого — вся колонка источник) */
                int *low = &s->low[(dz + LR) * LD + (dx + LR)];
                if (*low == 0x7fffffff) {
                    int l = min_y - 1;
                    for (int yy = max_y; yy >= min_y; yy--) if (damp_of(bs, get(ud, px, yy, pz)) != 0) { l = yy + 1; break; }
                    *low = l;
                }
                if (py >= *low) return 15 - cost;                     /* Дейкстра: первый найденный источник — самый дешёвый путь */
            }
            int op = damp_of(bs, st); if (op < 1) op = 1;             /* стоимость входа в эту клетку при движении источник → клетка */
            int nc = cost + op;
            if (nc > LR) continue;
            for (int d = 0; d < 6; d++) {
                int ex = dx + DX[d], ey = dy + DY[d], ez = dz + DZ[d];
                if (ex < -LR || ex > LR || ey < -LR || ey > LR || ez < -LR || ez > LR) continue;
                if (py + DY[d] < min_y || py + DY[d] > max_y + 1) continue;
                int ni = ((ey + LR) * LD + (ez + LR)) * LD + (ex + LR);
                if (nc < s->g[ni]) { s->g[ni] = (u8)nc; push(s, nc, ni); }
            }
        }
    }
    return block ? best : 0;
}

int light_sky_final(const BsTab *bs, LightGetFn get, void *ud, int min_y, int height, int x, int y, int z) {
    return light_search(bs, get, ud, min_y, height, x, y, z, 0);
}
int light_block_final(const BsTab *bs, LightGetFn get, void *ud, int min_y, int height, int x, int y, int z) {
    return light_search(bs, get, ud, min_y, height, x, y, z, 1);
}
int light_raw_final(const BsTab *bs, LightGetFn get, void *ud, int min_y, int height, int has_sky, int x, int y, int z) {
    int b = light_block_final(bs, get, ud, min_y, height, x, y, z);
    if (!has_sky || b >= 15) return b;
    int s = light_sky_final(bs, get, ud, min_y, height, x, y, z);
    return s > b ? s : b;
}

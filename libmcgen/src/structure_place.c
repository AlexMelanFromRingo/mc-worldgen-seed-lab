/* structure_place.c — ConcentricRingsStructurePlacement (strongholds): позиции колец. Геометрия — от LCG (48 бит seed), итоговый чанк —
 * от карты биомов на y = 0 в окне ±112 блоков (поиск BiomeSource.findBiomeHorizontal с «резервуарной» выборкой).
 * Поиск биомов выполняется лениво только для колец около запрошенного чанка (результат на кольцо не зависит от остальных). */
#include "structure.h"
#include <stdio.h>
#include <stdlib.h>
int world_biome_noise_hist(const McWorld *w, int qx, int qy, int qz, int *last);   /* biome.c */

static void ring_geometry(StructWorld *sw, StructSet *s) {
    int count = s->count, distance = s->distance, spread = s->spread;
    s->nring = count;
    s->ring_x = xcalloc((size_t)(count ? count : 1), sizeof(int)); s->ring_z = xcalloc((size_t)(count ? count : 1), sizeof(int));
    s->ring_seed = xcalloc((size_t)(count ? count : 1), sizeof(i64)); s->ring_done = xcalloc((size_t)(count ? count : 1), 1);
    RS r; rs_seed_lcg(&r, sw->w->seeds.structures);       /* concentricRingsSeed = seed мира */
    const double PI2 = 3.141592653589793 * 2.0;
    double angle = rs_double(&r) * 3.141592653589793 * 2.0;
    int pic = 0, circle = 0;
    for (int i = 0; i < count; i++) {
        double dist = (double)(4 * distance + distance * circle * 6) + (rs_double(&r) - 0.5) * ((double)distance * 2.5);
        s->ring_x[i] = jm_d2i(floor(cos(angle) * dist + 0.5));          /* (int)Math.round(double) */
        s->ring_z[i] = jm_d2i(floor(sin(angle) * dist + 0.5));
        s->ring_seed[i] = rs_long(&r);                                    /* random.fork() = new LegacyRandomSource(nextLong()) */
        angle += PI2 / (double)spread;
        if (++pic == spread) {
            circle++; pic = 0;
            spread += 2 * spread / (circle + 1);
            spread = spread < count - i ? spread : count - i;
            angle += rs_double(&r) * 3.141592653589793 * 2.0;
        }
    }
}

/* findBiomeHorizontal(x = (ix<<4)+8, 0, z = (iz<<4)+8, radius 112, skip 1, preferred, random, findClosest = false) → чанк или исходный */
static void ring_search(StructWorld *sw, StructSet *s, int i) {
    McWorld *w = sw->w;
    int ox = s->ring_x[i] * 16 + 8, oz = s->ring_z[i] * 16 + 8;
    int cqx = ox >> 2, cqz = oz >> 2, rad = 112 >> 2, qy = 0;
    RS r; rs_seed_lcg(&r, s->ring_seed[i]);
    int have = 0, found = 0, rqx = 0, rqz = 0, last = -1;
    for (int z = -rad; z <= rad; z++) for (int x = -rad; x <= rad; x++) {
        int b = world_biome_noise_hist(w, cqx + x, qy, cqz + z, &last);       /* ничьи R-дерева разрешаются в пользу предыдущего результата (lastResult потока игры) */
        if (b >= 0 && s->preferred[b]) {
            if (!have || rs_bound(&r, found + 1) == 0) { rqx = cqx + x; rqz = cqz + z; have = 1; }
            found++;
        }
    }
    if (have) { s->ring_x[i] = (rqx * 4) >> 4; s->ring_z[i] = (rqz * 4) >> 4; }
    s->ring_done[i] = 1;
}

/* вызывается из is_placement_chunk: >0 — позиции доступны для сравнения около (cx,cz) (делает нужные поиски) */
int structures_ring_positions_near(StructWorld *sw, StructSet *s, int cx, int cz) {
    mutex_lock(sw->lock);
    if (!s->ring_init) { ring_geometry(sw, s); s->ring_init = 1; }
    mutex_unlock(sw->lock);
    for (int i = 0; i < s->nring; i++) {
        if (s->ring_done[i]) continue;
        /* поиск смещает позицию не более чем на 7 чанков (±112 блоков) и ещё на 1 из-за округления до квартов */
        int dx = s->ring_x[i] - cx, dz = s->ring_z[i] - cz;
        if (dx < -9 || dx > 9 || dz < -9 || dz > 9) continue;
        ring_search(sw, s, i);       /* дубликат работы потоков безвреден: результат детерминирован */
    }
    return s->nring;
}

/* для тестов: вычислить все кольца набора (дорого: поиск биомов для каждого кольца) */
int structures_ring_force_all(StructWorld *sw, StructSet *s) {
    structures_ring_positions_near(sw, s, 1 << 28, 1 << 28);
    for (int i = 0; i < s->nring; i++) if (!s->ring_done[i]) ring_search(sw, s, i);
    return s->nring;
}

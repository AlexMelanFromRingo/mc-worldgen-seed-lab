/* feature_region.c — «WorldGenRegion» для стадии FEATURES: запись блоков с обновлением карт высот и пометками пост-обработки,
 * карты высот (Heightmap) по правилам версии, биом блока (BiomeManager.getBiome по клеткам чанков окна). */
#include "feature.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== карты высот */
static inline int hm_pred(const BsTab *bs, int type, int st) { return (bs->hmcls[st] >> type) & 1; }

void fchunk_prime_type(const BsTab *bs, FChunk *ch, int type, int min_y, int height) {
    i16 *hm = ch->hm[type];
    u8 done[256]; memset(done, 0, sizeof done);
    for (int i = 0; i < 256; i++) hm[i] = (i16)min_y;
    int left = 256;
    for (int y = height - 1; y >= 0 && left > 0; y--) {
        const u16 *row = ch->blocks + (size_t)y * 256;
        for (int i = 0; i < 256; i++) {
            if (done[i]) continue;
            if (hm_pred(bs, type, row[i])) { hm[i] = (i16)(min_y + y + 1); done[i] = 1; left--; }
        }
    }
    ch->hm_has |= (u8)(1 << type);
}

void fchunk_prime_final(const McGen *g, const BsTab *bs, FChunk *ch, int min_y, int height, int lazy_wg) {
    /* четыре «финальные» карты в один проход; WG-карты 26.1/26.2 тоже готовы (в 26.3 — лениво при первом запросе) */
    int types[6], nt = 0;
    types[nt++] = HM_WORLD_SURFACE; types[nt++] = HM_OCEAN_FLOOR; types[nt++] = HM_MOTION_BLOCKING; types[nt++] = HM_MOTION_BLOCKING_NO_LEAVES;
    if (!lazy_wg) { types[nt++] = HM_WORLD_SURFACE_WG; types[nt++] = HM_OCEAN_FLOOR_WG; }
    u8 done[6][256]; memset(done, 0, sizeof done);
    int left[6];
    for (int k = 0; k < nt; k++) { for (int i = 0; i < 256; i++) ch->hm[types[k]][i] = (i16)min_y; left[k] = 256; }
    int remaining = nt;
    for (int y = height - 1; y >= 0 && remaining > 0; y--) {
        const u16 *row = ch->blocks + (size_t)y * 256;
        for (int k = 0; k < nt; k++) {
            if (!left[k]) continue;
            int t = types[k]; i16 *hm = ch->hm[t];
            for (int i = 0; i < 256; i++) {
                if (done[k][i]) continue;
                if (hm_pred(bs, t, row[i])) { hm[i] = (i16)(min_y + y + 1); done[k][i] = 1; if (--left[k] == 0) remaining--; }
            }
        }
    }
    for (int k = 0; k < nt; k++) ch->hm_has |= (u8)(1 << types[k]);
    (void)g;
}

/* Heightmap.update(localX, y, localZ, state) для «финальных» карт (WG-карты после TERRAIN/CARVERS не обновляются) */
static void hm_update(const FCtx *c, FChunk *ch, int lx, int y, int lz, int st) {
    static const int T[4] = { HM_WORLD_SURFACE, HM_OCEAN_FLOOR, HM_MOTION_BLOCKING, HM_MOTION_BLOCKING_NO_LEAVES };
    const BsTab *bs = c->bs;
    for (int k = 0; k < 4; k++) {
        int t = T[k]; i16 *h = &ch->hm[t][lz * 16 + lx];
        int first = *h;
        if (y <= first - 2) continue;
        if (hm_pred(bs, t, st)) { if (y >= first) *h = (i16)(y + 1); }
        else if (first - 1 == y) {
            int found = 0;
            for (int yy = y - 1; yy >= c->min_y; yy--) {
                int s2 = ch->blocks[((size_t)(yy - c->min_y) * 16 + lz) * 16 + lx];
                if (hm_pred(bs, t, s2)) { *h = (i16)(yy + 1); found = 1; break; }
            }
            if (!found) *h = (i16)c->min_y;
        }
    }
}

int fc_height(FCtx *c, int type, int x, int z) {
    FChunk *ch = fc_chunk(c, x, z);
    if (!ch) return c->min_y;
    if (ch->full) { if (type == HM_OCEAN_FLOOR_WG) type = HM_OCEAN_FLOOR; else if (type == HM_WORLD_SURFACE_WG) type = HM_WORLD_SURFACE; }   /* FULL-чанк: ImposterProtoChunk.fixType */
    if (!(ch->hm_has & (1 << type))) fchunk_prime_type(c->bs, ch, type, c->min_y, c->height);
    return ch->hm[type][(z & 15) * 16 + (x & 15)];
}

/* ====================================================================== запись */
void fc_mark(FCtx *c, int x, int y, int z) {
    FChunk *ch = fc_chunk(c, x, z);
    if (!ch || !ch->marks || fc_outside(c, y)) return;
    ppmarks_add(ch->marks, (y - c->min_y) >> 4, x & 15, y & 15, z & 15);
}
void fc_mark_above(FCtx *c, int x, int y, int z) {
    for (int i = 0; i < 2; i++) {
        y++;
        if (fc_is_air(c, fc_get(c, x, y, z))) return;
        fc_mark(c, x, y, z);
    }
}

_Thread_local int fc_trace_writes;      /* отладка: MCGEN_TREE_TRACE_AT — печать всех записей одного дерева */
int fc_set(FCtx *c, int x, int y, int z, int st, int flags) {
    FChunk *ch = fc_chunk(c, x, z);
    if (fc_trace_writes) fprintf(stderr, "TW %d %d %d %s f=%d%s\n", x, y, z, mcgen_block_state_name(c->g, st), flags, ch ? "" : " (вне окна)");
    if (!ch) return 0;                                  /* ensureCanWrite: далеко от центра */
    if (fc_outside(c, y)) return 1;                      /* ProtoChunk: вне высот — void_air, запись не выполняется */
    u16 *b = &ch->blocks[((size_t)(y - c->min_y) * 16 + (z & 15)) * 16 + (x & 15)];
    { static int tx, ty, tz, tr = -1;           /* отладка: MCGEN_TRACE_POS="x,y,z" — кто и что пишет в эту ячейку (вместе с MCGEN_TRACE_ATT даёт порядок) */
      if (tr < 0) { const char *e = getenv("MCGEN_TRACE_POS"); tr = (e && sscanf(e, "%d,%d,%d", &tx, &ty, &tz) == 3) ? 1 : 0; }
      if (tr && x == tx && y == ty && z == tz) { extern _Thread_local const char *fc_cur_feat; fprintf(stderr, "SET chunk(%d,%d) (%d,%d,%d): %s -> %s flags=%d F=%s\n", c->ccx, c->ccz, x, y, z, mcgen_block_state_name(c->g, *b), mcgen_block_state_name(c->g, st), flags, fc_cur_feat ? fc_cur_feat : "?"); } }
    *b = (u16)st;
    hm_update(c, ch, x & 15, y, z & 15, st);
    if ((flags & 16) == 0) {
        int pp = BSF_PP(c->bs->flags[st]);
        if (pp == 1) fc_mark(c, x, y, z); else if (pp == 2) fc_mark(c, x, y + 1, z);
    }
    return 1;
}
void fc_set_raw(FCtx *c, int x, int y, int z, int st) {
    FChunk *ch = fc_chunk(c, x, z);
    if (fc_trace_writes) fprintf(stderr, "TW %d %d %d %s f=0%s\n", x, y, z, mcgen_block_state_name(c->g, st), ch ? "" : " (вне окна)");
    { static int tx, ty, tz, tr = -1;           /* MCGEN_TRACE_POS: и прямые записи (жилы, диски) */
      if (tr < 0) { const char *e = getenv("MCGEN_TRACE_POS"); tr = (e && sscanf(e, "%d,%d,%d", &tx, &ty, &tz) == 3) ? 1 : 0; }
      if (tr && x == tx && y == ty && z == tz && ch && !fc_outside(c, y)) { extern _Thread_local const char *fc_cur_feat; fprintf(stderr, "SETRAW chunk(%d,%d) (%d,%d,%d): %s -> %s F=%s\n", c->ccx, c->ccz, x, y, z, mcgen_block_state_name(c->g, ch->blocks[((size_t)(y - c->min_y) * 16 + (z & 15)) * 16 + (x & 15)]), mcgen_block_state_name(c->g, st), fc_cur_feat ? fc_cur_feat : "?"); } }
    if (!ch || fc_outside(c, y)) return;
    ch->blocks[((size_t)(y - c->min_y) * 16 + (z & 15)) * 16 + (x & 15)] = (u16)st;
}

/* ====================================================================== биомы: BiomeManager.getBiome */
static inline i64 zoom_lcg(i64 r, i64 cc) { u64 v = (u64)r; v *= v * 6364136223846793005ULL + 1442695040888963407ULL; return (i64)(v + (u64)cc); }
static inline double fiddle(i64 r) { i64 m = (r >> 24) % 1024; if (m < 0) m += 1024; return ((double)m / 1024.0 - 0.5) * 0.9; }
static double fiddled_distance(i64 seed, int x, int y, int z, double dx, double dy, double dz) {
    i64 r = seed;
    r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z); r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z);
    double fx = fiddle(r); r = zoom_lcg(r, seed);
    double fy = fiddle(r); r = zoom_lcg(r, seed);
    double fz = fiddle(r);
    return (dz + fz) * (dz + fz) + (dy + fy) * (dy + fy) + (dx + fx) * (dx + fx);
}

/* 26.4: секции чанка хранят биомы ПОБЛОЧНО (CachedChunkBiomeResolver: BiomeManager-масштабирование по клеткам 4×4×4 + «размытое» расстояние), а набор биомов окна декорации берётся из палитр секций.
 * Поэтому биом клетки соседнего чанка (граница ±1 клетка) может попасть в набор чанка, хотя в клетках самого чанка его нет (на сервере это даёт, например, лишнюю фичу гор).
 * Маска считается по блокам; группы 4×4×4 блоков с одинаковыми 8 клетками-кандидатами пропускаются. */
static int bm_cell(const FCtx *c, const FChunk *own, int qx, int qy, int qz) {
    int cx = qx >> 2, cz = qz >> 2;
    int qmin = c->min_y >> 2, qmax = qmin + (c->height >> 2) - 1;
    int cq = qy < qmin ? qmin : (qy > qmax ? qmax : qy);
    const FChunk *ch = own;
    if (cx != own->cx || cz != own->cz) {
        int gx = cx - c->gx0, gz = cz - c->gz0;
        if (gx >= 0 && gx < c->gnx && gz >= 0 && gz < c->gnz && c->grid[gz * c->gnx + gx]) ch = c->grid[gz * c->gnx + gx];
        else { qx = own->cx * 4 + (qx < own->cx * 4 ? 0 : 3); qz = own->cz * 4 + (qz < own->cz * 4 ? 0 : 3); }   /* вне окна: ближайшая клетка собственного чанка */
    }
    return ch->biomes[(size_t)(cq - qmin) * 16 + (qz & 3) * 4 + (qx & 3)];
}
void fchunk_block_biome_mask(const FCtx *c, FChunk *ch) {
    u8 m[32]; memset(m, 0, sizeof m);
    int qmin = c->min_y >> 2, qmax = qmin + (c->height >> 2) - 1;
    int x0 = ch->cx * 16, z0 = ch->cz * 16, ytop = c->min_y + c->height - 1;
    for (int pz = ch->cz * 4 - 1; pz <= ch->cz * 4 + 3; pz++)
    for (int px = ch->cx * 4 - 1; px <= ch->cx * 4 + 3; px++)
    for (int py = qmin - 1; py <= qmax; py++) {
        int cand[8], uniform = 1;
        for (int i = 0; i < 8; i++) {
            cand[i] = bm_cell(c, ch, (i & 4) ? px + 1 : px, (i & 2) ? py + 1 : py, (i & 1) ? pz + 1 : pz);
            if (cand[i] != cand[0]) uniform = 0;
        }
        if (uniform) { m[cand[0] >> 3] |= (u8)(1 << (cand[0] & 7)); continue; }
        for (int ax = px * 4; ax < px * 4 + 4; ax++) { int x = ax + 2; if (x < x0 || x > x0 + 15) continue;
        for (int az = pz * 4; az < pz * 4 + 4; az++) { int z = az + 2; if (z < z0 || z > z0 + 15) continue;
        for (int ay = py * 4; ay < py * 4 + 4; ay++) { int y = ay + 2; if (y < c->min_y || y > ytop) continue;
            double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
            int mi = 0; double md = INFINITY;
            for (int i = 0; i < 8; i++) {
                int xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
                double d = fiddled_distance(c->w->biome_zoom_seed, xe ? px : px + 1, ye ? py : py + 1, ze ? pz : pz + 1,
                                            xe ? fx : fx - 1.0, ye ? fy : fy - 1.0, ze ? fz : fz - 1.0);
                if (md > d) { mi = i; md = d; }
            }
            int b = cand[mi];              /* кодирование mi как у cand: бит 4 — x+1, 2 — y+1, 1 — z+1 */
            m[b >> 3] |= (u8)(1 << (b & 7));
        } } }
    }
    memcpy(ch->bio_mask, m, sizeof m);
}

int fc_biome_cell(const FCtx *c, int qx, int qy, int qz) {
    int cx = qx >> 2, cz = qz >> 2;
    if (cx < c->ccx - 1 || cx > c->ccx + 1 || cz < c->ccz - 1 || cz > c->ccz + 1) return c->plains;
    FChunk *ch = c->grid[(cz - c->gz0) * c->gnx + (cx - c->gx0)];
    int qmin = c->min_y >> 2, qmax = qmin + (c->height >> 2) - 1;
    int cq = qy < qmin ? qmin : (qy > qmax ? qmax : qy);
    return ch->biomes[(size_t)(cq - qmin) * 16 + (qz & 3) * 4 + (qx & 3)];
}

int fc_biome(const FCtx *c, int x, int y, int z) {
    /* 26.4: ChunkAccess.getBiome(x, y, z) сначала зажимает y в [minY, maxY] и читает поблочную палитру секции; до 26.4 BiomeManager масштабировал по исходному y (клетки зажимались позже) —
     * для y выше мира (жилы изумруда до y = 480) выбор клетки по «размытому» расстоянию различается */
    if (c->g->version >= V26_4) { int top = c->min_y + c->height - 1; if (y > top) y = top; else if (y < c->min_y) y = c->min_y; }
    int ax = x - 2, ay = y - 2, az = z - 2;
    int px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
    int mi = 0; double md = INFINITY;
    for (int i = 0; i < 8; i++) {
        int xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        double d = fiddled_distance(c->w->biome_zoom_seed, xe ? px : px + 1, ye ? py : py + 1, ze ? pz : pz + 1,
                                    xe ? fx : fx - 1.0, ye ? fy : fy - 1.0, ze ? fz : fz - 1.0);
        if (md > d) { mi = i; md = d; }
    }
    return fc_biome_cell(c, (mi & 4) == 0 ? px : px + 1, (mi & 2) == 0 ? py : py + 1, (mi & 1) == 0 ? pz : pz + 1);
}

/* ====================================================================== небесный свет на стадии FEATURES
 * Игра читает свет из ThreadedLevelLightEngine «как есть», без ожидания: getRawBrightness(pos, 0) → SkyLightSectionStorage.getLightValue (видимая копия данных, updating = false).
 *  - Секция света «хранит» данные (SectionType LIGHT_ONLY/LIGHT_AND_DATA), если непустая секция блоков (hasOnlyAir == false) есть у неё самой или у любого из 26 соседей, а чанк-владелец
 *    прошёл INITIALIZE_LIGHT (ThreadedLevelLightEngine.initializeLight → updateSectionStatus). Для только что созданной секции слой данных — нули (createDataLayer: lightOnInSection == false).
 *  - Верх колонки topSections = (самая высокая «хранящая» секция) + 1; клетка выше или в колонке без данных → 15, ниже (в том числе «дыры») → значение слоя ≥ нижнего слоя вышележащей секции = 0.
 *  - Свет начинает распространяться только на шаге LIGHT чанка, а он требует INITIALIZE_LIGHT всех 8 соседей, т.е. и центра региона, который как раз декорируется: внутри окна 3×3
 *    настоящего (ненулевого) света ещё нет (кроме узкой полосы от внешних уже освещённых чанков — не моделируется).
 * Какие чанки «видны» (INITIALIZE_LIGHT уже выполнен и применён световым потоком) — недетерминированно: свет обрабатывается отдельным потоком по тикам. Измерено по JFR: чанк, декорированный на 1 шаг раньше,
 * виден в 12 % случаев, на 2 — 45 %, на 3 — 70 %, на 9 и больше — всегда. Модель по умолчанию — лаг в шагах (MCGEN_FEATURES_INITLAG, по умолчанию 3); воспроизведение JFR задаёт маску точно. */
void fc_snapshot_sections(const FCtx *c, FChunk *ch) {
    int ns = c->height >> 4; if (ns > 64) ns = 64;
    u64 m = 0;
    for (int s = 0; s < ns; s++) {
        const u16 *b = ch->blocks + (size_t)s * 4096;
        for (int i = 0; i < 4096; i++) if (!fc_is_air(c, b[i])) { m |= 1ull << s; break; }
    }
    ch->sec_ne = m; ch->sec_ne_ok = 1;
}

void fc_init_mask(FCtx *c, int cx, int cz) {
    if (c->init_override_set) { c->init_mask = c->init_override; return; }
    FChunk *me = c->grid[(cz - c->gz0) * c->gnx + (cx - c->gx0)];
    u32 m = 0;
    for (int dz = -2; dz <= 2; dz++) for (int dx = -2; dx <= 2; dx++) {
        int gx = cx + dx - c->gx0, gz = cz + dz - c->gz0;
        if (gx < 0 || gx >= c->gnx || gz < 0 || gz >= c->gnz || (dx == 0 && dz == 0)) continue;
        const FChunk *x = c->grid[gz * c->gnx + gx];
        if (x->sec_ne_ok && x->seq != INT_MAX && me->seq != INT_MAX && (long)x->seq <= (long)me->seq - c->init_lag) m |= 1u << ((dz + 2) * 5 + dx + 2);
    }
    c->init_mask = m;
}

int fc_sky_light(const FCtx *c, int x, int y, int z) {
    if (fc_outside(c, y)) return 15;
    int px = x >> 4, pz = z >> 4, sy = (y - c->min_y) >> 4, top = -1;
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
        int nx = px + dx, nz = pz + dz, ddx = nx - c->ccx, ddz = nz - c->ccz;
        if (ddx < -2 || ddx > 2 || ddz < -2 || ddz > 2 || !((c->init_mask >> ((ddz + 2) * 5 + ddx + 2)) & 1)) continue;
        int gx = nx - c->gx0, gz = nz - c->gz0;
        if (gx < 0 || gx >= c->gnx || gz < 0 || gz >= c->gnz) continue;
        u64 ne = c->grid[gz * c->gnx + gx]->sec_ne;
        if (ne) { int t = 63 - __builtin_clzll(ne); if (t > top) top = t; }
    }
    return top >= 0 && sy <= top + 1 ? 0 : 15;
}

/* mc_gpu.cuh — CUDA-порт вычисления биома (Overworld/Nether/End) поверх engine/*.h.
 *
 * Все функции MC_HD (host+device): один и тот же код работает на GPU и в «CPU-режиме» для сверки.
 * БИТ-ТОЧНОСТЬ: компилировать nvcc --fmad=false (не -use_fast_math), хост-часть -ffp-contract=off.
 *
 * Что переиспользуется из engine/ без изменений: Xoroshiro/LCG, md5-хэши (готовые в McNoiseSpec), mc_lerp/smooth,
 * mc_floor_d, mc_wrap_old/new, mc_quantize, константы, формулы сплайна. Переписаны (математика та же):
 *   - R-дерево: узлы сжаты до 32 байт (i16-границы), потомки идут подряд (BFS-перенумерация), поиск итеративный
 *     (точная копия порядка обхода и тай-брейка mc_rt_search_node, candidate=-1 на старте);
 *   - сплайн: итеративный вместо рекурсивного (нет стека вызовов на GPU);
 *   - Overworld: «ПЛИТКА» — таблица перестановок ImprovedNoise (256 байт) строится в разделяемой памяти (раскладка без
 *     конфликтов банков), и ТУТ ЖЕ по ней считаются значения для пачки точек; затем таблица выбрасывается. Нет
 *     13-КБ состояния на поток и случайного доступа в локальную/глобальную память (он упирался в DRAM: ~30x медленнее);
 *   - Nether: LegacyFbm-пропуск октавы (262 вызова LCG) заменён скачком LCG (математически то же);
 *   - End: скачок на 17292 шага; окно перебора клеток островов сужается под маску наблюдения.
 */
#ifndef MC_GPU_CUH
#define MC_GPU_CUH
#include "../mc_biomes.h"
#include "../mc_end.h"

#define MCG_HD MC_HD
#define MCG_INL MC_HD MC_INLINE

/* ------------------------------------------------------------------------------------------------ */
/*  Параметры ядра (указатели — на устройство для GPU, на хост для CPU-режима)                       */
/* ------------------------------------------------------------------------------------------------ */
typedef struct alignas(16) { i16 lo[MC_RT_DIM]; i16 hi[MC_RT_DIM]; i16 first; u8 count; u8 biome; } McgNode;   /* 32 байта */

#define MCG_NE_INST 16          /* Nether: 2 шума * 2 стека * <=4 уровня */
#define MCG_MAX_OBS 64
#define MCG_MAX_BIOMES 128

typedef struct {
    int version, dim, mode;
    const McClimateSpec *clim;     /* Overworld */
    const McNoiseSpec *sp;         /* Overworld: [6] offset,temp,veg,cont,eros,ridge; Nether: [2] temp, veg */
    const McgNode *tree;           /* корень = узел 0 */
    int noff[8];                   /* Nether: noff[0]=0, noff[1]=2*n_levels(temp); Overworld: начало i-го шума (справочно) */
    u64 jm262, ja262;              /* LCG: скачок на 262 вызова */
    u64 jm_end, ja_end;            /* LCG: скачок на 17292 вызовов (End) */
    int end_ids[5];                /* id биомов End: the_end, highlands, midlands, small_end_islands, barrens */
    int ties;                      /* 1: наблюдение считается выполненным, если ЛЮБОЙ лист R-дерева с минимальным fitness даёт биом из маски */
} McgParams;

/* Наблюдение: точка (quart) + допустимое множество биомов (битовая маска) */
typedef struct { i32 qx, qy, qz, rad; u64 mask[2]; } McgObs;   /* 32 байта; rad — радиус окна клеток End (0..12) */

MCG_INL int mcg_in_mask(const McgObs *o, int biome) { return (int)((o->mask[biome >> 6] >> (biome & 63)) & 1ULL); }

/* Флаги в McgObs.rad: младшие 8 бит — радиус окна End; MCG_OBS_BLOCK — координаты наблюдения БЛОКОВЫЕ (как в F3):
 * биом тогда берётся так, как его видит игра — BiomeManager.getBiome(x,y,z) c «размытием» по hashed seed = SHA-256(seed). */
#define MCG_OBS_BLOCK 0x100
MCG_INL int mcg_obs_rad(const McgObs *o) { return o->rad & 0xFF; }

/* ---- BiomeManager.obfuscateSeed: Hashing.sha256().hashLong(seed).asLong() (8 байт LE -> SHA-256 -> первые 8 байт как LE long) ---- */
MCG_INL u32 mcg_rotr32(u32 x, int k) { return (x >> k) | (x << (32 - k)); }
MCG_INL i64 mcg_hashed_seed(i64 seed) {
    static const u32 K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    u32 W[64];
    u32 lo = (u32)((u64)seed), hi = (u32)((u64)seed >> 32);
    /* байты сообщения (LE long): b0..b7; слова — big-endian */
    W[0] = ((lo & 0xFF) << 24) | (((lo >> 8) & 0xFF) << 16) | (((lo >> 16) & 0xFF) << 8) | (lo >> 24);
    W[1] = ((hi & 0xFF) << 24) | (((hi >> 8) & 0xFF) << 16) | (((hi >> 16) & 0xFF) << 8) | (hi >> 24);
    W[2] = 0x80000000u;
    for (int i = 3; i < 15; i++) W[i] = 0;
    W[15] = 64;
    for (int i = 16; i < 64; i++) {
        u32 s0 = mcg_rotr32(W[i - 15], 7) ^ mcg_rotr32(W[i - 15], 18) ^ (W[i - 15] >> 3);
        u32 s1 = mcg_rotr32(W[i - 2], 17) ^ mcg_rotr32(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }
    u32 a = 0x6a09e667, b = 0xbb67ae85, c = 0x3c6ef372, d = 0xa54ff53a, e = 0x510e527f, f = 0x9b05688c, g = 0x1f83d9ab, h = 0x5be0cd19;
    for (int i = 0; i < 64; i++) {
        u32 S1 = mcg_rotr32(e, 6) ^ mcg_rotr32(e, 11) ^ mcg_rotr32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K[i] + W[i];
        u32 S0 = mcg_rotr32(a, 2) ^ mcg_rotr32(a, 13) ^ mcg_rotr32(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    u32 H0 = 0x6a09e667 + a, H1 = 0xbb67ae85 + b;       /* первые 8 байт дайджеста = H0,H1 big-endian */
    /* байты: H0>>24, H0>>16, H0>>8, H0, H1>>24, ... ; asLong() — little-endian */
    u64 v = 0;
    v |= (u64)(H0 >> 24); v |= (u64)((H0 >> 16) & 0xFF) << 8; v |= (u64)((H0 >> 8) & 0xFF) << 16; v |= (u64)(H0 & 0xFF) << 24;
    v |= (u64)(H1 >> 24) << 32; v |= (u64)((H1 >> 16) & 0xFF) << 40; v |= (u64)((H1 >> 8) & 0xFF) << 48; v |= (u64)(H1 & 0xFF) << 56;
    return (i64)v;
}

/* ---- BiomeManager.getBiome: блок (x,y,z) -> квартовая ячейка шума (biomeX,Y,Z). hs = hashed seed ---- */
MCG_INL i64 mcg_lcg_next(i64 seed, i64 inc) { return (i64)((u64)seed * ((u64)seed * 6364136223846793005ULL + 1442695040888963407ULL) + (u64)inc); }
MCG_INL double mcg_fiddle(i64 rval) { double u = (double)((rval >> 24) & 1023) / 1024.0; return (u - 0.5) * 0.9; }
MCG_INL void mcg_zoom(i64 hs, i32 x, i32 y, i32 z, i32 *oqx, i32 *oqy, i32 *oqz) {
    i32 ax = x - 2, ay = y - 2, az = z - 2;
    i32 px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
    int minI = 0; double minD = 1.0 / 0.0;
    for (int i = 0; i < 8; i++) {
        bool xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        i32 cx = xe ? px : px + 1, cy = ye ? py : py + 1, cz = ze ? pz : pz + 1;
        double dx = xe ? fx : fx - 1.0, dy = ye ? fy : fy - 1.0, dz = ze ? fz : fz - 1.0;
        i64 rv = hs;
        rv = mcg_lcg_next(rv, cx); rv = mcg_lcg_next(rv, cy); rv = mcg_lcg_next(rv, cz);
        rv = mcg_lcg_next(rv, cx); rv = mcg_lcg_next(rv, cy); rv = mcg_lcg_next(rv, cz);
        double fdx = mcg_fiddle(rv);
        rv = mcg_lcg_next(rv, hs); double fdy = mcg_fiddle(rv);
        rv = mcg_lcg_next(rv, hs); double fdz = mcg_fiddle(rv);
        double ddz = dz + fdz, ddy = dy + fdy, ddx = dx + fdx;
        double d = ddz * ddz + ddy * ddy + ddx * ddx;
        if (minD > d) { minI = i; minD = d; }
    }
    *oqx = (minI & 4) == 0 ? px : px + 1; *oqy = (minI & 2) == 0 ? py : py + 1; *oqz = (minI & 1) == 0 ? pz : pz + 1;
}
/* координаты наблюдения в кварты: как есть или через BiomeManager (нужен hashed seed — считается лениво один раз на seed) */
MCG_INL void mcg_obs_quart(const McgObs *o, i64 seed, i64 *hs, bool *have_hs, i32 q[3]) {
    if (!(o->rad & MCG_OBS_BLOCK)) { q[0] = o->qx; q[1] = o->qy; q[2] = o->qz; return; }
    if (!*have_hs) { *hs = mcg_hashed_seed(seed); *have_hs = true; }
    mcg_zoom(*hs, o->qx, o->qy, o->qz, &q[0], &q[1], &q[2]);
}

/* ------------------------------------------------------------------------------------------------ */
/*  R-дерево (итеративный поиск)                                                                    */
/* ------------------------------------------------------------------------------------------------ */
MCG_INL u64 mcg_node_dist(const McgNode *n, const i32 *tg) {
    u64 d = 0;
#pragma unroll
    for (int i = 0; i < MC_RT_DIM; i++) {
        i32 above = tg[i] - (i32)n->hi[i], below = (i32)n->lo[i] - tg[i];
        u32 x = (u32)(above > 0 ? above : (below > 0 ? below : 0));
        d += (u64)x * (u64)x;
    }
    return d;
}

/* Точная копия порядка обхода mc_rt_search_node (candidate=-1). tg[7] = {t,h,c,e,d,w,0}. Возврат — id биома. */
MCG_INL int mcg_rt_find(const McgNode *N, const i32 *tg) {
    int fn[12], fc[12], fcl[12]; u64 fm[12];
    int sp = 0;
    fn[0] = 0; fc[0] = 0; fm[0] = 0x7fffffffffffffffULL; fcl[0] = -1;
    for (;;) {
        const McgNode *n = &N[fn[sp]];
        if (fc[sp] < (int)n->count) {
            int ci = (int)n->first + fc[sp];
            const McgNode *cn = &N[ci];
            u64 cd = mcg_node_dist(cn, tg);
            if (fm[sp] > cd) {
                if (cn->count == 0) { fm[sp] = cd; fcl[sp] = ci; fc[sp]++; }      /* лист: search(ci) вернёт ci, ld == cd */
                else { sp++; fn[sp] = ci; fc[sp] = 0; fm[sp] = fm[sp - 1]; fcl[sp] = fcl[sp - 1]; }
            } else fc[sp]++;
        } else {
            int res = fcl[sp];
            if (sp == 0) return (int)N[res].biome;
            sp--;
            u64 ld = mcg_node_dist(&N[res], tg);
            if (fm[sp] > ld) { fm[sp] = ld; fcl[sp] = res; }
            fc[sp]++;
        }
    }
}

/* Все листья с МИНИМАЛЬНЫМ fitness (тай в игре недетерминирован: Climate.RTree.search стартует с lastResult потока) -> маска биомов.
 * Обход по ветвлению-и-границам: узел исследуется, если его нижняя оценка <= текущего минимума (равные — тоже). */
MCG_INL void mcg_rt_ties(const McgNode *N, const i32 *tg, u64 out[2]) {
    int fn[12], fc[12]; int sp = 0;
    u64 best = 0xffffffffffffffffULL, m0 = 0, m1 = 0;
    fn[0] = 0; fc[0] = 0;
    for (;;) {
        const McgNode *n = &N[fn[sp]];
        if (fc[sp] < (int)n->count) {
            int ci = (int)n->first + fc[sp]++;
            const McgNode *cn = &N[ci];
            u64 cd = mcg_node_dist(cn, tg);
            if (cd > best) continue;
            if (cn->count == 0) {
                if (cd < best) { best = cd; m0 = 0; m1 = 0; }
                int b = (int)cn->biome;
                if (b < 64) m0 |= 1ULL << b; else m1 |= 1ULL << (b - 64);
            } else { sp++; fn[sp] = ci; fc[sp] = 0; }
        } else { if (sp == 0) break; sp--; }
    }
    out[0] = m0; out[1] = m1;
}
/* выполнено ли наблюдение: биом b (результат обычного поиска) в маске, либо (режим ties) в маске любой из равных по fitness биомов */
MCG_INL bool mcg_obs_accept(const McgParams &P, const McgObs *o, const i32 *tg, int b) {
    if (mcg_in_mask(o, b)) return true;
    if (!P.ties) return false;
    u64 m[2]; mcg_rt_ties(P.tree, tg, m);
    return ((m[0] & o->mask[0]) | (m[1] & o->mask[1])) != 0;
}

/* ------------------------------------------------------------------------------------------------ */
/*  Кубический сплайн (итеративно; формулы — как mc_spline_eval)                                     */
/* ------------------------------------------------------------------------------------------------ */
MCG_INL float mcg_spline_eval(const McSpline *S, int root, const float *coords) {
    int fnode[10], fstart[10]; float fy1[10]; u8 fst[10];
    int sp = 0; float ret = 0.0f;
    fnode[0] = root; fst[0] = 0;
    for (;;) {
        int node = fnode[sp];
        if (fst[sp] == 0) {
            if (S->kind[node] == 0) { ret = S->val[node]; if (sp == 0) return ret; sp--; continue; }
            int off = S->off[node], n = S->npts[node];
            float input = coords[S->coord[node]];
            int from = 0, len = n;
            while (len > 0) {
                int half = len / 2, mid = from + half;
                if (input < S->loc[off + mid]) len = half; else { from = mid + 1; len -= half + 1; }
            }
            int start = from - 1, last = n - 1;
            fstart[sp] = start;
            if (start < 0)          { fst[sp] = 1; fnode[sp + 1] = S->child[off]; }
            else if (start == last) { fst[sp] = 2; fnode[sp + 1] = S->child[off + last]; }
            else                    { fst[sp] = 3; fnode[sp + 1] = S->child[off + start]; }
            sp++; fst[sp] = 0;
        } else {
            int off = S->off[node], n = S->npts[node], start = fstart[sp];
            float input = coords[S->coord[node]];
            if (fst[sp] == 1) { ret = mc_spline_linext(input, S, off, ret, 0); if (sp == 0) return ret; sp--; }
            else if (fst[sp] == 2) { ret = mc_spline_linext(input, S, off, ret, n - 1); if (sp == 0) return ret; sp--; }
            else if (fst[sp] == 3) { fy1[sp] = ret; fst[sp] = 4; fnode[sp + 1] = S->child[off + start + 1]; sp++; fst[sp] = 0; }
            else {
                float y1 = fy1[sp], y2 = ret;
                float x1 = S->loc[off + start], x2 = S->loc[off + start + 1];
                float t = (input - x1) / (x2 - x1);
                float d1 = S->der[off + start], d2 = S->der[off + start + 1];
                float a = d1 * (x2 - x1) - (y2 - y1);
                float b = -d2 * (x2 - x1) + (y2 - y1);
                ret = mc_lerp_ff(t, y1, y2) + t * (1.0f - t) * mc_lerp_ff(t, a, b);
                if (sp == 0) return ret; sp--;
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/*  «Плитка»: таблица перестановок (256 байт) на нить, в разделяемой памяти (GPU) или в массиве (CPU) */
/*  Раскладка GPU: байт i нити lane варпа w -> smem + w*8192 + (i>>2)*128 + lane*4 + (i&3): слово      */
/*  номер (i>>2) нити lane лежит в банке lane => любой доступ варпа без конфликтов банков.            */
/*  CPU: смещение = i.                                                                                */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { u8 *t; } McgTile;
#define MCG_TILE_WORDS 64
#define MCG_TILE_BYTES_PER_WARP 8192
#ifdef __CUDA_ARCH__
#define MCG_TSW 128
#else
#define MCG_TSW 4
#endif
MCG_INL int mcg_toff(int i) { return ((i & 0xFC) * (MCG_TSW / 4)) + (i & 3); }       /* i любой: берутся младшие 8 бит */
MCG_INL int mcg_pt(const McgTile &T, int i) { return (int)T.t[mcg_toff(i)]; }        /* ImprovedNoise.p(i) = perm[i & 255] */

MCG_INL void mcg_tile_identity(const McgTile &T) {
    for (int w = 0; w < MCG_TILE_WORDS; w++)
        *(u32 *)(T.t + w * MCG_TSW) = (u32)(4 * w) | ((u32)(4 * w + 1) << 8) | ((u32)(4 * w + 2) << 16) | ((u32)(4 * w + 3) << 24);
}
MCG_INL void mcg_tile_swap(const McgTile &T, int i, int j) {
    int oi = mcg_toff(i), oj = mcg_toff(j);
    u8 a = T.t[oi], b = T.t[oj];
    T.t[oi] = b; T.t[oj] = a;
}
/* то же, что improved_init_xoro (mc_noise.h): сначала xo,yo,zo (3 nextDouble * scale), затем Фишер–Йетс по xoro_next_int_bound */
MCG_INL void mcg_tile_gen_xoro(const McgTile &T, McXoro *r, double scale, double *xo, double *yo, double *zo) {
    *xo = xoro_next_double(r) * scale; *yo = xoro_next_double(r) * scale; *zo = xoro_next_double(r) * scale;
    mcg_tile_identity(T);
    for (int i = 0; i < 256; i++) { int off = xoro_next_int_bound(r, 256 - i); mcg_tile_swap(T, i, i + off); }
}
MCG_INL void mcg_tile_gen_lcg(const McgTile &T, McLcg *r, double scale, double *xo, double *yo, double *zo) {
    *xo = lcg_next_double(r) * scale; *yo = lcg_next_double(r) * scale; *zo = lcg_next_double(r) * scale;
    mcg_tile_identity(T);
    for (int i = 0; i < 256; i++) { int off = lcg_next_int_bound(r, 256 - i); mcg_tile_swap(T, i, i + off); }
}
/* перенести таблицу плитки в McImproved.p (для Nether, где таблицы живут в локальной памяти) */
MCG_INL void mcg_tile_store(const McgTile &T, McImproved *n) {
    u32 *d = (u32 *)n->p;
    for (int w = 0; w < MCG_TILE_WORDS; w++) d[w] = *(const u32 *)(T.t + w * MCG_TSW);
}

/* ---- градиенты без таблицы в памяти: 2-битные коды {0,1,-1} -> {0,1,3}; таблица mc_grad (12 строк + 4 повтора) ---- */
MCG_INL int mcg_gc(u32 C, int h) { int g = (int)((C >> (2 * h)) & 3u); return (g << 30) >> 30; }
#define MCG_GX 0x3100DDDDU
#define MCG_GY 0xDDDD00F5U
#define MCG_GZ 0xC4F5F500U
MCG_INL double mcg_graddot_d(int h, double x, double y, double z) {
    h &= 15; int gx = mcg_gc(MCG_GX, h), gy = mcg_gc(MCG_GY, h), gz = mcg_gc(MCG_GZ, h);
    return gx * x + gy * y + gz * z;
}
MCG_INL float mcg_graddot_f(int h, float x, float y, float z) {
    h &= 15; int gx = mcg_gc(MCG_GX, h), gy = mcg_gc(MCG_GY, h), gz = mcg_gc(MCG_GZ, h);
    return (float)gx * x + (float)gy * y + (float)gz * z;
}

/* ---- ImprovedNoise.noise (DOUBLE, 26.1/26.2) по плитке: копия improved_noise_d ---- */
MCG_INL double mcg_noise_d(const McgTile &T, double xo, double yo, double zo, double _x, double _y, double _z) {
    double x = _x + xo, y = _y + yo, z = _z + zo;
    i32 xf = mc_floor_d(x), yf = mc_floor_d(y), zf = mc_floor_d(z);
    double xr = x - xf, yr = y - yf, zr = z - zf;
    int x0 = mcg_pt(T, xf), x1 = mcg_pt(T, xf + 1);
    int xy00 = mcg_pt(T, x0 + yf), xy01 = mcg_pt(T, x0 + yf + 1);
    int xy10 = mcg_pt(T, x1 + yf), xy11 = mcg_pt(T, x1 + yf + 1);
    double d000 = mcg_graddot_d(mcg_pt(T, xy00 + zf),     xr,       yr,       zr);
    double d100 = mcg_graddot_d(mcg_pt(T, xy10 + zf),     xr - 1.0, yr,       zr);
    double d010 = mcg_graddot_d(mcg_pt(T, xy01 + zf),     xr,       yr - 1.0, zr);
    double d110 = mcg_graddot_d(mcg_pt(T, xy11 + zf),     xr - 1.0, yr - 1.0, zr);
    double d001 = mcg_graddot_d(mcg_pt(T, xy00 + zf + 1), xr,       yr,       zr - 1.0);
    double d101 = mcg_graddot_d(mcg_pt(T, xy10 + zf + 1), xr - 1.0, yr,       zr - 1.0);
    double d011 = mcg_graddot_d(mcg_pt(T, xy01 + zf + 1), xr,       yr - 1.0, zr - 1.0);
    double d111 = mcg_graddot_d(mcg_pt(T, xy11 + zf + 1), xr - 1.0, yr - 1.0, zr - 1.0);
    double xa = mc_smooth_d(xr), ya = mc_smooth_d(yr), za = mc_smooth_d(zr);
    return mc_lerp_d(za, mc_lerp_d(ya, mc_lerp_d(xa, d000, d100), mc_lerp_d(xa, d010, d110)),
                         mc_lerp_d(ya, mc_lerp_d(xa, d001, d101), mc_lerp_d(xa, d011, d111)));
}
/* ---- GradientNoise (FLOAT, 26.3) по плитке: копия improved_noise_f ---- */
MCG_INL float mcg_noise_f(const McgTile &T, double xo, double yo, double zo, double _x, double _y, double _z) {
    double x = mc_wrap_new(_x) + xo;
    double y = mc_wrap_new(_y) + yo;
    double z = mc_wrap_new(_z) + zo;
    i32 xf = mc_floor_d(x), yf = mc_floor_d(y), zf = mc_floor_d(z);
    float xr = (float)(x - xf), yr = (float)(y - yf), zr = (float)(z - zf);
    int x0 = mcg_pt(T, xf), x1 = mcg_pt(T, xf + 1);
    int xy00 = mcg_pt(T, x0 + yf), xy01 = mcg_pt(T, x0 + yf + 1);
    int xy10 = mcg_pt(T, x1 + yf), xy11 = mcg_pt(T, x1 + yf + 1);
    float d000 = mcg_graddot_f(mcg_pt(T, xy00 + zf),     xr,        yr,        zr);
    float d100 = mcg_graddot_f(mcg_pt(T, xy10 + zf),     xr - 1.0f, yr,        zr);
    float d010 = mcg_graddot_f(mcg_pt(T, xy01 + zf),     xr,        yr - 1.0f, zr);
    float d110 = mcg_graddot_f(mcg_pt(T, xy11 + zf),     xr - 1.0f, yr - 1.0f, zr);
    float d001 = mcg_graddot_f(mcg_pt(T, xy00 + zf + 1), xr,        yr,        zr - 1.0f);
    float d101 = mcg_graddot_f(mcg_pt(T, xy10 + zf + 1), xr - 1.0f, yr,        zr - 1.0f);
    float d011 = mcg_graddot_f(mcg_pt(T, xy01 + zf + 1), xr,        yr - 1.0f, zr - 1.0f);
    float d111 = mcg_graddot_f(mcg_pt(T, xy11 + zf + 1), xr - 1.0f, yr - 1.0f, zr - 1.0f);
    float xa = mc_smooth_f(xr), ya = mc_smooth_f(yr), za = mc_smooth_f(zr);
    return mc_lerp_f(za, mc_lerp_f(ya, mc_lerp_f(xa, d000, d100), mc_lerp_f(xa, d010, d110)),
                         mc_lerp_f(ya, mc_lerp_f(xa, d001, d101), mc_lerp_f(xa, d011, d111)));
}

/* ------------------------------------------------------------------------------------------------ */
/*  Overworld: ПАЧКА точек за один проход. Для каждого из 6 шумов таблицы октав строятся по одной     */
/*  (в разделяемой плитке) и сразу применяются ко всем точкам пачки; порядок суммирования по точке    */
/*  в точности как в normal_get_d / normal_get_f.                                                     */
/* ------------------------------------------------------------------------------------------------ */
template<int MODE> struct McgNV { typedef double T; };
template<> struct McgNV<MC_NOISE_FLOAT> { typedef float T; };

/* NormalNoise.getValue для n точек (X,Y,Z — double). pf — positional-фабрика корня; шум = pf.fromHashOf("minecraft:<name>") */
template<int MODE, int NP>
MCG_INL void mcg_normal_batch(const McgTile &T, const McNoiseSpec *s, u64 hl, u64 hh, const McXoroPos &pf, int n,
                              const double *X, const double *Y, const double *Z, typename McgNV<MODE>::T *out) {
    McXoro r = xoro_from_hash(&pf, hl, hh);
    McXoroPos pos1 = xoro_fork_positional(&r);
    McXoroPos pos2 = xoro_fork_positional(&r);
    int nl = s->n_levels;
    if (MODE == MC_NOISE_DOUBLE) {
        double v1[NP], v2[NP];
        for (int p = 0; p < n; p++) { v1[p] = 0.0; v2[p] = 0.0; }
        for (int k = 0; k < nl; k++) {
            McXoro rk = xoro_from_hash(&pos1, s->hash_lo[k], s->hash_hi[k]);
            double xo, yo, zo; mcg_tile_gen_xoro(T, &rk, 256.0, &xo, &yo, &zo);
            double f = s->freq[k], amp = s->amp_d[k], vf = s->vf_d[k];
            for (int p = 0; p < n; p++) {
                double a = mcg_noise_d(T, xo, yo, zo, mc_wrap_old(X[p] * f), mc_wrap_old(Y[p] * f), mc_wrap_old(Z[p] * f));
                v1[p] += amp * a * vf;
            }
        }
        for (int k = 0; k < nl; k++) {
            McXoro rk = xoro_from_hash(&pos2, s->hash_lo[k], s->hash_hi[k]);
            double xo, yo, zo; mcg_tile_gen_xoro(T, &rk, 256.0, &xo, &yo, &zo);
            double f = s->freq[k], amp = s->amp_d[k], vf = s->vf_d[k];
            for (int p = 0; p < n; p++) {
                double x2 = X[p] * 1.0181268882175227, y2 = Y[p] * 1.0181268882175227, z2 = Z[p] * 1.0181268882175227;
                double a = mcg_noise_d(T, xo, yo, zo, mc_wrap_old(x2 * f), mc_wrap_old(y2 * f), mc_wrap_old(z2 * f));
                v2[p] += amp * a * vf;
            }
        }
        for (int p = 0; p < n; p++) out[p] = (v1[p] + v2[p]) * s->value_factor;
    } else {
        float v[NP];
        for (int p = 0; p < n; p++) v[p] = 0.0f;
        for (int k = 0; k < nl; k++) {
            double f = s->freq[k]; float amp = s->amp_f[k];
            { McXoro rk = xoro_from_hash(&pos1, s->hash_lo[k], s->hash_hi[k]);
              double xo, yo, zo; mcg_tile_gen_xoro(T, &rk, 256.0, &xo, &yo, &zo);
              for (int p = 0; p < n; p++) v[p] += amp * mcg_noise_f(T, xo, yo, zo, X[p] * f, Y[p] * f, Z[p] * f); }
            double f2 = f * 1.0181268882175227;
            { McXoro rk = xoro_from_hash(&pos2, s->hash_lo[k], s->hash_hi[k]);
              double xo, yo, zo; mcg_tile_gen_xoro(T, &rk, 256.0, &xo, &yo, &zo);
              for (int p = 0; p < n; p++) v[p] += amp * mcg_noise_f(T, xo, yo, zo, X[p] * f2, Y[p] * f2, Z[p] * f2); }
        }
        for (int p = 0; p < n; p++) out[p] = v[p];
    }
}

/* Цели {t,h,c,e,d,w,0} для m<=KB точек (q: m троек quart). Копия mc_climate_overworld_raw + mc_target_from_raw. */
template<int MODE, int KB>
MCG_INL void mcg_ow_targets(const McgParams &P, i64 seed, const McgTile &T, const i32 *q, int m, i32 (*tg)[MC_RT_DIM]) {
    typedef typename McgNV<MODE>::T NV;
    const McClimateSpec *S = P.clim; const McNoiseSpec *sp = P.sp;
    McXoro root = xoro_from_long_seed(seed);
    McXoroPos pf = xoro_fork_positional(&root);
    int bx[KB], by[KB], bz[KB];
    double OX[2 * KB], OY[2 * KB], OZ[2 * KB];
    for (int p = 0; p < m; p++) {
        bx[p] = q[3 * p] * 4; by[p] = q[3 * p + 1] * 4; bz[p] = q[3 * p + 2] * 4;
        OX[2 * p] = bx[p] * 0.25;     OY[2 * p] = 0.0 * 0.25;       OZ[2 * p] = bz[p] * 0.25;        /* ShiftA: (x, 0, z) */
        OX[2 * p + 1] = bz[p] * 0.25; OY[2 * p + 1] = bx[p] * 0.25; OZ[2 * p + 1] = 0.0 * 0.25;      /* ShiftB: (z, x, 0) */
    }
    NV sh[2 * KB];
    mcg_normal_batch<MODE, 2 * KB>(T, &sp[0], S->hl[0], S->hh[0], pf, 2 * m, OX, OY, OZ, sh);
    double X[KB], Y[KB], Z[KB];
    for (int p = 0; p < m; p++) {
        if (MODE == MC_NOISE_DOUBLE) {
            double sx = (double)sh[2 * p] * 4.0, sz = (double)sh[2 * p + 1] * 4.0;
            X[p] = bx[p] * 0.25 + sx; Z[p] = bz[p] * 0.25 + sz; Y[p] = (double)by[p] * 0.0 + 0.0;
        } else {
            float sx = (float)sh[2 * p] * 4.0f, sz = (float)sh[2 * p + 1] * 4.0f;
            X[p] = bx[p] * 0.25 + (double)sx; Z[p] = bz[p] * 0.25 + (double)sz; Y[p] = (double)by[p] * 0.0;
        }
    }
    NV temp[KB], veg[KB], cont[KB], eros[KB], rdg[KB];
    mcg_normal_batch<MODE, KB>(T, &sp[1], S->hl[1], S->hh[1], pf, m, X, Y, Z, temp);
    mcg_normal_batch<MODE, KB>(T, &sp[2], S->hl[2], S->hh[2], pf, m, X, Y, Z, veg);
    mcg_normal_batch<MODE, KB>(T, &sp[3], S->hl[3], S->hh[3], pf, m, X, Y, Z, cont);
    mcg_normal_batch<MODE, KB>(T, &sp[4], S->hl[4], S->hh[4], pf, m, X, Y, Z, eros);
    mcg_normal_batch<MODE, KB>(T, &sp[5], S->hl[5], S->hh[5], pf, m, X, Y, Z, rdg);
    for (int p = 0; p < m; p++) {
        float raw[6];
        if (MODE == MC_NOISE_DOUBLE) {
            double rf = (fabs(fabs((double)rdg[p]) + S->rf_c1_d) + S->rf_c2_d) * S->rf_c3_d;
            float coords[4] = {(float)cont[p], (float)eros[p], (float)rdg[p], (float)rf};
            float spl = mcg_spline_eval(&S->offset, S->offset.root, coords);
            double offset = 0.0 * (1.0 + -1.0 * 1.0) + (S->off_const_d + (double)spl) * 1.0;
            double fac = ((double)by[p] - (double)S->grad_from_y) / ((double)S->grad_to_y - (double)S->grad_from_y);
            double grad = fac < 0.0 ? S->grad_from_d : (fac > 1.0 ? S->grad_to_d : S->grad_from_d + fac * (S->grad_to_d - S->grad_from_d));
            double depth = grad + offset;
            raw[0] = (float)temp[p]; raw[1] = (float)veg[p]; raw[2] = (float)cont[p]; raw[3] = (float)eros[p]; raw[4] = (float)depth; raw[5] = (float)rdg[p];
        } else {
            float a = fabsf((float)rdg[p]) + S->rf_c1_f;
            float rf = (fabsf(a) + S->rf_c2_f) * S->rf_c3_f;
            float coords[4] = {(float)cont[p], (float)eros[p], (float)rdg[p], rf};
            float spl = mcg_spline_eval(&S->offset, S->offset.root, coords);
            float offset = S->off_const_f + spl;
            int lo = S->grad_from_y < S->grad_to_y ? S->grad_from_y : S->grad_to_y;
            int hi = S->grad_from_y < S->grad_to_y ? S->grad_to_y : S->grad_from_y;
            int yy = by[p] < lo ? lo : (by[p] > hi ? hi : by[p]);
            int range = S->grad_to_y - S->grad_from_y;
            float factor = (S->grad_to_f - S->grad_from_f) / (float)range;
            float grad = S->grad_from_f + (float)(yy - S->grad_from_y) * factor;
            float depth = grad + offset;
            raw[0] = (float)temp[p]; raw[1] = (float)veg[p]; raw[2] = (float)cont[p]; raw[3] = (float)eros[p]; raw[4] = depth; raw[5] = (float)rdg[p];
        }
        for (int k = 0; k < 6; k++) tg[p][k] = (i32)mc_quantize(raw[k]);
        tg[p][6] = 0;
    }
}

template<int MODE>
struct McgOwS {
    static constexpr bool HAS_TG = true;
    /* биомы (и цели) для np точек: пачками по 8 (таблицы шумов строятся заново для каждой пачки) */
    static MCG_INL void points(const McgParams &P, i64 seed, const McgTile &T, const i32 *pts, int np, u8 *out, i32 *tgout) {
        for (int p0 = 0; p0 < np; p0 += 8) {
            int m = np - p0 < 8 ? np - p0 : 8;
            i32 tg[8][MC_RT_DIM];
            mcg_ow_targets<MODE, 8>(P, seed, T, pts + 3 * p0, m, tg);
            for (int i = 0; i < m; i++) {
                out[p0 + i] = (u8)mcg_rt_find(P.tree, tg[i]);
                if (tgout) for (int k = 0; k < 6; k++) tgout[(size_t)(p0 + i) * 6 + k] = tg[i][k];
            }
        }
    }
    /* число пройденных подряд наблюдений. Этапы: [0], [1..4], затем по 8 — таблицы пересобираются на каждом этапе, но до
     * второго этапа доходит малая доля кандидатов (первое наблюдение — самое селективное). */
    static MCG_INL int check(const McgParams &P, i64 seed, const McgTile &T, const McgObs *obs, int nobs) {
        int j = 0; i64 hs = 0; bool have_hs = false;
        while (j < nobs) {
            int rem = nobs - j;
            int m = (j == 0) ? 1 : (j == 1 ? (rem < 4 ? rem : 4) : (rem < 8 ? rem : 8));
            i32 q[24]; i32 tg[8][MC_RT_DIM];
            for (int i = 0; i < m; i++) mcg_obs_quart(&obs[j + i], seed, &hs, &have_hs, &q[3 * i]);
            if (m == 1) mcg_ow_targets<MODE, 1>(P, seed, T, q, 1, tg);
            else if (m <= 4) mcg_ow_targets<MODE, 4>(P, seed, T, q, m, tg);
            else mcg_ow_targets<MODE, 8>(P, seed, T, q, m, tg);
            for (int i = 0; i < m; i++) {
                int b = mcg_rt_find(P.tree, tg[i]);
                if (!mcg_obs_accept(P, &obs[j + i], tg[i], b)) return j + i;
            }
            j += m;
        }
        return nobs;
    }
};

/* ------------------------------------------------------------------------------------------------ */
/*  Nether                                                                                          */
/* ------------------------------------------------------------------------------------------------ */
/* normal_init_legacy_stack с пропуском октавы скачком LCG вместо 262 отдельных шагов (+плитка) */
MCG_INL void mcg_legacy_stack(McImproved *lev, const McNoiseSpec *s, McLcg *r, u64 jm, u64 ja, const McgTile &T) {
    int octs = s->octs, zeroIdx = -s->first_octave;
    int kOf[MC_MAX_OCT]; for (int i = 0; i < MC_MAX_OCT; i++) kOf[i] = -1;
    for (int k = 0; k < s->n_levels; k++) kOf[s->lvl_index[k]] = k;
    bool keepZero = zeroIdx >= 0 && zeroIdx < octs && kOf[zeroIdx] >= 0;
    {
        McImproved *n = keepZero ? &lev[kOf[zeroIdx]] : (McImproved *)0;
        double xo, yo, zo; mcg_tile_gen_lcg(T, r, 256.0, &xo, &yo, &zo);
        if (n) { n->xo = xo; n->yo = yo; n->zo = zo; mcg_tile_store(T, n); }
    }
    for (int i = zeroIdx - 1; i >= 0; i--) {
        if (i < octs && kOf[i] >= 0) {
            McImproved *n = &lev[kOf[i]];
            double xo, yo, zo; mcg_tile_gen_lcg(T, r, 256.0, &xo, &yo, &zo);
            n->xo = xo; n->yo = yo; n->zo = zo; mcg_tile_store(T, n);
        } else r->s = (r->s * jm + ja) & MC_LCG_MASK;     /* lcg_skip(r, 262) */
    }
}

template<int MODE>
struct McgNeS {
    static constexpr bool HAS_TG = true;
    McImproved inst[MCG_NE_INST];
    MCG_INL void init(const McgParams &P, i64 seed, const McgTile &T) {
        McLcg r0 = lcg_new(seed), r1 = lcg_new(seed + 1);
        int nt = P.sp[0].n_levels, nv = P.sp[1].n_levels;
        mcg_legacy_stack(&inst[0], &P.sp[0], &r0, P.jm262, P.ja262, T);
        mcg_legacy_stack(&inst[nt], &P.sp[0], &r0, P.jm262, P.ja262, T);
        mcg_legacy_stack(&inst[P.noff[1]], &P.sp[1], &r1, P.jm262, P.ja262, T);
        mcg_legacy_stack(&inst[P.noff[1] + nv], &P.sp[1], &r1, P.jm262, P.ja262, T);
    }
    MCG_INL void target(const McgParams &P, int qx, int qy, int qz, i32 tg[MC_RT_DIM]) {
        int bx = qx * 4, bz = qz * 4, by = qy * 4; float t, h;
        const McNormal *nt = (const McNormal *)&inst[0], *nv = (const McNormal *)&inst[P.noff[1]];
        if (MODE == MC_NOISE_FLOAT) {
            t = normal_get_f(nt, &P.sp[0], bx * 0.25, by * 0.0, bz * 0.25);
            h = normal_get_f(nv, &P.sp[1], bx * 0.25, by * 0.0, bz * 0.25);
        } else {
            t = (float)normal_get_d(nt, &P.sp[0], bx * 0.25 + 0.0, by * 0.0 + 0.0, bz * 0.25 + 0.0);
            h = (float)normal_get_d(nv, &P.sp[1], bx * 0.25 + 0.0, by * 0.0 + 0.0, bz * 0.25 + 0.0);
        }
        tg[0] = (i32)mc_quantize(t); tg[1] = (i32)mc_quantize(h);
        for (int i = 2; i < MC_RT_DIM; i++) tg[i] = 0;
    }
    static MCG_INL void points(const McgParams &P, i64 seed, const McgTile &T, const i32 *pts, int np, u8 *out, i32 *tgout) {
        McgNeS st; st.init(P, seed, T);
        for (int p = 0; p < np; p++) {
            i32 tg[MC_RT_DIM]; st.target(P, pts[3 * p], pts[3 * p + 1], pts[3 * p + 2], tg);
            out[p] = (u8)mcg_rt_find(P.tree, tg);
            if (tgout) for (int k = 0; k < 6; k++) tgout[(size_t)p * 6 + k] = tg[k];
        }
    }
    static MCG_INL int check(const McgParams &P, i64 seed, const McgTile &T, const McgObs *obs, int nobs) {
        McgNeS st; st.init(P, seed, T);
        int j = 0; i64 hs = 0; bool have_hs = false;
        for (; j < nobs; j++) {
            i32 q[3], tg[MC_RT_DIM]; mcg_obs_quart(&obs[j], seed, &hs, &have_hs, q);
            st.target(P, q[0], q[1], q[2], tg);
            if (!mcg_obs_accept(P, &obs[j], tg, mcg_rt_find(P.tree, tg))) break;
        }
        return j;
    }
};

/* ------------------------------------------------------------------------------------------------ */
/*  End                                                                                             */
/* ------------------------------------------------------------------------------------------------ */
/* SimplexNoise 2D (mc_simplex2_core) по плитке */
MCG_INL double mcg_simplex_corner_t(int gi, double x, double y, double z, double base) {
    double t0 = base - x * x - y * y - z * z;
    if (t0 < 0.0) return 0.0;
    t0 *= t0;
    int gx = mcg_gc(MCG_GX, gi), gy = mcg_gc(MCG_GY, gi), gz = mcg_gc(MCG_GZ, gi);
    return t0 * t0 * (gx * x + gy * y + gz * z);
}
MCG_INL double mcg_simplex2_tile(const McgTile &T, double xin, double yin) {
    const double SQRT_3 = 1.7320508075688772;
    const double F2 = 0.5 * (SQRT_3 - 1.0);
    const double G2 = (3.0 - SQRT_3) / 6.0;
    double s = (xin + yin) * F2;
    i32 i = mc_floor_d(xin + s), j = mc_floor_d(yin + s);
    double t = (i + j) * G2;
    double X0 = i - t, Y0 = j - t;
    double x0 = xin - X0, y0 = yin - Y0;
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }
    double x1 = x0 - i1 + G2, y1 = y0 - j1 + G2;
    double x2 = x0 - 1.0 + 2.0 * G2, y2 = y0 - 1.0 + 2.0 * G2;
    int ii = i & 0xFF, jj = j & 0xFF;
    int gi0 = mcg_pt(T, ii + mcg_pt(T, jj)) % 12;
    int gi1 = mcg_pt(T, ii + i1 + mcg_pt(T, jj + j1)) % 12;
    int gi2 = mcg_pt(T, ii + 1 + mcg_pt(T, jj + 1)) % 12;
    double n0 = mcg_simplex_corner_t(gi0, x0, y0, 0.0, 0.5);
    double n1 = mcg_simplex_corner_t(gi1, x1, y1, 0.0, 0.5);
    double n2 = mcg_simplex_corner_t(gi2, x2, y2, 0.0, 0.5);
    return 70.0 * (n0 + n1 + n2);
}

/* mc_end_height_value с окном клеток |xo|,|zo| <= rad (rad=12 — полностью как в engine) */
template<int MODE>
MCG_INL float mcg_end_height_value(const McgTile &T, i32 sx, i32 sz, int rad) {
    i32 chunkX = sx / 2, chunkZ = sz / 2;
    i32 subX = sx % 2, subZ = sz % 2;
    float doffs;
    if (MODE == MC_NOISE_DOUBLE) {
        float lenv = (float)sqrt((double)(float)(sx * sx + sz * sz));
        doffs = 100.0f - lenv * 8.0f;
        doffs = mc_clampf(doffs, -100.0f, 80.0f);
    } else doffs = -100.0f;
    for (i32 xo = -rad; xo <= rad; xo++) {
        for (i32 zo = -rad; zo <= rad; zo++) {
            i64 tx = (i64)chunkX + xo, tz = (i64)chunkZ + zo;
            if (tx * tx + tz * tz > 4096LL) {
                double nv = mcg_simplex2_tile(T, (double)tx, (double)tz);
                int isl = (MODE == MC_NOISE_DOUBLE) ? (nv < (double)-0.9f) : ((float)nv < -0.9f);
                if (isl) {
                    float ax = fabsf((float)tx), az = fabsf((float)tz);
                    float islandSize = fmodf(ax * 3439.0f + az * 147.0f, 13.0f) + 9.0f;
                    float xd = (float)(subX - xo * 2), zd = (float)(subZ - zo * 2);
                    float nd = 100.0f - (float)sqrt((double)(xd * xd + zd * zd)) * islandSize;
                    nd = mc_clampf(nd, -100.0f, 80.0f);
                    doffs = doffs > nd ? doffs : nd;
                }
            }
        }
    }
    return doffs;
}

template<int MODE>
MCG_INL double mcg_end_erosion(const McgTile &T, i32 bx, i32 bz, int rad) {
    float h = mcg_end_height_value<MODE>(T, bx / 8, bz / 8, rad);
    if (MODE == MC_NOISE_DOUBLE) return ((double)h - 8.0) / 128.0;
    float dx = (float)(0 - bx), dz = (float)(0 - bz);
    float len = (float)sqrt((double)(dx * dx + 0.0f * 0.0f + dz * dz));
    float c = mc_clampf(100.0f - len, -100.0f, 80.0f);
    c = (c - 8.0f) * 0.0078125f;
    float o = (h - 8.0f) / 128.0f;
    return (double)(c > o ? c : o);
}

/* радиус окна клеток по маске (host): минимальный порог doffs, на котором членство в маске меняется.
 * Границы классов: islands|barrens: doffs=-20, barrens|midlands: 0, midlands|highlands: 40.
 * Клетка важна, только если nd >= T-1, т.е. dist*size <= 101-T, size>=9, |2*xo-subX| >= 2|xo|-1. */
static inline int mcg_end_rad_for_mask(const u64 mask[2], const int end_ids[5]) {
    int m[4];   /* порядок по возрастанию doffs: islands, barrens, midlands, highlands */
    const int order[4] = {3, 4, 2, 1};
    for (int i = 0; i < 4; i++) { int b = end_ids[order[i]]; m[i] = (int)((mask[b >> 6] >> (b & 63)) & 1ULL); }
    static const int TH[3] = {-20, 0, 40};
    int T = 1000;
    for (int i = 0; i < 3; i++) if (m[i] != m[i + 1]) { if (TH[i] < T) T = TH[i]; }
    if (T == 1000) return 0;                      /* решение не зависит от поля высот */
    double dmax = (101.0 - (double)T) / 9.0;
    int rad = (int)((dmax + 1.0) / 2.0);
    return rad > 12 ? 12 : rad;
}

template<int MODE>
struct McgEnS {
    static constexpr bool HAS_TG = false;
    /* TheEndBiomeSource.getNoiseBiome; таблица simplex уже построена в плитке T */
    static MCG_INL int biome(const McgParams &P, const McgTile &T, int qx, int qz, int rad) {
        i32 bx = qx * 4, bz = qz * 4;
        i32 cx = bx >> 4, cz = bz >> 4;
        if ((i64)cx * cx + (i64)cz * cz <= 4096LL) return P.end_ids[MC_END_BIOME_END];
        i32 wbx = (cx * 2 + 1) * 8, wbz = (cz * 2 + 1) * 8;
        double h = mcg_end_erosion<MODE>(T, wbx, wbz, rad);
        int cls;
        if (h > 0.25) cls = MC_END_HIGHLANDS;
        else if (h >= -0.0625) cls = MC_END_MIDLANDS;
        else cls = h < -0.21875 ? MC_END_ISLANDS : MC_END_BARRENS;
        return P.end_ids[cls];
    }
    static MCG_INL void init(const McgParams &P, i64 seed, const McgTile &T) {
        u64 state48 = ((u64)seed ^ MC_LCG_MUL) & MC_LCG_MASK;
        McLcg r; r.s = (state48 * P.jm_end + P.ja_end) & MC_LCG_MASK;     /* mc_end_init_from_state */
        double xo, yo, zo; mcg_tile_gen_lcg(T, &r, 256.0, &xo, &yo, &zo);
    }
    static MCG_INL void points(const McgParams &P, i64 seed, const McgTile &T, const i32 *pts, int np, u8 *out, i32 *tgout) {
        init(P, seed, T);
        for (int p = 0; p < np; p++) {
            out[p] = (u8)biome(P, T, pts[3 * p], pts[3 * p + 2], 12);
            if (tgout) for (int k = 0; k < 6; k++) tgout[(size_t)p * 6 + k] = 0;
        }
    }
    static MCG_INL int check(const McgParams &P, i64 seed, const McgTile &T, const McgObs *obs, int nobs) {
        init(P, seed, T);
        int j = 0; i64 hs = 0; bool have_hs = false;
        for (; j < nobs; j++) {
            i32 q[3]; mcg_obs_quart(&obs[j], seed, &hs, &have_hs, q);
            if (!mcg_in_mask(&obs[j], biome(P, T, q[0], q[2], mcg_obs_rad(&obs[j])))) break;
        }
        return j;
    }
};

/* Генератор seed'ов: kind 0 — список; 1 — структурный seed: base | (i << 48), i < 65536; 2 — base + i */
typedef struct { int kind; const i64 *list; u64 base; u64 n; } McgSeedGen;
MCG_INL i64 mcg_seed_at(const McgSeedGen &g, u64 i) {
    if (g.kind == 0) return g.list[i];
    if (g.kind == 1) return (i64)(g.base | (i << 48));
    return (i64)(g.base + i);
}

#endif /* MC_GPU_CUH */

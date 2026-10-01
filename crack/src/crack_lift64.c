/*
 * crack-lift64 — structure seed (48 бит) -> 64-битный world seed.
 *
 * Перебирает 2^16 верхних бит для каждого кандидата и проверяет по возрастанию стоимости:
 *   (a) hashed seed (BiomeManager.obfuscateSeed = первые 8 байт SHA-256 от 8 байт seed little-endian, прочитанные как little-endian long);
 *   (b) биомы Nether/End (зависят только от 48 бит: фильтр по structure seed; блок-уровень — через зум BiomeManager + hashed seed);
 *   (c) биомы Overworld (зависят от всех 64 бит; Xoroshiro-климат, R-дерево — движок engine/).
 * Блок-уровень: BiomeManager.getBiome(x,y,z) — зум по 8 соседним квартам с «размытием» от hashed seed (BiomeManager.java).
 *
 * Сборка: см. crack/Makefile (gcc -O2 -ffp-contract=off -fopenmp).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <omp.h>
#include "mc_data.h"
#include "mc_end.h"

#define MASK48 0xFFFFFFFFFFFFULL

/* =========================================== SHA-256 =========================================== */
static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* BiomeManager.obfuscateSeed(seed) = Hashing.sha256().hashLong(seed).asLong() — скалярная реализация */
static u64 obfuscate_seed_scalar(u64 seed) {
    u32 w[64];
    u8 m[8]; for (int i = 0; i < 8; i++) m[i] = (u8)(seed >> (8 * i));            /* hashLong: little-endian */
    w[0] = ((u32)m[0] << 24) | ((u32)m[1] << 16) | ((u32)m[2] << 8) | m[3];
    w[1] = ((u32)m[4] << 24) | ((u32)m[5] << 16) | ((u32)m[6] << 8) | m[7];
    w[2] = 0x80000000u;
    for (int i = 3; i < 15; i++) w[i] = 0;
    w[15] = 64;
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = 0x6a09e667, b = 0xbb67ae85, c = 0x3c6ef372, d = 0xa54ff53a, e = 0x510e527f, f = 0x9b05688c, g = 0x1f83d9ab, h = 0x5be0cd19;
    for (int i = 0; i < 64; i++) {
        u32 S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K256[i] + w[i];
        u32 S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    u32 h0 = a + 0x6a09e667, h1 = b + 0xbb67ae85;
    /* первые 8 байт дайджеста: h0, h1 big-endian; asLong() читает их как little-endian long */
    u32 lo = __builtin_bswap32(h0), hi = __builtin_bswap32(h1);
    return (u64)lo | ((u64)hi << 32);
}

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#include <cpuid.h>
/* SHA-NI (Intel/AMD SHA extensions): один блок SHA-256, ~15x быстрее скалярного кода */
__attribute__((target("sha,sse4.1,ssse3")))
static u64 obfuscate_seed_shani(u64 seed) {
    u32 b0 = __builtin_bswap32((u32)seed), b1 = __builtin_bswap32((u32)(seed >> 32));   /* слова big-endian от little-endian байт seed */
    __m128i W[4];
    W[0] = _mm_set_epi32(0, (int)0x80000000u, (int)b1, (int)b0);
    W[1] = _mm_setzero_si128(); W[2] = _mm_setzero_si128();
    W[3] = _mm_set_epi32(64, 0, 0, 0);
    __m128i TMP = _mm_set_epi32((int)0xa54ff53a, (int)0x3c6ef372, (int)0xbb67ae85, (int)0x6a09e667);           /* lane0 = a, ..., lane3 = d */
    __m128i STATE1 = _mm_set_epi32((int)0x5be0cd19, (int)0x1f83d9ab, (int)0x9b05688c, (int)0x510e527f);        /* lane0 = e, ..., lane3 = h */
    TMP = _mm_shuffle_epi32(TMP, 0xB1);
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);
    __m128i STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);
    const __m128i ABEF_SAVE = STATE0, CDGH_SAVE = STATE1;
    for (int i = 0; i < 16; i++) {
        __m128i MSG = _mm_add_epi32(W[i & 3], _mm_loadu_si128((const __m128i *)&K256[4 * i]));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        if (i < 12) {
            __m128i t = _mm_sha256msg1_epu32(W[i & 3], W[(i + 1) & 3]);
            t = _mm_add_epi32(t, _mm_alignr_epi8(W[(i + 3) & 3], W[(i + 2) & 3], 4));
            W[i & 3] = _mm_sha256msg2_epu32(t, W[(i + 3) & 3]);
        }
    }
    STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
    STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);
    TMP = _mm_shuffle_epi32(STATE0, 0x1B);
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);            /* lane0 = A, lane1 = B, ... */
    u32 h0 = (u32)_mm_extract_epi32(STATE0, 0), h1 = (u32)_mm_extract_epi32(STATE0, 1);
    return (u64)__builtin_bswap32(h0) | ((u64)__builtin_bswap32(h1) << 32);
}
static int have_shani(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    if (!(b & (1u << 29))) return 0;                        /* SHA */
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    return (c & (1u << 19)) && (c & (1u << 9));            /* SSE4.1, SSSE3 */
}
#else
static u64 obfuscate_seed_shani(u64 seed) { return obfuscate_seed_scalar(seed); }
static int have_shani(void) { return 0; }
#endif
static int g_shani = 0;
static inline u64 obfuscate_seed(u64 seed) { return g_shani ? obfuscate_seed_shani(seed) : obfuscate_seed_scalar(seed); }

/* =========================================== BiomeManager (зум) =========================================== */
static u64 lcg_next64(u64 rval, u64 c) { rval *= rval * 6364136223846793005ULL + 1442695040888963407ULL; return rval + c; }
static double get_fiddle(u64 rval) {
    double uniform = (double)(((i64)rval >> 24) & 1023) / 1024.0;      /* Math.floorMod(rval >> 24, 1024) / 1024.0 */
    return (uniform - 0.5) * 0.9;
}
static double fiddled_distance(u64 seed, i32 xr, i32 yr, i32 zr, double dx, double dy, double dz) {
    u64 r = seed;
    r = lcg_next64(r, (u64)(i64)xr); r = lcg_next64(r, (u64)(i64)yr); r = lcg_next64(r, (u64)(i64)zr);
    r = lcg_next64(r, (u64)(i64)xr); r = lcg_next64(r, (u64)(i64)yr); r = lcg_next64(r, (u64)(i64)zr);
    double fx = get_fiddle(r);
    r = lcg_next64(r, seed); double fy = get_fiddle(r);
    r = lcg_next64(r, seed); double fz = get_fiddle(r);
    double a = dz + fz, b = dy + fy, c = dx + fx;
    return a * a + b * b + c * c;
}
/* BiomeManager.getBiome(x,y,z): выбирает квартовую ячейку (biomeX,biomeY,biomeZ) по hashed seed */
static void zoom_cell(u64 hashed, i32 x, i32 y, i32 z, i32 *qx, i32 *qy, i32 *qz) {
    i32 ax = x - 2, ay = y - 2, az = z - 2;
    i32 px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (double)(ax & 3) / 4.0, fy = (double)(ay & 3) / 4.0, fz = (double)(az & 3) / 4.0;
    int minI = 0; double minD = 1.0 / 0.0;
    for (int i = 0; i < 8; i++) {
        int xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        i32 cx = xe ? px : px + 1, cy = ye ? py : py + 1, cz = ze ? pz : pz + 1;
        double dx = xe ? fx : fx - 1.0, dy = ye ? fy : fy - 1.0, dz = ze ? fz : fz - 1.0;
        double d = fiddled_distance(hashed, cx, cy, cz, dx, dy, dz);
        if (minD > d) { minI = i; minD = d; }
    }
    *qx = (minI & 4) == 0 ? px : px + 1; *qy = (minI & 2) == 0 ? py : py + 1; *qz = (minI & 1) == 0 ? pz : pz + 1;
}

/* =========================================== «случайный» world seed =========================================== */
/* WorldOptions.randomSeed() = RandomSource.create().nextLong(), RandomSource.create(long) = LegacyRandomSource (26.1–26.3):
 * W = (a << 32) + b, a = (int)(s1 >> 16), b = (int)(s2 >> 16), s2 = A*s1 + C (mod 2^48) — 48-битное состояние s1 определяет W целиком.
 * По нижним 48 битам W находим верхние 16 (NextLongReverser): 2^16 переборов x (младшие 16 бит s1) + линейное уравнение для верхних 16 бит a.
 * Возвращает число кандидатов (обычно 0–3, в среднем 1). Верно ТОЛЬКО для seed, сгенерированных игрой (пустое поле seed), не для введённых вручную. */
static int random_world_seeds(u64 S48, u64 *out, int maxn) {
    u32 b_u = (u32)(S48 & 0xFFFFFFFFu); i32 b = (i32)b_u;
    u32 a_lo16 = (u32)(((S48 >> 32) + (b < 0 ? 1 : 0)) & 0xFFFF);
    u32 A16 = (u32)(0x5DEECE66DULL & 0xFFFF), inv = A16;                       /* обратный к A mod 2^16 (Ньютон) */
    for (int i = 0; i < 5; i++) inv = (inv * (2u - A16 * inv)) & 0xFFFF;
    int n = 0;
    for (u32 x = 0; x < 65536; x++) {
        u64 u = (0x5DEECE66DULL * (((u64)a_lo16 << 16) | x) + 0xBULL) & MASK48;
        if (((u >> 16) & 0xFFFF) != (b_u & 0xFFFF)) continue;
        u32 ahi = (((b_u >> 16) - (u32)(u >> 32)) * inv) & 0xFFFF;
        u32 a_u = (ahi << 16) | a_lo16;
        u64 st1 = ((u64)a_u << 16) | x, st2 = (st1 * 0x5DEECE66DULL + 0xBULL) & MASK48;
        if ((u32)(st2 >> 16) != b_u) continue;
        i64 W = ((i64)(i32)a_u << 32) + (i64)b;
        if (((u64)W & MASK48) != S48) continue;
        if (n < maxn) out[n++] = (u64)W;
    }
    return n;
}

/* =========================================== наблюдения =========================================== */
typedef struct { int dim; int quart; i32 x, y, z; int biome; int line; } BObs;     /* dim: 0 overworld, 1 nether, 2 end */

typedef struct {
    int version;
    McVersionData V;
    McClimateSpec *preset[1];
    McBiomeTree *T_ow, *T_ne;
    int *leaf_of[MC_BIOME_COUNT]; int n_leaf[MC_BIOME_COUNT];       /* листья R-дерева по биомам (для тай-брейков), Overworld */
    int *leaf_of_ne[MC_BIOME_COUNT]; int n_leaf_ne[MC_BIOME_COUNT];
} Ctx;

static void index_leaves(const McBiomeTree *T, int **leaf_of, int *n_leaf) {
    for (int b = 0; b < MC_BIOME_COUNT; b++) { leaf_of[b] = NULL; n_leaf[b] = 0; }
    for (int i = 0; i < T->n_nodes; i++) if (T->node[i].biome >= 0 && T->node[i].count == 0) n_leaf[T->node[i].biome]++;
    for (int b = 0; b < MC_BIOME_COUNT; b++) { leaf_of[b] = (int *)malloc(sizeof(int) * (size_t)(n_leaf[b] + 1)); n_leaf[b] = 0; }
    for (int i = 0; i < T->n_nodes; i++) if (T->node[i].biome >= 0 && T->node[i].count == 0) { int b = T->node[i].biome; leaf_of[b][n_leaf[b]++] = i; }
}

/* Совпадает ли наблюдаемый биом с результатом поиска; при равенстве fitness игра может вернуть любой из равных
 * (Climate.RTree.search зависит от lastResult потока) -> принимаем любой из биомов с минимальным расстоянием. */
static int tree_biome_matches(const McBiomeTree *T, int **leaf_of, const int *n_leaf, const McTarget *t, int want) {
    i64 target[MC_RT_DIM] = {t->t, t->h, t->c, t->e, t->d, t->w, 0};
    int leaf = mc_rt_search_node(T, T->root, target, -1);
    if (T->node[leaf].biome == want) return 1;
    if (want < 0 || want >= MC_BIOME_COUNT) return 0;
    i64 d = mc_rnode_distance(&T->node[leaf], target);
    for (int k = 0; k < n_leaf[want]; k++) if (mc_rnode_distance(&T->node[leaf_of[want][k]], target) <= d) return 1;
    return 0;
}

/* Overworld: биом в квартовой ячейке */
static int ow_quart_matches(const Ctx *C, const McClimate *cl, const McClimateSpec *S, i32 qx, i32 qy, i32 qz, int want) {
    float raw[6];
    mc_climate_overworld_raw(cl, S, qx * 4, qy * 4, qz * 4, raw);
    McTarget t = mc_target_from_raw(raw);
    return tree_biome_matches(C->T_ow, (int **)C->leaf_of, C->n_leaf, &t, want);
}

/* Nether: состояние (шумы) — зависит только от 48 бит */
typedef struct { McNormal nt, nv; } NetherState;
static void nether_init(const Ctx *C, NetherState *N, i64 seed) {
    McLcg r0 = lcg_new(seed), r1 = lcg_new(seed + 1);
    normal_init_legacy(&N->nt, &C->V.nether_temp, &r0); normal_init_legacy(&N->nv, &C->V.nether_veg, &r1);
}
static int nether_quart_matches(const Ctx *C, const NetherState *N, i32 qx, i32 qy, i32 qz, int want) {
    int bx = qx * 4, bz = qz * 4, by = qy * 4; float t, h;
    if (C->version == MC_26_3) {
        t = normal_get_f(&N->nt, &C->V.nether_temp, bx * 0.25, by * 0.0, bz * 0.25);
        h = normal_get_f(&N->nv, &C->V.nether_veg, bx * 0.25, by * 0.0, bz * 0.25);
    } else {
        t = (float)normal_get_d(&N->nt, &C->V.nether_temp, bx * 0.25 + 0.0, by * 0.0 + 0.0, bz * 0.25 + 0.0);
        h = (float)normal_get_d(&N->nv, &C->V.nether_veg, bx * 0.25 + 0.0, by * 0.0 + 0.0, bz * 0.25 + 0.0);
    }
    McTarget tg = {mc_quantize(t), mc_quantize(h), 0, 0, 0, 0};
    return tree_biome_matches(C->T_ne, (int **)C->leaf_of_ne, C->n_leaf_ne, &tg, want);
}
/* End */
static const char *const END_NAMES[5] = {"the_end", "end_highlands", "end_midlands", "small_end_islands", "end_barrens"};
static int end_biome_id(int k) { return mc_biome_id(END_NAMES[k]); }

/* =========================================== разбор файлов =========================================== */
static int parse_i64(const char *s, i64 *out) {
    char *e; errno = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { unsigned long long v = strtoull(s, &e, 16); if (*e) return 0; *out = (i64)v; return 1; }
    if (s[0] == '-') { long long v = strtoll(s, &e, 10); if (*e || errno) return 0; *out = v; return 1; }
    unsigned long long v = strtoull(s, &e, 10); if (*e || errno) return 0; *out = (i64)v; return 1;
}
static int dim_from(const char *s) {
    if (!strcmp(s, "overworld") || !strcmp(s, "ow")) return 0;
    if (!strcmp(s, "nether") || !strcmp(s, "the_nether")) return 1;
    if (!strcmp(s, "end") || !strcmp(s, "the_end")) return 2;
    return -1;
}

typedef struct { BObs *a; int n, cap; } BList;
static void blist_add(BList *l, BObs o) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->a = (BObs *)realloc(l->a, sizeof(BObs) * (size_t)l->cap); } l->a[l->n++] = o; }

/* формат строк: `x z y biome` | `x z biome`; директивы @dim D, @preset P, @quart, @block; # — комментарий */
static int parse_biomes(const char *path, int default_dim, int default_quart, int default_y, BList *out, char *preset, size_t presz) {
    FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "не могу открыть %s\n", path); return 0; }
    char line[512]; int ln = 0, dim = default_dim, quart = default_quart;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#'); if (h) *h = 0;
        for (char *p = line; *p; p++) if (*p == ',' || *p == ';' || *p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
        char *tok[6]; int nt = 0; char *sp = NULL;
        for (char *t = strtok_r(line, " ", &sp); t && nt < 6; t = strtok_r(NULL, " ", &sp)) tok[nt++] = t;
        if (nt == 0) continue;
        if (tok[0][0] == '@') {
            if (!strcmp(tok[0], "@dim") && nt >= 2) { dim = dim_from(tok[1]); if (dim < 0) { fprintf(stderr, "%s:%d: неизвестное измерение\n", path, ln); return 0; } }
            else if (!strcmp(tok[0], "@preset") && nt >= 2) snprintf(preset, presz, "%s", tok[1]);
            else if (!strcmp(tok[0], "@quart")) quart = 1;
            else if (!strcmp(tok[0], "@block")) quart = 0;
            else { fprintf(stderr, "%s:%d: неизвестная директива %s\n", path, ln, tok[0]); return 0; }
            continue;
        }
        BObs o; memset(&o, 0, sizeof o); o.dim = dim; o.quart = quart; o.line = ln;
        i64 x, y, z; const char *bn;
        if (nt == 4) { if (!parse_i64(tok[0], &x) || !parse_i64(tok[1], &z) || !parse_i64(tok[2], &y)) { fprintf(stderr, "%s:%d: ожидалось `x z y biome`\n", path, ln); return 0; } bn = tok[3]; }
        else if (nt == 3) { if (!parse_i64(tok[0], &x) || !parse_i64(tok[1], &z)) { fprintf(stderr, "%s:%d: ожидалось `x z [y] biome`\n", path, ln); return 0; } y = default_y; bn = tok[2]; }
        else { fprintf(stderr, "%s:%d: ожидалось `x z y biome`\n", path, ln); return 0; }
        o.x = (i32)x; o.y = (i32)y; o.z = (i32)z;
        if (dim == 2 && nt == 3) o.y = 0;
        o.biome = mc_biome_id(bn);
        if (o.biome < 0) { fprintf(stderr, "%s:%d: неизвестный биом `%s`\n", path, ln, bn); return 0; }
        blist_add(out, o);
    }
    fclose(f); return 1;
}

static void usage(void) {
    fprintf(stderr,
"crack-lift64 — structure seed (48 бит) -> 64-битный world seed.\n\n"
"  crack-lift64 --version 26.3 (--struct-seed S | --seeds FILE|-) [--hashed H] [--biomes FILE] [опции]\n\n"
"Вход (кандидаты structure seed): --struct-seed S (десятичное/0x), либо --seeds FILE (по одному на строку; `#` — комментарий;\n"
"  годится вывод crack-struct). Значения приводятся к 48 битам.\n"
"Проверки (дешёвые первыми), перебор 2^16 верхних бит на кандидата:\n"
"  --hashed H     hashed seed из пакета логина/респауна (long; десятичное со знаком или 0x...): SHA-256\n"
"  --biomes FILE  наблюдения биомов: строки `x z y biome` (блоки; `x z biome` — y по умолчанию), директивы `@dim overworld|nether|end`,\n"
"                 `@preset normal|large_biomes|amplified`, `@quart` (координаты в квартах) / `@block`; `#` — комментарий\n"
"Опции:\n"
"  --version V    26.1 | 26.2 | 26.3 (по умолчанию 26.3)\n"
"  --dim D        измерение по умолчанию для --biomes (overworld)\n"
"  --preset P     пресет мира по умолчанию (normal)\n"
"  --quart        координаты в --biomes по умолчанию в квартах (1 кварт = 4 блока)\n"
"  --y N          y по умолчанию для строк `x z biome` (64)\n"
"  --emit-all     печатать все 65536 кандидатов, если проверки не сузили верхние 16 бит\n"
"  --random       предположить, что seed сгенерирован игрой (пустое поле seed: RandomSource.create().nextLong()) — верхние 16 бит\n"
"                 однозначно (обычно 1 кандидат) выводятся из нижних 48; действует по умолчанию, если нет --hashed/--biomes; --no-random отключает\n"
"  --threads N    число потоков OpenMP\n"
"  --root DIR     корень проекта (содержит data/ с climate-<V>.txt и params/); принимается и сам каталог data/ (--data-dir DIR); по умолчанию — корень от\n"
"                 расположения бинарника или $MCGEN_ROOT\n"
"  -v             подробный вывод\n\n"
"Вывод (stdout): знаковый 64-битный world seed на строку; `# ...` — комментарии (вердикт по каждому structure seed). Сводка — stderr.\n"
"Код возврата: 0 — есть хотя бы один world seed / проверенный structure seed; 1 — ничего не прошло; 2 — ошибка входа.\n");
}

static char *find_root_c(const char *argv0) {
    static char root[PATH_MAX];
    const char *r = getenv("MCGEN_ROOT"); if (r) { snprintf(root, sizeof root, "%s", r); return root; }
    char buf[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) { snprintf(buf, sizeof buf, "%s", argv0); n = (ssize_t)strlen(buf); } else buf[n] = 0;
    char *sl = strrchr(buf, '/'); if (sl) *sl = 0; else strcpy(buf, ".");
    const char *rels[2] = {"/../..", "/.."};
    for (int i = 0; i < 2; i++) { char cand[PATH_MAX + 16]; snprintf(cand, sizeof cand, "%s%s/data", buf, rels[i]); if (access(cand, F_OK) == 0) { snprintf(root, sizeof root, "%s%s", buf, rels[i]); return root; } }
    snprintf(root, sizeof root, "."); return root;
}

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    const char *version = "26.3", *seeds_path = NULL, *biomes_path = NULL, *dim_s = "overworld", *preset_s = "normal", *data_dir = NULL;
    int have_single = 0, have_hashed = 0, quart = 0, emit_all = 0, verbose = 0, default_y = 64, rand_mode = 2; i64 single = 0, hashed = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEED(nm) (i + 1 < argc ? argv[++i] : (fprintf(stderr, "опция %s требует значение\n", nm), exit(2), (char *)0))
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--version")) version = NEED(a);
        else if (!strcmp(a, "--struct-seed")) { if (!parse_i64(NEED(a), &single)) { fprintf(stderr, "плохой --struct-seed\n"); return 2; } have_single = 1; }
        else if (!strcmp(a, "--seeds")) seeds_path = NEED(a);
        else if (!strcmp(a, "--hashed")) { if (!parse_i64(NEED(a), &hashed)) { fprintf(stderr, "плохой --hashed\n"); return 2; } have_hashed = 1; }
        else if (!strcmp(a, "--biomes")) biomes_path = NEED(a);
        else if (!strcmp(a, "--dim")) dim_s = NEED(a);
        else if (!strcmp(a, "--preset")) preset_s = NEED(a);
        else if (!strcmp(a, "--quart")) quart = 1;
        else if (!strcmp(a, "--y")) default_y = atoi(NEED(a));
        else if (!strcmp(a, "--emit-all")) emit_all = 1;
        else if (!strcmp(a, "--random")) rand_mode = 1;
        else if (!strcmp(a, "--no-random")) rand_mode = 0;
        else if (!strcmp(a, "--threads")) omp_set_num_threads(atoi(NEED(a)));
        else if (!strcmp(a, "--data-dir") || !strcmp(a, "--root")) data_dir = NEED(a);
        else if (!strcmp(a, "-v")) verbose = 1;
        else { fprintf(stderr, "неизвестная опция %s (см. --help)\n", a); return 2; }
    }
    g_shani = have_shani();
    int ver = mc_version_from_name(version);
    if (ver < 0) { fprintf(stderr, "неизвестная версия %s (движок biomes поддерживает 26.1|26.2|26.3)\n", version); return 2; }
    if (!have_single && !seeds_path) { usage(); return 2; }
    int default_dim = dim_from(dim_s); if (default_dim < 0) { fprintf(stderr, "неизвестное измерение %s\n", dim_s); return 2; }

    /* --- кандидаты --- */
    u64 *cands = NULL; size_t nc = 0, capc = 0;
    if (have_single) { cands = (u64 *)malloc(sizeof(u64)); cands[nc++] = (u64)single & MASK48; capc = 1; }
    if (seeds_path) {
        FILE *f = !strcmp(seeds_path, "-") ? stdin : fopen(seeds_path, "r");
        if (!f) { fprintf(stderr, "не могу открыть %s\n", seeds_path); return 2; }
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#'); if (h) *h = 0;
            char *sp = NULL; char *t = strtok_r(line, " \t\r\n,;", &sp); if (!t) continue;
            i64 v; if (!parse_i64(t, &v)) { fprintf(stderr, "плохое число в %s: %s\n", seeds_path, t); return 2; }
            if (nc == capc) { capc = capc ? capc * 2 : 1024; cands = (u64 *)realloc(cands, sizeof(u64) * capc); }
            cands[nc++] = (u64)v & MASK48;
        }
        if (f != stdin) fclose(f);
    }
    if (nc == 0) { fprintf(stderr, "нет кандидатов structure seed\n"); return 1; }

    /* --- биомные наблюдения --- */
    BList bl = {0}; char preset[32]; snprintf(preset, sizeof preset, "%s", preset_s);
    if (biomes_path && !parse_biomes(biomes_path, default_dim, quart, default_y, &bl, preset, sizeof preset)) return 2;
    if (!strcmp(preset, "normal")) snprintf(preset, sizeof preset, "overworld");
    int n_ow = 0, n_ne = 0, n_en = 0;
    for (int i = 0; i < bl.n; i++) { if (bl.a[i].dim == 0) n_ow++; else if (bl.a[i].dim == 1) n_ne++; else n_en++; }
    if (!have_hashed && bl.n == 0 && rand_mode == 0) fprintf(stderr, "предупреждение: нет ни --hashed, ни --biomes (и --no-random): верхние 16 бит определить нечем (65536 кандидатов на structure seed)\n");

    /* --- движок --- */
    static char root_buf[PATH_MAX];
    const char *root = data_dir ? data_dir : find_root_c(argv[0]);
    if (data_dir) {      /* допускаем и корень проекта, и сам каталог data/ */
        char probe[PATH_MAX + 32]; snprintf(probe, sizeof probe, "%s/data/climate-%s.txt", data_dir, version);
        if (access(probe, F_OK) != 0) { snprintf(probe, sizeof probe, "%s/climate-%s.txt", data_dir, version);
            if (access(probe, F_OK) == 0) { snprintf(root_buf, sizeof root_buf, "%s/..", data_dir); root = root_buf; } }
    }
    setenv("MCGEN_ROOT", root, 1);
    static Ctx C; memset(&C, 0, sizeof C); C.version = ver;
    double t0 = now();
    if (bl.n) {
        if (mc_load_version(&C.V, ver)) { fprintf(stderr, "ошибка загрузки данных движка (корень %s)\n", root); return 2; }
        if (n_ow) {
            C.T_ow = mc_load_biome_tree(ver, "overworld"); if (!C.T_ow) return 2;
            index_leaves(C.T_ow, C.leaf_of, C.n_leaf);
        }
        if (n_ne) { C.T_ne = mc_load_biome_tree(ver, "nether"); if (!C.T_ne) return 2; index_leaves(C.T_ne, C.leaf_of_ne, C.n_leaf_ne); }
    }
    McClimateSpec *S = NULL;
    if (n_ow) { S = mc_preset(&C.V, preset); if (!S) { fprintf(stderr, "нет пресета %s (есть: overworld/amplified/large_biomes)\n", preset); return 2; } }
    int end_mode = (ver == MC_26_3) ? MC_NOISE_FLOAT : MC_NOISE_DOUBLE;
    int end_ids[5]; for (int k = 0; k < 5; k++) end_ids[k] = end_biome_id(k);
    if (verbose) fprintf(stderr, "движок загружен за %.2f с (root=%s); наблюдений биомов: overworld %d, nether %d, end %d; hashed: %s\n", now() - t0, root, n_ow, n_ne, n_en, have_hashed ? "да" : "нет");

    /* порядок проверки внутри кандидата: hashed -> nether/end (quart), nether/end (block) -> overworld */
    int nthreads = omp_get_max_threads();
    size_t total_out = 0, n_determined = 0, n_verified_only = 0, n_none = 0;
    double t_start = now();
    u64 *surv = (u64 *)malloc(sizeof(u64) * 65536);
    if (have_hashed && bl.n == 0 && rand_mode != 1) {
        /* быстрый путь: только hashed seed — параллелим по кандидатам, внутри 2^16 SHA-256 (SHA-NI при наличии) */
        u64 *found = (u64 *)malloc(sizeof(u64) * nc * 4); int *nf = (int *)calloc(nc, sizeof(int));
        #pragma omp parallel for schedule(dynamic, 8)
        for (long ci = 0; ci < (long)nc; ci++) {
            u64 S48 = cands[ci];
            for (u64 hi = 0; hi < 65536; hi++) {
                u64 W = (hi << 48) | S48;
                if (obfuscate_seed(W) == (u64)hashed && nf[ci] < 4) found[ci * 4 + nf[ci]++] = W;
            }
        }
        for (size_t ci = 0; ci < nc; ci++) {
            if (!nf[ci]) { n_none++; continue; }
            for (int k = 0; k < nf[ci]; k++) { u64 W = found[ci * 4 + k]; n_determined++;
                printf("%lld\t0x%016llx\t# struct=%llu hi16=0x%04llx hashed\n", (long long)W, (unsigned long long)W, (unsigned long long)(W & MASK48), (unsigned long long)(W >> 48)); }
        }
        fflush(stdout);
        fprintf(stderr, "crack-lift64: кандидатов structure seed %zu, потоков %d, SHA-256: %s: world seed найдено %zu, отвергнуто %zu; %.3f с\n", nc, nthreads, g_shani ? "SHA-NI" : "скалярный",
                n_determined, n_none, now() - t_start);
        free(found); free(nf); free(surv);
        return n_determined ? 0 : 1;
    }
    for (size_t ci = 0; ci < nc; ci++) {
        u64 S48 = cands[ci];
        int rejected = 0;
        /* nether/end: состояние (шумы) зависит только от S48 */
        NetherState *NS = NULL; McEnd *EN = NULL;
        if (n_ne) { NS = (NetherState *)malloc(sizeof(NetherState)); nether_init(&C, NS, (i64)S48); }
        if (n_en) { EN = (McEnd *)malloc(sizeof(McEnd)); mc_end_init(EN, (i64)S48); }
        int has_block_hi = 0;      /* есть ли проверки, зависящие от верхних бит: hashed / блок-уровень */
        if (have_hashed) has_block_hi = 1;
        /* quart-level nether/end — один раз на S48 */
        for (int i = 0; i < bl.n && !rejected; i++) {
            const BObs *o = &bl.a[i];
            if (o->dim == 0) { if (!o->quart) has_block_hi = 1; continue; }
            if (!o->quart) { has_block_hi = 1; continue; }
            int ok;
            if (o->dim == 1) ok = nether_quart_matches(&C, NS, o->x, o->y, o->z, o->biome);
            else ok = end_ids[mc_end_biome(EN, end_mode, o->x, o->z)] == o->biome;
            if (!ok) rejected = 1;
        }
        if (rejected) { n_none++; free(NS); free(EN); if (verbose) fprintf(stderr, "  structure seed %llu: отвергнут биомами Nether/End (quart)\n", (unsigned long long)S48); continue; }
        /* список верхних бит: все 65536, либо кандидаты «случайного» seed (RandomSource.create().nextLong()) */
        u64 rl[64]; int nrand = 0;
        int use_random = (rand_mode == 1) || (rand_mode == 2 && !have_hashed && bl.n == 0);
        if (use_random) nrand = random_world_seeds(S48, rl, 64);
        int small_hint = (S48 < (1ULL << 31)) || (S48 >= MASK48 - (1ULL << 31) + 1);
        int any_hi_check = have_hashed || n_ow > 0 || has_block_hi || use_random;
        size_t ns = 0;
        if (use_random && nrand == 0 && rand_mode == 2) any_hi_check = 0;       /* авто-режим: не случайный seed -> подсказки */
        if (!any_hi_check) {
            /* верхние 16 бит проверками не сужены */
            n_verified_only++;
            printf("# UNDETERMINED struct_seed=%llu hi16=any (65536 кандидатов)%s%s\n", (unsigned long long)S48, (n_ne + n_en) ? " [structure seed подтверждён биомами Nether/End]" : "",
                   use_random ? " [не является результатом RandomSource.create().nextLong(): seed введён вручную]" : "");
            if (emit_all) for (u64 hi = 0; hi < 65536; hi++) printf("%lld\n", (long long)((hi << 48) | S48));
            free(NS); free(EN);
            if (S48 < (1ULL << 31)) printf("# GUESS small/text seed: %llu (число или hashCode строки укладывается в int32)\n", (unsigned long long)S48);
            else if (S48 >= MASK48 - (1ULL << 31) + 1) printf("# GUESS small/text seed: %lld (отрицательный int32)\n", (long long)(i64)(S48 | 0xFFFF000000000000ULL));
            continue;
        }
        int nhi = use_random ? nrand : 65536;
        #pragma omp parallel
        {
            McClimate *cl = n_ow ? (McClimate *)malloc(sizeof(McClimate)) : NULL;
            #pragma omp for schedule(dynamic, 16)
            for (int hi = 0; hi < nhi; hi++) {
                u64 W = use_random ? rl[hi] : (((u64)hi << 48) | S48);
                u64 H = 0;
                if (have_hashed || has_block_hi) H = obfuscate_seed(W);
                if (have_hashed && H != (u64)hashed) continue;
                int ok = 1;
                for (int i = 0; i < bl.n && ok; i++) {
                    const BObs *o = &bl.a[i];
                    if (o->dim == 0 || o->quart) continue;
                    i32 qx, qy, qz; zoom_cell(H, o->x, o->y, o->z, &qx, &qy, &qz);
                    if (o->dim == 1) ok = nether_quart_matches(&C, NS, qx, qy, qz, o->biome);
                    else ok = end_ids[mc_end_biome(EN, end_mode, qx, qz)] == o->biome;
                }
                if (!ok) continue;
                if (n_ow) {
                    mc_climate_init_overworld(cl, S, (i64)W);
                    for (int i = 0; i < bl.n && ok; i++) {
                        const BObs *o = &bl.a[i];
                        if (o->dim != 0) continue;
                        i32 qx = o->x, qy = o->y, qz = o->z;
                        if (!o->quart) zoom_cell(H, o->x, o->y, o->z, &qx, &qy, &qz);
                        ok = ow_quart_matches(&C, cl, S, qx, qy, qz, o->biome);
                    }
                }
                if (ok) {
                    #pragma omp critical
                    { surv[ns++] = W; }
                }
            }
            free(cl);
        }
        free(NS); free(EN);
        if (ns == 0) { n_none++; if (verbose || use_random) fprintf(stderr, "  structure seed %llu: ни один из %d world seed не прошёл\n", (unsigned long long)S48, nhi); continue; }
        /* сортировка для детерминизма */
        for (size_t a = 1; a < ns; a++) { u64 v = surv[a]; size_t b = a; while (b > 0 && surv[b - 1] > v) { surv[b] = surv[b - 1]; b--; } surv[b] = v; }
        if (!use_random && !have_hashed && n_ow == 0 && ns > 64) {
            /* только Nether/End: верхние 16 бит почти не влияют (лишь у границ биомов через зум) */
            n_verified_only++;
            printf("# UNDETERMINED struct_seed=%llu hi16=any (прошло %zu из 65536 world seed; биомы Nether/End зависят от 48 бит)\n", (unsigned long long)S48, ns);
            if (emit_all) for (size_t k = 0; k < ns; k++) printf("%lld\n", (long long)surv[k]);
            if (S48 < (1ULL << 31)) printf("# GUESS small/text seed: %llu (число или hashCode строки укладывается в int32)\n", (unsigned long long)S48);
            else if (S48 >= MASK48 - (1ULL << 31) + 1) printf("# GUESS small/text seed: %lld (отрицательный int32)\n", (long long)(i64)(S48 | 0xFFFF000000000000ULL));
            continue;
        }
        n_determined += ns; total_out += ns;
        for (size_t k = 0; k < ns; k++) {
            printf("%lld\t0x%016llx\t# struct=%llu hi16=0x%04llx%s%s%s\n", (long long)surv[k], (unsigned long long)surv[k], (unsigned long long)(surv[k] & MASK48), (unsigned long long)(surv[k] >> 48),
                   have_hashed ? " hashed" : "", bl.n ? " biomes" : "", use_random ? " random-seed-assumption" : "");
        }
        if (use_random && !have_hashed && bl.n == 0) printf("# NOTE struct=%llu: верхние 16 бит выведены в предположении, что seed сгенерирован игрой (пустое поле seed); для введённого вручную — --hashed/--biomes\n", (unsigned long long)S48);
        if (small_hint && use_random) printf("# GUESS small/text seed: %lld (если seed введён вручную как число/строка)\n", S48 < (1ULL << 31) ? (long long)S48 : (long long)(i64)(S48 | 0xFFFF000000000000ULL));
        if (ns > 1) fprintf(stderr, "предупреждение: structure seed %llu: прошло %zu world seed (мало наблюдений?)\n", (unsigned long long)S48, ns);
    }
    fflush(stdout);
    fprintf(stderr, "crack-lift64: кандидатов structure seed %zu, потоков %d: world seed найдено %zu, structure seed без сужения верхних бит %zu, отвергнуто %zu; %.3f с\n",
            nc, nthreads, n_determined, n_verified_only, n_none, now() - t_start);
    (void)total_out; free(surv);
    return (n_determined + n_verified_only) ? 0 : 1;
}

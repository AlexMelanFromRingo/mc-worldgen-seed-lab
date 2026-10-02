/* mesh.c — C-ядро меширования чанка (поток W4). См. mcgen_mesh.h. C11, без внешних зависимостей, потокобезопасно (состояния нет). */
#include "mcgen_mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MASK48 ((1ULL << 48) - 1)
#define FULL_MASK_ID 1
#define MAX_QY 160 /* до 640 блоков высоты */

static const int DIR_OPP[6] = { 1, 0, 3, 2, 5, 4 };
static const int DIR_DX[6] = { 0, 0, 0, 0, -1, 1 };
static const int DIR_DY[6] = { -1, 1, 0, 0, 0, 0 };
static const int DIR_DZ[6] = { 0, 0, -1, 1, 0, 0 };

int mcmesh_abi_version(void) { return MCMESH_ABI_VERSION; }

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                        java.util.Random / LegacyRandomSource
 * ---------------------------------------------------------------------------------------------------------------------------- */
typedef struct { uint64_t seed; } Lcg;

static inline void lcg_set(Lcg *r, int64_t s) { r->seed = ((uint64_t)s ^ 0x5DEECE66DULL) & MASK48; }
static inline int32_t lcg_next(Lcg *r, int bits)
{
    r->seed = (r->seed * 0x5DEECE66DULL + 0xBULL) & MASK48;
    return (int32_t)(uint32_t)(r->seed >> (48 - bits));
}
static inline int32_t lcg_next_int(Lcg *r, int32_t bound)
{
    int32_t v = lcg_next(r, 31);
    int32_t m = bound - 1;
    if ((bound & m) == 0) {
        v = (int32_t)(((int64_t)bound * (int64_t)v) >> 31);
    } else {
        for (int32_t u = v;; u = lcg_next(r, 31)) {
            v = u % bound;
            if ((int32_t)((uint32_t)u - (uint32_t)v + (uint32_t)m) >= 0) break;
        }
    }
    return v;
}
static inline int64_t lcg_next_long(Lcg *r)
{
    int64_t hi = (int64_t)lcg_next(r, 32);
    int64_t lo = (int64_t)lcg_next(r, 32);
    return (int64_t)((uint64_t)(hi << 32) + (uint64_t)lo);
}

static inline int64_t mth_get_seed(int32_t x, int32_t y, int32_t z)
{
    uint64_t s = (uint64_t)(int64_t)(int32_t)((uint32_t)x * 3129871u) ^ ((uint64_t)(int64_t)z * 116129781ULL) ^ (uint64_t)(int64_t)y;
    s = s * s * 42317861ULL + s * 11ULL;
    return (int64_t)s >> 16;
}

int64_t mcmesh_mth_get_seed(int32_t x, int32_t y, int32_t z) { return mth_get_seed(x, y, z); }
int mcmesh_pick_weighted(int64_t seed, int32_t total)
{
    Lcg r;
    lcg_set(&r, seed);
    return lcg_next_int(&r, total);
}

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                         шум болот (SimplexNoise 2D)
 * ---------------------------------------------------------------------------------------------------------------------------- */
static const int8_t GRAD[16][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
                                    { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 }, { 1, 1, 0 }, { 0, -1, 1 }, { -1, 1, 0 }, { 0, -1, -1 } };

static inline double corner2(int gi, double x, double y)
{
    double t0 = 0.5 - x * x - y * y;
    if (t0 < 0.0) return 0.0;
    t0 *= t0;
    return t0 * t0 * (GRAD[gi][0] * x + GRAD[gi][1] * y);
}

float mcmesh_swamp_noise(const uint8_t *perm, double xin, double yin)
{
    const double F2 = 0.5 * (1.7320508075688772 - 1.0);
    const double G2 = (3.0 - 1.7320508075688772) / 6.0;
    double s = (xin + yin) * F2;
    int i = (int)floor(xin + s);
    int j = (int)floor(yin + s);
    double t = (i + j) * G2;
    double x0 = xin - (i - t);
    double y0 = yin - (j - t);
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }
    double x1 = x0 - i1 + G2, y1 = y0 - j1 + G2;
    double x2 = x0 - 1.0 + 2.0 * G2, y2 = y0 - 1.0 + 2.0 * G2;
    int ii = i & 0xFF, jj = j & 0xFF;
#define PERM(v) ((int)perm[(v) & 0xFF])
    int gi0 = PERM(ii + PERM(jj)) % 12;
    int gi1 = PERM(ii + i1 + PERM(jj + j1)) % 12;
    int gi2 = PERM(ii + 1 + PERM(jj + 1)) % 12;
#undef PERM
    return (float)(70.0 * (corner2(gi0, x0, y0) + corner2(gi1, x1, y1) + corner2(gi2, x2, y2)));
}

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                                    контекст
 * ---------------------------------------------------------------------------------------------------------------------------- */
typedef struct Ctx {
    const McMeshTables *t;
    const McMeshOptions *o;
    McMeshOutput *out;
    const uint16_t *B[9];
    const uint8_t *Bio[9];
    int H, nqy;
    int ax0, az0; /* абсолютные координаты начала чанка */
    uint32_t opt;
    int blend;
    uint32_t *tint_map[MAX_QY * 4]; /* лениво: [qy][вид-2] */
    uint8_t region[2][17][32];      /* маски полос жидкости: [ось Z/X][число строк] */
    float scale;
    float yoff;
    int err;
    /* кандидаты на слияние */
    struct Cand *cand;
    int ncand, capcand;
} Ctx;

typedef struct Cand {
    float pos[12];      /* MC-оси, блоки от начала чанка */
    float uv[8];
    uint8_t rgb[3];
    uint8_t mat, dir, corner;   /* corner: порядок вершин относительно ячейки (по 2 бита на вершину) */
    uint32_t block;
    float plane;
    int32_t a, b;       /* ячейка в плоскости грани */
} Cand;

static inline int get_state(const Ctx *c, int x, int y, int z)
{
    if ((unsigned)y >= (unsigned)c->H) return -1;
    int dx = x < 0 ? 0 : (x >= 16 ? 2 : 1);
    int dz = z < 0 ? 0 : (z >= 16 ? 2 : 1);
    const uint16_t *b = c->B[dz * 3 + dx];
    if (!b) return -1;
    return b[(((y << 4) + (z & 15)) << 4) + (x & 15)];
}
static inline uint32_t sflags(const Ctx *c, int s) { return s < 0 ? 0u : c->t->st_flags[s]; }

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                                    выход
 * ---------------------------------------------------------------------------------------------------------------------------- */
static int out_reserve(McMeshOutput *o, int need)
{
    if (o->n_quads + need <= o->capacity) return 0;
    int cap = o->capacity ? o->capacity : 8192;
    while (cap < o->n_quads + need) cap *= 2;
    float *p = (float *)realloc(o->pos, (size_t)cap * 12 * sizeof(float));
    if (!p) return -1;
    o->pos = p;
    float *u = (float *)realloc(o->uv, (size_t)cap * 8 * sizeof(float));
    if (!u) return -1;
    o->uv = u;
    uint8_t *cl = (uint8_t *)realloc(o->col, (size_t)cap * 16);
    if (!cl) return -1;
    o->col = cl;
    uint8_t *m = (uint8_t *)realloc(o->mat, (size_t)cap);
    if (!m) return -1;
    o->mat = m;
    uint32_t *b = (uint32_t *)realloc(o->block, (size_t)cap * sizeof(uint32_t));
    if (!b) return -1;
    o->block = b;
    uint8_t *d = (uint8_t *)realloc(o->dir, (size_t)cap);
    if (!d) return -1;
    o->dir = d;
    uint8_t *mg = (uint8_t *)realloc(o->merged, (size_t)cap);
    if (!mg) return -1;
    o->merged = mg;
    float *rc = (float *)realloc(o->rect, (size_t)cap * 4 * sizeof(float));
    if (!rc) return -1;
    o->rect = rc;
    o->capacity = cap;
    return 0;
}

/* Итоговые байты цвета вершины: оттенок × (опционально) затенение. */
static inline void final_rgb(const Ctx *c, uint32_t rgb, float shade, uint8_t out[3])
{
    uint8_t r = (uint8_t)((rgb >> 16) & 255), g = (uint8_t)((rgb >> 8) & 255), b = (uint8_t)(rgb & 255);
    if (c->opt & MCM_OPT_BAKE_SHADE) {
        r = (uint8_t)(r * shade + 0.5f);
        g = (uint8_t)(g * shade + 0.5f);
        b = (uint8_t)(b * shade + 0.5f);
    }
    out[0] = r; out[1] = g; out[2] = b;
}

/* Пишет квад в выход: pos — 4 вершины (x,y,z) в блоках от начала чанка (оси Minecraft), uv — 4 пары; merged: uv уже в системе выхода. */
static void write_quad(Ctx *c, const float *pos, const float *uv, const uint8_t rgb[3], int mat, uint32_t blockIdx, int dir, int merged, const float *rect)
{
    McMeshOutput *o = c->out;
    if (out_reserve(o, 1)) { c->err = -2; return; }
    int n = o->n_quads++;
    float *p = o->pos + (size_t)n * 12;
    float sc = c->scale;
    if (c->opt & MCM_OPT_BLENDER_AXES) {
        for (int i = 0; i < 4; i++) {
            p[i * 3 + 0] = pos[i * 3 + 0] * sc;
            p[i * 3 + 1] = -pos[i * 3 + 2] * sc;
            p[i * 3 + 2] = (pos[i * 3 + 1] + c->yoff) * sc;
        }
    } else {
        for (int i = 0; i < 4; i++) {
            p[i * 3 + 0] = pos[i * 3 + 0] * sc;
            p[i * 3 + 1] = (pos[i * 3 + 1] + c->yoff) * sc;
            p[i * 3 + 2] = pos[i * 3 + 2] * sc;
        }
    }
    float *q = o->uv + (size_t)n * 8;
    if ((c->opt & MCM_OPT_BLENDER_UV) && !merged) {
        for (int i = 0; i < 4; i++) { q[i * 2] = uv[i * 2]; q[i * 2 + 1] = 1.0f - uv[i * 2 + 1]; }
    } else {
        memcpy(q, uv, 8 * sizeof(float));
    }
    uint8_t *cl = o->col + (size_t)n * 16;
    for (int i = 0; i < 4; i++) { cl[i * 4] = rgb[0]; cl[i * 4 + 1] = rgb[1]; cl[i * 4 + 2] = rgb[2]; cl[i * 4 + 3] = 255; }
    o->mat[n] = (uint8_t)mat;
    o->block[n] = blockIdx;
    o->dir[n] = (uint8_t)dir;
    o->merged[n] = (uint8_t)merged;
    if (merged) memcpy(o->rect + (size_t)n * 4, rect, 4 * sizeof(float));
    else memset(o->rect + (size_t)n * 4, 0, 4 * sizeof(float));
    o->n_mat[mat]++;
}

/* Добавляет квад (или кандидата на слияние). */
static inline void emit_quad(Ctx *c, const float *pos, const float *uv, uint32_t rgb, float shade, int mat, uint32_t blockIdx, int dir, int mergeable)
{
    uint8_t b3[3];
    final_rgb(c, rgb, shade, b3);
    if (mergeable && (c->opt & MCM_OPT_MERGE)) {
        if (c->ncand == c->capcand) {
            int nc = c->capcand ? c->capcand * 2 : 4096;
            Cand *p = (Cand *)realloc(c->cand, (size_t)nc * sizeof(Cand));
            if (!p) { c->err = -2; return; }
            c->cand = p;
            c->capcand = nc;
        }
        Cand *k = &c->cand[c->ncand++];
        memcpy(k->pos, pos, 12 * sizeof(float));
        memcpy(k->uv, uv, 8 * sizeof(float));
        k->rgb[0] = b3[0]; k->rgb[1] = b3[1]; k->rgb[2] = b3[2];
        k->mat = (uint8_t)mat; k->dir = (uint8_t)dir;
        k->block = blockIdx;
        int axis = dir < 2 ? 1 : (dir < 4 ? 2 : 0);
        k->plane = pos[axis];
        int bx = (int)(blockIdx & 15), bz = (int)((blockIdx >> 4) & 15), by = (int)(blockIdx >> 8);
        if (dir < 2) { k->a = bx; k->b = bz; }
        else if (dir < 4) { k->a = bx; k->b = by; }
        else { k->a = bz; k->b = by; }
        {   /* код порядка вершин: (ka, kb) каждой вершины относительно ячейки */
            int pa = dir < 4 ? 0 : 2, pb = dir < 2 ? 2 : 1;
            unsigned code = 0;
            for (int i = 0; i < 4; i++) {
                int ka = (int)floorf(pos[i * 3 + pa] - (float)k->a + 0.5f) & 1;
                int kb = (int)floorf(pos[i * 3 + pb] - (float)k->b + 0.5f) & 1;
                code |= (unsigned)((ka << 1) | kb) << (2 * i);
            }
            k->corner = (uint8_t)code;
        }
        return;
    }
    write_quad(c, pos, uv, b3, mat, blockIdx, dir, 0, NULL);
}

static int cand_cmp(const void *pa, const void *pb)
{
    const Cand *x = (const Cand *)pa, *y = (const Cand *)pb;
    if (x->dir != y->dir) return (int)x->dir - (int)y->dir;
    if (x->mat != y->mat) return (int)x->mat - (int)y->mat;
    int d = memcmp(x->rgb, y->rgb, 3);
    if (d) return d;
    if (x->corner != y->corner) return (int)x->corner - (int)y->corner;
    if (x->plane != y->plane) return x->plane < y->plane ? -1 : 1;
    d = memcmp(x->uv, y->uv, 8 * sizeof(float));
    if (d) return d;
    if (x->b != y->b) return x->b < y->b ? -1 : 1;
    if (x->a != y->a) return x->a < y->a ? -1 : 1;
    return 0;
}

static int cand_same_group(const Cand *x, const Cand *y)
{
    return x->dir == y->dir && x->mat == y->mat && x->corner == y->corner && !memcmp(x->rgb, y->rgb, 3) && x->plane == y->plane && !memcmp(x->uv, y->uv, 8 * sizeof(float));
}

typedef struct Rect { int a0, len, b0, h; int tmpl; } Rect;

/* Выпускает прямоугольник w×h ячеек, шаблон — кандидат tmpl (его ячейка (a0,b0) — начало). */
static void emit_rect(Ctx *c, const Cand *k, int w, int h)
{
    if (w == 1 && h == 1) {
        write_quad(c, k->pos, k->uv, k->rgb, k->mat, k->block, k->dir, 0, NULL);
        return;
    }
    int dir = k->dir;
    int pa = dir < 4 ? 0 : 2;           /* ось a: x (для U/D, N/S) либо z (для W/E) */
    int pb = dir < 2 ? 2 : 1;           /* ось b: z (U/D) либо y */
    float base_a = (float)k->a, base_b = (float)k->b;
    float pos[12], uv[8];
    int ka[4], kb[4];
    for (int i = 0; i < 4; i++) {
        ka[i] = (int)floorf(k->pos[i * 3 + pa] - base_a + 0.5f);
        kb[i] = (int)floorf(k->pos[i * 3 + pb] - base_b + 0.5f);
        memcpy(pos + i * 3, k->pos + i * 3, 3 * sizeof(float));
        pos[i * 3 + pa] = base_a + (float)(ka[i] * w);
        pos[i * 3 + pb] = base_b + (float)(kb[i] * h);
    }
    float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
    for (int i = 0; i < 4; i++) {
        float u = k->uv[i * 2], v = k->uv[i * 2 + 1];
        umin = fminf(umin, u); umax = fmaxf(umax, u); vmin = fminf(vmin, v); vmax = fmaxf(vmax, v);
    }
    int lu[4], lv[4];
    for (int i = 0; i < 4; i++) {
        lu[i] = k->uv[i * 2] > 0.5f * (umin + umax);
        lv[i] = k->uv[i * 2 + 1] > 0.5f * (vmin + vmax);
    }
    /* какая ось плоскости управляет u: lu == ka (или обратно) для всех вершин */
    int u_by_a = 1;
    for (int i = 0; i < 4; i++) if (lu[i] != ka[i] && lu[i] != 1 - ka[i]) u_by_a = 0;
    if (u_by_a) {
        int dir_fwd = 1, dir_bwd = 1;
        for (int i = 0; i < 4; i++) { if (lu[i] != ka[i]) dir_fwd = 0; if (lu[i] != 1 - ka[i]) dir_bwd = 0; }
        if (!dir_fwd && !dir_bwd) u_by_a = 0;
    }
    float ext_u = u_by_a ? (float)w : (float)h;
    float ext_v = u_by_a ? (float)h : (float)w;
    int blender_uv = (c->opt & MCM_OPT_BLENDER_UV) != 0;
    for (int i = 0; i < 4; i++) {
        uv[i * 2] = (float)lu[i] * ext_u;
        uv[i * 2 + 1] = blender_uv ? (float)(1 - lv[i]) * ext_v : (float)lv[i] * ext_v;
    }
    float rect[4];
    rect[0] = umin;
    rect[2] = umax - umin;
    if (blender_uv) { rect[1] = 1.0f - vmax; rect[3] = vmax - vmin; }
    else { rect[1] = vmin; rect[3] = vmax - vmin; }
    write_quad(c, pos, uv, k->rgb, k->mat, k->block, k->dir, 1, rect);
    c->out->n_merged_from += w * h;
}

static void merge_pass(Ctx *c)
{
    int n = c->ncand;
    if (n == 0) return;
    qsort(c->cand, (size_t)n, sizeof(Cand), cand_cmp);
    Rect *open = (Rect *)malloc(sizeof(Rect) * 512);
    if (!open) { c->err = -2; return; }
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && cand_same_group(&c->cand[i], &c->cand[j])) j++;
        /* группа [i, j): ячейки отсортированы по (b, a) */
        int nopen = 0, prev_b = -1000;
        int k = i;
        while (k < j) {
            int b = c->cand[k].b;
            int rowend = k;
            while (rowend < j && c->cand[rowend].b == b) rowend++;
            if (nopen > 0 && b != prev_b + 1) {
                for (int r = 0; r < nopen; r++) emit_rect(c, &c->cand[open[r].tmpl], open[r].len, open[r].h);
                nopen = 0;
            }
            /* сегменты ряда */
            int extended[512];
            for (int r = 0; r < nopen && r < 512; r++) extended[r] = 0;
            Rect next[512];
            int nnext = 0;
            int m = k;
            while (m < rowend) {
                int a0 = c->cand[m].a, len = 1;
                while (m + len < rowend && c->cand[m + len].a == a0 + len) len++;
                int found = -1;
                for (int r = 0; r < nopen && r < 512; r++)
                    if (!extended[r] && open[r].a0 == a0 && open[r].len == len) { found = r; break; }
                if (found >= 0 && nnext < 512) {
                    extended[found] = 1;
                    next[nnext] = open[found];
                    next[nnext].h++;
                    nnext++;
                } else if (nnext < 512) {
                    next[nnext].a0 = a0; next[nnext].len = len; next[nnext].b0 = b; next[nnext].h = 1; next[nnext].tmpl = m;
                    nnext++;
                } else {
                    emit_rect(c, &c->cand[m], len, 1);
                }
                m += len;
            }
            for (int r = 0; r < nopen && r < 512; r++)
                if (!extended[r]) emit_rect(c, &c->cand[open[r].tmpl], open[r].len, open[r].h);
            for (int r = 0; r < nnext; r++) open[r] = next[r];
            nopen = nnext;
            prev_b = b;
            k = rowend;
        }
        for (int r = 0; r < nopen; r++) emit_rect(c, &c->cand[open[r].tmpl], open[r].len, open[r].h);
        i = j;
    }
    free(open);
}

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                         цвета биомов (оттенок), окно (2r+1)²
 * ---------------------------------------------------------------------------------------------------------------------------- */
static inline int biome_at(const Ctx *c, int x, int qy, int z)
{
    int dx = x < 0 ? 0 : (x >= 16 ? 2 : 1);
    int dz = z < 0 ? 0 : (z >= 16 ? 2 : 1);
    const uint8_t *b = c->Bio[dz * 3 + dx];
    if (!b) {
        b = c->Bio[4];
        if (!b) return 0;
        x = x < 0 ? 0 : (x > 15 ? 15 : x);
        z = z < 0 ? 0 : (z > 15 ? 15 : z);
    }
    int v = b[(qy * 4 + ((z & 15) >> 2)) * 4 + ((x & 15) >> 2)];
    return v < c->t->n_biomes ? v : 0;
}

static inline uint32_t biome_color(const Ctx *c, int b, int kind, int wx, int wz)
{
    const McMeshTables *t = c->t;
    if (kind == MCM_TINT_GRASS) {
        if (t->biome_mod[b] == 2) return mcmesh_swamp_noise(t->swamp_perm, wx * 0.0225, wz * 0.0225) < -0.1 ? 0x4C763Cu : 0x6A7039u;
        return t->biome_rgb[b * 4 + 0] & 0xFFFFFFu;
    }
    return t->biome_rgb[b * 4 + (kind - MCM_TINT_GRASS)] & 0xFFFFFFu;
}

static uint32_t *get_tint_map(Ctx *c, int qy, int kind)
{
    int slot = qy * 4 + (kind - 2);
    uint32_t *m = c->tint_map[slot];
    if (m) return m;
    m = (uint32_t *)malloc(256 * sizeof(uint32_t));
    if (!m) { c->err = -2; return NULL; }
    c->tint_map[slot] = m;
    int r = c->blend;
    int W = 16 + 2 * r;
    /* отсчёты цвета по окну */
    uint32_t samp[30 * 30];
    for (int zz = -r; zz < 16 + r; zz++)
        for (int xx = -r; xx < 16 + r; xx++) {
            int b = biome_at(c, xx, qy, zz);
            samp[(zz + r) * W + (xx + r)] = biome_color(c, b, kind, c->ax0 + xx, c->az0 + zz);
        }
    if (r == 0) {
        memcpy(m, samp, 256 * sizeof(uint32_t));
        return m;
    }
    int n = (2 * r + 1) * (2 * r + 1);
    int tmp[3][30 * 16];
    for (int zz = 0; zz < W; zz++)
        for (int x = 0; x < 16; x++) {
            int sr = 0, sg = 0, sb = 0;
            for (int dx = 0; dx <= 2 * r; dx++) {
                uint32_t col = samp[zz * W + x + dx];
                sr += (col >> 16) & 255; sg += (col >> 8) & 255; sb += col & 255;
            }
            tmp[0][zz * 16 + x] = sr; tmp[1][zz * 16 + x] = sg; tmp[2][zz * 16 + x] = sb;
        }
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int sr = 0, sg = 0, sb = 0;
            for (int dz = 0; dz <= 2 * r; dz++) {
                sr += tmp[0][(z + dz) * 16 + x]; sg += tmp[1][(z + dz) * 16 + x]; sb += tmp[2][(z + dz) * 16 + x];
            }
            m[z * 16 + x] = ((uint32_t)((sr / n) & 255) << 16) | ((uint32_t)((sg / n) & 255) << 8) | (uint32_t)((sb / n) & 255);
        }
    return m;
}

static inline uint32_t tint_at(Ctx *c, int kind, int x, int y, int z)
{
    int qy = y >> 2;
    uint32_t *m = get_tint_map(c, qy, kind);
    return m ? m[z * 16 + x] : 0xFFFFFFu;
}

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                         отсечение граней (Block.shouldRenderFace)
 * ---------------------------------------------------------------------------------------------------------------------------- */
static inline int skip_rendering(const Ctx *c, int sa, uint32_t fa, int sb, uint32_t fb, int dir)
{
    const McMeshTables *t = c->t;
    int cls = (int)((fa >> MCM_SKIP_SHIFT) & 7);
    switch (cls) {
    case MCM_SKIP_NONE: return 0;
    case MCM_SKIP_SAME:
    case MCM_SKIP_POWDER: return sb >= 0 && t->st_block[sb] == t->st_block[sa];
    case MCM_SKIP_LEAVES: return !(c->opt & MCM_OPT_CUTOUT_LEAVES) && (fb & MCM_F_LEAVES);
    case MCM_SKIP_ROOTS: return sb >= 0 && (int)t->st_block[sb] == t->mangrove_roots_block && dir < 2;
    case MCM_SKIP_LIQUID: return (fb & (MCM_F_WATER | MCM_F_LAVA)) && (((fa & MCM_F_WATER) != 0) == ((fb & MCM_F_WATER) != 0));
    case MCM_SKIP_BARS: {
        if (sb < 0) return 0;
        if (t->st_block[sb] == t->st_block[sa] || ((fa & MCM_F_BARS) && (fb & MCM_F_BARS))) {
            if (dir < 2) return 1;
            int ca = (fa >> (MCM_CONN_SHIFT + dir - 2)) & 1;
            int cb = (fb >> (MCM_CONN_SHIFT + DIR_OPP[dir] - 2)) & 1;
            if (ca && cb) return 1;
        }
        return 0;
    }
    }
    return 0;
}

/* sa — состояние блока (не -1), sb — сосед (-1 = воздух/неизвестно) */
static inline int should_render_face(const Ctx *c, int sa, int sb, int dir)
{
    const McMeshTables *t = c->t;
    int occ = sb >= 0 ? t->st_occ[(size_t)sb * 6 + DIR_OPP[dir]] : 0;
    if (occ == FULL_MASK_ID) return 0;
    uint32_t fa = t->st_flags[sa];
    uint32_t fb = sflags(c, sb);
    if (skip_rendering(c, sa, fa, sb, fb, dir)) return 0;
    if (occ == 0) return 1;
    int shape = t->st_occ[(size_t)sa * 6 + dir];
    if (shape == 0) return 1;
    return !t->occ_cover[(size_t)shape * t->n_masks + occ];
}

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                                 блоки с моделью
 * ---------------------------------------------------------------------------------------------------------------------------- */
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void model_block(Ctx *c, int x, int y, int z, int s, uint32_t f, uint32_t blockIdx)
{
    const McMeshTables *t = c->t;
    int g0 = t->st_grp_off[s], g1 = t->st_grp_off[s + 1];
    int ax = c->ax0 + x, az = c->az0 + z, ay = y + c->o->min_y;

    /* выбор вариантов по позиции */
    int pick_seed_valid = 0;
    int64_t pick_seed = 0;
    if (f & MCM_F_RANDOM) {
        int64_t seed0 = mth_get_seed(ax, ay, az);
        if (f & MCM_F_MULTIPART) {
            Lcg r;
            lcg_set(&r, seed0);
            pick_seed = lcg_next_long(&r);
        } else {
            pick_seed = seed0;
        }
        pick_seed_valid = 1;
    }
    /* смещение */
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    if (f & (MCM_F_OFF_XZ | MCM_F_OFF_XYZ)) {
        int64_t seed = mth_get_seed(ax, 0, az);
        double mh = (double)t->st_off_h[s];
        ox = (float)clampd(((double)((float)(seed & 15) / 15.0f) - 0.5) * 0.5, -mh, mh);
        oz = (float)clampd(((double)((float)((seed >> 8) & 15) / 15.0f) - 0.5) * 0.5, -mh, mh);
        if (f & MCM_F_OFF_XYZ) oy = (float)(((double)((float)((seed >> 4) & 15) / 15.0f) - 1.0) * (double)t->st_off_v[s]);
    }

    int8_t cull_cache[6] = { -1, -1, -1, -1, -1, -1 };
    int64_t tint_cache[6] = { -1, -1, -1, -1, -1, -1 }; /* по виду оттенка */
    int force_solid = !(c->opt & MCM_OPT_CUTOUT_LEAVES) && (f & MCM_F_LEAVES);
    float pos[12];

    for (int gi = g0; gi < g1; gi++) {
        int g = t->grp_list[gi];
        int v0 = t->grp_var_off[g], v1 = t->grp_var_off[g + 1];
        int vi = v0;
        int total = t->grp_total[g];
        if (total > 1 && pick_seed_valid && !(c->opt & MCM_OPT_NO_VARIANTS)) {
            Lcg r;
            lcg_set(&r, pick_seed);
            int sel = lcg_next_int(&r, total);
            for (vi = v0; vi < v1; vi++) {
                sel -= t->var_weight[vi];
                if (sel < 0) break;
            }
            if (vi >= v1) vi = v1 - 1;
        }
        int b = t->var_baked[vi];
        int q0 = t->baked_q_off[b], q1 = t->baked_q_off[b + 1];
        for (int q = q0; q < q1; q++) {
            int cull = t->q_cull[q];
            if (cull >= 0) {
                int r = cull_cache[cull];
                if (r < 0) {
                    int sb = get_state(c, x + DIR_DX[cull], y + DIR_DY[cull], z + DIR_DZ[cull]);
                    r = cull_cache[cull] = (int8_t)should_render_face(c, s, sb, cull);
                }
                if (!r) continue;
            }
            const float *qp = t->q_pos + (size_t)q * 12;
            for (int i = 0; i < 4; i++) {
                pos[i * 3 + 0] = x + ox + qp[i * 3 + 0];
                pos[i * 3 + 1] = y + oy + qp[i * 3 + 1];
                pos[i * 3 + 2] = z + oz + qp[i * 3 + 2];
            }
            int kind = t->q_tint[q];
            uint32_t rgb = 0xFFFFFFu;
            if (kind == MCM_TINT_CONST) {
                rgb = t->q_rgb[q];
            } else if (kind >= MCM_TINT_GRASS) {
                if (tint_cache[kind] < 0) tint_cache[kind] = tint_at(c, kind, x, (f & MCM_F_TINT_BELOW) && y > 0 ? y - 1 : y, z);
                rgb = (uint32_t)tint_cache[kind];
            }
            int layer = force_solid ? 0 : t->q_layer[q];
            emit_quad(c, pos, t->q_uv + (size_t)q * 8, rgb, c->o->shade[t->q_shade[q]], layer, blockIdx, t->q_dir[q],
                      t->q_merge[q] && !(f & (MCM_F_OFF_XZ | MCM_F_OFF_XYZ)));
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                                     жидкости
 * ---------------------------------------------------------------------------------------------------------------------------- */
static inline int fkind(uint32_t f) { return (f & MCM_F_WATER) ? 1 : ((f & MCM_F_LAVA) ? 2 : 0); }
static inline int famount(uint32_t f) { return (int)((f >> MCM_FLUID_AMOUNT_SHIFT) & 15); }

static inline int mask_bit(const uint8_t *m, int u, int v)
{
    int i = u * 16 + v;
    return (m[i >> 3] >> (7 - (i & 7))) & 1;
}

static void init_regions(Ctx *c)
{
    /* region[0]: для граней по оси X (u = y, v = z): строки u < n целиком; region[1]: для граней по оси Z (u = x, v = y): биты v < n */
    memset(c->region, 0, sizeof(c->region));
    for (int n = 0; n <= 16; n++) {
        for (int u = 0; u < 16; u++)
            for (int v = 0; v < 16; v++) {
                int i = u * 16 + v;
                if (u < n) c->region[0][n][i >> 3] |= (uint8_t)(0x80 >> (i & 7));
                if (v < n) c->region[1][n][i >> 3] |= (uint8_t)(0x80 >> (i & 7));
            }
    }
}

/* isFaceOccludedByState(dirX, height, sX): закрывает ли грань состояния sX (обращённая к жидкости) грань жидкого бруска высотой height */
static int fluid_occluded_by_state(const Ctx *c, int dirX, float height, int sX)
{
    if (sX < 0) return 0;
    const McMeshTables *t = c->t;
    int occ = t->st_occ[(size_t)sX * 6 + DIR_OPP[dirX]];
    if (occ == 0) return 0;
    if (occ == FULL_MASK_ID) return dirX != MCM_UP || height == 1.0f;
    /* частичная форма: блок жидкости 1×height×1 */
    const uint8_t *om = t->occ_masks + (size_t)occ * 32;
    if (dirX == MCM_UP) {
        if (height < 1.0f - 1e-7f) return 0;
        for (int i = 0; i < 32; i++) if (om[i] != 0xFF) return 0;
        return 1;
    }
    if (dirX == MCM_DOWN) {
        for (int i = 0; i < 32; i++) if (om[i] != 0xFF) return 0;
        return 1;
    }
    int rows = (int)ceilf(height * 16.0f - 1e-4f);
    if (rows > 16) rows = 16;
    const uint8_t *rg = c->region[dirX >= 4 ? 0 : 1][rows];
    for (int i = 0; i < 32; i++) if (rg[i] & ~om[i]) return 0;
    return 1;
}

static float fluid_height_at(const Ctx *c, int x, int y, int z, int kind)
{
    int s = get_state(c, x, y, z);
    uint32_t f = sflags(c, s);
    if (fkind(f) == kind) {
        int sa = get_state(c, x, y + 1, z);
        return fkind(sflags(c, sa)) == kind ? 1.0f : (float)famount(f) / 9.0f;
    }
    return (f & MCM_F_SOLID) ? -1.0f : 0.0f;
}
static float fluid_height_known(const Ctx *c, int x, int y, int z, int kind, int s, uint32_t f)
{
    if (fkind(f) == kind) {
        int sa = get_state(c, x, y + 1, z);
        return fkind(sflags(c, sa)) == kind ? 1.0f : (float)famount(f) / 9.0f;
    }
    (void)s;
    return (f & MCM_F_SOLID) ? -1.0f : 0.0f;
}

static float avg_height(const Ctx *c, int kind, float hSelf, float h2, float h1, int cx, int cy, int cz)
{
    if (h1 >= 1.0f || h2 >= 1.0f) return 1.0f;
    float wsum = 0.0f, wcnt = 0.0f;
    if (h1 > 0.0f || h2 > 0.0f) {
        float hc = fluid_height_at(c, cx, cy, cz, kind);
        if (hc >= 1.0f) return 1.0f;
        if (hc >= 0.8f) { wsum += hc * 10.0f; wcnt += 10.0f; }
        else if (hc >= 0.0f) { wsum += hc; wcnt += 1.0f; }
    }
    float hs[3] = { hSelf, h1, h2 };
    for (int i = 0; i < 3; i++) {
        float h = hs[i];
        if (h >= 0.8f) { wsum += h * 10.0f; wcnt += 10.0f; }
        else if (h >= 0.0f) { wsum += h; wcnt += 1.0f; }
    }
    return wsum / wcnt;
}

/* FlowingFluid.getFlow: возвращает нормализованный (x, z) потока; (0,0) — стоячая */
static void fluid_flow(const Ctx *c, int x, int y, int z, int kind, float selfHeight, int falling, double *ofx, double *ofz)
{
    static const int HD[4] = { MCM_NORTH, MCM_EAST, MCM_SOUTH, MCM_WEST };
    double fx = 0.0, fz = 0.0;
    for (int i = 0; i < 4; i++) {
        int d = HD[i];
        int nx = x + DIR_DX[d], nz = z + DIR_DZ[d];
        int sn = get_state(c, nx, y, nz);
        uint32_t fn = sflags(c, sn);
        int kn = fkind(fn);
        if (kn == 0 || kn == kind) {
            float nh = kn ? (float)famount(fn) / 9.0f : 0.0f;
            float dist = 0.0f;
            if (nh == 0.0f) {
                if (!(fn & MCM_F_BLOCKS_FLOW)) {
                    int sb = get_state(c, nx, y - 1, nz);
                    uint32_t fb = sflags(c, sb);
                    int kb = fkind(fb);
                    if (kb == 0 || kb == kind) {
                        nh = kb ? (float)famount(fb) / 9.0f : 0.0f;
                        if (nh > 0.0f) dist = selfHeight - (nh - 0.8888889f);
                    }
                }
            } else if (nh > 0.0f) {
                dist = selfHeight - nh;
            }
            if (dist != 0.0f) { fx += DIR_DX[d] * (double)dist; fz += DIR_DZ[d] * (double)dist; }
        }
    }
    double len = sqrt(fx * fx + fz * fz);
    double nx = 0, nz = 0, ny = 0;
    if (len >= 1.0e-5) { nx = fx / len; nz = fz / len; }
    if (falling) {
        for (int i = 0; i < 4; i++) {
            int d = HD[i];
            int px = x + DIR_DX[d], pz = z + DIR_DZ[d];
            int found = 0;
            for (int up = 0; up < 2 && !found; up++) {
                int sp = get_state(c, px, y + up, pz);
                uint32_t fp = sflags(c, sp);
                if (fkind(fp) == kind) continue; /* жидкость той же — не твёрдая грань */
                if (sp >= 0 && (c->t->st_sturdy[sp] >> d) & 1) found = 1;
            }
            if (found) {
                ny = -6.0;
                double l2 = sqrt(nx * nx + ny * ny + nz * nz);
                nx /= l2; ny /= l2; nz /= l2;
                *ofx = nx; *ofz = nz;
                return;
            }
        }
    }
    double l = sqrt(nx * nx + ny * ny + nz * nz);
    if (l < 1.0e-5) { *ofx = 0.0; *ofz = 0.0; return; }
    *ofx = nx / l; *ofz = nz / l;
}

static inline void set_vert(float *p, float *uv, int i, float x, float y, float z, float u, float v)
{
    p[i * 3] = x; p[i * 3 + 1] = y; p[i * 3 + 2] = z;
    uv[i * 2] = u; uv[i * 2 + 1] = v;
}

static void fluid_face(Ctx *c, const float *p, const float *uv, uint32_t rgb, float shade, int mat, uint32_t blockIdx, int dir, int back, int mergeable)
{
    emit_quad(c, p, uv, rgb, shade, mat, blockIdx, dir, mergeable);
    if (back) {
        float p2[12], u2[8];
        static const int order[4] = { 0, 3, 2, 1 };
        for (int i = 0; i < 4; i++) {
            memcpy(p2 + i * 3, p + order[i] * 3, 3 * sizeof(float));
            memcpy(u2 + i * 2, uv + order[i] * 2, 2 * sizeof(float));
        }
        emit_quad(c, p2, u2, rgb, shade, mat, blockIdx, dir, mergeable);
    }
}

static void fluid_block(Ctx *c, int x, int y, int z, int s, uint32_t f, uint32_t blockIdx)
{
    const McMeshTables *t = c->t;
    int kind = fkind(f);
    int sD = get_state(c, x, y - 1, z), sU = get_state(c, x, y + 1, z);
    int sN = get_state(c, x, y, z - 1), sS = get_state(c, x, y, z + 1);
    int sW = get_state(c, x - 1, y, z), sE = get_state(c, x + 1, y, z);
    uint32_t fD = sflags(c, sD), fU = sflags(c, sU), fN = sflags(c, sN), fS = sflags(c, sS), fW = sflags(c, sW), fE = sflags(c, sE);

    int renderUp = fkind(fU) != kind;
    /* shouldRenderFace(fluid, self, dir, neighborFluid) = !same && !occludedBySelf */
#define SELF_OCC(dir) fluid_occluded_by_state(c, DIR_OPP[dir], 1.0f, s)
    int renderDown = fkind(fD) != kind && !SELF_OCC(MCM_DOWN) && !fluid_occluded_by_state(c, MCM_DOWN, 0.8888889f, sD);
    int renderN = fkind(fN) != kind && !SELF_OCC(MCM_NORTH);
    int renderS = fkind(fS) != kind && !SELF_OCC(MCM_SOUTH);
    int renderW = fkind(fW) != kind && !SELF_OCC(MCM_WEST);
    int renderE = fkind(fE) != kind && !SELF_OCC(MCM_EAST);
#undef SELF_OCC
    if (!(renderUp || renderDown || renderN || renderS || renderW || renderE)) return;

    int mat = kind == 1 ? MCM_MAT_WATER : t->fluid_layer[1];
    uint32_t tint = 0xFFFFFFu;
    if (kind == 1) tint = tint_at(c, MCM_TINT_WATER, x, y, z);

    float hSelf = fluid_height_known(c, x, y, z, kind, s, f);
    float hNE, hNW, hSE, hSW;
    if (hSelf >= 1.0f) {
        hNE = hNW = hSE = hSW = 1.0f;
    } else {
        float hN = fluid_height_known(c, x, y, z - 1, kind, sN, fN);
        float hS = fluid_height_known(c, x, y, z + 1, kind, sS, fS);
        float hE = fluid_height_known(c, x + 1, y, z, kind, sE, fE);
        float hW = fluid_height_known(c, x - 1, y, z, kind, sW, fW);
        hNE = avg_height(c, kind, hSelf, hN, hE, x + 1, y, z - 1);
        hNW = avg_height(c, kind, hSelf, hN, hW, x - 1, y, z - 1);
        hSE = avg_height(c, kind, hSelf, hS, hE, x + 1, y, z + 1);
        hSW = avg_height(c, kind, hSelf, hS, hW, x - 1, y, z + 1);
    }
    const float *sh = c->o->shade;
    float fx = (float)x, fy = (float)y, fz = (float)z;
    const float OFFS = 0.001f;
    float bottomOffs = renderDown ? OFFS : 0.0f;
    float P[12], UV[8];
    int base = kind == 1 ? 0 : 3; /* индекс still в fluid_rect: вода 0, лава 3; flow = +1; overlay (только вода) = 2 */

    if (renderUp) {
        float minH = fminf(fminf(hNW, hSW), fminf(hSE, hNE));
        if (!fluid_occluded_by_state(c, MCM_UP, minH, sU)) {
            hNW -= OFFS; hSW -= OFFS; hSE -= OFFS; hNE -= OFFS;
            double flx, flz;
            fluid_flow(c, x, y, z, kind, (float)famount(f) / 9.0f, (f & MCM_F_FALLING) != 0, &flx, &flz);
            float u00, v00, u01, v01, u10, v10, u11, v11;
            int still_flat = (flx == 0.0 && flz == 0.0) && hNW == hSW && hSW == hSE && hSE == hNE;
            if (flx == 0.0 && flz == 0.0) {
                const float *r = t->fluid_rect[base];
                u00 = r[0]; v00 = r[1];
                u01 = u00; v01 = r[3];
                u10 = r[2]; v10 = v01;
                u11 = u10; v11 = v00;
            } else {
                const float *r = t->fluid_rect[base + 1];
                float angle = (float)atan2(flz, flx) - 1.5707964f;
                float sn = sinf(angle) * 0.25f, cs = cosf(angle) * 0.25f;
#define GU(fr) (r[0] + (r[2] - r[0]) * (fr))
#define GV(fr) (r[1] + (r[3] - r[1]) * (fr))
                u00 = GU(0.5f + (-cs - sn)); v00 = GV(0.5f + (-cs + sn));
                u01 = GU(0.5f + (-cs + sn)); v01 = GV(0.5f + (cs + sn));
                u10 = GU(0.5f + (cs + sn)); v10 = GV(0.5f + (cs - sn));
                u11 = GU(0.5f + (cs - sn)); v11 = GV(0.5f + (-cs - sn));
            }
            /* обратная грань поверхности: shouldRenderBackwardUpFace(above) */
            int back = 0;
            for (int ox = -1; ox <= 1 && !back; ox++)
                for (int oz = -1; oz <= 1; oz++) {
                    int sp = get_state(c, x + ox, y + 1, z + oz);
                    uint32_t fp = sflags(c, sp);
                    if (fkind(fp) != kind && !(fp & MCM_F_OPAQUE)) { back = 1; break; }
                }
            set_vert(P, UV, 0, fx + 0.0f, fy + hNW, fz + 0.0f, u00, v00);
            set_vert(P, UV, 1, fx + 0.0f, fy + hSW, fz + 1.0f, u01, v01);
            set_vert(P, UV, 2, fx + 1.0f, fy + hSE, fz + 1.0f, u10, v10);
            set_vert(P, UV, 3, fx + 1.0f, fy + hNE, fz + 0.0f, u11, v11);
            fluid_face(c, P, UV, tint, sh[MCM_UP], mat, blockIdx, MCM_UP, back, still_flat);
        }
    }
    if (renderDown) {
        const float *r = t->fluid_rect[base];
        set_vert(P, UV, 0, fx, fy + bottomOffs, fz, r[0], r[1]);
        set_vert(P, UV, 1, fx + 1.0f, fy + bottomOffs, fz, r[2], r[1]);
        set_vert(P, UV, 2, fx + 1.0f, fy + bottomOffs, fz + 1.0f, r[2], r[3]);
        set_vert(P, UV, 3, fx, fy + bottomOffs, fz + 1.0f, r[0], r[3]);
        fluid_face(c, P, UV, tint, sh[MCM_DOWN], mat, blockIdx, MCM_DOWN, 0, 1);
    }
    static const int HORIZ[4] = { MCM_NORTH, MCM_SOUTH, MCM_WEST, MCM_EAST };
    for (int hi = 0; hi < 4; hi++) {
        int d = HORIZ[hi];
        float hh0, hh1, x0, z0, x1, z1;
        int rc, sNb;
        uint32_t fNb;
        switch (d) {
        case MCM_NORTH: hh0 = hNW; hh1 = hNE; x0 = fx; x1 = fx + 1.0f; z0 = fz + OFFS; z1 = fz + OFFS; rc = renderN; sNb = sN; fNb = fN; break;
        case MCM_SOUTH: hh0 = hSE; hh1 = hSW; x0 = fx + 1.0f; x1 = fx; z0 = fz + 1.0f - OFFS; z1 = fz + 1.0f - OFFS; rc = renderS; sNb = sS; fNb = fS; break;
        case MCM_WEST: hh0 = hSW; hh1 = hNW; x0 = fx + OFFS; x1 = fx + OFFS; z0 = fz + 1.0f; z1 = fz; rc = renderW; sNb = sW; fNb = fW; break;
        default: hh0 = hNE; hh1 = hSE; x0 = fx + 1.0f - OFFS; x1 = fx + 1.0f - OFFS; z0 = fz; z1 = fz + 1.0f; rc = renderE; sNb = sE; fNb = fE; break;
        }
        if (!rc || fluid_occluded_by_state(c, d, fmaxf(hh0, hh1), sNb)) continue;
        const float *r = t->fluid_rect[base + 1];
        int overlay = 0;
        if (kind == 1 && (fNb & (MCM_F_LEAVES)) ) overlay = 1;
        if (kind == 1 && sNb >= 0) {
            /* HalfTransparentBlock (стекло, лёд, слизь, …) тоже даёт оверлей */
            if (((fNb >> MCM_SKIP_SHIFT) & 7) == MCM_SKIP_SAME) overlay = 1;
        }
        if (overlay) r = t->fluid_rect[2];
        float u0 = r[0], u1 = r[0] + (r[2] - r[0]) * 0.5f;
        float v01 = r[1] + (r[3] - r[1]) * ((1.0f - hh0) * 0.5f);
        float v02 = r[1] + (r[3] - r[1]) * ((1.0f - hh1) * 0.5f);
        float v1 = r[1] + (r[3] - r[1]) * 0.5f;
        float shadeSide = sh[d <= 3 ? MCM_NORTH : MCM_WEST] * sh[MCM_UP];
        set_vert(P, UV, 0, x0, fy + hh0, z0, u0, v01);
        set_vert(P, UV, 1, x1, fy + hh1, z1, u1, v02);
        set_vert(P, UV, 2, x1, fy + bottomOffs, z1, u1, v1);
        set_vert(P, UV, 3, x0, fy + bottomOffs, z0, u0, v1);
        fluid_face(c, P, UV, tint, shadeSide, mat, blockIdx, d, !overlay, 0);
    }
}

/* ------------------------------------------------------------------------------------------------------------------------------
 *                                                    вход в ядро
 * ---------------------------------------------------------------------------------------------------------------------------- */
int mcmesh_chunk(const McMeshTables *t, const McMeshInput *in, const McMeshOptions *o, McMeshOutput *out)
{
    if (!t || !in || !o || !out) return -1;
    if (t->abi != MCMESH_ABI_VERSION) return -3;
    memset(out, 0, sizeof(*out));
    if (!in->blocks[4] || o->n_sections <= 0) return -1;
    if (o->n_sections * 4 > MAX_QY) return -1;
    Ctx *c = (Ctx *)calloc(1, sizeof(Ctx));
    if (!c) return -2;
    c->t = t;
    c->o = o;
    c->out = out;
    memcpy(c->B, in->blocks, sizeof(c->B));
    memcpy(c->Bio, in->biomes, sizeof(c->Bio));
    c->H = o->n_sections * 16;
    c->nqy = o->n_sections * 4;
    c->ax0 = o->cx * 16;
    c->az0 = o->cz * 16;
    c->opt = o->flags;
    c->blend = o->blend_radius < 0 ? 0 : (o->blend_radius > 7 ? 7 : o->blend_radius);
    c->scale = o->scale > 0.0f ? o->scale : 1.0f;
    c->yoff = (float)o->y_offset;
    init_regions(c);

    const uint16_t *blk = in->blocks[4];
    const uint32_t *fl = t->st_flags;
    const uint32_t DRAW = MCM_F_GEOM | MCM_F_WATER | MCM_F_LAVA;
    uint32_t want = 0;
    if (!(o->flags & MCM_OPT_NO_MODELS)) want |= MCM_F_GEOM;
    if (!(o->flags & MCM_OPT_NO_FLUIDS)) want |= MCM_F_WATER | MCM_F_LAVA;
    (void)DRAW;
    int ns = t->n_states;
    for (int sec = 0; sec < o->n_sections; sec++) {
        int y0 = sec * 16;
        const uint16_t *sb = blk + (size_t)y0 * 256;
        int any = 0;
        for (int i = 0; i < 4096; i++) {
            unsigned s = sb[i];
            if (s < (unsigned)ns && (fl[s] & want)) { any = 1; break; }
        }
        if (!any) { out->n_sections_skipped++; continue; }
        for (int yy = 0; yy < 16; yy++) {
            int y = y0 + yy;
            for (int z = 0; z < 16; z++) {
                const uint16_t *row = blk + (((size_t)y << 4) + z) * 16;
                for (int x = 0; x < 16; x++) {
                    unsigned s = row[x];
                    if (s >= (unsigned)ns) continue;
                    uint32_t f = fl[s];
                    if (!(f & want)) continue;
                    out->n_blocks_visited++;
                    uint32_t bi = (uint32_t)(((y << 4) + z) * 16 + x);
                    if (f & want & (MCM_F_WATER | MCM_F_LAVA)) fluid_block(c, x, y, z, (int)s, f, bi);
                    if (f & want & MCM_F_GEOM) model_block(c, x, y, z, (int)s, f, bi);
                    if (c->err) goto done;
                }
            }
        }
    }
    if (!c->err && (c->opt & MCM_OPT_MERGE)) merge_pass(c);
done:;
    int err = c->err;
    free(c->cand);
    for (int i = 0; i < MAX_QY * 4; i++) free(c->tint_map[i]);
    free(c);
    if (err) { mcmesh_output_free(out); return err; }
    return 0;
}

void mcmesh_output_free(McMeshOutput *o)
{
    if (!o) return;
    free(o->pos); free(o->uv); free(o->col); free(o->mat); free(o->block); free(o->dir); free(o->merged); free(o->rect);
    memset(o, 0, sizeof(*o));
}

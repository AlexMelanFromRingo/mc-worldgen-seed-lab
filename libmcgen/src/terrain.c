/* terrain.c — стадия TERRAIN («заполнение шумом»): плотность final_density + Aquifer → камень/вода/лава/воздух.
 *
 * 26.3+ (этот файл): как NoiseBasedChunkGenerator.doFill 26.3 — final_density.sampleVolume по объёму чанка
 *   16×H×16 (шаг 1) в кэширующем контексте NoiseChunk, затем по столбцам сверху вниз Aquifer.computeSubstance.
 *   Порядок вызовов повторяет игру: конструктор NoiseBasedAquifer (maxSurfaceLevel → sampleVolume поверхности)
 *   выполняется ДО плотности — это влияет на содержимое ячеек кэша (cache) и, значит, на точечные значения.
 *   Жилы руд 26.3 — правила материала (ore_vein) — terrain_veins.c.
 * 26.1/26.2: terrain_old.c (NoiseChunk с интерполяцией ячеек, OreVeinifier).
 */
#include "mcgen_internal.h"
#include "mcgen_tweaks_table.h"
#include "df_old.h"
#include "fluidpp.h"
#include <stdlib.h>
#include <stdio.h>

int terrain_old_fill(McWorld *w, void *octx, int cx, int cz, uint16_t *blocks, PPMarks *marks, char *err, size_t errlen);   /* terrain_old.c */
void *terrain_old_ctx_new(McWorld *w);
void terrain_old_ctx_free(void *p);
void veins_apply_new(McWorld *w, SCtx *x, int cx, int cz, int y0, int ny, uint16_t *blocks);              /* terrain_veins.c */

/* ---------------- жидкости ---------------- */
typedef struct { int level; int type; } Fluid;     /* Aquifer.FluidStatus: уровень и состояние */

static inline int fluid_at(const McGen *g, Fluid f, int y) { return y < f.level ? f.type : g->st_air; }
static inline int fluid_eq(Fluid a, Fluid b) { return a.level == b.level && a.type == b.type; }
#define WAY_BELOW_MIN_Y (-2032 * 16)

typedef struct { int lava_level, sea_level, sea_type, lava_type; } Picker;   /* NoiseBasedChunkGenerator.createFluidPicker */
static inline Fluid pick(const Picker *p, int y) {
    Fluid f;
    int thr = p->lava_level < p->sea_level ? p->lava_level : p->sea_level;
    if (y < thr) { f.level = p->lava_level; f.type = p->lava_type; } else { f.level = p->sea_level; f.type = p->sea_type; }
    return f;
}

/* ---------------- NoiseBasedAquifer 26.3 ---------------- */
typedef struct { u64 key; int val; int used; } SurfEnt;
typedef struct {
    McWorld *w; SCtx *x; const McGen *g;
    Picker pk;
    int min_gx, min_gy, min_gz, gsx, gsy, gsz;
    int *loc;        /* 3 int на ячейку (x,y,z); loc_set */
    u8 *loc_set;
    Fluid *status; u8 *status_set;
    int skip_above_y;
    SurfEnt *surf; int surf_cap, surf_n;
    int enabled;
    int sched;       /* shouldScheduleFluidUpdate последнего вызова */
} Aq;

static inline int gx_of(int b) { return b >> 4; }
static inline int gy_of(int b) { return jm_floordiv(b, 12); }
static inline int from_gx(int g, int o) { return (g << 4) + o; }
static inline int from_gy(int g, int o) { return g * 12 + o; }

static inline u64 colpack(int x, int z) { return (u64)(u32)x | ((u64)(u32)z << 32); }
static int *surf_slot(Aq *a, u64 key, int *found) {
    if (a->surf_n * 2 + 2 > a->surf_cap) {
        int oc = a->surf_cap; SurfEnt *oe = a->surf;
        a->surf_cap = oc ? oc * 2 : 256; a->surf = xcalloc((size_t)a->surf_cap, sizeof(SurfEnt)); a->surf_n = 0;
        for (int i = 0; i < oc; i++) if (oe[i].used) { int f; *surf_slot(a, oe[i].key, &f) = oe[i].val; }
        free(oe);
    }
    u64 h = key * 0x9E3779B97F4A7C15ULL;
    int i = (int)(h >> 40) & (a->surf_cap - 1);
    while (a->surf[i].used && a->surf[i].key != key) i = (i + 1) & (a->surf_cap - 1);
    *found = a->surf[i].used;
    if (!a->surf[i].used) { a->surf[i].used = 1; a->surf[i].key = key; a->surf_n++; }
    return &a->surf[i].val;
}
static int surface_level(Aq *a, int bx, int bz) {
    int qx = (bx >> 2) << 2, qz = (bz >> 2) << 2;
    int found; int *v = surf_slot(a, colpack(qx, qz), &found);
    if (!found) *v = jm_floor_f(s_value(a->x, a->w->s_aq[AQ_SURFACE_LEVEL], qx, 0, qz));
    return *v;
}
static int max_surface_level(Aq *a, int minbx, int minbz, int maxbx, int maxbz) {
    int mqx = minbx >> 2, Mqx = maxbx >> 2, mqz = minbz >> 2, Mqz = maxbz >> 2;
    Vol v = { Mqx - mqx + 1, 1, Mqz - mqz + 1, mqx * 4, 0, mqz * 4, 4, 1, 4 };
    float *buf = sctx_acquire(a->x, vol_size(&v));
    s_volume(a->x, a->w->s_aq[AQ_SURFACE_LEVEL], buf, &v);
    int maxy = (int)0x80000000u;
    for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) {
        int sl = jm_floor_f(buf[vol_idx(&v, x, 0, z)]);
        int found; *surf_slot(a, colpack(vol_bx(&v, x), vol_bz(&v, z)), &found) = sl;
        if (sl > maxy) maxy = sl;
    }
    sctx_release(a->x, buf);
    return maxy;
}

static void aq_init(Aq *a, McWorld *w, SCtx *x, const Vol *v, const Picker *pk) {
    memset(a, 0, sizeof *a);
    a->w = w; a->x = x; a->g = w->g; a->pk = *pk;
    a->enabled = w->ns->has_aquifers && w->tweak[MCGEN_TWEAK_AQUIFERS] != 0.0;
    if (!a->enabled) return;
    a->min_gx = gx_of(v->x0 - 5);
    int max_gx = gx_of(vol_max_x(v) - 5) + 1;
    a->gsx = max_gx - a->min_gx + 1;
    a->min_gy = gy_of(v->y0 + 1) - 1;
    int max_gy = gy_of(vol_max_y(v) + 1) + 1;
    a->gsy = max_gy - a->min_gy + 1;
    a->min_gz = gx_of(v->z0 - 5);
    int max_gz = gx_of(vol_max_z(v) - 5) + 1;
    a->gsz = max_gz - a->min_gz + 1;
    int tot = a->gsx * a->gsy * a->gsz;
    a->loc = xcalloc((size_t)tot * 3, sizeof(int)); a->loc_set = xcalloc((size_t)tot, 1);
    a->status = xcalloc((size_t)tot, sizeof(Fluid)); a->status_set = xcalloc((size_t)tot, 1);
    int mx = max_surface_level(a, from_gx(a->min_gx, 0), from_gx(a->min_gz, 0), from_gx(max_gx, 9), from_gx(max_gz, 9)) + 8;
    int skip_gy = gy_of(mx + 12) + 1;
    a->skip_above_y = from_gy(skip_gy, 11) - 1;
}
static void aq_free(Aq *a) { free(a->loc); free(a->loc_set); free(a->status); free(a->status_set); free(a->surf); }

static int aq_index(const Aq *a, int gx, int gy, int gz) { return ((gy - a->min_gy) * a->gsz + (gz - a->min_gz)) * a->gsx + (gx - a->min_gx); }

static double similarity(int d1, int d2) { return 1.0 - (double)(d2 - d1) / 25.0; }

static int compute_random_fluid_level(Aq *a, int x, int y, int z, int lowest) {
    int cxl = jm_floordiv(x, 16), cyl = jm_floordiv(y, 40), czl = jm_floordiv(z, 16);
    int mid = cyl * 40 + 20;
    double spread = (double)(s_value(a->x, a->w->s_aq[AQ_SPREAD], cxl, cyl, czl) * 10.0f);
    int q = jm_floor_d(spread / 3.0) * 3;
    int target = mid + q;
    return lowest < target ? lowest : target;
}
static int compute_surface_level(Aq *a, int x, int y, int z, Fluid global, int lowest, int center_under) {
    double partial, fully;
    if (s_value(a->x, a->w->s_aq[AQ_EXCLUSION], x, y, z) > 0.0f) { partial = -1.0; fully = -1.0; }
    else {
        int dist = (lowest + 8) - y;
        double ff = 0.0;
        if (center_under) {
            double t = ((double)dist - 0.0) / (64.0 - 0.0);
            ff = t < 0.0 ? 1.0 : (t > 1.0 ? 0.0 : jm_lerp(t, 1.0, 0.0));
        }
        double nv = jm_clamp((double)s_value(a->x, a->w->s_aq[AQ_FLOOD], x, y, z), -1.0, 1.0);
        double inv = (ff - 1.0) / (0.0 - 1.0);
        double full_thr = jm_lerp(inv, -0.3, 0.8);
        double part_thr = jm_lerp(inv, -0.8, 0.4);
        partial = nv - part_thr; fully = nv - full_thr;
    }
    if (fully > 0.0) return global.level;
    if (partial > 0.0) return compute_random_fluid_level(a, x, y, z, lowest);
    return WAY_BELOW_MIN_Y;
}
static int compute_fluid_type(Aq *a, int x, int y, int z, Fluid global, int level) {
    int ft = global.type;
    if (level <= -10 && level != WAY_BELOW_MIN_Y && global.type != a->g->st_lava) {
        float lv = s_value(a->x, a->w->s_aq[AQ_LAVA], jm_floordiv(x, 64), jm_floordiv(y, 40), jm_floordiv(z, 64));
        if (fabs((double)lv) > 0.3) ft = a->g->st_lava;
    }
    return ft;
}
static const int SURF_OFFS[13][2] = { {0,0},{-2,-1},{-1,-1},{0,-1},{1,-1},{-3,0},{-2,0},{-1,0},{1,0},{-2,1},{-1,1},{0,1},{1,1} };
static Fluid compute_fluid(Aq *a, int x, int y, int z) {
    Fluid global = pick(&a->pk, y);
    int lowest = 0x7fffffff;
    int top = y + 12, bottom = y - 12;
    int center_under = 0;
    for (int i = 0; i < 13; i++) {
        int sx = x + (SURF_OFFS[i][0] << 4), sz = z + (SURF_OFFS[i][1] << 4);
        int sl = surface_level(a, sx, sz);
        int adj = sl + 8;
        int start = SURF_OFFS[i][0] == 0 && SURF_OFFS[i][1] == 0;
        if (start && bottom > adj) return global;
        int pokes = top > adj;
        if (pokes || start) {
            Fluid gs = pick(&a->pk, adj);
            if (!gen_is_air(a->g, fluid_at(a->g, gs, adj))) {
                if (start) center_under = 1;
                if (pokes) return gs;
            }
        }
        if (sl < lowest) lowest = sl;
    }
    int lvl = compute_surface_level(a, x, y, z, global, lowest, center_under);
    Fluid f; f.level = lvl; f.type = compute_fluid_type(a, x, y, z, global, lvl);
    return f;
}
static Fluid aq_status(Aq *a, int idx) {
    if (a->status_set[idx]) return a->status[idx];
    Fluid f = compute_fluid(a, a->loc[3 * idx], a->loc[3 * idx + 1], a->loc[3 * idx + 2]);
    a->status[idx] = f; a->status_set[idx] = 1;
    return f;
}
static double pressure(Aq *a, int x, int y, int z, double *barrier, Fluid s1, Fluid s2) {
    const McGen *g = a->g;
    int t1 = fluid_at(g, s1, y), t2 = fluid_at(g, s2, y);
    int l1 = gen_is_block(g, t1, g->blk_lava), w1 = gen_is_block(g, t1, g->blk_water);
    int l2 = gen_is_block(g, t2, g->blk_lava), w2 = gen_is_block(g, t2, g->blk_water);
    if ((l1 && w2) || (w1 && l2)) return 2.0;
    int dy = abs(s1.level - s2.level);
    if (dy == 0) return 0.0;
    double avg = 0.5 * (double)(s1.level + s2.level);
    double above = (double)y + 0.5 - avg;
    double base = (double)dy / 2.0;
    double edge = base - fabs(above);
    double grad;
    if (above > 0.0) { double c = 0.0 + edge; grad = c > 0.0 ? c / 1.5 : c / 2.5; }
    else { double c = 3.0 + edge; grad = c > 0.0 ? c / 3.0 : c / 10.0; }
    double nv;
    if (!(grad < -2.0) && !(grad > 2.0)) {
        if (*barrier != *barrier) *barrier = (double)s_value(a->x, a->w->s_aq[AQ_BARRIER], x, y, z);
        nv = *barrier;
    } else nv = 0.0;
    return 2.0 * (nv + grad);
}
/* Aquifer.computeSubstance: −1 — «нет» (null → default_block) */
static int aq_substance(Aq *a, int x, int y, int z, double density) {
    const McGen *g = a->g;
    a->sched = 0;
    if (density > 0.0) return -1;
    Fluid global = pick(&a->pk, y);
    if (!a->enabled) return fluid_at(g, global, y);
    if (y > a->skip_above_y) return fluid_at(g, global, y);
    if (gen_is_block(g, fluid_at(g, global, y), g->blk_lava)) return g->st_lava;
    int xa = gx_of(x - 5), ya = gy_of(y + 1), za = gx_of(z - 5);
    int d1 = 0x7fffffff, d2 = 0x7fffffff, d3 = 0x7fffffff, d4 = 0x7fffffff;
    int c1 = 0, c2 = 0, c3 = 0, c4 = 0;
    for (int x1 = 0; x1 <= 1; x1++) for (int y1 = -1; y1 <= 1; y1++) for (int z1 = 0; z1 <= 1; z1++) {
        int sgx = xa + x1, sgy = ya + y1, sgz = za + z1;
        int idx = aq_index(a, sgx, sgy, sgz);
        if (!a->loc_set[idx]) {
            Rnd r = pos_at(&a->w->aquifer_pos, sgx, sgy, sgz);
            int ox = rnd_next_int_bound(&r, 10), oy = rnd_next_int_bound(&r, 9), oz = rnd_next_int_bound(&r, 10);
            a->loc[3 * idx] = from_gx(sgx, ox); a->loc[3 * idx + 1] = from_gy(sgy, oy); a->loc[3 * idx + 2] = from_gx(sgz, oz);
            a->loc_set[idx] = 1;
        }
        int dx = a->loc[3 * idx] - x, dy = a->loc[3 * idx + 1] - y, dz = a->loc[3 * idx + 2] - z;
        int nd = dx * dx + dy * dy + dz * dz;
        if (d1 >= nd) { c4 = c3; c3 = c2; c2 = c1; c1 = idx; d4 = d3; d3 = d2; d2 = d1; d1 = nd; }
        else if (d2 >= nd) { c4 = c3; c3 = c2; c2 = idx; d4 = d3; d3 = d2; d2 = nd; }
        else if (d3 >= nd) { c4 = c3; c3 = idx; d4 = d3; d3 = nd; }
        else if (d4 >= nd) { c4 = idx; d4 = nd; }
    }
    Fluid s1 = aq_status(a, c1);
    double sim12 = similarity(d1, d2);
    int fstate = fluid_at(g, s1, y);
    const double FUS = 1.0 - (144.0 - 100.0) / 25.0;   /* FLOWING_UPDATE_SIMULARITY = similarity(10², 12²) */
    if (sim12 <= 0.0) {
        if (sim12 >= FUS) { Fluid s2 = aq_status(a, c2); a->sched = !(s1.level == s2.level && s1.type == s2.type); }
        return fstate;
    }
    if (gen_is_block(g, fstate, g->blk_water) && gen_is_block(g, fluid_at(g, pick(&a->pk, y - 1), y - 1), g->blk_lava)) { a->sched = 1; return fstate; }
    double barrier = NAN;
    Fluid s2 = aq_status(a, c2);
    double b12 = sim12 * pressure(a, x, y, z, &barrier, s1, s2);
    if (density + b12 > 0.0) return -1;
    Fluid s3 = aq_status(a, c3);
    double sim13 = similarity(d1, d3);
    if (sim13 > 0.0) {
        double b13 = sim12 * sim13 * pressure(a, x, y, z, &barrier, s1, s3);
        if (density + b13 > 0.0) return -1;
    }
    double sim23 = similarity(d2, d3);
    if (sim23 > 0.0) {
        double b23 = sim12 * sim23 * pressure(a, x, y, z, &barrier, s2, s3);
        if (density + b23 > 0.0) return -1;
    }
    {
        #define FEQ(p, q) ((p).level == (q).level && (p).type == (q).type)
        int f12 = !FEQ(s1, s2), f23 = sim23 >= FUS && !FEQ(s2, s3), f13 = sim13 >= FUS && !FEQ(s1, s3);
        if (!f12 && !f23 && !f13) a->sched = sim13 >= FUS && similarity(d1, d4) >= FUS && !FEQ(s1, aq_status(a, c4));
        else a->sched = 1;
        #undef FEQ
    }
    return fstate;
}

/* ---------------- контекст и заполнение ---------------- */
struct TerrainCtx { SCtx *x; void *old; PPMarks marks; };
TerrainCtx *terrain_ctx_new(McWorld *w) {
    TerrainCtx *t = xcalloc(1, sizeof *t);
    if (w->g->newf) t->x = sctx_new(w->nc, 1);
    else t->old = terrain_old_ctx_new(w);
    return t;
}
void terrain_ctx_free(TerrainCtx *t) { if (!t) return; sctx_free(t->x); if (t->old) terrain_old_ctx_free(t->old); ppmarks_free(&t->marks); free(t); }
const PPMarks *terrain_marks(const TerrainCtx *t) { return &t->marks; }

void terrain_picker(const McWorld *w, int *lava_level, int *sea_level) {
    *sea_level = w->ns->sea_level + (int)w->tweak[MCGEN_TWEAK_SEA_LEVEL_OFFSET];
    *lava_level = -54 + (int)w->tweak[MCGEN_TWEAK_LAVA_LEVEL_OFFSET];
}

int terrain_fill_chunk(McWorld *w, TerrainCtx *t, int cx, int cz, uint16_t *blocks, char *err, size_t errlen) {
    const McGen *g = w->g;
    size_t tot = (size_t)w->height * 256;
    for (size_t i = 0; i < tot; i++) blocks[i] = (uint16_t)g->st_air;
    ppmarks_clear(&t->marks);
    if (!g->newf) return terrain_old_fill(w, t->old, cx, cz, blocks, &t->marks, err, errlen);
    /* NoiseSettings.clampToHeightAccessor */
    int nmin = w->ns->min_y > w->min_y ? w->ns->min_y : w->min_y;
    int ntop = w->ns->min_y + w->ns->height; if (ntop > w->min_y + w->height) ntop = w->min_y + w->height;
    int nh = ntop - nmin;
    if (nh <= 0) return MCGEN_OK;
    SCtx *x = t->x;
    sctx_reset_caches(x);
    Vol v = { 16, nh, 16, cx * 16, nmin, cz * 16, 1, 1, 1 };
    Picker pk; terrain_picker(w, &pk.lava_level, &pk.sea_level);
    pk.sea_type = w->def_fluid; pk.lava_type = g->st_lava;
    Aq aq; aq_init(&aq, w, x, &v, &pk);
    float *dens = sctx_acquire(x, vol_size(&v));
    s_volume(x, w->s_rf[RF_FINAL_DENSITY], dens, &v);
    int defb = w->def_block;
    for (int z = 0; z < 16; z++) for (int xx = 0; xx < 16; xx++) for (int y = nh - 1; y >= 0; y--) {
        int by = nmin + y;
        int st = aq_substance(&aq, cx * 16 + xx, by, cz * 16 + z, (double)dens[vol_idx(&v, xx, y, z)]);
        if (st < 0) st = defb;
        blocks[((size_t)(by - w->min_y) * 16 + z) * 16 + xx] = (uint16_t)st;
        if (aq.sched && (g->state_cls[st] & 4)) ppmarks_add(&t->marks, (by - w->min_y) >> 4, xx, by & 15, z);
    }
    sctx_release(x, dens);
    aq_free(&aq);
    if (w->tweak[MCGEN_TWEAK_ORE_VEINS] != 0.0) veins_apply_new(w, x, cx, cz, nmin - w->min_y, nh, blocks);
    return MCGEN_OK;
}

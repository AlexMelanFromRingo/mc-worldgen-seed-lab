/* terrain_old.c — TERRAIN для 26.1/26.2: NoiseBasedChunkGenerator.doFill (цикл по ячейкам NoiseChunk),
 * Aquifer.NoiseBasedAquifer (версия с NoiseChunk/FunctionContext), OreVeinifier (жилы — часть заполнения шумом).
 * Порядок правил блока (MaterialRuleList): сначала аквифер (null при плотности > 0), затем жилы; null → default_block. */
#include "mcgen_internal.h"
#include "mcgen_tweaks_table.h"
#include "df_old.h"
#include "fluidpp.h"
#include <stdlib.h>
#include <stdio.h>

void terrain_picker(const McWorld *w, int *lava_level, int *sea_level);

typedef struct { int level; int type; } Fluid;
static inline int fluid_at(const McGen *g, Fluid f, int y) { return y < f.level ? f.type : g->st_air; }
#define WAY_BELOW_MIN_Y (-2032 * 16)
typedef struct { int lava_level, sea_level, sea_type, lava_type; } Picker;
static inline Fluid pick(const Picker *p, int y) {
    Fluid f; int thr = p->lava_level < p->sea_level ? p->lava_level : p->sea_level;
    if (y < thr) { f.level = p->lava_level; f.type = p->lava_type; } else { f.level = p->sea_level; f.type = p->sea_type; }
    return f;
}

typedef struct {
    McWorld *w; const McGen *g; NChunk *nc; Picker pk; int enabled;
    int min_gx, min_gy, min_gz, gsx, gsy, gsz;
    int *loc; u8 *loc_set; Fluid *status; u8 *status_set; int cap;
    int skip_above_y;
    int sched;
    int pt;          /* 1 — вызов из карверов: контекст — SinglePointContext(x, y, z), а не NoiseChunk (барьерный шум по точке) */
} AqOld;

static inline int gx_of(int b) { return b >> 4; }
static inline int gy_of(int b) { return jm_floordiv(b, 12); }
static inline int from_gx(int g, int o) { return (g << 4) + o; }
static inline int from_gy(int g, int o) { return g * 12 + o; }

static void aq_init(AqOld *a, McWorld *w, NChunk *nc, int cx, int cz, int min_y, int height, const Picker *pk) {
    a->w = w; a->g = w->g; a->nc = nc; a->pk = *pk;
    a->enabled = w->ns->aquifers_enabled && w->tweak[MCGEN_TWEAK_AQUIFERS] != 0.0;
    if (!a->enabled) return;
    int minbx = cx * 16, maxbx = cx * 16 + 15, minbz = cz * 16, maxbz = cz * 16 + 15;
    a->min_gx = gx_of(minbx - 5); int max_gx = gx_of(maxbx - 5) + 1; a->gsx = max_gx - a->min_gx + 1;
    a->min_gy = gy_of(min_y + 1) - 1; int max_gy = gy_of(min_y + height + 1) + 1; a->gsy = max_gy - a->min_gy + 1;
    a->min_gz = gx_of(minbz - 5); int max_gz = gx_of(maxbz - 5) + 1; a->gsz = max_gz - a->min_gz + 1;
    int tot = a->gsx * a->gsy * a->gsz;
    if (tot > a->cap) {
        free(a->loc); free(a->loc_set); free(a->status); free(a->status_set);
        a->loc = xmalloc(sizeof(int) * 3 * (size_t)tot); a->loc_set = xmalloc((size_t)tot); a->status = xmalloc(sizeof(Fluid) * (size_t)tot); a->status_set = xmalloc((size_t)tot);
        a->cap = tot;
    }
    memset(a->loc_set, 0, (size_t)tot); memset(a->status_set, 0, (size_t)tot);
    int mx = nchunk_max_prelim_surface(nc, from_gx(a->min_gx, 0), from_gx(a->min_gz, 0), from_gx(max_gx, 9), from_gx(max_gz, 9)) + 8;
    int skip_gy = gy_of(mx + 12) + 1;
    a->skip_above_y = from_gy(skip_gy, 11) - 1;
}
static int aq_index(const AqOld *a, int gx, int gy, int gz) { return ((gy - a->min_gy) * a->gsz + (gz - a->min_gz)) * a->gsx + (gx - a->min_gx); }
static double similarity(int d1, int d2) { return 1.0 - (double)(d2 - d1) / 25.0; }

static int random_fluid_level(AqOld *a, int x, int y, int z, int lowest) {
    int cxl = jm_floordiv(x, 16), cyl = jm_floordiv(y, 40), czl = jm_floordiv(z, 16);
    int mid = cyl * 40 + 20;
    double spread = nchunk_router_point(a->nc, RF_FLUID_SPREAD, cxl, cyl, czl) * 10.0;
    int q = jm_floor_d(spread / 3.0) * 3;
    int target = mid + q;
    return lowest < target ? lowest : target;
}
static int surface_level_calc(AqOld *a, int x, int y, int z, Fluid global, int lowest, int center_under) {
    double partial, fully;
    /* OverworldBiomeBuilder.isDeepDarkRegion(erosion, depth, SinglePointContext) — функции обёрнутого роутера чанка */
    if (nchunk_router_point(a->nc, RF_EROSION, x, y, z) < (double)-0.225f && nchunk_router_point(a->nc, RF_DEPTH, x, y, z) > (double)0.9f) {
        partial = -1.0; fully = -1.0;
    } else {
        int dist = lowest + 8 - y;
        double ff = 0.0;
        if (center_under) { double t = ((double)dist - 0.0) / (64.0 - 0.0); ff = t < 0.0 ? 1.0 : (t > 1.0 ? 0.0 : jm_lerp(t, 1.0, 0.0)); }
        double nv = jm_clamp(nchunk_router_point(a->nc, RF_FLUID_FLOOD, x, y, z), -1.0, 1.0);
        double inv = (ff - 1.0) / (0.0 - 1.0);
        double full_thr = jm_lerp(inv, -0.3, 0.8), part_thr = jm_lerp(inv, -0.8, 0.4);
        partial = nv - part_thr; fully = nv - full_thr;
    }
    if (fully > 0.0) return global.level;
    if (partial > 0.0) return random_fluid_level(a, x, y, z, lowest);
    return WAY_BELOW_MIN_Y;
}
static int fluid_type(AqOld *a, int x, int y, int z, Fluid global, int level) {
    int ft = global.type;
    if (level <= -10 && level != WAY_BELOW_MIN_Y && global.type != a->g->st_lava) {
        double lv = nchunk_router_point(a->nc, RF_LAVA, jm_floordiv(x, 64), jm_floordiv(y, 40), jm_floordiv(z, 64));
        if (fabs(lv) > 0.3) ft = a->g->st_lava;
    }
    return ft;
}
static const int SURF_OFFS[13][2] = { {0,0},{-2,-1},{-1,-1},{0,-1},{1,-1},{-3,0},{-2,0},{-1,0},{1,0},{-2,1},{-1,1},{0,1},{1,1} };
static Fluid compute_fluid(AqOld *a, int x, int y, int z) {
    Fluid global = pick(&a->pk, y);
    int lowest = 0x7fffffff, top = y + 12, bottom = y - 12, center_under = 0;
    for (int i = 0; i < 13; i++) {
        int sx = x + (SURF_OFFS[i][0] << 4), sz = z + (SURF_OFFS[i][1] << 4);
        int sl = nchunk_prelim_surface(a->nc, sx, sz);
        int adj = sl + 8;
        int start = SURF_OFFS[i][0] == 0 && SURF_OFFS[i][1] == 0;
        if (start && bottom > adj) return global;
        int pokes = top > adj;
        if (pokes || start) {
            Fluid gs = pick(&a->pk, adj);
            if (!gen_is_air(a->g, fluid_at(a->g, gs, adj))) { if (start) center_under = 1; if (pokes) return gs; }
        }
        if (sl < lowest) lowest = sl;
    }
    int lvl = surface_level_calc(a, x, y, z, global, lowest, center_under);
    Fluid f; f.level = lvl; f.type = fluid_type(a, x, y, z, global, lvl);
    return f;
}
static Fluid aq_status(AqOld *a, int idx) {
    if (a->status_set[idx]) return a->status[idx];
    Fluid f = compute_fluid(a, a->loc[3 * idx], a->loc[3 * idx + 1], a->loc[3 * idx + 2]);
    a->status[idx] = f; a->status_set[idx] = 1;
    return f;
}
static double pressure(AqOld *a, int bx, int by, int bz, double *barrier, Fluid s1, Fluid s2) {
    const McGen *g = a->g;
    int t1 = fluid_at(g, s1, by), t2 = fluid_at(g, s2, by);
    int l1 = gen_is_block(g, t1, g->blk_lava), w1 = gen_is_block(g, t1, g->blk_water);
    int l2 = gen_is_block(g, t2, g->blk_lava), w2 = gen_is_block(g, t2, g->blk_water);
    if ((l1 && w2) || (w1 && l2)) return 2.0;
    int dy = abs(s1.level - s2.level);
    if (dy == 0) return 0.0;
    double avg = 0.5 * (double)(s1.level + s2.level);
    double above = (double)by + 0.5 - avg;
    double base = (double)dy / 2.0;
    double edge = base - fabs(above);
    double grad;
    if (above > 0.0) { double c = 0.0 + edge; grad = c > 0.0 ? c / 1.5 : c / 2.5; }
    else { double c = 3.0 + edge; grad = c > 0.0 ? c / 3.0 : c / 10.0; }
    double nv;
    if (!(grad < -2.0) && !(grad > 2.0)) {
        if (*barrier != *barrier) *barrier = a->pt ? nchunk_router_point(a->nc, RF_BARRIER, bx, by, bz) : nchunk_router_here(a->nc, RF_BARRIER);   /* barrierNoise.compute(context = NoiseChunk | SinglePointContext) */
        nv = *barrier;
    } else nv = 0.0;
    return 2.0 * (nv + grad);
}
static int aq_substance_xyz(AqOld *a, int x, int y, int z, double density);
static int aq_substance(AqOld *a, double density) { return aq_substance_xyz(a, nchunk_block_x(a->nc), nchunk_block_y(a->nc), nchunk_block_z(a->nc), density); }
static int aq_substance_xyz(AqOld *a, int x, int y, int z, double density) {
    const McGen *g = a->g;
    a->sched = 0;
    if (density > 0.0) return -1;
    Fluid global = pick(&a->pk, y);
    if (!a->enabled) return fluid_at(g, global, y);
    if (y > a->skip_above_y) return fluid_at(g, global, y);
    if (gen_is_block(g, fluid_at(g, global, y), g->blk_lava)) return g->st_lava;
    int xa = gx_of(x - 5), ya = gy_of(y + 1), za = gx_of(z - 5);
    int d1 = 0x7fffffff, d2 = 0x7fffffff, d3 = 0x7fffffff, d4 = 0x7fffffff, c1 = 0, c2 = 0, c3 = 0, c4 = 0;
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
    if (density + sim12 * pressure(a, x, y, z, &barrier, s1, s2) > 0.0) return -1;
    Fluid s3 = aq_status(a, c3);
    double sim13 = similarity(d1, d3);
    if (sim13 > 0.0 && density + sim12 * sim13 * pressure(a, x, y, z, &barrier, s1, s3) > 0.0) return -1;
    double sim23 = similarity(d2, d3);
    if (sim23 > 0.0 && density + sim12 * sim23 * pressure(a, x, y, z, &barrier, s2, s3) > 0.0) return -1;
    {
        #define FEQ(p, q) ((p).level == (q).level && (p).type == (q).type)
        int f12 = !FEQ(s1, s2), f23 = sim23 >= FUS && !FEQ(s2, s3), f13 = sim13 >= FUS && !FEQ(s1, s3);
        if (!f12 && !f23 && !f13) a->sched = sim13 >= FUS && similarity(d1, d4) >= FUS && !FEQ(s1, aq_status(a, c4));
        else a->sched = 1;
        #undef FEQ
    }
    return fstate;
}

/* ---- OreVeinifier ---- */
typedef struct { int ore, raw, filler, min_y, max_y; } VeinType;
static int ore_vein(McWorld *w, NChunk *nc, const VeinType vt[2]) {
    double tog = nchunk_router_here(nc, RF_VEIN_TOGGLE);
    int y = nchunk_block_y(nc);
    const VeinType *v = tog > 0.0 ? &vt[0] : &vt[1];
    double ridged = fabs(tog);
    int dtop = v->max_y - y, dbot = y - v->min_y;
    if (dbot < 0 || dtop < 0) return -1;
    int dedge = dtop < dbot ? dtop : dbot;
    double t = ((double)dedge - 0.0) / (20.0 - 0.0);
    double edge = t < 0.0 ? -0.2 : (t > 1.0 ? 0.0 : jm_lerp(t, -0.2, 0.0));
    if (ridged + edge < (double)0.4f) return -1;
    Rnd r = pos_at(&w->ore_pos, nchunk_block_x(nc), y, nchunk_block_z(nc));
    if (rnd_next_float(&r) > 0.7f) return -1;
    if (nchunk_router_here(nc, RF_VEIN_RIDGED) >= 0.0) return -1;
    double t2 = (ridged - (double)0.4f) / ((double)0.6f - (double)0.4f);
    double rich = t2 < 0.0 ? (double)0.1f : (t2 > 1.0 ? (double)0.3f : jm_lerp(t2, (double)0.1f, (double)0.3f));
    if ((double)rnd_next_float(&r) < rich && nchunk_router_here(nc, RF_VEIN_GAP) > (double)-0.3f)
        return rnd_next_float(&r) < 0.02f ? v->raw : v->ore;
    return v->filler;
}

typedef struct { NChunk *nc; AqOld aq; VeinType vt[2]; } OldCtx;
void *terrain_old_ctx_new(McWorld *w) {
    OldCtx *c = xcalloc(1, sizeof *c);
    c->nc = nchunk_new(w->old);
    const McGen *g = w->g;
    c->vt[0].ore = gen_state_id(g, "minecraft:copper_ore"); c->vt[0].raw = gen_state_id(g, "minecraft:raw_copper_block");
    c->vt[0].filler = gen_state_id(g, "minecraft:granite"); c->vt[0].min_y = 0; c->vt[0].max_y = 50;
    c->vt[1].ore = gen_state_id(g, "minecraft:deepslate_iron_ore"); c->vt[1].raw = gen_state_id(g, "minecraft:raw_iron_block");
    c->vt[1].filler = gen_state_id(g, "minecraft:tuff"); c->vt[1].min_y = -60; c->vt[1].max_y = -8;
    return c;
}
void terrain_old_ctx_free(void *p) {
    OldCtx *c = p; if (!c) return;
    nchunk_free(c->nc); free(c->aq.loc); free(c->aq.loc_set); free(c->aq.status); free(c->aq.status_set); free(c);
}

/* стадия CARVERS (carver.c): computeSubstance(SinglePointContext(x, y, z), 0.0) водоносного слоя последнего заполненного чанка */
int terrain_old_carve_substance(void *octx, int x, int y, int z) {
    OldCtx *oc = octx; oc->aq.pt = 1;
    int st = aq_substance_xyz(&oc->aq, x, y, z, 0.0);
    oc->aq.pt = 0;
    return st;
}
int terrain_old_carve_sched(void *octx) { return ((OldCtx *)octx)->aq.sched; }

int terrain_old_fill(McWorld *w, void *octx, int cx, int cz, uint16_t *blocks, PPMarks *marks, char *err, size_t errlen) {
    OldCtx *oc = octx;
    const McGen *g = w->g;
    int nmin = w->ns->min_y > w->min_y ? w->ns->min_y : w->min_y;
    int ntop = w->ns->min_y + w->ns->height; if (ntop > w->min_y + w->height) ntop = w->min_y + w->height;
    int nh = ntop - nmin;
    if (nh <= 0) return MCGEN_OK;
    NChunk *nc = oc->nc;
    nchunk_begin(nc, cx * 16, cz * 16, nmin, nh);
    Picker pk; terrain_picker(w, &pk.lava_level, &pk.sea_level); pk.sea_type = w->def_fluid; pk.lava_type = g->st_lava;
    aq_init(&oc->aq, w, nc, cx, cz, nmin, nh, &pk);
    int veins = w->ns->ore_veins_enabled && w->tweak[MCGEN_TWEAK_ORE_VEINS] != 0.0;
    int cw = nchunk_cell_width(nc), ch = nchunk_cell_height(nc);
    int cell_min_y = jm_floordiv(nmin, ch), ccy = jm_floordiv(nh, ch), ccx = 16 / cw, ccz = 16 / cw;
    int defb = w->def_block;
    nchunk_init_first_cell_x(nc);
    for (int cxi = 0; cxi < ccx; cxi++) {
        nchunk_advance_cell_x(nc, cxi);
        for (int czi = 0; czi < ccz; czi++) {
            for (int cyi = ccy - 1; cyi >= 0; cyi--) {
                nchunk_select_cell_yz(nc, cyi, czi);
                for (int yi = ch - 1; yi >= 0; yi--) {
                    int py = (cell_min_y + cyi) * ch + yi;
                    nchunk_update_y(nc, py, (double)yi / ch);
                    for (int xi = 0; xi < cw; xi++) {
                        int px = cx * 16 + cxi * cw + xi;
                        nchunk_update_x(nc, px, (double)xi / cw);
                        for (int zi = 0; zi < cw; zi++) {
                            int pz = cz * 16 + czi * cw + zi;
                            nchunk_update_z(nc, pz, (double)zi / cw);
                            int st = aq_substance(&oc->aq, nchunk_full_density(nc));
                            if (st < 0 && veins) st = ore_vein(w, nc, oc->vt);
                            if (st < 0) st = defb;
                            int ly = py - w->min_y;
                            if (ly >= 0 && ly < w->height) {
                                blocks[((size_t)ly * 16 + (pz & 15)) * 16 + (px & 15)] = (uint16_t)st;
                                if (oc->aq.sched && (g->state_cls[st] & 4)) ppmarks_add(marks, ly >> 4, px & 15, py & 15, pz & 15);
                            }
                        }
                    }
                }
            }
        }
        nchunk_swap_slices(nc);
    }
    nchunk_stop(nc);
    (void)err; (void)errlen;
    return MCGEN_OK;
}

/* feature_ore.c — руды: ore (OreFeature: цепочка эллипсоидов) и scattered_ore (ScatteredOreFeature). */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct OreCfg { int n; RuleTest **rt; int *state; int size; float discard; } OreCfg;

static void *ore_parse(FParse *p, const Js *cfg) {
    OreCfg *o = fp_alloc(p, sizeof *o);
    Js *t = js_get(cfg, "targets");
    if (!js_is_arr(t)) { fp_fail(p, "ore: нет targets"); return NULL; }
    o->n = t->n; o->rt = fp_alloc(p, sizeof(RuleTest *) * (size_t)(t->n ? t->n : 1)); o->state = fp_alloc(p, sizeof(int) * (size_t)(t->n ? t->n : 1));
    for (int i = 0; i < t->n; i++) {
        o->rt[i] = fp_ruletest(p, js_get(t->items[i], "target")); if (!o->rt[i]) return NULL;
        o->state[i] = bs_from_json(p->bs, js_get(t->items[i], "state"));
        if (o->state[i] < 0) { fp_fail(p, "ore: плохое state"); return NULL; }
    }
    o->size = js_int(js_get(cfg, "size"), 0);
    o->discard = js_numf(js_get(cfg, "discard_chance_on_air_exposure"), 0.0f);
    return o;
}

static int adjacent_to_air(const FCtx *c, int x, int y, int z) {
    for (int d = 0; d < 6; d++) {
        int nx = x + DIR_DX[d], ny = y + DIR_DY[d], nz = z + DIR_DZ[d];
        int st = fc_outside(c, ny) ? c->bs->st_air : fc_get(c, nx, ny, nz);     /* BulkSectionAccess: вне высот — воздух */
        if (fc_is_air(c, st)) return 1;
    }
    return 0;
}
/* AbstractOreFeature.canPlaceOre */
static int can_place_ore(FCtx *c, const OreCfg *o, int k, int st, int x, int y, int z) {
    if (!ruletest_test(c, o->rt[k], st, x, y, z)) return 0;
    if (o->discard <= 0.0f) return 1;
    if (o->discard < 1.0f && frnd_float(c->rnd) >= o->discard) return 1;
    if (o->discard >= 1.0f) return !adjacent_to_air(c, x, y, z);
    return !adjacent_to_air(c, x, y, z);
}

static int ore_do_place(FCtx *c, const OreCfg *o, double x0, double x1, double z0, double z1, double y0, double y1,
                        int xs, int ys, int zs, int szxz, int szy) {
    int placed = 0, size = o->size;
    size_t nbits = (size_t)szxz * szy * szxz;
    u8 *tested = xcalloc(nbits ? nbits : 1, 1);
    double *data = xmalloc(sizeof(double) * 4 * (size_t)(size ? size : 1));
    FRnd *r = c->rnd;
    const float PIF = 3.14159274f;
    for (int i = 0; i < size; i++) {
        float step = (float)i / (float)size;
        double xx = jm_lerp((double)step, x0, x1), yy = jm_lerp((double)step, y0, y1), zz = jm_lerp((double)step, z0, z1);
        double ss = frnd_double(r) * (double)size / 16.0;
        double rad = (((double)(fm_sin((double)(PIF * step)) + 1.0f)) * ss + 1.0) / 2.0;
        data[i * 4] = xx; data[i * 4 + 1] = yy; data[i * 4 + 2] = zz; data[i * 4 + 3] = rad;
    }
    for (int i1 = 0; i1 < size - 1; i1++) {
        if (!(data[i1 * 4 + 3] <= 0.0)) {
            for (int i2 = i1 + 1; i2 < size; i2++) {
                if (!(data[i2 * 4 + 3] <= 0.0)) {
                    double dx = data[i1 * 4] - data[i2 * 4], dy = data[i1 * 4 + 1] - data[i2 * 4 + 1], dz = data[i1 * 4 + 2] - data[i2 * 4 + 2], dr = data[i1 * 4 + 3] - data[i2 * 4 + 3];
                    if (dr * dr > dx * dx + dy * dy + dz * dz) {
                        if (dr > 0.0) data[i2 * 4 + 3] = -1.0; else data[i1 * 4 + 3] = -1.0;
                    }
                }
            }
        }
    }
    for (int i = 0; i < size; i++) {
        double rad = data[i * 4 + 3];
        if (rad < 0.0) continue;
        double xx = data[i * 4], yy = data[i * 4 + 1], zz = data[i * 4 + 2];
        int xmin = jm_floor_d(xx - rad); if (xmin < xs) xmin = xs;
        int ymin = jm_floor_d(yy - rad); if (ymin < ys) ymin = ys;
        int zmin = jm_floor_d(zz - rad); if (zmin < zs) zmin = zs;
        int xmax = jm_floor_d(xx + rad); if (xmax < xmin) xmax = xmin;
        int ymax = jm_floor_d(yy + rad); if (ymax < ymin) ymax = ymin;
        int zmax = jm_floor_d(zz + rad); if (zmax < zmin) zmax = zmin;
        for (int x = xmin; x <= xmax; x++) {
            double xd = ((double)x + 0.5 - xx) / rad;
            if (xd * xd >= 1.0) continue;
            for (int y = ymin; y <= ymax; y++) {
                double yd = ((double)y + 0.5 - yy) / rad;
                if (xd * xd + yd * yd >= 1.0) continue;
                for (int z = zmin; z <= zmax; z++) {
                    double zd = ((double)z + 0.5 - zz) / rad;
                    if (xd * xd + yd * yd + zd * zd < 1.0 && !fc_outside(c, y)) {
                        size_t bi = (size_t)(x - xs) + (size_t)(y - ys) * szxz + (size_t)(z - zs) * szxz * szy;
                        if (!tested[bi]) {
                            tested[bi] = 1;
                            if (fc_ensure_can_write(c, x, z)) {
                                int st = fc_get(c, x, y, z);
                                for (int k = 0; k < o->n; k++) {
                                    if (can_place_ore(c, o, k, st, x, y, z)) { fc_set_raw(c, x, y, z, o->state[k]); placed++; break; }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    free(tested); free(data);
    return placed > 0;
}


/* OreFeature.doPlace 26.4-snapshot-2: сферы режутся по колонкам (x, z) -> [yMin, yMax] (объединение отрезков всех сфер колонки), клетки обходятся колонка за колонкой
 * (x внешний, z, затем y по возрастанию) — порядок обхода влияет на вызовы nextFloat() (discard_chance_on_air_exposure); отсев сфер — по расстоянию вдоль оси. */
static int ore_do_place_v264(FCtx *c, const OreCfg *o, double x0, double x1, double z0, double z1, double y0, double y1, int xs, int zs, int szxz) {
    int placed = 0, size = o->size;
    double *data = xmalloc(sizeof(double) * 4 * (size_t)(size ? size : 1));
    FRnd *r = c->rnd;
    const float PIF = 3.14159274f;
    double max_r = 0.0;
    for (int i = 0; i < size; i++) {
        float step = (float)i / (float)size;
        double xx = jm_lerp((double)step, x0, x1), yy = jm_lerp((double)step, y0, y1), zz = jm_lerp((double)step, z0, z1);
        double ss = frnd_double(r) * (double)size / 16.0;
        double rad = (((double)(fm_sin((double)(PIF * step)) + 1.0f)) * ss + 1.0) / 2.0;
        data[i * 4] = xx; data[i * 4 + 1] = yy; data[i * 4 + 2] = zz; data[i * 4 + 3] = rad;
        if (rad > max_r) max_r = rad;
    }
    double dx0 = x1 - x0, dy0 = y1 - y0, dz0 = z1 - z0;
    double step_dist = sqrt(dx0 * dx0 + dy0 * dy0 + dz0 * dz0) / (double)size;
    for (int i2 = 0; i2 < size - 1; i2++) {
        if (data[i2 * 4 + 3] > 0.0) {
            double radius = data[i2 * 4 + 3];
            for (int j = i2 + 1; j < size; j++) {
                double other = data[j * 4 + 3];
                if (other > 0.0) {
                    double dist = (double)(j - i2) * step_dist, dr = radius - other;
                    if (dr * dr > dist * dist) {
                        if (dr > 0.0) data[j * 4 + 3] = -1.0;
                        else { data[i2 * 4 + 3] = -1.0; break; }
                    }
                    if (dist > max_r) break;
                }
            }
        }
    }
    int lo_y = c->min_y, hi_y = c->min_y + c->height - 1;        /* level.getMinY()/getMaxY() */
    int grid_max_x = xs + szxz - 1, grid_max_z = zs + szxz - 1;
    size_t ncol = (size_t)szxz * szxz;
    int *col_min = xmalloc(sizeof(int) * (ncol ? ncol : 1)), *col_max = xmalloc(sizeof(int) * (ncol ? ncol : 1));
    for (size_t i = 0; i < ncol; i++) { col_min[i] = 0x7FFFFFFF; col_max[i] = (int)0x80000000; }
    int tminx = szxz, tmaxx = -1, tminz = szxz, tmaxz = -1;
    for (int i = 0; i < size; i++) {
        double rad = data[i * 4 + 3];
        if (rad < 0.0) continue;
        double rsq = rad * rad, xx = data[i * 4], yy = data[i * 4 + 1], zz = data[i * 4 + 2];
        int xmin = jm_floor_d(xx - rad); if (xmin < xs) xmin = xs;
        int zmin = jm_floor_d(zz - rad); if (zmin < zs) zmin = zs;
        int xmax = jm_floor_d(xx + rad); if (xmax < xmin) xmax = xmin; if (xmax > grid_max_x) xmax = grid_max_x;
        int zmax = jm_floor_d(zz + rad); if (zmax < zmin) zmax = zmin; if (zmax > grid_max_z) zmax = grid_max_z;
        if (xmin - xs < tminx) tminx = xmin - xs;
        if (xmax - xs > tmaxx) tmaxx = xmax - xs;
        if (zmin - zs < tminz) tminz = zmin - zs;
        if (zmax - zs > tmaxz) tmaxz = zmax - zs;
        for (int x = xmin; x <= xmax; x++) {
            double dx = ((double)x + 0.5) - xx, rem_x = rsq - dx * dx;
            if (rem_x > 0.0) {
                int row = (x - xs) * szxz - zs;
                for (int z = zmin; z <= zmax; z++) {
                    double dz = ((double)z + 0.5) - zz, rem = rem_x - dz * dz;
                    if (rem > 0.0) {
                        double half = sqrt(rem);
                        int ylo = jm_floor_d((yy - 0.5) - half) + 1; if (ylo < lo_y) ylo = lo_y;
                        int yhi = jm_d2i(ceil((yy - 0.5) + half)) - 1; if (yhi > hi_y) yhi = hi_y;
                        if (ylo <= yhi) {
                            int idx = row + z;
                            if (ylo < col_min[idx]) col_min[idx] = ylo;
                            if (yhi > col_max[idx]) col_max[idx] = yhi;
                        }
                    }
                }
            }
        }
    }
    for (int gx = tminx; gx <= tmaxx; gx++) {
        int x = xs + gx;
        for (int gz = tminz; gz <= tmaxz; gz++) {
            int idx = gx * szxz + gz, ymin = col_min[idx];
            if (ymin == 0x7FFFFFFF) continue;
            int ymax = col_max[idx], z = zs + gz;
            if (!fc_ensure_can_write(c, x, z)) continue;
            for (int y = ymin; y <= ymax; y++) {
                int st = fc_get(c, x, y, z);
                for (int k = 0; k < o->n; k++) {
                    if (can_place_ore(c, o, k, st, x, y, z)) { fc_set_raw(c, x, y, z, o->state[k]); placed++; break; }
                }
            }
        }
    }
    free(col_min); free(col_max); free(data);
    return placed > 0;
}

static int ore_place_impl(FCtx *c, const void *cfg, int ox, int oy, int oz);
static int ore_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    static int tr = -1; if (tr < 0) tr = getenv("MCGEN_ORE_TRACE") != NULL;       /* отладка: сверка попыток жилы с настоящим сервером (tools/gt/agent, хук OreFeature.place) */
    int res = ore_place_impl(c, cfg, ox, oy, oz);
    if (tr) {
        char wg[64] = "";
        if (oy >= 40 && oy <= 110) { long sum = 0; int mx = -9999; for (int dx = -9; dx <= 9; dx++) for (int dz = -9; dz <= 9; dz++) { int h = fc_height(c, HM_OCEAN_FLOOR_WG, ox + dx, oz + dz); sum += h; if (h > mx) mx = h; } snprintf(wg, sizeof wg, " wg=%ld/%d", sum, mx);
            const char *at = getenv("MCGEN_ORE_WGAT"); char key[48]; snprintf(key, sizeof key, "%d,%d,%d", ox, oy, oz);
            if (at && strstr(at, key)) { fprintf(stderr, "WGBOX %d %d %d:", ox, oy, oz); for (int dz = -9; dz <= 9; dz++) for (int dx = -9; dx <= 9; dx++) fprintf(stderr, " %d", fc_height(c, HM_OCEAN_FLOOR_WG, ox + dx, oz + dz)); fprintf(stderr, "\n"); } }
        fprintf(stderr, "ORE chunk(%d,%d) at (%d,%d,%d) res=%d rnd=%llx%s\n", c->ccx, c->ccz, ox, oy, oz, res, (unsigned long long)c->rnd->x.lo, wg);
    }
    return res;
}
static int ore_place_impl(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const OreCfg *o = cfg; FRnd *r = c->rnd;
    const float PIF = 3.14159274f;
    float dir = frnd_float(r) * PIF;
    float spread = (float)o->size / 8.0f;
    int max_radius = jm_d2i(ceil((double)(((float)o->size / 16.0f * 2.0f + 1.0f) / 2.0f)));
    double x0 = (double)ox + sin((double)dir) * (double)spread, x1 = (double)ox - sin((double)dir) * (double)spread;
    double z0 = (double)oz + cos((double)dir) * (double)spread, z1 = (double)oz - cos((double)dir) * (double)spread;
    double y0 = (double)(oy + frnd_int_bound(r, 3) - 2);
    double y1 = (double)(oy + frnd_int_bound(r, 3) - 2);
    int cs = jm_d2i(ceil((double)spread));
    int xs = ox - cs - max_radius, ys = oy - 2 - max_radius, zs = oz - cs - max_radius;
    int szxz = 2 * (cs + max_radius), szy = 2 * (2 + max_radius);
    if (c->g->version >= V26_4) {         /* level.anyHeightMatches(OCEAN_FLOOR_WG, xStart, zStart, xStart + sizeXZ − 1, zStart + sizeXZ − 1, yStart, MAX) */
        for (int xp = xs; xp < xs + szxz; xp++) for (int zp = zs; zp < zs + szxz; zp++)
            if (ys <= fc_height(c, HM_OCEAN_FLOOR_WG, xp, zp)) return ore_do_place_v264(c, o, x0, x1, z0, z1, y0, y1, xs, zs, szxz);
        return 0;
    }
    for (int xp = xs; xp <= xs + szxz; xp++) for (int zp = zs; zp <= zs + szxz; zp++)
        if (ys <= fc_height(c, HM_OCEAN_FLOOR_WG, xp, zp)) return ore_do_place(c, o, x0, x1, z0, z1, y0, y1, xs, ys, zs, szxz, szy);
    return 0;
}

static int scattered_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const OreCfg *o = cfg; FRnd *r = c->rnd;
    int tries = frnd_int_bound(r, o->size + 1);
    for (int i = 0; i < tries; i++) {
        int m = i < 7 ? i : 7;
        int d[3];
        for (int k = 0; k < 3; k++) { float a = frnd_float(r); float b = frnd_float(r); d[k] = jm_roundf((a - b) * (float)m); }
        int x = ox + d[0], y = oy + d[1], z = oz + d[2];
        int st = fc_get(c, x, y, z);
        for (int k = 0; k < o->n; k++) {
            if (can_place_ore(c, o, k, st, x, y, z)) { fc_set(c, x, y, z, o->state[k], 2); break; }
        }
    }
    return 1;
}

static const FeatType T_ORE = { "minecraft:ore", ore_parse, ore_place };
static const FeatType T_SCATTERED = { "minecraft:scattered_ore", ore_parse, scattered_place };
void feature_register_ore(void) { feature_register_type(&T_ORE); feature_register_type(&T_SCATTERED); }

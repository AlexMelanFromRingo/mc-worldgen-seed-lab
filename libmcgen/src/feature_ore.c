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

static int ore_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
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

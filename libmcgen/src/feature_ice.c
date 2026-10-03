/* feature_ice.c — ледяные фичи: blue_ice, spike (ледяные шипы), iceberg (айсберги). Вычисления float/double — как в Java (HotSpot: Math.pow(x, 2.0) == x*x). */
#include "feature_misc.h"
#include <stdio.h>
#include <stdlib.h>

static int blk_of(FParse *p, const char *name) { return bs_block_index(p->bs, name); }
static int def_of(FParse *p, const char *name) { int b = bs_block_index(p->bs, name); return b < 0 ? -1 : bs_default(p->bs, b); }
static inline int mth_ceil_f(float f) { int i = (int)f; return f > (float)i ? i + 1 : i; }
static inline float mth_clamp_f(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ====================================================================== blue_ice */
typedef struct BlueIceCfg { int water, packed, ice, blue; int blue_state; } BlueIceCfg;
static void *blueice_parse(FParse *p, const Js *cfg) {
    BlueIceCfg *b = fp_alloc(p, sizeof *b);
    b->water = blk_of(p, "minecraft:water"); b->packed = blk_of(p, "minecraft:packed_ice"); b->ice = blk_of(p, "minecraft:ice"); b->blue = blk_of(p, "minecraft:blue_ice");
    b->blue_state = bs_default(p->bs, b->blue);
    return b;
}
static int blueice_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const BlueIceCfg *b = cfg; FRnd *r = c->rnd;
    if (oy > c->sea_level - 1) return 0;
    if (!fc_is_block(c, fc_get(c, ox, oy, oz), b->water) && !fc_is_block(c, fc_get(c, ox, oy - 1, oz), b->water)) return 0;
    int found = 0;
    for (int d = 0; d < 6; d++) if (d != DIR_DOWN && fc_is_block(c, fc_get(c, ox + DIR_DX[d], oy + DIR_DY[d], oz + DIR_DZ[d]), b->packed)) { found = 1; break; }
    if (!found) return 0;
    fc_set(c, ox, oy, oz, b->blue_state, 2);
    for (int i = 0; i < 200; i++) {
        int yo = frnd_int_bound(r, 5) - frnd_int_bound(r, 6);
        int xz = 3;
        if (yo < 2) xz += yo / 2;
        if (xz >= 1) {
            int dx = frnd_int_bound(r, xz) - frnd_int_bound(r, xz);
            int dz = frnd_int_bound(r, xz) - frnd_int_bound(r, xz);
            int px = ox + dx, py = oy + yo, pz = oz + dz;
            int st = fc_get(c, px, py, pz);
            if (fc_is_air(c, st) || fc_is_block(c, st, b->water) || fc_is_block(c, st, b->packed) || fc_is_block(c, st, b->ice)) {
                for (int d = 0; d < 6; d++) if (fc_is_block(c, fc_get(c, px + DIR_DX[d], py + DIR_DY[d], pz + DIR_DZ[d]), b->blue)) { fc_set(c, px, py, pz, b->blue_state, 2); break; }
            }
        }
    }
    return 1;
}

/* ====================================================================== spike (ice_spike) */
typedef struct SpikeCfg { int state; BPred *can_place_on, *can_replace; } SpikeCfg;
static void *spike_parse(FParse *p, const Js *cfg) {
    SpikeCfg *s = fp_alloc(p, sizeof *s);
    s->state = bs_from_json(p->bs, js_get(cfg, "state")); if (s->state < 0) { fp_fail(p, "spike: плохое state"); return NULL; }
    s->can_place_on = fp_bpred(p, js_get(cfg, "can_place_on")); s->can_replace = fp_bpred(p, js_get(cfg, "can_replace"));
    return s->can_place_on && s->can_replace ? s : NULL;
}
static int spike_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const SpikeCfg *s = cfg; FRnd *r = c->rnd;
    while (fc_is_air(c, fc_get(c, ox, oy, oz)) && oy > c->min_y + 2) oy--;
    if (!bpred_test(c, s->can_place_on, ox, oy, oz)) return 0;
    oy += frnd_int_bound(r, 4);
    int height = frnd_int_bound(r, 4) + 7;
    int width = height / 4 + frnd_int_bound(r, 2);
    if (width > 1 && frnd_int_bound(r, 60) == 0) oy += 10 + frnd_int_bound(r, 30);
    for (int yo = 0; yo < height; yo++) {
        float scale = (1.0F - (float)yo / (float)height) * (float)width;
        int nw = mth_ceil_f(scale);
        for (int xo = -nw; xo <= nw; xo++) {
            float dx = (float)iabs_(xo) - 0.25F;
            for (int zo = -nw; zo <= nw; zo++) {
                float dz = (float)iabs_(zo) - 0.25F;
                if (((xo == 0 && zo == 0) || !(dx * dx + dz * dz > scale * scale))
                    && ((xo != -nw && xo != nw && zo != -nw && zo != nw) || !(frnd_float(r) > 0.75F))) {
                    int px = ox + xo, py = oy + yo, pz = oz + zo;
                    if (fc_is_air(c, fc_get(c, px, py, pz)) || bpred_test(c, s->can_replace, px, py, pz)) fc_set(c, px, py, pz, s->state, 3);
                    if (yo != 0 && nw > 1) {
                        py = oy - yo;
                        if (fc_is_air(c, fc_get(c, px, py, pz)) || bpred_test(c, s->can_replace, px, py, pz)) fc_set(c, px, py, pz, s->state, 3);
                    }
                }
            }
        }
    }
    int pw = width - 1;
    if (pw < 0) pw = 0; else if (pw > 1) pw = 1;
    for (int xo = -pw; xo <= pw; xo++) for (int zo = -pw; zo <= pw; zo++) {
        int cx = ox + xo, cy = oy - 1, cz = oz + zo, run = 50;
        if (iabs_(xo) == 1 && iabs_(zo) == 1) run = frnd_int_bound(r, 5);
        while (cy > 50) {
            int st = fc_get(c, cx, cy, cz);
            if (!fc_is_air(c, st) && !bpred_test(c, s->can_replace, cx, cy, cz) && st != s->state) break;
            fc_set(c, cx, cy, cz, s->state, 3);
            cy--;
            if (--run <= 0) { cy -= frnd_int_bound(r, 5) + 1; run = frnd_int_bound(r, 5); }
        }
    }
    return 1;
}

/* ====================================================================== iceberg */
typedef struct IceCfg { int main, water, snow_block, ice, snow, packed, blue, air_state, water_state, snow_block_state, snow_state; } IceCfg;
static void *iceberg_parse(FParse *p, const Js *cfg) {
    IceCfg *s = fp_alloc(p, sizeof *s);
    s->main = bs_from_json(p->bs, js_get(cfg, "state")); if (s->main < 0) { fp_fail(p, "iceberg: плохое state"); return NULL; }
    s->water = blk_of(p, "minecraft:water"); s->snow_block = blk_of(p, "minecraft:snow_block"); s->ice = blk_of(p, "minecraft:ice"); s->snow = blk_of(p, "minecraft:snow");
    s->packed = blk_of(p, "minecraft:packed_ice"); s->blue = blk_of(p, "minecraft:blue_ice");
    s->air_state = def_of(p, "minecraft:air"); s->water_state = def_of(p, "minecraft:water"); s->snow_block_state = def_of(p, "minecraft:snow_block");
    return s;
}
#define IS_ICEBERG(c, s, st) (fc_is_block(c, st, (s)->packed) || fc_is_block(c, st, (s)->snow_block) || fc_is_block(c, st, (s)->blue))
typedef struct IceCtx { FCtx *c; const IceCfg *s; FRnd *r; } IceCtx;

static double ice_dist_circle(IceCtx *k, int xo, int zo, int radius) {
    float off = 10.0F * mth_clamp_f(frnd_float(k->r), 0.2F, 0.8F) / (float)radius;
    double dx = (double)xo, dz = (double)zo;
    return (double)off + dx * dx + dz * dz - (double)radius * (double)radius;
}
static double ice_dist_ellipse(int xo, int zo, int ox, int oz, int a, int c, double angle) {
    double u = (((double)(xo - ox)) * cos(angle) - ((double)(zo - oz)) * sin(angle)) / (double)a;
    double v = (((double)(xo - ox)) * sin(angle) + ((double)(zo - oz)) * cos(angle)) / (double)c;
    return u * u + v * v - 1.0;
}
static int ice_radius_round(IceCtx *k, int yo, int height, int width) {
    float kk = 3.5F - frnd_float(k->r);
    float y2 = (float)((double)yo * (double)yo);
    float scale = (1.0F - y2 / ((float)height * kk)) * (float)width;
    if (height > 15 + frnd_int_bound(k->r, 5)) {
        int t = yo < 3 + frnd_int_bound(k->r, 6) ? yo / 2 : yo;
        scale = (1.0F - (float)t / ((float)height * kk * 0.4F)) * (float)width;
    }
    return mth_ceil_f(scale / 2.0F);
}
static int ice_radius_ellipse(int yo, int height, int width) {
    float y2 = (float)((double)yo * (double)yo);
    float scale = (1.0F - y2 / ((float)height * 1.0F)) * (float)width;
    return mth_ceil_f(scale / 2.0F);
}
static int ice_radius_steep(IceCtx *k, int yo, int height, int width) {
    float kk = 1.0F + frnd_float(k->r) / 2.0F;
    float scale = (1.0F - (float)yo / ((float)height * kk)) * (float)width;
    return mth_ceil_f(scale / 2.0F);
}
static void ice_set_block(IceCtx *k, int x, int y, int z, int hdiff, int height, int ellipse, int snow_top) {
    FCtx *c = k->c; const IceCfg *s = k->s; FRnd *r = k->r;
    int st = fc_get(c, x, y, z);
    if (fc_is_air(c, st) || fc_is_block(c, st, s->snow_block) || fc_is_block(c, st, s->ice) || fc_is_block(c, st, s->water)) {
        int randomness = !ellipse || frnd_double(r) > 0.05;
        int divisor = ellipse ? 3 : 2;
        if (snow_top && !fc_is_block(c, st, s->water) && (double)hdiff <= (double)frnd_int_bound(r, imax_(1, height / divisor)) + (double)height * 0.6 && randomness)
            fc_set(c, x, y, z, s->snow_block_state, 3);
        else fc_set(c, x, y, z, s->main, 3);
    }
}
static int ice_ellipse_c(int yo, int height, int c) { if (yo > 0 && height - yo <= 3) c -= 4 - (height - yo); return c; }
static void ice_gen_block(IceCtx *k, int ox, int oy, int oz, int height, int xo, int yo, int zo, int radius, int a, int ellipse, int ec, double angle, int snow_top) {
    double sd = ellipse ? ice_dist_ellipse(xo, zo, 0, 0, a, ice_ellipse_c(yo, height, ec), angle) : ice_dist_circle(k, xo, zo, radius);
    if (sd < 0.0) {
        double cmp = ellipse ? -0.5 : (double)(-6 - frnd_int_bound(k->r, 3));
        if (sd > cmp && frnd_double(k->r) > 0.9) return;
        ice_set_block(k, ox + xo, oy + yo, oz + zo, height - yo, height, ellipse, snow_top);
    }
}
static void ice_smooth(IceCtx *k, int ox, int oy, int oz, int width, int height, int ellipse, int ea) {
    FCtx *c = k->c; const IceCfg *s = k->s;
    int a = ellipse ? ea : width / 2;
    for (int x = -a; x <= a; x++) for (int z = -a; z <= a; z++) for (int yo = 0; yo <= height; yo++) {
        int px = ox + x, py = oy + yo, pz = oz + z;
        int st = fc_get(c, px, py, pz);
        if (IS_ICEBERG(c, s, st) || fc_is_block(c, st, s->snow)) {
            if (fc_is_air(c, fc_get(c, px, py - 1, pz))) {
                fc_set(c, px, py, pz, s->air_state, 3);
                fc_set(c, px, py + 1, pz, s->air_state, 3);
            } else if (IS_ICEBERG(c, s, st)) {
                int cnt = 0;
                if (!(IS_ICEBERG(c, s, fc_get(c, px - 1, py, pz)))) cnt++;
                if (!(IS_ICEBERG(c, s, fc_get(c, px + 1, py, pz)))) cnt++;
                if (!(IS_ICEBERG(c, s, fc_get(c, px, py, pz - 1)))) cnt++;
                if (!(IS_ICEBERG(c, s, fc_get(c, px, py, pz + 1)))) cnt++;
                if (cnt >= 3) fc_set(c, px, py, pz, s->air_state, 3);
            }
        }
    }
}
static void ice_carve(IceCtx *k, int radius, int yo, int gx, int gy, int gz, int under, double angle, int lx, int lz, int ea, int ec) {
    FCtx *c = k->c; const IceCfg *s = k->s;
    int a = radius + 1 + ea / 3, cc = imin_(radius - 3, 3) + ec / 2 - 1;
    for (int xo = -a; xo < a; xo++) for (int zo = -a; zo < a; zo++) {
        double sd = ice_dist_ellipse(xo, zo, lx, lz, a, cc, angle);
        if (sd < 0.0) {
            int px = gx + xo, py = gy + yo, pz = gz + zo, st = fc_get(c, px, py, pz);
            if (IS_ICEBERG(c, s, st) || fc_is_block(c, st, s->snow_block)) {
                if (under) fc_set(c, px, py, pz, s->water_state, 3);
                else {
                    fc_set(c, px, py, pz, s->air_state, 3);
                    if (fc_is_block(c, fc_get(c, px, py + 1, pz), s->snow)) fc_set(c, px, py + 1, pz, s->air_state, 3);
                }
            }
        }
    }
}
static int iceberg_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const IceCfg *s = cfg; FRnd *r = c->rnd; IceCtx k = { c, s, r };
    oy = c->sea_level;
    int snow_top = frnd_double(r) > 0.7;
    const double PI = 3.141592653589793;
    double angle = frnd_double(r) * 2.0 * PI;
    int ea = 11 - frnd_int_bound(r, 5);
    int ec = 3 + frnd_int_bound(r, 3);
    int ellipse = frnd_double(r) > 0.7;
    int over = ellipse ? frnd_int_bound(r, 6) + 6 : frnd_int_bound(r, 15) + 3;
    if (!ellipse && frnd_double(r) > 0.9) over += frnd_int_bound(r, 19) + 7;
    int under = imin_(over + frnd_int_bound(r, 11), 18);
    int w0 = over + frnd_int_bound(r, 7); int w1 = frnd_int_bound(r, 5);
    int width = imin_(w0 - w1, 11);
    int a = ellipse ? ea : 11;
    for (int xo = -a; xo < a; xo++) for (int zo = -a; zo < a; zo++) for (int yo = 0; yo < over; yo++) {
        int radius = ellipse ? ice_radius_ellipse(yo, over, width) : ice_radius_round(&k, yo, over, width);
        if (ellipse || xo < radius) ice_gen_block(&k, ox, oy, oz, over, xo, yo, zo, radius, a, ellipse, ec, angle, snow_top);
    }
    ice_smooth(&k, ox, oy, oz, width, over, ellipse, ea);
    for (int xo = -a; xo < a; xo++) for (int zo = -a; zo < a; zo++) for (int yo = -1; yo > -under; yo--) {
        int na = a;
        if (ellipse) { float y2 = (float)((double)yo * (double)yo); na = mth_ceil_f((float)a * (1.0F - y2 / ((float)under * 8.0F))); }
        int radius = ice_radius_steep(&k, -yo, under, width);
        if (xo < radius) ice_gen_block(&k, ox, oy, oz, under, xo, yo, zo, radius, na, ellipse, ec, angle, snow_top);
    }
    int cut = ellipse ? frnd_double(r) > 0.1 : frnd_double(r) > 0.7;
    if (cut) {
        int sx = frnd_bool(r) ? -1 : 1, sz = frnd_bool(r) ? -1 : 1;
        int xoff = frnd_int_bound(r, imax_(width / 2 - 2, 1));
        if (frnd_bool(r)) xoff = width / 2 + 1 - frnd_int_bound(r, imax_(width - width / 2 - 1, 1));
        int zoff = frnd_int_bound(r, imax_(width / 2 - 2, 1));
        if (frnd_bool(r)) zoff = width / 2 + 1 - frnd_int_bound(r, imax_(width - width / 2 - 1, 1));
        if (ellipse) { xoff = zoff = frnd_int_bound(r, imax_(ea - 5, 1)); }
        int lx = sx * xoff, lz = sz * zoff;
        double ang = ellipse ? angle + (PI / 2) : frnd_double(r) * 2.0 * PI;
        for (int yo = 0; yo < over - 3; yo++) { int radius = ice_radius_round(&k, yo, over, width); ice_carve(&k, radius, yo, ox, oy, oz, 0, ang, lx, lz, ea, ec); }
        for (int yo = -1; yo > -over + frnd_int_bound(r, 5); yo--) { int radius = ice_radius_steep(&k, -yo, over, width); ice_carve(&k, radius, yo, ox, oy, oz, 1, ang, lx, lz, ea, ec); }
    }
    return 1;
}

static const FeatType T_BLUEICE = { "minecraft:blue_ice", blueice_parse, blueice_place };
static const FeatType T_SPIKE = { "minecraft:spike", spike_parse, spike_place };
static const FeatType T_ICEBERG = { "minecraft:iceberg", iceberg_parse, iceberg_place };
void feature_register_ice(void) {
    feature_register_type(&T_BLUEICE); feature_register_type(&T_SPIKE); feature_register_type(&T_ICEBERG);
}

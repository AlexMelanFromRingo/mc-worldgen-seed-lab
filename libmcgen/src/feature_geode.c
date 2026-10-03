/* feature_geode.c — аметистовая геода (GeodeFeature). Шум геоды: NormalNoise(-4, [1.0]) на LegacyRandomSource(сид мира): 26.3+ — «createParity» (float, NoiseStack),
 * 26.1/26.2 — NormalNoise.create (double). Порядок вызовов ГСЧ — как в игре. */
#include "feature_misc.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct GeodeCfg {
    BSProv *filling, *inner, *alt_inner, *middle, *outer;
    int ninner; int inner_placements[8];
    u8 *cannot_replace, *invalid;
    double f_filling, f_inner, f_middle, f_outer;
    double crack_chance, crack_base, crack_off_i; int crack_off;
    double use_potential, use_alt0; int require_alt0;
    IntProv *outer_wall, *points, *point_offset;
    int min_gen, max_gen; double noise_mul; int invalid_thr;
    int air, fluid_none_dummy;
} GeodeCfg;

static double dflt_d(const Js *o, const char *k, double d) { const Js *v = js_get(o, k); return v ? js_num(v, d) : d; }

static void *geode_parse(FParse *p, const Js *cfg) {
    GeodeCfg *g = fp_alloc(p, sizeof *g);
    const Js *b = js_get(cfg, "blocks");
    g->filling = fp_bsprov(p, js_get(b, "filling_provider")); g->inner = fp_bsprov(p, js_get(b, "inner_layer_provider"));
    g->alt_inner = fp_bsprov(p, js_get(b, "alternate_inner_layer_provider")); g->middle = fp_bsprov(p, js_get(b, "middle_layer_provider"));
    g->outer = fp_bsprov(p, js_get(b, "outer_layer_provider"));
    if (!g->filling || !g->inner || !g->alt_inner || !g->middle || !g->outer) return NULL;
    const Js *ip = js_get(b, "inner_placements");
    if (!js_is_arr(ip) || ip->n < 1 || ip->n > 8) { fp_fail(p, "geode: inner_placements"); return NULL; }
    for (int i = 0; i < ip->n; i++) { g->inner_placements[i] = bs_from_json(p->bs, ip->items[i]); if (g->inner_placements[i] < 0) { fp_fail(p, "geode: плохое inner_placement"); return NULL; } }
    g->ninner = ip->n;
    g->cannot_replace = fp_blockset(p, js_get(b, "cannot_replace")); g->invalid = fp_blockset(p, js_get(b, "invalid_blocks"));
    if (!g->cannot_replace || !g->invalid) return NULL;
    const Js *l = js_get(cfg, "layers");
    g->f_filling = dflt_d(l, "filling", 1.7); g->f_inner = dflt_d(l, "inner_layer", 2.2); g->f_middle = dflt_d(l, "middle_layer", 3.2); g->f_outer = dflt_d(l, "outer_layer", 4.2);
    const Js *c = js_get(cfg, "crack");
    g->crack_chance = dflt_d(c, "generate_crack_chance", 1.0); g->crack_base = dflt_d(c, "base_crack_size", 2.0);
    g->crack_off = js_get(c, "crack_point_offset") ? js_int(js_get(c, "crack_point_offset"), 2) : 2;
    g->use_potential = dflt_d(cfg, "use_potential_placements_chance", 0.35); g->use_alt0 = dflt_d(cfg, "use_alternate_layer0_chance", 0.0);
    g->require_alt0 = js_get(cfg, "placements_require_layer0_alternate") ? js_bool(js_get(cfg, "placements_require_layer0_alternate"), 1) : 1;
    /* значения по умолчанию IntProvider: UniformInt(4,5), (3,4), (1,2) */
    static const char *D_OW = "{\"type\":\"minecraft:uniform\",\"min_inclusive\":4,\"max_inclusive\":5}";
    static const char *D_PT = "{\"type\":\"minecraft:uniform\",\"min_inclusive\":3,\"max_inclusive\":4}";
    static const char *D_PO = "{\"type\":\"minecraft:uniform\",\"min_inclusive\":1,\"max_inclusive\":2}";
    const char *defs[3] = { D_OW, D_PT, D_PO }; const char *keys[3] = { "outer_wall_distance", "distribution_points", "point_offset" };
    IntProv **dst[3] = { &g->outer_wall, &g->points, &g->point_offset };
    for (int i = 0; i < 3; i++) {
        const Js *v = js_get(cfg, keys[i]);
        if (v) *dst[i] = fp_intprov(p, v);
        else { JsDoc *d = js_parse(defs[i], strlen(defs[i]), NULL, 0); *dst[i] = fp_intprov(p, js_root(d)); js_free(d); }
        if (!*dst[i]) return NULL;
    }
    g->min_gen = js_get(cfg, "min_gen_offset") ? js_int(js_get(cfg, "min_gen_offset"), -16) : -16;
    g->max_gen = js_get(cfg, "max_gen_offset") ? js_int(js_get(cfg, "max_gen_offset"), 16) : 16;
    g->noise_mul = dflt_d(cfg, "noise_multiplier", 0.05);
    g->invalid_thr = js_int(js_get(cfg, "invalid_blocks_threshold"), 0);
    g->air = bs_default(p->bs, bs_block_index(p->bs, "minecraft:air"));
    return g;
}

/* NormalNoise.createParity(-4, 1.0) → параметры (NoiseParams) — арифметика Java: computeParityBaseAmplitude для одной октавы с амплитудой 1.0 */
static void geode_noise_params(NoiseParams *P) {
    memset(P, 0, sizeof *P);
    P->new_format = 1; P->first_octave = -4; P->count = 1; P->has_mod = 0; P->normalize = 1;
    /* buildOctaves(-4, 1.0, 1, normalize, [1.0]): амплитуда 1.0 * 0.5^0 / (0.5^-1 − 1.0) */
    double amp = 1.0 * (1.0 * pow(0.5, -(double)(1 - 1)) / (pow(0.5, -(double)1) - 1.0));
    double dev = sqrt((0.2702247831245211 * amp) * (0.2702247831245211 * amp));
    double new_nf = (amp * 0.3333333333333333) / (dev * sqrt(2.0));
    double old_nf = 1.0 * 0.5 * 0.3333333333333333 / (0.1 * (1.0 + 1.0 / (double)(0 + 1)));
    P->base_amplitude = old_nf / new_nf;
}

static int geode_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    const GeodeCfg *g = cfg; FRnd *r = c->rnd; const BsTab *bs = c->bs;
    int px[24], py[24], pz[24], poff[24], np = 0;
    int num_points = intprov_sample(g->points, r);
    if (num_points > 24) num_points = 24;          /* IntProviders.codec(1, 20); у ванили не более 4 */
    /* шум на LegacyRandomSource(сид мира): 26.3 — float NoiseStack, 26.1/26.2 — double NormalNoise */
    NStack ns; OldNormal on; int newf = c->g->newf;
    {
        NoiseParams P; geode_noise_params(&P);
        Rnd lr = rnd_legacy_seed(c->w->seeds.features);
        if (newf) nn_new_create(&ns, &P, &lr);
        else { P.new_format = 0; P.amp[0] = 1.0; old_normal_create(&on, &lr, &P, 1); }
    }
    int crack_x[3], crack_y[3], crack_z[3], ncrack = 0;
    double crack_adj = (double)num_points / (double)intprov_max(g->outer_wall);
    double inner_air = 1.0 / sqrt(g->f_filling);
    double innermost = 1.0 / sqrt(g->f_inner + crack_adj);
    double inner_crust = 1.0 / sqrt(g->f_middle + crack_adj);
    double outer_crust = 1.0 / sqrt(g->f_outer + crack_adj);
    double crack_size = 1.0 / sqrt(g->crack_base + frnd_double(r) / 2.0 + (num_points > 3 ? crack_adj : 0.0));
    int gen_crack = (double)frnd_float(r) < g->crack_chance;
    int invalid = 0, ok = 1;
    for (int i = 0; i < num_points && ok; i++) {
        int x = intprov_sample(g->outer_wall, r), y = intprov_sample(g->outer_wall, r), z = intprov_sample(g->outer_wall, r);
        int bx = ox + x, by = oy + y, bz = oz + z;
        int st = fc_get(c, bx, by, bz);
        if (fc_is_air(c, st) || fc_in_set(c, st, g->invalid)) { if (++invalid > g->invalid_thr) { ok = 0; break; } }
        px[np] = bx; py[np] = by; pz[np] = bz; poff[np] = intprov_sample(g->point_offset, r); np++;
    }
    if (!ok) { if (newf) ns_free(&ns); else old_normal_free(&on); return 0; }
    if (gen_crack) {
        int idx = frnd_int_bound(r, 4), co = num_points * 2 + 1;
        static const int YS[3] = { 7, 5, 1 };
        for (int k = 0; k < 3; k++) {
            int dx = 0, dz = 0;
            if (idx == 0) dx = co; else if (idx == 1) dz = co; else if (idx == 2) { dx = co; dz = co; }
            crack_x[k] = ox + dx; crack_y[k] = oy + YS[k]; crack_z[k] = oz + dz;
        }
        ncrack = 3;
    }
    int *pot = NULL, npot = 0, cap = 0;
    int mn = g->min_gen, mx = g->max_gen;
    BcIt it; bc_init(&it, ox + mn, oy + mn, oz + mn, ox + mx, oy + mx, oz + mx);
    int x, y, z;
    #define SAFE_SET(px_, py_, pz_, state_) do { int st_ = (state_); if (!fc_in_set(c, fc_get(c, px_, py_, pz_), g->cannot_replace)) fc_set(c, px_, py_, pz_, st_, 2); } while (0)
    while (bc_next(&it, &x, &y, &z)) {
        double nval = newf ? (double)ns_get(&ns, (double)x, (double)y, (double)z) : old_normal_get(&on, (double)x, (double)y, (double)z);
        double noise_off = nval * g->noise_mul;
        double dsum = 0.0;
        for (int k = 0; k < np; k++) {
            double dx = (double)(x - px[k]), dy = (double)(y - py[k]), dz = (double)(z - pz[k]);
            double d2 = dx * dx + dy * dy + dz * dz;
            dsum += 1.0 / sqrt(d2 + (double)poff[k]) + noise_off;
        }
        if (dsum < outer_crust) continue;
        if (dsum >= inner_air) { SAFE_SET(x, y, z, bsprov_state(c, g->filling, x, y, z)); continue; }
        double dcrack = 0.0;
        for (int k = 0; k < ncrack; k++) {
            double dx = (double)(x - crack_x[k]), dy = (double)(y - crack_y[k]), dz = (double)(z - crack_z[k]);
            dcrack += 1.0 / sqrt(dx * dx + dy * dy + dz * dz + (double)g->crack_off) + noise_off;
        }
        if (gen_crack && dcrack >= crack_size) {
            SAFE_SET(x, y, z, g->air);       /* scheduleTick соседних жидкостей — не блоки */
        } else if (dsum >= innermost) {
            int alt = (double)frnd_float(r) < g->use_alt0;
            SAFE_SET(x, y, z, bsprov_state(c, alt ? g->alt_inner : g->inner, x, y, z));
            if ((!g->require_alt0 || alt) && (double)frnd_float(r) < g->use_potential) {
                if (npot * 3 + 3 > cap) { cap = cap ? cap * 2 : 192; pot = xrealloc(pot, sizeof(int) * (size_t)cap); }
                pot[npot * 3] = x; pot[npot * 3 + 1] = y; pot[npot * 3 + 2] = z; npot++;
            }
        } else if (dsum >= inner_crust) {
            SAFE_SET(x, y, z, bsprov_state(c, g->middle, x, y, z));
        } else if (dsum >= outer_crust) {
            SAFE_SET(x, y, z, bsprov_state(c, g->outer, x, y, z));
        }
    }
    for (int i = 0; i < npot; i++) {
        int cx = pot[i * 3], cy = pot[i * 3 + 1], cz = pot[i * 3 + 2];
        int st = g->inner_placements[frnd_int_bound(r, g->ninner)];
        for (int d = 0; d < 6; d++) {
            static const char *FACE[6] = { "down", "up", "north", "south", "west", "east" };
            if (bs_has_prop(bs, st, "facing")) st = bs_with(bs, st, "facing", FACE[d]);
            int qx = cx + DIR_DX[d], qy = cy + DIR_DY[d], qz = cz + DIR_DZ[d];
            int ps = fc_get(c, qx, qy, qz);
            if (bs_has_prop(bs, st, "waterlogged")) st = bs_with(bs, st, "waterlogged", BS_FL_SOURCE(bs->fluid[ps]) && BS_FL_TYPE(bs->fluid[ps]) != FL_NONE ? "true" : "false");
            /* BuddingAmethystBlock.canClusterGrowAtState: воздух или полная вода */
            if (fc_is_air(c, ps) || (fc_state_is_water(c, ps) && BS_FL_AMOUNT(bs->fluid[ps]) == 8)) { SAFE_SET(qx, qy, qz, st); break; }
        }
    }
    #undef SAFE_SET
    free(pot);
    if (newf) ns_free(&ns); else old_normal_free(&on);
    return 1;
}

static const FeatType T_GEODE = { "minecraft:geode", geode_parse, geode_place };
void feature_register_geode(void) { feature_register_type(&T_GEODE); }

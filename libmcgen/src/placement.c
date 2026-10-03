/* placement.c — PlacementModifier'ы (levelgen/placement) и FeaturePlacer: цепочка модификаторов позиций → Feature.place.
 *
 * Порядок обработки — обход в глубину: модификатор формирует ВСЕ позиции для одной входной позиции (с потреблением ГСЧ), затем каждая
 * выходная позиция проходит остаток цепочки до самой фичи, прежде чем берётся следующая (FeaturePlacer 26.3; в 26.1/26.2 — те же
 * результаты через ленивые Stream.flatMap). Добавить модификатор: новая строка в PM_TYPES + ветка в pm_run + (при необходимости) поле в PMod. */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>

enum { PM_COUNT, PM_COUNT_LAYER, PM_NOISE_COUNT, PM_NOISE_THRESH, PM_RARITY, PM_IN_SQUARE, PM_HEIGHT_RANGE, PM_HEIGHTMAP, PM_BIOME, PM_ENV_SCAN,
       PM_SURF_REL, PM_SURF_WATER, PM_BLOCK_FILTER, PM_OFFSET, PM_FIXED, PM_RANDOM_CHANCE, PM_RANDOMLY, PM_CUBOID };

struct PMod {
    int kind;
    IntProv *ip, *ix, *iy, *iz;      /* count / count_on_every_layer: ip; offset: ix, iy, iz; cuboid: ix = xz_size, iy = y_size */
    int ratio; double factor, offset;/* noise_based_count */
    double level; int below, above;  /* noise_threshold_count */
    int chance;                      /* rarity_filter */
    float fchance;                   /* random_chance */
    HeightProv *hp;
    int hm, tmin, tmax;              /* heightmap / surface_relative_threshold_filter / surface_water_depth_filter (tmax) */
    int dir, max_steps; BPred *target, *allowed;   /* environment_scan */
    BPred *pred;
    int nfixed; int *fx;             /* fixed_placement: x,y,z подряд */
    int nsub; struct PMod **sub;     /* randomly_selected */
    int inc_edges, inc_interior;     /* cuboid */
};
typedef struct PMod PMod;

static int parse_one(FParse *p, const Js *v, PMod **out);

int placement_parse_list(FParse *p, const Js *arr, PMod ***out, int *n) {
    if (!js_is_arr(arr)) return fp_fail(p, "placement: ожидался массив");
    PMod **l = fp_alloc(p, sizeof(PMod *) * (size_t)(arr->n ? arr->n : 1));
    for (int i = 0; i < arr->n; i++) if (!parse_one(p, arr->items[i], &l[i])) return 0;
    *out = l; *n = arr->n;
    return 1;
}

static int parse_one(FParse *p, const Js *v, PMod **out) {
    if (!js_is_obj(v)) return fp_fail(p, "PlacementModifier: ожидался объект");
    const char *t = js_str(js_get(v, "type"), NULL);
    if (!t) return fp_fail(p, "PlacementModifier: нет type");
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    PMod *m = fp_alloc(p, sizeof *m); *out = m;
    if (!strcmp(t, "count")) { m->kind = PM_COUNT; m->ip = fp_intprov(p, js_get(v, "count")); return m->ip != NULL; }
    if (!strcmp(t, "count_on_every_layer")) { m->kind = PM_COUNT_LAYER; m->ip = fp_intprov(p, js_get(v, "count")); return m->ip != NULL; }
    if (!strcmp(t, "noise_based_count")) {
        m->kind = PM_NOISE_COUNT; m->ratio = js_int(js_get(v, "noise_to_count_ratio"), 0); m->factor = js_num(js_get(v, "noise_factor"), 1.0);
        m->offset = js_num(js_get(v, "noise_offset"), 0.0); return 1;
    }
    if (!strcmp(t, "noise_threshold_count")) {
        m->kind = PM_NOISE_THRESH; m->level = js_num(js_get(v, "noise_level"), 0); m->below = js_int(js_get(v, "below_noise"), 0); m->above = js_int(js_get(v, "above_noise"), 0); return 1;
    }
    if (!strcmp(t, "rarity_filter")) { m->kind = PM_RARITY; m->chance = js_int(js_get(v, "chance"), 1); return 1; }
    if (!strcmp(t, "random_chance")) { m->kind = PM_RANDOM_CHANCE; m->fchance = js_numf(js_get(v, "chance"), 0); return 1; }
    if (!strcmp(t, "in_square")) { m->kind = PM_IN_SQUARE; return 1; }
    if (!strcmp(t, "height_range")) { m->kind = PM_HEIGHT_RANGE; m->hp = fp_heightprov(p, js_get(v, "height")); return m->hp != NULL; }
    if (!strcmp(t, "heightmap")) {
        m->kind = PM_HEIGHTMAP; m->hm = hm_type_from_name(js_str(js_get(v, "heightmap"), NULL));
        return m->hm >= 0 ? 1 : fp_fail(p, "heightmap: неизвестная карта");
    }
    if (!strcmp(t, "biome")) { m->kind = PM_BIOME; return 1; }
    if (!strcmp(t, "environment_scan")) {
        m->kind = PM_ENV_SCAN; m->dir = dir_from_name(js_str(js_get(v, "direction_of_search"), NULL));
        if (m->dir < 0) return fp_fail(p, "environment_scan: плохое direction_of_search");
        m->target = fp_bpred(p, js_get(v, "target_condition")); if (!m->target) return 0;
        Js *as = js_get(v, "allowed_search_condition");
        m->allowed = as ? fp_bpred(p, as) : bpred_true(p); if (!m->allowed) return 0;
        m->max_steps = js_int(js_get(v, "max_steps"), 1); return 1;
    }
    if (!strcmp(t, "surface_relative_threshold_filter")) {
        m->kind = PM_SURF_REL; m->hm = hm_type_from_name(js_str(js_get(v, "heightmap"), NULL));
        if (m->hm < 0) return fp_fail(p, "surface_relative_threshold_filter: плохая карта");
        m->tmin = js_int(js_get(v, "min_inclusive"), (int)0x80000000); m->tmax = js_int(js_get(v, "max_inclusive"), 0x7fffffff); return 1;
    }
    if (!strcmp(t, "surface_water_depth_filter")) { m->kind = PM_SURF_WATER; m->tmax = js_int(js_get(v, "max_water_depth"), 0); return 1; }
    if (!strcmp(t, "block_predicate_filter")) { m->kind = PM_BLOCK_FILTER; m->pred = fp_bpred(p, js_get(v, "predicate")); return m->pred != NULL; }
    if (!strcmp(t, "offset")) {
        m->kind = PM_OFFSET; m->ix = fp_intprov(p, js_get(v, "x")); m->iy = fp_intprov(p, js_get(v, "y")); m->iz = fp_intprov(p, js_get(v, "z"));
        return m->ix && m->iy && m->iz;
    }
    if (!strcmp(t, "random_offset")) {       /* 26.1/26.2: xz_spread, y_spread (порядок выборок: x, y, z) */
        m->kind = PM_OFFSET; m->ix = fp_intprov(p, js_get(v, "xz_spread")); m->iy = fp_intprov(p, js_get(v, "y_spread")); m->iz = m->ix;
        return m->ix && m->iy;
    }
    if (!strcmp(t, "fixed_placement")) {
        Js *l = js_get(v, "positions");
        if (!js_is_arr(l)) return fp_fail(p, "fixed_placement: нет positions");
        m->kind = PM_FIXED; m->nfixed = l->n; m->fx = fp_alloc(p, sizeof(int) * 3 * (size_t)(l->n ? l->n : 1));
        for (int i = 0; i < l->n; i++) {
            Js *e = l->items[i];
            if (!js_is_arr(e) || e->n != 3) return fp_fail(p, "fixed_placement: позиция из 3 чисел");
            for (int k = 0; k < 3; k++) m->fx[i * 3 + k] = js_int(e->items[k], 0);
        }
        return 1;
    }
    if (!strcmp(t, "randomly_selected")) {
        m->kind = PM_RANDOMLY;
        return placement_parse_list(p, js_get(v, "placements"), &m->sub, &m->nsub) && m->nsub > 0;
    }
    if (!strcmp(t, "cuboid")) {
        m->kind = PM_CUBOID; m->ix = fp_intprov(p, js_get(v, "xz_size")); m->iy = fp_intprov(p, js_get(v, "y_size"));
        m->inc_edges = js_bool(js_get(v, "include_edges"), 1); m->inc_interior = js_bool(js_get(v, "include_interior"), 1);
        return m->ix && m->iy;
    }
    return fp_fail(p, "PlacementModifier: неизвестный тип %s", t);
}

/* ====================================================================== выполнение */
typedef struct { int *v; int n, cap; int buf[96]; } PlcVec;   /* позиции x,y,z подряд */
static void plc_init(PlcVec *a) { a->v = a->buf; a->n = 0; a->cap = 32; }
static void plc_add(PlcVec *a, int x, int y, int z) {
    if (a->n == a->cap) {
        int nc = a->cap * 2;
        if (a->v == a->buf) { int *nv = xmalloc(sizeof(int) * 3 * (size_t)nc); memcpy(nv, a->v, sizeof(int) * 3 * (size_t)a->n); a->v = nv; }
        else a->v = xrealloc(a->v, sizeof(int) * 3 * (size_t)nc);
        a->cap = nc;
    }
    a->v[a->n * 3] = x; a->v[a->n * 3 + 1] = y; a->v[a->n * 3 + 2] = z; a->n++;
}
static void plc_free(PlcVec *a) { if (a->v != a->buf) free(a->v); }

/* CountOnEveryLayerPlacement.findOnGroundYPosition */
static int is_empty_layer_block(const FCtx *c, int st) {
    return fc_is_air(c, st) || c->g->state_block[st] == c->g->blk_water || c->g->state_block[st] == c->g->blk_lava;
}
static int find_on_ground(FCtx *c, int x, int ystart, int z, int layer_to_place) {
    int cur_layer = 0;
    int cur = fc_get(c, x, ystart, z);
    for (int y = ystart; y >= c->min_y + 1; y--) {
        int below = fc_get(c, x, y - 1, z);
        if (!is_empty_layer_block(c, below) && is_empty_layer_block(c, cur) && c->g->state_block[below] != c->bedrock_blk) {
            if (cur_layer == layer_to_place) return y - 1 + 1;
            cur_layer++;
        }
        cur = below;
    }
    return 0x7fffffff;
}

static void pm_run(FCtx *c, const PMod *m, int x, int y, int z, PlcVec *out, const Placed *top, int *rejected_biome);

static int placed_run(FCtx *c, const Placed *pf, int idx, int x, int y, int z, int biome_check) {
    if (idx == pf->nmods) return feat_place(c, pf->feat, x, y, z);
    PlcVec out; plc_init(&out);
    int dummy = 0;
    pm_run(c, pf->mods[idx], x, y, z, &out, biome_check ? pf : NULL, &dummy);
    if (c->fw->debug) { const char *tr = getenv("MCGEN_TRACE_PM"); if (tr && pf->id && strstr(pf->id, tr)) { fprintf(stderr, "[pm] %s mod#%d kind=%d in(%d,%d,%d) -> %d", pf->id, idx, pf->mods[idx] ? *(int *)pf->mods[idx] : -1, x, y, z, out.n); if (out.n) fprintf(stderr, " first(%d,%d,%d)", out.v[0], out.v[1], out.v[2]); fprintf(stderr, "\n"); } }
    int any = 0;
    for (int i = 0; i < out.n; i++) any |= placed_run(c, pf, idx + 1, out.v[i * 3], out.v[i * 3 + 1], out.v[i * 3 + 2], biome_check);
    plc_free(&out);
    return any;
}

int placed_place(FCtx *c, const Placed *pf, int x, int y, int z, int biome_check) {
    if (pf->nmods == 0) return feat_place(c, pf->feat, x, y, z);
    return placed_run(c, pf, 0, x, y, z, biome_check);
}

static void pm_run(FCtx *c, const PMod *m, int x, int y, int z, PlcVec *out, const Placed *top, int *unused) {
    FRnd *r = c->rnd;
    switch (m->kind) {
    case PM_COUNT: { int n = intprov_sample(m->ip, r); for (int i = 0; i < n; i++) plc_add(out, x, y, z); break; }
    case PM_NOISE_COUNT: {
        double noise = biome_info_noise(c->g, x / m->factor, z / m->factor);
        int n = jm_d2i(ceil((noise + m->offset) * m->ratio));
        for (int i = 0; i < n; i++) plc_add(out, x, y, z);
        break;
    }
    case PM_NOISE_THRESH: {
        double noise = biome_info_noise(c->g, x / 200.0, z / 200.0);
        int n = noise < m->level ? m->below : m->above;
        for (int i = 0; i < n; i++) plc_add(out, x, y, z);
        break;
    }
    case PM_RARITY: if (frnd_float(r) < 1.0f / (float)m->chance) plc_add(out, x, y, z); break;
    case PM_RANDOM_CHANCE: if (frnd_float(r) < m->fchance) plc_add(out, x, y, z); break;
    case PM_IN_SQUARE: { int nx = frnd_int_bound(r, 16) + x; int nz = frnd_int_bound(r, 16) + z; plc_add(out, nx, y, nz); break; }
    case PM_HEIGHT_RANGE: plc_add(out, x, heightprov_sample(m->hp, c), z); break;
    case PM_HEIGHTMAP: { int h = fc_height(c, m->hm, x, z); if (h > c->min_y) plc_add(out, x, h, z); break; }
    case PM_BIOME: {
        int b = fc_biome(c, x, y, z);
        if (c->fw->debug && getenv("MCGEN_TRACE_BIOME") && top) fprintf(stderr, "[biome] %s (%d,%d,%d): %s -> %s\n", top->id, x, y, z, c->g->biome_names[b], (top->index >= 0 && ((c->fw->biome_has[b][top->index >> 6] >> (top->index & 63)) & 1)) ? "да" : "НЕТ");
        if (!top || (top->index >= 0 && (c->fw->biome_has[b][top->index >> 6] >> (top->index & 63)) & 1)) plc_add(out, x, y, z);
        break;
    }
    case PM_COUNT_LAYER: {
        int layer = 0, found;
        do {
            found = 0;
            for (int i = 0; i < intprov_sample(m->ip, r); i++) {     /* count выбирается заново на каждой итерации (как в игре) */
                int px = frnd_int_bound(r, 16) + x, pz = frnd_int_bound(r, 16) + z;
                int start = fc_height(c, HM_MOTION_BLOCKING, px, pz);
                int py = find_on_ground(c, px, start, pz, layer);
                if (py != 0x7fffffff) { plc_add(out, px, py, pz); found = 1; }
            }
            layer++;
        } while (found);
        break;
    }
    case PM_ENV_SCAN: {
        int px = x, py = y, pz = z;
        if (bpred_test(c, m->allowed, px, py, pz)) {
            for (int i = 0; i < m->max_steps; i++) {
                if (bpred_test(c, m->target, px, py, pz)) { plc_add(out, px, py, pz); return; }
                px += DIR_DX[m->dir]; py += DIR_DY[m->dir]; pz += DIR_DZ[m->dir];
                if (fc_outside(c, py)) return;
                if (!bpred_test(c, m->allowed, px, py, pz)) break;
            }
            if (bpred_test(c, m->target, px, py, pz)) plc_add(out, px, py, pz);
        }
        break;
    }
    case PM_SURF_REL: {
        long surface = fc_height(c, m->hm, x, z);
        long mn = surface + m->tmin, mx = surface + m->tmax;
        if (mn <= y && y <= mx) plc_add(out, x, y, z);
        break;
    }
    case PM_SURF_WATER: {
        int floor = fc_height(c, HM_OCEAN_FLOOR, x, z), surf = fc_height(c, HM_WORLD_SURFACE, x, z);
        if (surf - floor <= m->tmax) plc_add(out, x, y, z);
        break;
    }
    case PM_BLOCK_FILTER: if (bpred_test(c, m->pred, x, y, z)) plc_add(out, x, y, z); break;
    case PM_OFFSET: {
        int dx = intprov_sample(m->ix, r); int dy = intprov_sample(m->iy, r); int dz = intprov_sample(m->iz, r);
        plc_add(out, x + dx, y + dy, z + dz);
        break;
    }
    case PM_FIXED: {
        int cx = x >> 4, cz = z >> 4;
        for (int i = 0; i < m->nfixed; i++) if ((m->fx[i * 3] >> 4) == cx && (m->fx[i * 3 + 2] >> 4) == cz) plc_add(out, m->fx[i * 3], m->fx[i * 3 + 1], m->fx[i * 3 + 2]);
        break;
    }
    case PM_RANDOMLY: { int k = frnd_int_bound(r, m->nsub); pm_run(c, m->sub[k], x, y, z, out, top, unused); break; }
    case PM_CUBOID: {
        int height = intprov_sample(m->iy, r); int width = intprov_sample(m->ix, r); int length = intprov_sample(m->ix, r);
        if (getenv("MCGEN_VEG_DEBUG")) fprintf(stderr, "cuboid origin %d %d %d size %d %d %d chunk %d %d\n", x, y, z, width, height, length, c->ccx, c->ccz);/*DBG*/
        for (int dx = 0; dx <= width; dx++) for (int dy = 0; dy <= height; dy++) for (int dz = 0; dz <= length; dz++) {
            if ((m->inc_edges || (dx != 0 && dx != width) || (dy != 0 && dy != height)) &&
                (m->inc_edges || (dz != 0 && dz != length) || (dy != 0 && dy != height)) &&
                (m->inc_edges || (dx != 0 && dx != width) || (dz != 0 && dz != length)) &&
                (m->inc_interior || dx == 0 || dx == width || dy == 0 || dy == height || dz == 0 || dz == length))
                plc_add(out, x + dx, y + dy, z + dz);
        }
        break;
    }
    }
}

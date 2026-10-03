/* feature_bsp.c — BlockStateProvider (levelgen/feature/stateproviders): simple, weighted, rule_based, randomized_int, rotated.
 * Шумовые провайдеры (noise, noise_threshold, dual_noise) — у группы фич «растительность» (цветы): пока fp_fail («не поддержан»). */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>
#include <alloca.h>

enum { SP_SIMPLE, SP_WEIGHTED, SP_RULE, SP_RANDINT, SP_ROTATED, SP_NOISE, SP_NOISE_THRESH, SP_DUAL, SP_RANDBLOCK };
typedef struct NzGen { NStack ns; OldNormal on; } NzGen;       /* NormalNoise: 26.3+ (float, NStack) или 26.1/26.2 (double) */
typedef struct SPRule { BPred *cond; BSProv *then; } SPRule;
struct BSProv {
    int kind;
    int state;                          /* SIMPLE */
    int n, total; int *w; int *states;  /* WEIGHTED */
    SPRule *rules; BSProv *fallback;    /* RULE */
    BSProv *src; char *prop; IntProv *values;   /* RANDINT (src — и для ROTATED) */
    int dir;                            /* ROTATED: −1 случайное */
    /* шумовые провайдеры */
    NzGen noise, slow; float scale, slow_scale; float threshold, high_chance;
    int nstates; int *states2;          /* states / low_states */
    int nhigh; int *high; int deflt;    /* high_states, default_state */
    int vmin, vmax;                     /* dual_noise: variety */
};

static const char *norm_type(const char *t, char *buf, size_t n) {
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    snprintf(buf, n, "%s", t);
    static const char *SUF[] = { "_state_provider", "_block_provider", "_provider", NULL };
    for (int i = 0; SUF[i]; i++) { size_t l = strlen(buf), s = strlen(SUF[i]); if (l > s && !strcmp(buf + l - s, SUF[i])) { buf[l - s] = 0; break; } }
    return buf;
}

static int noise_gen_init(FParse *p, NzGen *n, const Js *params, i64 seed) {
    NoiseParams P; char e[128];
    if (noise_params_parse(params, p->newf, &P, e, sizeof e)) return fp_fail(p, "noise: %s", e);
    Rnd r = rnd_legacy_seed(seed);                    /* new WorldgenRandom(new LegacyRandomSource(seed)) */
    if (p->newf) nn_new_create(&n->ns, &P, &r); else old_normal_create(&n->on, &r, &P, 1);
    return 1;
}
static int state_list(FParse *p, const Js *v, int **out, int *n) {
    if (!js_is_arr(v) || v->n == 0) return 0;
    *out = fp_alloc(p, sizeof(int) * (size_t)v->n); *n = v->n;
    for (int i = 0; i < v->n; i++) { (*out)[i] = bs_from_json(p->bs, v->items[i]); if ((*out)[i] < 0) return 0; }
    return 1;
}
/* NoiseBasedStateProvider.getNoiseValue: 26.3+ — float (NoiseStack.get), 26.1/26.2 — double */
static double nz_value(const FCtx *c, const NzGen *n, int x, int y, int z, double scale) {
    return c->g->newf ? (double)ns_get(&n->ns, (double)x * scale, (double)y * scale, (double)z * scale)
                      : old_normal_get(&n->on, (double)x * scale, (double)y * scale, (double)z * scale);
}
static int nz_pick(const FCtx *c, const int *st, int n, double nv) {
    if (c->g->newf) { float pv = jm_clampf((1.0f + (float)nv) / 2.0f, 0.0f, 0.9999f); return st[(int)(pv * (float)n)]; }
    double pv = jm_clamp((1.0 + nv) / 2.0, 0.0, 0.9999); return st[(int)(pv * (double)n)];
}

int veg_holderset_blocks(FParse *p, const Js *v, int **out);       /* feature_veg.c */
BSProv *fp_bsprov(FParse *p, const Js *v) {
    if (!v) { fp_fail(p, "BlockStateProvider: нет значения"); return NULL; }
    if (js_is_str(v)) {
        /* ссылка на реестр worldgen/block_state_provider (26.3+); в 26.1/26.2 строки-состояния в этом месте не встречаются */
        BSProv *cached = sm_get(&p->fw->bsp_cache, v->s);
        if (cached) return cached;
        const Js *d = fp_load_json(p, "block_state_provider", v->s);
        if (!d) {
            int st = bs_from_json(p->bs, v);       /* допускаем и состояние «minecraft:stone» */
            if (st < 0) { fp_fail(p, "BlockStateProvider: нет %s", v->s); return NULL; }
            p->err[0] = 0;
            BSProv *b = fp_alloc(p, sizeof *b); b->kind = SP_SIMPLE; b->state = st; return b;
        }
        BSProv *b = fp_bsprov(p, d);
        if (b) sm_put(&p->fw->bsp_cache, v->s, b);
        return b;
    }
    if (!js_is_obj(v)) { fp_fail(p, "BlockStateProvider: ожидалась строка или объект"); return NULL; }
    BSProv *b = fp_alloc(p, sizeof *b);
    const char *t = js_str(js_get(v, "type"), NULL);
    if (!t) {
        b->kind = SP_SIMPLE; b->state = bs_from_json(p->bs, v);
        if (b->state < 0) { fp_fail(p, "BlockStateProvider: плохое состояние"); return NULL; }
        return b;
    }
    char nt[64]; norm_type(t, nt, sizeof nt);
    if (!strcmp(nt, "simple")) {
        b->kind = SP_SIMPLE; b->state = bs_from_json(p->bs, js_get(v, "state"));
        if (b->state < 0) { fp_fail(p, "simple_state_provider: плохое state"); return NULL; }
    }
    else if (!strcmp(nt, "weighted")) {
        Js *e = js_get(v, "entries");
        if (!js_is_arr(e) || e->n == 0) { fp_fail(p, "weighted_state_provider: нет entries"); return NULL; }
        b->kind = SP_WEIGHTED; b->n = e->n; b->w = fp_alloc(p, sizeof(int) * (size_t)e->n); b->states = fp_alloc(p, sizeof(int) * (size_t)e->n);
        for (int i = 0; i < e->n; i++) {
            b->w[i] = js_int(js_get(e->items[i], "weight"), 1); b->total += b->w[i];
            b->states[i] = bs_from_json(p->bs, js_get(e->items[i], "data"));
            if (b->states[i] < 0) { fp_fail(p, "weighted_state_provider: плохое data"); return NULL; }
        }
    }
    else if (!strcmp(nt, "rule_based")) {
        Js *r = js_get(v, "rules");
        if (!js_is_arr(r)) { fp_fail(p, "rule_based: нет rules"); return NULL; }
        b->kind = SP_RULE; b->n = r->n; b->rules = fp_alloc(p, sizeof(SPRule) * (size_t)(r->n ? r->n : 1));
        for (int i = 0; i < r->n; i++) {
            b->rules[i].cond = fp_bpred(p, js_get(r->items[i], "if_true")); if (!b->rules[i].cond) return NULL;
            b->rules[i].then = fp_bsprov(p, js_get(r->items[i], "then")); if (!b->rules[i].then) return NULL;
        }
        Js *fb = js_get(v, "fallback");
        if (fb) { b->fallback = fp_bsprov(p, fb); if (!b->fallback) return NULL; }
    }
    else if (!strcmp(nt, "randomized_int")) {
        b->kind = SP_RANDINT; b->src = fp_bsprov(p, js_get(v, "source")); if (!b->src) return NULL;
        const char *pr = js_str(js_get(v, "property"), NULL); if (!pr) { fp_fail(p, "randomized_int: нет property"); return NULL; }
        b->prop = fp_strdup(p, pr);
        b->values = fp_intprov(p, js_get(v, "values")); if (!b->values) return NULL;
    }
    else if (!strcmp(nt, "random_block")) {         /* RandomBlockProvider (W10): блок из HolderSet в порядке набора, состояние по умолчанию */
        int *bl = NULL, nb = veg_holderset_blocks(p, js_get(v, "blocks"), &bl);
        if (nb < 0) { fp_fail(p, "random_block: плохие blocks"); return NULL; }
        b->kind = SP_RANDBLOCK; b->n = nb; b->states = fp_alloc(p, sizeof(int) * (size_t)(nb ? nb : 1));
        for (int i = 0; i < nb; i++) b->states[i] = bs_default(p->bs, bl[i]);
    }
    else if (!strcmp(nt, "rotated")) {
        b->kind = SP_ROTATED; b->src = fp_bsprov(p, js_get(v, "state")); if (!b->src) return NULL;
        const char *d = js_str(js_get(v, "direction"), NULL); b->dir = d ? dir_from_name(d) : -1;
    }
    else if (!strcmp(nt, "noise") || !strcmp(nt, "noise_threshold") || !strcmp(nt, "dual_noise")) {
        b->kind = !strcmp(nt, "noise") ? SP_NOISE : !strcmp(nt, "dual_noise") ? SP_DUAL : SP_NOISE_THRESH;
        i64 seed = (i64)js_num(js_get(v, "seed"), 0);
        b->scale = js_numf(js_get(v, "scale"), 1.0f);
        if (!noise_gen_init(p, &b->noise, js_get(v, "noise"), seed)) return NULL;
        if (b->kind == SP_NOISE_THRESH) {
            b->threshold = js_numf(js_get(v, "threshold"), 0); b->high_chance = js_numf(js_get(v, "high_chance"), 0);
            b->deflt = bs_from_json(p->bs, js_get(v, "default_state"));
            if (b->deflt < 0 || !state_list(p, js_get(v, "low_states"), &b->states2, &b->nstates) || !state_list(p, js_get(v, "high_states"), &b->high, &b->nhigh)) return fp_fail(p, "noise_threshold: плохие состояния"), NULL;
        } else {
            if (!state_list(p, js_get(v, "states"), &b->states2, &b->nstates)) return fp_fail(p, "noise: плохие states"), NULL;
            if (b->kind == SP_DUAL) {
                Js *vr = js_get(v, "variety");
                if (js_is_arr(vr) && vr->n == 2) { b->vmin = js_int(vr->items[0], 1); b->vmax = js_int(vr->items[1], 1); }
                else if (js_is_obj(vr)) { b->vmin = js_int(js_get(vr, "min_inclusive"), 1); b->vmax = js_int(js_get(vr, "max_inclusive"), 1); }
                else return fp_fail(p, "dual_noise: плохое variety"), NULL;
                b->slow_scale = js_numf(js_get(v, "slow_scale"), 1.0f);
                if (!noise_gen_init(p, &b->slow, js_get(v, "slow_noise"), seed)) return NULL;
            }
        }
    }
    else { fp_fail(p, "BlockStateProvider: тип %s не поддержан", t); return NULL; }
    return b;
}

int bsprov_optional(FCtx *c, const BSProv *b, int x, int y, int z) {
    switch (b->kind) {
    case SP_SIMPLE: return b->state;
    case SP_WEIGHTED: {
        int sel = frnd_int_bound(c->rnd, b->total);
        for (int i = 0; i < b->n; i++) { sel -= b->w[i]; if (sel < 0) return b->states[i]; }
        return b->states[b->n - 1];
    }
    case SP_RULE: {
        for (int i = 0; i < b->n; i++) {
            if (bpred_test(c, b->rules[i].cond, x, y, z)) {
                int s = bsprov_optional(c, b->rules[i].then, x, y, z);
                if (s >= 0) return s;
            }
        }
        return b->fallback ? bsprov_optional(c, b->fallback, x, y, z) : -1;
    }
    case SP_RANDBLOCK: return b->n ? b->states[frnd_int_bound(c->rnd, b->n)] : -1;
    default: return bsprov_state(c, b, x, y, z);
    }
}
static int noise_state(FCtx *c, const BSProv *b, int x, int y, int z) {
    if (b->kind == SP_NOISE) return nz_pick(c, b->states2, b->nstates, nz_value(c, &b->noise, x, y, z, (double)b->scale));
    if (b->kind == SP_NOISE_THRESH) {
        double local = nz_value(c, &b->noise, x, y, z, (double)b->scale);
        if (local < (double)b->threshold) return b->states2[frnd_int_bound(c->rnd, b->nstates)];
        if (frnd_float(c->rnd) < b->high_chance) return b->high[frnd_int_bound(c->rnd, b->nhigh)];
        return b->deflt;
    }
    /* dual_noise */
    double vn = nz_value(c, &b->slow, x, y, z, (double)b->slow_scale);
    double f = (vn - -1.0) / (1.0 - -1.0);
    double lo = (double)b->vmin, hi = (double)(b->vmax + 1);
    int variety = jm_d2i(f < 0.0 ? lo : (f > 1.0 ? hi : lo + f * (hi - lo)));
    int *poss = variety > 0 ? alloca(sizeof(int) * (size_t)variety) : NULL;
    for (int i = 0; i < variety; i++) poss[i] = nz_pick(c, b->states2, b->nstates, nz_value(c, &b->slow, x + i * 54545, y, z + i * 34234, (double)b->slow_scale));
    return variety > 0 ? nz_pick(c, poss, variety, nz_value(c, &b->noise, x, y, z, (double)b->scale)) : b->states2[0];
}
int bsprov_state(FCtx *c, const BSProv *b, int x, int y, int z) {
    switch (b->kind) {
    case SP_RULE: case SP_RANDBLOCK: { int s = bsprov_optional(c, b, x, y, z); return s >= 0 ? s : fc_get(c, x, y, z); }
    case SP_RANDINT: {
        int st = bsprov_state(c, b->src, x, y, z);
        if (!bs_has_prop(c->bs, st, b->prop)) return st;
        char num[16]; snprintf(num, sizeof num, "%d", intprov_sample(b->values, c->rnd));
        return bs_with(c->bs, st, b->prop, num) >= 0 ? bs_with(c->bs, st, b->prop, num) : st;
    }
    case SP_ROTATED: {
        int dir = b->dir >= 0 ? b->dir : frnd_int_bound(c->rnd, 6);    /* Direction.getRandom: Util.getRandom(values, random) */
        static const char *N[6] = { "down", "up", "north", "south", "west", "east" };
        static const char *AX[6] = { "y", "y", "z", "z", "x", "x" };
        int st = bsprov_state(c, b->src, x, y, z);
        st = bs_try_with(c->bs, st, "axis", AX[dir]);
        st = bs_try_with(c->bs, st, "facing", N[dir]);
        return st;
    }
    case SP_NOISE: case SP_NOISE_THRESH: case SP_DUAL: return noise_state(c, b, x, y, z);
    default: return bsprov_optional(c, b, x, y, z);
    }
}

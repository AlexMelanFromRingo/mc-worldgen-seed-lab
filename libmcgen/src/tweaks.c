/* tweaks.c — «тонкие настройки мира». Таблица — libmcgen/gen/mcgen_tweaks_table.h (порождается gen_tweaks.py
 * из libmcgen/tweaks.json). При значениях по умолчанию НИЧЕГО не меняется (ни деревья функций, ни масштабы) — мир
 * побитово ванильный; это проверяется тестами G1/G2 (там настройки не передаются) и tests/tweaks_default.sh.
 *
 * Реализация (стадия TERRAIN и биомы):
 *   sea_level_offset, lava_level_offset — глобальный «выбор жидкости» (NoiseBasedChunkGenerator.createFluidPicker);
 *   aquifers, ore_veins                 — включение NoiseBasedAquifer и жил;
 *   climate_scale_xz/y                  — множители координат шумов климатического домена (растяжение биомов и
 *                                         континентов вместе с формой рельефа, которая от них зависит);
 *   cave_size                           — то же для шумов пещер (cave_*, spaghetti_*, noodle*, pillar*);
 *   terrain_amplitude                   — отклонение offset от уровня моря и jaggedness умножаются на A;
 *   terrain_steepness                   — factor умножается на S;
 *   cave_density                        — к функциям …/caves/* и шуму cave_cheese прибавляется (1−D)·0.15
 *                                         (D = 0 — пещер нет: эти функции заменяются на +10⁶).
 * Остальные настройки (canyon_frequency, ore_*, feature_density, tree_size, structure_frequency) — для стадий других
 * потоков; здесь только хранятся.
 */
#include "mcgen_internal.h"
#include "mcgen_tweaks_table.h"
#include <stdio.h>
#include <stdlib.h>

const McTweakInfo *tweaks_table(int *n) { *n = MCGEN_TWEAK_COUNT; return MCGEN_TWEAK_TABLE; }

int tweaks_apply(McWorld *w, const McTweakValue *tw, int n, char *err, size_t errlen) {
    for (int i = 0; i < n; i++) {
        if (!tw[i].id) { set_err(err, errlen, "tweak: пустой id"); return -1; }
        int k = -1;
        for (int j = 0; j < MCGEN_TWEAK_COUNT; j++) if (!strcmp(MCGEN_TWEAK_TABLE[j].id, tw[i].id)) k = j;
        if (k < 0) { set_err(err, errlen, "tweak: неизвестный id '%s'", tw[i].id); return -1; }
        double v = tw[i].value;
        if (!(v >= MCGEN_TWEAK_TABLE[k].min && v <= MCGEN_TWEAK_TABLE[k].max)) {
            set_err(err, errlen, "tweak %s = %g вне [%g, %g]", tw[i].id, v, MCGEN_TWEAK_TABLE[k].min, MCGEN_TWEAK_TABLE[k].max); return -1;
        }
        if (MCGEN_TWEAK_TABLE[k].is_int) v = floor(v + 0.5);
        w->tweak[k] = v;
    }
    return 0;
}

/* ---------------- построение деревьев ---------------- */
static Df *mkc(double v) { Df *f = xcalloc(1, sizeof(Df)); f->t = DF_CONST; f->n0.d = v; f->n0.f = (float)v; return f; }
static Df *mk2(DfType t, Df *a, Df *b) { Df *f = xcalloc(1, sizeof(Df)); f->t = t; f->a = a; f->b = b; return f; }
static DfSpline *sp_clone(const DfSpline *s);
Df *df_clone(const Df *f) {
    if (!f) return NULL;
    Df *c = xcalloc(1, sizeof(Df));
    *c = *f;
    c->hash = 0;
    c->a = df_clone(f->a); c->b = df_clone(f->b); c->c = df_clone(f->c); c->d = df_clone(f->d);
    if (f->nlist) { c->list = xcalloc((size_t)f->nlist, sizeof(Df *)); for (int i = 0; i < f->nlist; i++) c->list[i] = df_clone(f->list[i]); }
    if (f->nthr) { c->thr = xcalloc((size_t)f->nthr, sizeof(DNum)); memcpy(c->thr, f->thr, sizeof(DNum) * (size_t)f->nthr); }
    c->name = f->name ? xstrdup(f->name) : NULL;
    c->spline = sp_clone(f->spline);
    return c;
}
static DfSpline *sp_clone(const DfSpline *s) {
    if (!s) return NULL;
    DfSpline *c = xcalloc(1, sizeof *c); *c = *s;
    c->coord = df_clone(s->coord);
    if (s->n) {
        c->loc = xcalloc((size_t)s->n, sizeof(DNum)); memcpy(c->loc, s->loc, sizeof(DNum) * (size_t)s->n);
        c->der = xcalloc((size_t)s->n, sizeof(DNum)); memcpy(c->der, s->der, sizeof(DNum) * (size_t)s->n);
        c->val = xcalloc((size_t)s->n, sizeof(DfSpline *));
        for (int i = 0; i < s->n; i++) c->val[i] = sp_clone(s->val[i]);
    }
    return c;
}
/* замена узлов шума cave_cheese на add(шум, c) (или константу); 1 — что-то заменено */
static int wrap_cheese(Df *f, double c, int kill) {
    if (!f) return 0;
    int ch = 0;
    Df **kids[4] = { &f->a, &f->b, &f->c, &f->d };
    for (int k = 0; k < 4; k++) {
        Df *x = *kids[k];
        if (x && x->t == DF_NOISE && x->name && !strcmp(x->name, "minecraft:cave_cheese")) {
            *kids[k] = kill ? (df_free(x), mkc(1e6)) : mk2(DF_ADD, x, mkc(c)); ch = 1;
        } else ch |= wrap_cheese(x, c, kill);
    }
    for (int i = 0; i < f->nlist; i++) {
        Df *x = f->list[i];
        if (x && x->t == DF_NOISE && x->name && !strcmp(x->name, "minecraft:cave_cheese")) { f->list[i] = kill ? (df_free(x), mkc(1e6)) : mk2(DF_ADD, x, mkc(c)); ch = 1; }
        else ch |= wrap_cheese(x, c, kill);
    }
    return ch;
}
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }

static void over_put(McWorld *w, const char *id, Df *f) { Df *old = sm_get(&w->df_over, id); if (old) df_free(old); sm_put(&w->df_over, id, f); }
static void df_free_cb(void *p) { df_free(p); }

/* вызывается из mcgen_world_new до компиляции */
void tweaks_prepare(McWorld *w) {
    const McGen *g = w->g;
    double A = w->tweak[MCGEN_TWEAK_TERRAIN_AMPLITUDE], S = w->tweak[MCGEN_TWEAK_TERRAIN_STEEPNESS], D = w->tweak[MCGEN_TWEAK_CAVE_DENSITY];
    w->sea_level = w->ns->sea_level + (int)w->tweak[MCGEN_TWEAK_SEA_LEVEL_OFFSET];
    w->noise_mxz = w->tweak[MCGEN_TWEAK_CLIMATE_SCALE_XZ] != 1.0 ? 1.0 / w->tweak[MCGEN_TWEAK_CLIMATE_SCALE_XZ] : 1.0;
    w->noise_my = w->tweak[MCGEN_TWEAK_CLIMATE_SCALE_Y] != 1.0 ? 1.0 / w->tweak[MCGEN_TWEAK_CLIMATE_SCALE_Y] : 1.0;
    w->cave_m = w->tweak[MCGEN_TWEAK_CAVE_SIZE] != 1.0 ? 1.0 / w->tweak[MCGEN_TWEAK_CAVE_SIZE] : 1.0;
    int dim_ow = w->dim_kind == 0;
    if (A == 1.0 && S == 1.0 && D == 1.0) return;
    double c = (1.0 - D) * 0.15; int kill = D == 0.0;
    for (int i = 0; i < g->dfs.cap; i++) {
        const char *id = g->dfs.e[i].key; if (!id) continue;
        const Df *orig = g->dfs.e[i].val;
        if (dim_ow && A != 1.0 && ends_with(id, "/offset"))
            over_put(w, id, mk2(DF_ADD, mk2(DF_MUL, mk2(DF_ADD, df_clone(orig), mkc(0.50375)), mkc(A)), mkc(-0.50375)));
        else if (dim_ow && A != 1.0 && ends_with(id, "/jaggedness")) over_put(w, id, mk2(DF_MUL, df_clone(orig), mkc(A)));
        else if (S != 1.0 && ends_with(id, "/factor")) over_put(w, id, mk2(DF_MUL, df_clone(orig), mkc(S)));
        else if (D != 1.0 && strstr(id, "/caves/")) over_put(w, id, kill ? mkc(1e6) : mk2(DF_ADD, df_clone(orig), mkc(c)));
        else if (D != 1.0) {
            Df *cl = df_clone(orig);
            if (wrap_cheese(cl, c, kill)) over_put(w, id, cl); else df_free(cl);
        }
    }
    if (D != 1.0) for (int k = 0; k < RF__COUNT; k++) {
        if (!w->ns->rf[k]) continue;
        Df *cl = df_clone(w->ns->rf[k]);
        if (wrap_cheese(cl, c, kill)) w->rf_over[k] = cl; else df_free(cl);
    }
}
void tweaks_release(McWorld *w) {
    sm_free(&w->df_over, df_free_cb);
    for (int k = 0; k < RF__COUNT; k++) { df_free(w->rf_over[k]); w->rf_over[k] = NULL; }
}
/* множители координат шума: климат (XZ/Y), пещеры (все оси); 1.0 — без изменений */
void world_noise_scale(const McWorld *w, const char *name, double *mxz, double *my) {
    *mxz = 1.0; *my = 1.0;
    if (sm_has(&w->climate_noises, name)) { *mxz = w->noise_mxz; *my = w->noise_my; return; }
    const char *n = strchr(name, ':') ? strchr(name, ':') + 1 : name;
    if (w->cave_m != 1.0 && (!strncmp(n, "cave_", 5) || !strncmp(n, "spaghetti_", 10) || !strncmp(n, "noodle", 6) || !strncmp(n, "pillar", 6))) {
        *mxz = w->cave_m; *my = w->cave_m;
    }
}

/* feature_misc_tmpl.c — фичи на шаблонах NBT: fossil (окаменелости), template (desert_well и др.). Размещение шаблона, повороты и процессоры — API W9
 * (template.h / processor.h); здесь — только логика фич: порядок вызовов ГСЧ, выбор шаблона/поворота, пересчёт позиции, подсчёт «пустых углов». */
#include "feature_misc.h"
#include "template.h"
#include <stdio.h>
#include <stdlib.h>

/* ГСЧ фичи (FRnd поверх Xoroshiro) как RS для API шаблонов: состояние копируется туда и обратно */
static inline void rs_wrap(RS *rs, const FRnd *r) { rs->kind = 1; rs->f = *r; }
static inline void rs_unwrap(FRnd *r, const RS *rs) { *r = rs->f; }

static const Proc *g_procs_none[1];
/* Ресурсы шаблонов (template_get/proclist_*) берут w->lock через structures_world_get, а разбор фич выполняется под тем же w->lock (features_world_get):
 * разрешать их при разборе нельзя (взаимная блокировка). Поэтому в разборе запоминаются только Js-узлы/идентификаторы (документы живут до освобождения мира),
 * а шаблоны и списки процессоров загружаются при первом размещении (потокобезопасно, кэш мира у W9; повторная запись одного и того же указателя безвредна). */
typedef struct ProcSet { const Js *js; const Proc **p; int n; int ready; } ProcSet;
static void procs_resolve(McWorld *w, ProcSet *ps) {
    if (__atomic_load_n(&ps->ready, __ATOMIC_ACQUIRE)) return;
    if (ps->js) {
        const ProcList *pl = proclist_from_json(w, ps->js);
        if (pl && pl->n > 0) { const Proc **arr = xmalloc(sizeof(Proc *) * (size_t)pl->n); for (int i = 0; i < pl->n; i++) arr[i] = pl->p[i]; ps->p = arr; ps->n = pl->n; }
    }
    __atomic_store_n(&ps->ready, 1, __ATOMIC_RELEASE);
}
static const Proc *const *procs_arr(const ProcSet *ps) { return ps->p ? (const Proc *const *)ps->p : (const Proc *const *)g_procs_none; }

/* BlockPos → поворот вектора Rotation.rotate(Direction) */
static void rot_vec(int rot, int dx, int dz, int *ox, int *oz) {
    for (int i = 0; i < rot; i++) { int t = dx; dx = -dz; dz = t; }       /* по часовой: x→z, z→−x */
    *ox = dx; *oz = dz;
}

/* ====================================================================== template */
typedef struct TplEntry { const char *id; const Template *t; int nrot; int rots[4]; int weight; } TplEntry;
typedef struct TplCfg { int n, total; TplEntry *e; ProcSet procs; } TplCfg;
static int parse_rot(const char *s) {
    if (!s) return -1; if (!strncmp(s, "minecraft:", 10)) s += 10;
    if (!strcmp(s, "none")) return ROT_NONE; if (!strcmp(s, "clockwise_90")) return ROT_CW90; if (!strcmp(s, "180")) return ROT_CW180; if (!strcmp(s, "counterclockwise_90")) return ROT_CCW90;
    if (!strcmp(s, "clockwise_180")) return ROT_CW180;
    return -1;
}
static void *tpl_parse(FParse *p, const Js *cfg) {
    TplCfg *s = fp_alloc(p, sizeof *s);
    const Js *l = js_get(cfg, "templates");
    if (!js_is_arr(l) || l->n < 1) { fp_fail(p, "template: нет templates"); return NULL; }
    s->n = l->n; s->e = fp_alloc(p, sizeof(TplEntry) * (size_t)l->n);
    for (int i = 0; i < l->n; i++) {
        const Js *it = l->items[i], *d = js_get(it, "data");
        const char *id = js_str(js_get(d, "id"), NULL);
        if (!id) { fp_fail(p, "template: нет id"); return NULL; }
        s->e[i].id = fp_strdup(p, id);
        s->e[i].weight = js_get(it, "weight") ? js_int(js_get(it, "weight"), 1) : 1; s->total += s->e[i].weight;
        const Js *rl = js_get(d, "rotations");
        if (js_is_arr(rl) && rl->n > 0) {
            s->e[i].nrot = rl->n > 4 ? 4 : rl->n;
            for (int k = 0; k < s->e[i].nrot; k++) { s->e[i].rots[k] = parse_rot(js_str(rl->items[k], NULL)); if (s->e[i].rots[k] < 0) { fp_fail(p, "template: плохой rotation"); return NULL; } }
        } else { s->e[i].nrot = 4; for (int k = 0; k < 4; k++) s->e[i].rots[k] = k; }
    }
    s->procs.js = js_get(cfg, "processors");
    return s;
}
static const Template *tpl_get(FCtx *c, TplEntry *e) {
    const Template *t = __atomic_load_n(&e->t, __ATOMIC_ACQUIRE);
    if (!t) { t = template_get(c->w, e->id); __atomic_store_n(&e->t, t, __ATOMIC_RELEASE); }
    return t;
}
static int tpl_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    TplCfg *s = (TplCfg *)cfg; FRnd *r = c->rnd;
    int sel = frnd_int_bound(r, s->total), idx = 0;
    for (; idx < s->n - 1; idx++) { sel -= s->e[idx].weight; if (sel < 0) break; }
    TplEntry *e = &s->e[idx];
    const Template *tpl = tpl_get(c, e);
    int rot = e->rots[frnd_int_bound(r, e->nrot)];
    if (!tpl) return 0;
    procs_resolve(c->w, &s->procs);
    /* getRotatedOffset: rotation.rotate(WEST для X / NORTH для Z) * (size / 2) */
    int ax, az, bx, bz;
    rot_vec(rot, -1, 0, &ax, &az); ax *= tpl->sx / 2; az *= tpl->sx / 2;
    rot_vec(rot, 0, -1, &bx, &bz); bx *= tpl->sz / 2; bz *= tpl->sz / 2;
    int x = ox + ax + bx, y = oy, z = oz + az + bz;
    TSettings ts; tsettings_init(&ts);
    ts.rot = rot; ts.procs = procs_arr(&s->procs); ts.nprocs = s->procs.n; ts.known_shape = 0;
    RS rs; rs_wrap(&rs, r); ts.rnd = &rs; ts.srnd = &rs;
    int res = template_place(c, c->w, tpl, x, y, z, x, y, z, &ts, c->w->seeds.structures, 3);
    rs_unwrap(r, &rs);
    return res;
}

/* ====================================================================== fossil */
typedef struct FossilCfg { int n; const char **base_id, **overlay_id; const Template **base, **overlay; ProcSet pbase, poverlay; int max_empty; } FossilCfg;
static void *fossil_parse(FParse *p, const Js *cfg) {
    FossilCfg *f = fp_alloc(p, sizeof *f);
    const Js *fl = js_get(cfg, "fossil_structures"), *ol = js_get(cfg, "overlay_structures");
    if (!js_is_arr(fl) || !js_is_arr(ol) || fl->n != ol->n || fl->n < 1) { fp_fail(p, "fossil: структуры"); return NULL; }
    f->n = fl->n; f->base = fp_alloc(p, sizeof(void *) * (size_t)f->n); f->overlay = fp_alloc(p, sizeof(void *) * (size_t)f->n);
    f->base_id = fp_alloc(p, sizeof(char *) * (size_t)f->n); f->overlay_id = fp_alloc(p, sizeof(char *) * (size_t)f->n);
    for (int i = 0; i < f->n; i++) { f->base_id[i] = fp_strdup(p, js_str(fl->items[i], "")); f->overlay_id[i] = fp_strdup(p, js_str(ol->items[i], "")); }
    f->pbase.js = js_get(cfg, "fossil_processors"); f->poverlay.js = js_get(cfg, "overlay_processors");
    f->max_empty = js_int(js_get(cfg, "max_empty_corners_allowed"), 0);
    return f;
}
static int fossil_place(FCtx *c, const void *cfg, int ox, int oy, int oz) {
    FossilCfg *f = (FossilCfg *)cfg; FRnd *r = c->rnd;
    int rot = frnd_int_bound(r, 4);                       /* Rotation.getRandom */
    int idx = frnd_int_bound(r, f->n);
    if (!f->base[idx]) { f->base[idx] = template_get(c->w, f->base_id[idx]); f->overlay[idx] = template_get(c->w, f->overlay_id[idx]); }
    const Template *tb = f->base[idx], *to = f->overlay[idx];
    if (!tb || !to) return 0;
    procs_resolve(c->w, &f->pbase); procs_resolve(c->w, &f->poverlay);
    int cx0 = (ox >> 4) * 16, cz0 = (oz >> 4) * 16;
    BB bounds = bb_make(cx0 - 16, c->min_y, cz0 - 16, cx0 + 15 + 16, c->min_y + c->height - 1, cz0 + 15 + 16);
    int sx, sy, sz; tpl_size(tb, rot, &sx, &sy, &sz);
    int lx = ox - sx / 2, lz = oz - sz / 2;               /* Java: целочисленное деление к нулю */
    int lowest = oy;
    for (int xs = 0; xs < sx; xs++) for (int zs = 0; zs < sz; zs++) lowest = imin_(lowest, fc_height(c, HM_OCEAN_FLOOR_WG, lx + xs, lz + zs));
    int target_y = imax_(lowest - 15 - frnd_int_bound(r, 10), c->min_y + 10);
    /* getZeroPositionWithTransform(lowCorner.atY(targetY), NONE, rotation) */
    int zx = lx, zz = lz, szx = tb->sx - 1, szz = tb->sz - 1;
    if (rot == ROT_CCW90) zz += szx; else if (rot == ROT_CW90) zx += szz; else if (rot == ROT_CW180) { zx += szx; zz += szz; }
    BB bb = tpl_bounding_box(tb, zx, target_y, zz, rot, MIR_NONE, 0, 0);
    int empty = 0;
    for (int i = 0; i < 8; i++) {
        int x = (i & 1) ? bb.x1 : bb.x0, y = (i & 2) ? bb.y1 : bb.y0, z = (i & 4) ? bb.z1 : bb.z0;
        int st = fc_get(c, x, y, z);
        if (fc_is_air(c, st) || fc_is_block(c, st, c->g->blk_lava) || fc_is_block(c, st, c->g->blk_water)) empty++;
    }
    if (empty > f->max_empty) return 0;
    TSettings ts; tsettings_init(&ts);
    ts.rot = rot; ts.bounds = &bounds; ts.known_shape = 0;
    RS rs; rs_wrap(&rs, r); ts.rnd = &rs; ts.srnd = &rs;
    ts.procs = procs_arr(&f->pbase); ts.nprocs = f->pbase.n;
    template_place(c, c->w, tb, zx, target_y, zz, zx, target_y, zz, &ts, c->w->seeds.structures, 260);
    ts.procs = procs_arr(&f->poverlay); ts.nprocs = f->poverlay.n;
    template_place(c, c->w, to, zx, target_y, zz, zx, target_y, zz, &ts, c->w->seeds.structures, 260);
    rs_unwrap(r, &rs);
    return 1;
}

static const FeatType T_TPL = { "minecraft:template", tpl_parse, tpl_place };
static const FeatType T_FOSSIL = { "minecraft:fossil", fossil_parse, fossil_place };
void feature_register_tmpl(void) { feature_register_type(&T_TPL); feature_register_type(&T_FOSSIL); }

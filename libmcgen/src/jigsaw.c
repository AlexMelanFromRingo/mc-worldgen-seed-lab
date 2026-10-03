/* jigsaw.c — сборка построек из шаблонов (JigsawPlacement), пулы элементов, часть PoolElementStructurePiece.
 * Расход ГСЧ и порядок обхода — как в игре (проверка по сохранённым стартам эталонных миров). */
#include "jigsaw.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- пулы и элементы */
enum { PE_EMPTY, PE_SINGLE, PE_LEGACY, PE_FEATURE, PE_LIST };
struct PoolElem {
    int kind, proj;                          /* proj: 0 rigid, 1 terrain_matching */
    char *loc; const Template *tpl;          /* single/legacy: шаблон (лениво) */
    const ProcList *procs;
    int liquid_override;                     /* −1 нет, 0 ignore_waterlogging, 1 apply_waterlogging */
    PoolElem **sub; int nsub;                /* list */
    const Js *feature_js; Placed *feat;      /* feature: id placed_feature */
};
typedef struct Pool {
    char *id; int loaded_ok;
    struct Pool *fallback; char *fallback_id;
    PoolElem **tpl; int ntpl;                /* расширено по весам (templates) */
    PoolElem **raw; int nraw;
    int max_size; int max_size_set;
} Pool;
typedef struct JStore { McMutex *lock; StrMap pools; PoolElem empty; PtrVec docs; PtrVec elems; Pool *empty_pool; } JStore;

void *jigsaw_store_new(void) { JStore *s = xcalloc(1, sizeof *s); s->lock = mutex_new(); s->empty.kind = PE_EMPTY; s->empty.proj = 1; s->empty.liquid_override = -1; return s; }
static void elem_free(PoolElem *e) { if (!e) return; free(e->loc); free(e->sub); free(e); }
static void pool_free(void *v) { Pool *p = v; if (!p) return; free(p->id); free(p->fallback_id); free(p->tpl); free(p->raw); free(p); }
void jigsaw_world_free(void *store) {
    JStore *s = store; if (!s) return;
    sm_free(&s->pools, pool_free);
    for (int i = 0; i < s->elems.n; i++) elem_free(s->elems.v[i]);
    for (int i = 0; i < s->docs.n; i++) js_free(s->docs.v[i]);
    pv_free(&s->elems); pv_free(&s->docs); mutex_free(s->lock); free(s);
}
static JStore *jstore(McWorld *w) { StructWorld *sw = structures_world_get(w); return sw ? (JStore *)sw->pools : NULL; }

static void norm_id(const char *in, char *out, size_t n) { if (strchr(in, ':')) snprintf(out, n, "%s", in); else snprintf(out, n, "minecraft:%s", in); }
static int id_eq(const char *a, const char *b) {
    char x[256], y[256]; norm_id(a, x, sizeof x); norm_id(b, y, sizeof y); return !strcmp(x, y);
}

static PoolElem *parse_element(McWorld *w, JStore *s, const Js *v, int inherited_proj);
static Pool *pool_get(McWorld *w, JStore *s, const char *id);

static PoolElem *parse_element(McWorld *w, JStore *s, const Js *v, int inherited_proj) {
    const char *t = js_str(js_get(v, "element_type"), "");
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    if (!strcmp(t, "empty_pool_element")) return &s->empty;
    PoolElem *e = xcalloc(1, sizeof *e); pv_push(&s->elems, e);
    e->liquid_override = -1;
    const char *pj = js_str(js_get(v, "projection"), NULL);
    e->proj = pj ? (!strcmp(pj, "terrain_matching") ? 1 : 0) : inherited_proj;
    if (!strcmp(t, "single_pool_element") || !strcmp(t, "legacy_single_pool_element")) {
        e->kind = t[0] == 'l' ? PE_LEGACY : PE_SINGLE;
        char full[256]; norm_id(js_str(js_get(v, "location"), ""), full, sizeof full); e->loc = xstrdup(full);
        const Js *pr = js_get(v, "processors");
        e->procs = pr ? proclist_from_json(w, pr) : proclist_empty();
        if (!e->procs) e->procs = proclist_empty();
        const char *ls = js_str(js_get(v, "override_liquid_settings"), NULL);
        if (ls) e->liquid_override = !strcmp(ls, "apply_waterlogging");
    } else if (!strcmp(t, "list_pool_element")) {
        e->kind = PE_LIST; const Js *a = js_get(v, "elements");
        for (int i = 0; js_is_arr(a) && i < a->n; i++) {
            PoolElem *c = parse_element(w, s, a->items[i], e->proj);
            e->sub = xrealloc(e->sub, (size_t)(e->nsub + 1) * sizeof *e->sub); e->sub[e->nsub++] = c;
        }
    } else if (!strcmp(t, "feature_pool_element")) {
        e->kind = PE_FEATURE; e->feature_js = js_get(v, "feature");
    } else { e->kind = PE_EMPTY; }
    return e;
}

static Pool *pool_get(McWorld *w, JStore *s, const char *id0) {
    char id[256]; norm_id(id0, id, sizeof id);
    mutex_lock(s->lock);
    Pool *p = sm_get(&s->pools, id);
    if (p || sm_has(&s->pools, id)) { mutex_unlock(s->lock); return p; }
    const char *c = strchr(id, ':');
    char path[1024]; snprintf(path, sizeof path, "%s/data/%.*s/worldgen/template_pool/%s.json", w->g->pack, (int)(c - id), id, c + 1);
    char err[128];
    JsDoc *d = js_parse_file(path, err, sizeof err);
    if (!d) { sm_put(&s->pools, id, NULL); mutex_unlock(s->lock); return NULL; }
    pv_push(&s->docs, d);
    p = xcalloc(1, sizeof *p); p->id = xstrdup(id);
    sm_put(&s->pools, id, p);                       /* до разбора: допускает самоссылку fallback */
    const Js *r = js_root(d);
    char fb[256]; norm_id(js_str(js_get(r, "fallback"), "minecraft:empty"), fb, sizeof fb);
    p->fallback_id = xstrdup(fb);
    const Js *els = js_get(r, "elements");
    for (int i = 0; js_is_arr(els) && i < els->n; i++) {
        const Js *it = els->items[i];
        PoolElem *e = parse_element(w, s, js_get(it, "element"), 0);
        int wt = js_int(js_get(it, "weight"), 1);
        p->raw = xrealloc(p->raw, (size_t)(p->nraw + 1) * sizeof *p->raw); p->raw[p->nraw++] = e;
        p->tpl = xrealloc(p->tpl, (size_t)(p->ntpl + wt) * sizeof *p->tpl);
        for (int k = 0; k < wt; k++) p->tpl[p->ntpl++] = e;
    }
    p->loaded_ok = 1;
    mutex_unlock(s->lock);
    return p;
}
static Pool *pool_fallback(McWorld *w, JStore *s, Pool *p) {
    if (!p->fallback) p->fallback = pool_get(w, s, p->fallback_id);
    return p->fallback;
}
static int pool_is_empty_id(const Pool *p) { return !strcmp(p->id, "minecraft:empty"); }

/* ---------------------------------------------------------------- операции над элементами */
typedef struct JInfo {
    int x, y, z, state; int rollable;
    const char *name, *pool, *target; int place_prio, sel_prio;
    int front, top;                                       /* направления DIR_* */
} JInfo;

static const Template *elem_template(McWorld *w, PoolElem *e) {
    if (!e->tpl) e->tpl = template_get(w, e->loc);
    return e->tpl;
}
static void orient_dirs(const BsTab *bs, int state, int *front, int *top) {
    const char *v = NULL; *front = DIR_NORTH; *top = DIR_UP;
    if (!bs_get_prop(bs, state, "orientation", &v)) return;
    char tmp[24]; snprintf(tmp, sizeof tmp, "%s", v); char *u = strchr(tmp, '_'); if (!u) return; *u = 0;
    static const char *N[6] = { "down", "up", "north", "south", "west", "east" };
    for (int i = 0; i < 6; i++) { if (!strcmp(N[i], tmp)) *front = i; if (!strcmp(N[i], u + 1)) *top = i; }
}
static BB elem_bb(McWorld *w, PoolElem *e, int x, int y, int z, int rot) {
    switch (e->kind) {
    case PE_SINGLE: case PE_LEGACY: { const Template *t = elem_template(w, e); if (!t) return bb_make(x, y, z, x, y, z); return tpl_bounding_box(t, x, y, z, rot, MIR_NONE, 0, 0); }
    case PE_LIST: {
        BB r = bb_empty();
        for (int i = 0; i < e->nsub; i++) if (e->sub[i]->kind != PE_EMPTY) r = bb_union(r, elem_bb(w, e->sub[i], x, y, z, rot));
        return r;
    }
    case PE_FEATURE: return bb_make(x, y, z, x, y, z);        /* size ZERO: BoundingBox(x, y, z, x + 0, y + 0, z + 0) */
    default: return bb_make(x, y, z, x, y, z);
    }
}
/* getShuffledJigsawBlocks: возвращает malloc-массив; ГСЧ — random */
static int elem_jigsaws(McWorld *w, PoolElem *e, int x, int y, int z, int rot, RS *rnd, JInfo **out) {
    const BsTab *bs = bs_get(w->g);
    *out = NULL;
    if (e->kind == PE_LIST && e->nsub > 0) return elem_jigsaws(w, e->sub[0], x, y, z, rot, rnd, out);
    if (e->kind == PE_FEATURE) {
        JInfo *j = xcalloc(1, sizeof *j); j->x = x; j->y = y; j->z = z; j->state = -1; j->rollable = 1;
        j->name = NULL; j->pool = "minecraft:empty"; j->target = "minecraft:empty"; j->front = DIR_DOWN; j->top = DIR_SOUTH;
        *out = j; return 1;
    }
    if (e->kind != PE_SINGLE && e->kind != PE_LEGACY) return 0;
    const Template *t = elem_template(w, e); if (!t) return 0;
    const TPal *pal = tpl_palette_at(t, x, y, z);
    if (!pal || pal->nj == 0) return 0;
    int n = pal->nj;
    JInfo *a = xcalloc((size_t)n, sizeof *a);
    for (int i = 0; i < n; i++) {
        const TJig *jg = &pal->j[i]; JInfo *o = &a[i];
        int wx, wz; tpl_transform(jg->x, jg->z, MIR_NONE, rot, 0, 0, &wx, &wz);
        o->x = wx + x; o->y = jg->y + y; o->z = wz + z;
        o->state = bsx_rotate(bs, jg->state, rot); o->rollable = jg->rollable; o->name = jg->name; o->pool = jg->pool; o->target = jg->target;
        o->place_prio = jg->place_prio; o->sel_prio = jg->sel_prio;
        orient_dirs(bs, o->state, &o->front, &o->top);
    }
    for (int i = n; i > 1; i--) { int sw = rs_bound(rnd, i); JInfo tmp = a[i - 1]; a[i - 1] = a[sw]; a[sw] = tmp; }      /* Util.shuffle */
    /* стабильная сортировка по selection_priority по убыванию */
    for (int i = 1; i < n; i++) { JInfo k = a[i]; int j = i - 1; while (j >= 0 && a[j].sel_prio < k.sel_prio) { a[j + 1] = a[j]; j--; } a[j + 1] = k; }
    *out = a; return n;
}
static int can_attach(const JInfo *src, const JInfo *tgt) {
    return src->front == dir_opp(tgt->front) && (src->rollable || src->top == tgt->top) && (tgt->name == NULL || id_eq(src->target, tgt->name));
}

/* Util.shuffledCopy(templates, random) */
static PoolElem **shuffled_templates(const Pool *p, RS *rnd) {
    PoolElem **a = xmalloc((size_t)(p->ntpl ? p->ntpl : 1) * sizeof *a);
    memcpy(a, p->tpl, (size_t)p->ntpl * sizeof *a);
    for (int i = p->ntpl; i > 1; i--) { int sw = rs_bound(rnd, i); PoolElem *t = a[i - 1]; a[i - 1] = a[sw]; a[sw] = t; }
    return a;
}
static int pool_max_size(McWorld *w, Pool *p) {
    if (!p->max_size_set) {
        int m = 0, any = 0;
        for (int i = 0; i < p->ntpl; i++) {
            PoolElem *e = p->tpl[i]; if (e->kind == PE_EMPTY) continue;
            BB b = elem_bb(w, e, 0, 0, 0, ROT_NONE); int ys = bb_yspan(&b); if (!any || ys > m) m = ys; any = 1;
        }
        p->max_size = any ? m : 0; p->max_size_set = 1;
    }
    return p->max_size;
}

/* ---------------------------------------------------------------- свободный объём (VoxelShape) */
typedef struct Free { int ox0, oy0, oz0, ox1, oy1, oz1; BB *occ; int nocc, cap; } Free;   /* outer: [min, max) */
static void free_occupy(Free *f, BB b) { if (f->nocc == f->cap) { f->cap = f->cap ? f->cap * 2 : 16; f->occ = xrealloc(f->occ, (size_t)f->cap * sizeof(BB)); } f->occ[f->nocc++] = b; }
/* deflated(target) ⊆ free ? */
static int free_fits(const Free *f, const BB *t) {
    if (t->x0 < f->ox0 || t->y0 < f->oy0 || t->z0 < f->oz0 || t->x1 + 1 > f->ox1 || t->y1 + 1 > f->oy1 || t->z1 + 1 > f->oz1) return 0;
    for (int i = 0; i < f->nocc; i++) if (bb_intersects(&f->occ[i], t)) return 0;
    return 1;
}

typedef struct PState { StPiece *piece; Free *free; int depth, prio; long seq; int done; } PState;
typedef struct JCfg JCfg;

/* ---------------------------------------------------------------- конфигурация JigsawStructure */
typedef struct Alias { int type; char *alias, *target; int n; struct Alias **kid; int *w; int total; struct Alias ***grp; int *gn; } Alias;   /* type: 0 direct, 1 random, 2 random_group */
struct JCfg {
    char *start_pool, *start_jigsaw; int size; HProv *start_height; int expansion_hack; int proj_hm;
    int max_h, max_v; int nal; Alias **al; int pad_bottom, pad_top; int liquid_apply;
};

static Alias *parse_alias(const Js *v) {
    Alias *a = xcalloc(1, sizeof *a);
    const char *t = js_str(js_get(v, "type"), ""); if (!strncmp(t, "minecraft:", 10)) t += 10;
    char buf[256];
    if (!strcmp(t, "direct")) { a->type = 0; norm_id(js_str(js_get(v, "alias"), ""), buf, sizeof buf); a->alias = xstrdup(buf); norm_id(js_str(js_get(v, "target"), ""), buf, sizeof buf); a->target = xstrdup(buf); }
    else if (!strcmp(t, "random")) {
        a->type = 1; norm_id(js_str(js_get(v, "alias"), ""), buf, sizeof buf); a->alias = xstrdup(buf);
        const Js *tg = js_get(v, "targets");
        for (int i = 0; js_is_arr(tg) && i < tg->n; i++) {
            a->kid = xrealloc(a->kid, (size_t)(a->n + 1) * sizeof *a->kid); a->w = xrealloc(a->w, (size_t)(a->n + 1) * sizeof *a->w);
            Alias *k = xcalloc(1, sizeof *k); norm_id(js_str(js_get(tg->items[i], "data"), ""), buf, sizeof buf); k->target = xstrdup(buf);
            a->kid[a->n] = k; a->w[a->n] = js_int(js_get(tg->items[i], "weight"), 1); a->total += a->w[a->n]; a->n++;
        }
    } else {
        a->type = 2; const Js *gs = js_get(v, "groups");
        for (int i = 0; js_is_arr(gs) && i < gs->n; i++) {
            a->grp = xrealloc(a->grp, (size_t)(a->n + 1) * sizeof *a->grp); a->gn = xrealloc(a->gn, (size_t)(a->n + 1) * sizeof *a->gn); a->w = xrealloc(a->w, (size_t)(a->n + 1) * sizeof *a->w);
            const Js *data = js_get(gs->items[i], "data"); int m = js_is_arr(data) ? data->n : 0;
            a->grp[a->n] = xcalloc((size_t)(m ? m : 1), sizeof(Alias *)); a->gn[a->n] = m;
            for (int k = 0; k < m; k++) a->grp[a->n][k] = parse_alias(data->items[k]);
            a->w[a->n] = js_int(js_get(gs->items[i], "weight"), 1); a->total += a->w[a->n]; a->n++;
        }
    }
    return a;
}
static void alias_free(Alias *a) {
    if (!a) return;
    free(a->alias); free(a->target);
    for (int i = 0; i < a->n; i++) { if (a->kid) alias_free(a->kid[i]); if (a->grp) { for (int k = 0; k < a->gn[i]; k++) alias_free(a->grp[i][k]); free(a->grp[i]); } }
    free(a->kid); free(a->w); free(a->grp); free(a->gn); free(a);
}
typedef struct AliasMap { int n; char (*key)[256]; char (*val)[256]; } AliasMap;
static void alias_put(AliasMap *m, const char *k, const char *v) {
    m->key = xrealloc(m->key, (size_t)(m->n + 1) * sizeof *m->key); m->val = xrealloc(m->val, (size_t)(m->n + 1) * sizeof *m->val);
    snprintf(m->key[m->n], 256, "%s", k); snprintf(m->val[m->n], 256, "%s", v); m->n++;
}
static int weighted_pick(const int *w, int n, int total, RS *r) { int sel = rs_bound(r, total); for (int i = 0; i < n; i++) { if (sel < w[i]) return i; sel -= w[i]; } return n - 1; }
static void alias_resolve(const Alias *a, RS *r, AliasMap *m) {
    if (a->type == 0) alias_put(m, a->alias, a->target);
    else if (a->type == 1) { int i = weighted_pick(a->w, a->n, a->total, r); alias_put(m, a->alias, a->kid[i]->target); }
    else { int i = weighted_pick(a->w, a->n, a->total, r); for (int k = 0; k < a->gn[i]; k++) alias_resolve(a->grp[i][k], r, m); }
}
static const char *alias_lookup(const AliasMap *m, const char *id) {
    for (int i = m->n - 1; i >= 0; i--) if (!strcmp(m->key[i], id)) return m->val[i];     /* ImmutableMap.Builder.build: дубликатов быть не должно */
    return id;
}

static void *jig_parse(StructWorld *sw, const Js *c, char *err, size_t errlen) {
    JCfg *g = xcalloc(1, sizeof *g);
    char buf[256];
    norm_id(js_str(js_get(c, "start_pool"), ""), buf, sizeof buf); g->start_pool = xstrdup(buf);
    const char *sj = js_str(js_get(c, "start_jigsaw_name"), NULL); if (sj) { norm_id(sj, buf, sizeof buf); g->start_jigsaw = xstrdup(buf); }
    g->size = js_int(js_get(c, "size"), 0);
    g->start_height = hprov_parse(js_get(c, "start_height"), err, errlen);
    if (!g->start_height) { free(g); return NULL; }
    g->expansion_hack = js_bool(js_get(c, "use_expansion_hack"), 0);
    const char *ph = js_str(js_get(c, "project_start_to_heightmap"), NULL);
    g->proj_hm = ph ? hm_type_from_name(ph) : -1;
    const Js *md = js_get(c, "max_distance_from_center");
    if (js_is_obj(md)) { g->max_h = js_int(js_get(md, "horizontal"), 80); g->max_v = js_int(js_get(md, "vertical"), 4096); }
    else { g->max_h = g->max_v = js_int(md, 80); }
    const Js *al = js_get(c, "pool_aliases");
    for (int i = 0; js_is_arr(al) && i < al->n; i++) { g->al = xrealloc(g->al, (size_t)(g->nal + 1) * sizeof *g->al); g->al[g->nal++] = parse_alias(al->items[i]); }
    const Js *dp = js_get(c, "dimension_padding");
    if (js_is_obj(dp)) { g->pad_bottom = js_int(js_get(dp, "bottom"), 0); g->pad_top = js_int(js_get(dp, "top"), 0); }
    else if (dp) g->pad_bottom = g->pad_top = js_int(dp, 0);
    const char *ls = js_str(js_get(c, "liquid_settings"), "apply_waterlogging");
    g->liquid_apply = strcmp(ls, "ignore_waterlogging") != 0;
    (void)sw;
    return g;
}
static void jig_free(void *p) {
    JCfg *g = p; if (!g) return;
    free(g->start_pool); free(g->start_jigsaw); hprov_free(g->start_height);
    for (int i = 0; i < g->nal; i++) alias_free(g->al[i]);
    free(g->al); free(g);
}

/* ---------------------------------------------------------------- часть PoolElementStructurePiece */
static void jpiece_add_junction(JPiece *p, JJunction j) {
    if (p->nj == p->cj) { p->cj = p->cj ? p->cj * 2 : 4; p->junc = xrealloc(p->junc, (size_t)p->cj * sizeof(JJunction)); }
    p->junc[p->nj++] = j;
}
static void jpiece_move(StPiece *sp, int dx, int dy, int dz) { JPiece *p = sp->data; p->x += dx; p->y += dy; p->z += dz; p->bb0 = bb_moved(p->bb0, dx, dy, dz); }
static void jpiece_free(void *d) { JPiece *p = d; if (!p) return; free(p->junc); free(p); }
const JPiece *jigsaw_piece_data(const StPiece *p) { return p->vt == &PIECE_JIGSAW ? p->data : NULL; }
int jigsaw_piece_projection(const StPiece *p) { const JPiece *j = jigsaw_piece_data(p); return j ? j->el->proj : 0; }

static StPiece *make_piece(PoolElem *el, int x, int y, int z, int gld, int rot, BB bb, BB bb0, int liquid_apply) {
    JPiece *p = xcalloc(1, sizeof *p);
    p->bb0 = bb0; p->el = el; p->x = x; p->y = y; p->z = z; p->ground_delta = gld; p->rot = rot; p->liquid_apply = liquid_apply;
    StPiece *sp = piece_new(&PIECE_JIGSAW, bb, -1, 0, p);
    sp->rot = rot;
    return sp;
}

/* SinglePoolElement.getSettings + place */
static int elem_place(StCtx *c, PoolElem *e, int x, int y, int z, int rot, int ref_x, int ref_y, int ref_z, int liquid_apply, int keep_jigsaws) {
    McWorld *w = c->w;
    switch (e->kind) {
    case PE_EMPTY: return 1;
    case PE_LIST: {
        for (int i = 0; i < e->nsub; i++) if (!elem_place(c, e->sub[i], x, y, z, rot, ref_x, ref_y, ref_z, liquid_apply, keep_jigsaws)) return 0;
        return 1;
    }
    case PE_FEATURE: {
        FWorld *fw = features_world_get(w);
        if (!fw) return 1;
        if (!e->feat) {
            FParse fp; memset(&fp, 0, sizeof fp); fp.fw = fw; fp.g = w->g; fp.bs = fw->bs; fp.w = w; fp.version = fw->version; fp.newf = fw->newf;
            e->feat = fp_placed(&fp, e->feature_js);
        }
        if (!e->feat) return 1;
        FCtx fcc = *c->fc; fcc.fw = fw; fcc.rnd = &c->rs->f;          /* FeaturePoolElement: PlacedFeature.place(level, generator, random, pos) на ГСЧ структуры */
        placed_place(&fcc, e->feat, x, y, z, 0);
        c->fc->fail |= fcc.fail;
        return 1;
    }
    default: break;
    }
    const Template *t = elem_template(w, e);
    if (!t) return 0;
    const Proc *procs[16]; int np = 0;
    /* SinglePoolElement.getSettings: [STRUCTURE_BLOCK, jigsaw_replacement, список элемента, проекция]; LegacySinglePoolElement убирает STRUCTURE_BLOCK
     * из начала и добавляет STRUCTURE_AND_AIR В КОНЕЦ — поэтому jigsaw → final_state (воздух) уже обработан и затем отбрасывается */
    if (e->kind != PE_LEGACY) procs[np++] = proc_builtin(w, PB_STRUCTURE_BLOCK);
    if (!keep_jigsaws) procs[np++] = proc_builtin(w, PB_JIGSAW_REPLACEMENT);
    if (e->procs) for (int i = 0; i < e->procs->n && np < 14; i++) procs[np++] = e->procs->p[i];
    if (e->proj == 1) procs[np++] = proc_gravity(w, HM_WORLD_SURFACE_WG, -1);
    if (e->kind == PE_LEGACY) procs[np++] = proc_builtin(w, PB_STRUCTURE_AND_AIR);
    TSettings s; tsettings_init(&s);
    s.rot = rot; s.bounds = &c->chunk; s.procs = procs; s.nprocs = np;
    s.waterlog = e->liquid_override >= 0 ? e->liquid_override : liquid_apply;
    return template_place(c->fc, w, t, x, y, z, ref_x, ref_y, ref_z, &s, w->seeds.structures, 18);
}

static void jpiece_post(StCtx *c, StPiece *sp, int rx, int ry, int rz) {
    JPiece *p = sp->data;
    elem_place(c, p->el, p->x, p->y, p->z, p->rot, rx, ry, rz, p->liquid_apply, 0);
}
static const char *ROTN[4] = { "NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90" };
static void jpiece_dump(const StPiece *sp, StrBuf *o) {
    const JPiece *p = sp->data;
    const char *loc = p->el->kind == PE_FEATURE ? (js_is_str(p->el->feature_js) ? p->el->feature_js->s : "feature") : (p->el->loc ? p->el->loc : (p->el->kind == PE_LIST ? "list" : "empty"));
    sb_printf(o, ",\"pos\":[%d,%d,%d],\"rot\":\"%s\",\"gld\":%d,\"loc\":\"%s\",\"proj\":\"%s\",\"junctions\":[", p->x, p->y, p->z, ROTN[p->rot], p->ground_delta,
              loc, p->el->proj ? "terrain_matching" : "rigid");
    for (int i = 0; i < p->nj; i++) sb_printf(o, "%s[%d,%d,%d,%d,\"%s\"]", i ? "," : "", p->junc[i].sx, p->junc[i].sgy, p->junc[i].sz, p->junc[i].dy, p->junc[i].dest_proj ? "terrain_matching" : "rigid");
    sb_puts(o, "]");
    /* bb0: bounding box элемента без «expansion hack» — в таком виде части восстанавливаются из NBT (так видны сохранённые старты эталона) */
    sb_printf(o, ",\"bb0\":[%d,%d,%d,%d,%d,%d]", p->bb0.x0, p->bb0.y0, p->bb0.z0, p->bb0.x1, p->bb0.y1, p->bb0.z1);
}
const PieceVT PIECE_JIGSAW = { "minecraft:jigsaw", jpiece_post, jpiece_move, jpiece_free, jpiece_dump };

/* ---------------------------------------------------------------- JigsawPlacement.addPieces */
typedef struct JStub { StPiece *center; AliasMap alias; int cx, cy, cz; } JStub;

static int jig_find(GenCtx *c, const void *cfgp, Stub *out) {
    const JCfg *g = cfgp; McWorld *w = c->w; JStore *s = jstore(w);
    int height = hprov_sample(g->start_height, &c->rs, c->gen_min_y, c->gen_depth, w->sea_level);
    int sx = c->cx * 16, sz = c->cz * 16, sy = height;                          /* startPos */
    Pool *start_pool = NULL;
    AliasMap am; memset(&am, 0, sizeof am);
    if (g->nal) {
        /* PoolAliasLookup.create: RandomSource.create(seed).forkPositional().at(pos) */
        RS base; rs_seed_lcg(&base, c->seed); i64 fseed = rs_long(&base);
        RS r; rs_seed_lcg(&r, mth_get_seed(sx, sy, sz) ^ fseed);
        for (int i = 0; i < g->nal; i++) alias_resolve(g->al[i], &r, &am);
    }
    start_pool = pool_get(w, s, alias_lookup(&am, g->start_pool));
    if (!start_pool) { free(am.key); free(am.val); return 0; }
    int rot = rs_bound(&c->rs, 4);                                              /* Rotation.getRandom */
    PoolElem *center = start_pool->ntpl == 0 ? &s->empty : start_pool->tpl[rs_bound(&c->rs, start_pool->ntpl)];
    if (center->kind == PE_EMPTY) { free(am.key); free(am.val); return 0; }
    int ax = sx, ay = sy, az = sz;
    if (g->start_jigsaw) {
        JInfo *js; int n = elem_jigsaws(w, center, sx, sy, sz, rot, &c->rs, &js); int found = 0;
        for (int i = 0; i < n; i++) if (js[i].name && id_eq(js[i].name, g->start_jigsaw)) { ax = js[i].x; ay = js[i].y; az = js[i].z; found = 1; break; }
        free(js);
        if (!found) { free(am.key); free(am.val); return 0; }
    }
    int lax = ax - sx, lay = ay - sy, laz = az - sz;                              /* localAnchorPosition */
    int px = sx - lax, py = sy - lay, pz = sz - laz;                              /* adjustedPosition */
    BB box = elem_bb(w, center, px, py, pz, rot);
    StPiece *cp = make_piece(center, px, py, pz, 1, rot, box, box, g->liquid_apply);   /* getGroundLevelDelta() = 1 */
    int centerX = (box.x1 + box.x0) / 2, centerZ = (box.z1 + box.z0) / 2;
    int bottomY;
    if (g->proj_hm < 0) bottomY = py;
    else {
        if (!gen_could_exist_in_column(c, centerX, centerZ, c->min_y, c->min_y + c->height - 1)) { piece_free(cp); free(am.key); free(am.val); return 0; }
        bottomY = sy + gen_first_free_height(c, centerX, centerZ, g->proj_hm);
    }
    int old_ground = box.y0 + ((JPiece *)cp->data)->ground_delta;
    piece_move(cp, 0, bottomY - old_ground, 0);
    if (g->pad_bottom || g->pad_top) {
        int lo = c->min_y + g->pad_bottom, hi = c->min_y + c->height - 1 - g->pad_top;
        if (cp->bb.y0 < lo || cp->bb.y1 > hi) { piece_free(cp); free(am.key); free(am.val); return 0; }
    }
    JStub *st = xcalloc(1, sizeof *st);
    st->center = cp; st->alias = am;
    out->x = centerX; out->y = bottomY + lay; out->z = centerZ; out->state = st;
    st->cx = centerX; st->cy = out->y; st->cz = centerZ;
    return 1;
}

typedef struct Placer {
    McWorld *w; JStore *s; GenCtx *c; const JCfg *g; PieceVec *pieces; AliasMap *am; int max_depth;
    PState *q; int nq, cq; long seq; Free **frees; int nfrees, cfrees;
} Placer;
static Free *new_free(Placer *p) { Free *f = xcalloc(1, sizeof *f); if (p->nfrees == p->cfrees) { p->cfrees = p->cfrees ? p->cfrees * 2 : 16; p->frees = xrealloc(p->frees, (size_t)p->cfrees * sizeof *p->frees); } p->frees[p->nfrees++] = f; return f; }

static void try_placing_children(Placer *P, StPiece *src, Free *context_free, int depth) {
    McWorld *w = P->w; JStore *s = P->s; GenCtx *c = P->c;
    JPiece *sj = src->data;
    PoolElem *src_el = sj->el;
    int src_rigid = src_el->proj == 0;
    Free *source_free = NULL;
    BB src_bb = src->bb; int src_box_y = src_bb.y0;
    JInfo *jigs; int nj = elem_jigsaws(w, src_el, sj->x, sj->y, sj->z, sj->rot, &c->rs, &jigs);
    for (int ji = 0; ji < nj; ji++) {
        const JInfo *sjig = &jigs[ji];
        int sdir = sjig->front;
        int jx = sjig->x, jy = sjig->y, jz = sjig->z;
        int tjx = jx + DIR_DX[sdir], tjy = jy + DIR_DY[sdir], tjz = jz + DIR_DZ[sdir];
        int src_local_y = jy - src_box_y;
        int src_base_h = (int)0x80000000u;
        char pool_name[256]; norm_id(alias_lookup(P->am, sjig->pool), pool_name, sizeof pool_name);
        Pool *tpool = pool_get(w, s, pool_name);
        if (!tpool) continue;
        if (tpool->ntpl == 0 && !pool_is_empty_id(tpool)) continue;
        Pool *fb = pool_fallback(w, s, tpool);
        if (!fb) continue;
        if (fb->ntpl == 0 && !pool_is_empty_id(fb)) continue;
        int attach_inside = bb_inside(&src_bb, tjx, tjy, tjz);
        Free *children_free;
        if (attach_inside) {
            children_free = source_free;
            if (!source_free) { source_free = new_free(P); source_free->ox0 = src_bb.x0; source_free->oy0 = src_bb.y0; source_free->oz0 = src_bb.z0; source_free->ox1 = src_bb.x1 + 1; source_free->oy1 = src_bb.y1 + 1; source_free->oz1 = src_bb.z1 + 1; }
            children_free = source_free;
        } else children_free = context_free;
        PoolElem **t1 = NULL; int n1 = 0;
        PoolElem **a = NULL, **b = NULL;
        if (depth != P->max_depth) { a = shuffled_templates(tpool, &c->rs); n1 = tpool->ntpl; }
        b = shuffled_templates(fb, &c->rs);
        int ntp = n1 + fb->ntpl;
        t1 = xmalloc((size_t)(ntp ? ntp : 1) * sizeof *t1);
        if (n1) memcpy(t1, a, (size_t)n1 * sizeof *t1);
        memcpy(t1 + n1, b, (size_t)fb->ntpl * sizeof *t1);
        free(a); free(b);
        int prio = sjig->place_prio;
        int attached = 0;
        for (int ti = 0; ti < ntp && !attached; ti++) {
            PoolElem *te = t1[ti];
            if (te->kind == PE_EMPTY) break;
            /* Rotation.getShuffled: Util.shuffledCopy(values(), random) */
            int rots[4] = { 0, 1, 2, 3 };
            for (int i = 4; i > 1; i--) { int sw = rs_bound(&c->rs, i); int t = rots[i - 1]; rots[i - 1] = rots[sw]; rots[sw] = t; }
            for (int ri = 0; ri < 4 && !attached; ri++) {
                int trot = rots[ri];
                JInfo *tj; int ntj = elem_jigsaws(w, te, 0, 0, 0, trot, &c->rs, &tj);
                BB hack = elem_bb(w, te, 0, 0, 0, trot);
                int expand_to = 0;
                if (P->g->expansion_hack && bb_yspan(&hack) <= 16) {
                    int mx = 0;
                    for (int k = 0; k < ntj; k++) {
                        int fx = tj[k].x + DIR_DX[tj[k].front], fy = tj[k].y + DIR_DY[tj[k].front], fz = tj[k].z + DIR_DZ[tj[k].front];
                        if (!bb_inside(&hack, fx, fy, fz)) continue;
                        char cn[256]; norm_id(alias_lookup(P->am, tj[k].pool), cn, sizeof cn);
                        Pool *cp = pool_get(w, s, cn);
                        int v = 0;
                        if (cp) { Pool *cf = pool_fallback(w, s, cp); int s1 = pool_max_size(w, cp), s2 = cf ? pool_max_size(w, cf) : 0; v = s1 > s2 ? s1 : s2; }
                        if (v > mx) mx = v;
                    }
                    expand_to = mx;
                }
                for (int k = 0; k < ntj && !attached; k++) {
                    if (!can_attach(sjig, &tj[k])) continue;
                    int lx = tj[k].x, ly = tj[k].y, lz = tj[k].z;
                    int rbx = tjx - lx, rby = tjy - ly, rbz = tjz - lz;
                    BB raw = elem_bb(w, te, rbx, rby, rbz, trot);
                    int raw_y = raw.y0;
                    int t_rigid = te->proj == 0;
                    int delta_y = src_local_y - ly + DIR_DY[sdir];
                    int tby;
                    if (src_rigid && t_rigid) tby = src_box_y + delta_y;
                    else {
                        if (src_base_h == (int)0x80000000u) src_base_h = gen_first_free_height(c, jx, jz, HM_WORLD_SURFACE_WG);
                        tby = src_base_h - ly;
                    }
                    int y_off = tby - raw_y;
                    BB tbb = bb_moved(raw, 0, y_off, 0);
                    int tbx = rbx, tby_pos = rby + y_off, tbz = rbz;
                    if (expand_to > 0) {
                        int ns = expand_to + 1 > tbb.y1 - tbb.y0 ? expand_to + 1 : tbb.y1 - tbb.y0;
                        tbb = bb_encaps_pt(tbb, tbb.x0, tbb.y0 + ns, tbb.z0);
                    }
                    if (!free_fits(children_free, &tbb)) continue;
                    free_occupy(children_free, tbb);
                    int sgld = sj->ground_delta;
                    int tgld = t_rigid ? sgld - delta_y : 1;
                    StPiece *tp = make_piece(te, tbx, tby_pos, tbz, tgld, trot, tbb, bb_moved(raw, 0, y_off, 0), P->g->liquid_apply);
                    int jyv;
                    if (src_rigid) jyv = src_box_y + src_local_y;
                    else if (t_rigid) jyv = tby + ly;
                    else {
                        if (src_base_h == (int)0x80000000u) src_base_h = gen_first_free_height(c, jx, jz, HM_WORLD_SURFACE_WG);
                        jyv = src_base_h + delta_y / 2;
                    }
                    JJunction j1 = { tjx, jyv - src_local_y + sgld, tjz, delta_y, te->proj };
                    JJunction j2 = { jx, jyv - ly + tgld, jz, -delta_y, src_el->proj };
                    jpiece_add_junction(sj, j1); jpiece_add_junction(tp->data, j2);
                    pvec_push(P->pieces, tp);
                    if (depth + 1 <= P->max_depth) {
                        if (P->nq == P->cq) { P->cq = P->cq ? P->cq * 2 : 32; P->q = xrealloc(P->q, (size_t)P->cq * sizeof *P->q); }
                        PState *ps = &P->q[P->nq++]; ps->piece = tp; ps->free = children_free; ps->depth = depth + 1; ps->prio = prio; ps->seq = P->seq++; ps->done = 0;
                    }
                    attached = 1;
                }
                free(tj);
            }
        }
        free(t1);
    }
    free(jigs);
}

static int jig_build(GenCtx *c, const void *cfgp, Stub *stub, PieceVec *out) {
    const JCfg *g = cfgp; McWorld *w = c->w; JStore *s = jstore(w);
    JStub *st = stub->state;
    pvec_push(out, st->center);
    if (g->size > 0) {
        Placer P; memset(&P, 0, sizeof P);
        P.w = w; P.s = s; P.c = c; P.g = g; P.pieces = out; P.am = &st->alias; P.max_depth = g->size;
        /* AABB: centerX ± horizontal, [max(centerY − vertical, minY + padBottom), min(centerY + vertical + 1, maxY + 1 − padTop)) */
        Free *f0 = new_free(&P);
        f0->ox0 = st->cx - g->max_h; f0->oz0 = st->cz - g->max_h; f0->ox1 = st->cx + g->max_h + 1; f0->oz1 = st->cz + g->max_h + 1;
        int lo = st->cy - g->max_v, lo2 = c->min_y + g->pad_bottom; f0->oy0 = lo > lo2 ? lo : lo2;
        int hi = st->cy + g->max_v + 1, hi2 = c->min_y + c->height - g->pad_top; f0->oy1 = hi < hi2 ? hi : hi2;
        /* Shapes.join(aabb, AABB.of(box), ONLY_FIRST): box — тот же объект BoundingBox, что у centerPiece; move() меняет его на месте,
         * поэтому в игре используется bounding box центральной части ПОСЛЕ смещения по высоте */
        free_occupy(f0, st->center->bb);
        try_placing_children(&P, st->center, f0, 0);
        for (;;) {
            int best = -1;
            for (int i = 0; i < P.nq; i++) if (!P.q[i].done && (best < 0 || P.q[i].prio > P.q[best].prio || (P.q[i].prio == P.q[best].prio && P.q[i].seq < P.q[best].seq))) best = i;
            if (best < 0) break;
            P.q[best].done = 1;
            PState ps = P.q[best];
            try_placing_children(&P, ps.piece, ps.free, ps.depth);
        }
        for (int i = 0; i < P.nfrees; i++) { free(P.frees[i]->occ); free(P.frees[i]); }
        free(P.frees); free(P.q);
    }
    free(st->alias.key); free(st->alias.val); free(st);
    stub->state = NULL;
    return 1;
}

const StructType STRUCT_JIGSAW = { "minecraft:jigsaw", jig_parse, jig_find, jig_build, NULL, jig_free };
void jigsaw_register(void) { structure_register_type(&STRUCT_JIGSAW); piece_register_type(&PIECE_JIGSAW); }

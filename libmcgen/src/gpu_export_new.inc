/* gpu_export_new.inc — экспорт скомпилированного дерева сэмплеров S (26.3+) в плоскую программу McgProg для libmcgen_cuda.
 * Включается в КОНЕЦ df_new.c (нужны приватные типы S/SSp/K_*). Поток W7 (GPU). Вызывается из gpu_bridge.c.
 *
 * Дерево сэмплеров — ориентированный ациклический граф (общие кэши): узлы нумеруются обходом в глубину с мемо по указателю,
 * дочерние всегда получают меньший индекс, чем родитель (удобно для пошагового исполнения на видеокарте). */
#include "../gpu/mcgen_gpu_abi.h"

#define GPU_SA(name) _Static_assert((int)K_##name == (int)MCGK_##name, "K_" #name " != MCGK_" #name)
GPU_SA(CONST); GPU_SA(NOISE); GPU_SA(NOISE_XZ); GPU_SA(NOISE_XYZ); GPU_SA(SHIFT_B); GPU_SA(END); GPU_SA(DIST); GPU_SA(GRAD_CLAMP);
GPU_SA(GRAD_REPEAT); GPU_SA(GRAD_MIRROR); GPU_SA(CTX_ALPHA); GPU_SA(CTX_OFFSET); GPU_SA(CTX_BEARD); GPU_SA(ABS); GPU_SA(SQUARE);
GPU_SA(CUBE); GPU_SA(SQRT); GPU_SA(LEAKY); GPU_SA(RECIP); GPU_SA(NEG); GPU_SA(SQUEEZE); GPU_SA(LOG); GPU_SA(SIGN); GPU_SA(ROUND_INT);
GPU_SA(ROUND); GPU_SA(ADD); GPU_SA(CADD); GPU_SA(CSUB); GPU_SA(SUB); GPU_SA(MUL); GPU_SA(CMUL); GPU_SA(DIV); GPU_SA(CDIV); GPU_SA(MIN);
GPU_SA(CMIN); GPU_SA(MAX); GPU_SA(CMAX); GPU_SA(POW_CB); GPU_SA(POW_CE); GPU_SA(POW); GPU_SA(SPLINE); GPU_SA(LERP); GPU_SA(LERP_CF);
GPU_SA(LERP_CS); GPU_SA(CLAMP); GPU_SA(RANGE_C); GPU_SA(RANGE); GPU_SA(ISEL1); GPU_SA(ISEL); GPU_SA(CACHE); GPU_SA(BLEND_DENSITY);
GPU_SA(INTERP); GPU_SA(SLICE_X); GPU_SA(SLICE_Y); GPU_SA(SLICE_Z); GPU_SA(SLICE_XZ); GPU_SA(FTS);
#undef GPU_SA

typedef struct { int cm, cx, cy, cz; } GCtx;     /* контекст координат: срезы slice фиксируют оси */
typedef struct {
    McgProg *p;
    const S **skey; GCtx *sctx; int *sval; int ns, cap;  /* (S*, контекст) → индекс узла (мемо) */
    const NStack **nkey; int *nval; int nn, ncap;        /* NStack* → индекс McgNoise */
    int cap_nodes, cap_noise, cap_layers, cap_oct, cap_sp, cap_splf, cap_spc, cap_arr, cap_thr;
    int fail; int bake_ctx;     /* bake_ctx: 1 — срезы slice «запекаются» в контекст узлов (точечный вычислитель); 0 — срезы остаются узлами (объёмный исполнитель) */
} GExp;

#define GX_GROW(arr, n, cap) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 64; (arr) = xrealloc((arr), sizeof(*(arr)) * (size_t)(cap)); } } while (0)

static int gx_noise(GExp *g, const NStack *s) {
    for (int i = 0; i < g->nn; i++) if (g->nkey[i] == s) return g->nval[i];
    McgProg *p = g->p;
    GX_GROW(p->noise, p->nnoise, g->cap_noise);
    int idx = p->nnoise++;
    McgNoise *mn = &p->noise[idx];
    memset(mn, 0, sizeof *mn);
    mn->l0 = p->nlayers; mn->n0 = s->n; mn->l1 = 0; mn->n1 = 0;
    for (int i = 0; i < s->n; i++) {
        const NLayer *L = &s->l[i];
        GX_GROW(p->layers, p->nlayers, g->cap_layers);
        GX_GROW(p->octs, p->noct, g->cap_oct);
        McgLayer *ml = &p->layers[p->nlayers++];
        memset(ml, 0, sizeof *ml);
        ml->a = L->freq; ml->b = L->fudge; ml->amp_f = L->amp; ml->kind = L->kind; ml->oct = p->noct;
        McgOct *mo = &p->octs[p->noct++];
        mo->xo = L->n.xo; mo->yo = L->n.yo; mo->zo = L->n.zo; memcpy(mo->p, L->n.p, 256);
    }
    if (g->nn == g->ncap) { g->ncap = g->ncap ? g->ncap * 2 : 32; g->nkey = xrealloc(g->nkey, sizeof(*g->nkey) * (size_t)g->ncap); g->nval = xrealloc(g->nval, sizeof(int) * (size_t)g->ncap); }
    g->nkey[g->nn] = s; g->nval[g->nn++] = idx;
    return idx;
}
static int gx_oct(GExp *g, const GNoise *n) {
    McgProg *p = g->p;
    GX_GROW(p->octs, p->noct, g->cap_oct);
    McgOct *mo = &p->octs[p->noct];
    mo->xo = n->xo; mo->yo = n->yo; mo->zo = n->zo; memcpy(mo->p, n->p, 256);
    return p->noct++;
}
static int gx_node(GExp *g, const S *s, GCtx cx);
static int gx_spline(GExp *g, const SSp *sp, const int *slot_of_ci) {
    McgProg *p = g->p;
    GX_GROW(p->sp, p->nsp, g->cap_sp);
    int idx = p->nsp++;
    McgSpline ms; memset(&ms, 0, sizeof ms);
    if (sp->is_const) { ms.is_const = 1; ms.value = sp->value; p->sp[idx] = ms; return idx; }
    ms.coord = sp->ci; ms.n = sp->n;
    ms.loc0 = p->nsplf;
    for (int i = 0; i < 2 * sp->n; i++) { GX_GROW(p->splf, p->nsplf, g->cap_splf); p->splf[p->nsplf++] = i < sp->n ? sp->loc[i] : sp->der[i - sp->n]; }
    int tmp[4096]; int *ch = sp->n <= 4096 ? tmp : xmalloc(sizeof(int) * (size_t)sp->n);
    for (int i = 0; i < sp->n; i++) ch[i] = gx_spline(g, sp->val[i], slot_of_ci);
    ms.val0 = p->nspc;
    for (int i = 0; i < sp->n; i++) { GX_GROW(p->spc, p->nspc, g->cap_spc); p->spc[p->nspc++] = ch[i]; }
    if (ch != tmp) free(ch);
    p->sp[idx] = ms;   /* p->sp мог переместиться при рекурсии */
    return idx;
}
static int gx_node(GExp *g, const S *s, GCtx cx) {
    if (!s) return -1;
    for (int i = 0; i < g->ns; i++) if (g->skey[i] == s && g->sctx[i].cm == cx.cm && g->sctx[i].cx == cx.cx && g->sctx[i].cy == cx.cy && g->sctx[i].cz == cx.cz) return g->sval[i];
    McgProg *p = g->p;
    McgNode n; memset(&n, 0, sizeof n);
    n.k = s->k; n.a = n.b = n.c = -1; n.ns = -1; n.sp = -1; n.oct = -1; n.cid = s->cid;
    n.cm = cx.cm; n.cx = cx.cx; n.cy = cx.cy; n.cz = cx.cz;
    n.i0 = s->i0; n.i1 = s->i1; n.i2 = s->i2; n.i3 = s->i3; n.f0 = s->f0; n.f1 = s->f1; n.f2 = s->f2; n.f3 = s->f3; n.d0 = s->d0; n.d1 = s->d1;
    GCtx cc = cx;      /* контекст детей: срезы фиксируют оси (позднейший срез перекрывает ранний) */
    if (g->bake_ctx) switch (s->k) {
    case K_SLICE_X: cc.cm |= 1; cc.cx = s->i0; break;
    case K_SLICE_Y: cc.cm |= 2; cc.cy = s->i0; break;
    case K_SLICE_Z: cc.cm |= 4; cc.cz = s->i0; break;
    case K_SLICE_XZ: cc.cm |= 5; cc.cx = s->i0; cc.cz = s->i1; break;
    default: break;
    }
    if (s->a) n.a = gx_node(g, s->a, cc);
    if (s->b) n.b = gx_node(g, s->b, cc);
    if (s->c) n.c = gx_node(g, s->c, cc);
    if (s->ns) n.ns = gx_noise(g, s->ns);
    if (s->gn) n.oct = gx_oct(g, s->gn);
    if (s->k == K_ISEL) {
        int tmp[256]; int *ids = s->narr <= 256 ? tmp : xmalloc(sizeof(int) * (size_t)s->narr);
        for (int i = 0; i < s->narr; i++) ids[i] = gx_node(g, s->arr[i], cc);
        n.arr0 = p->narr; n.narr = s->narr;
        for (int i = 0; i < s->narr; i++) { GX_GROW(p->arr, p->narr, g->cap_arr); p->arr[p->narr++] = ids[i]; }
        if (ids != tmp) free(ids);
        n.thr0 = p->nthr; n.nthr = s->nthr;
        for (int i = 0; i < s->nthr; i++) { GX_GROW(p->thr, p->nthr, g->cap_thr); p->thr[p->nthr++] = (double)s->thr[i]; }
    }
    if (s->k == K_SPLINE) {
        int tmp[64]; int *ids = s->ncoord <= 64 ? tmp : xmalloc(sizeof(int) * (size_t)s->ncoord);
        for (int i = 0; i < s->ncoord; i++) ids[i] = gx_node(g, s->coords[i], cc);
        n.arr0 = p->narr; n.narr = s->ncoord;
        for (int i = 0; i < s->ncoord; i++) { GX_GROW(p->arr, p->narr, g->cap_arr); p->arr[p->narr++] = ids[i]; }
        if (ids != tmp) free(ids);
        n.sp = gx_spline(g, s->sp, NULL);
    }
    GX_GROW(p->nodes, p->nnodes, g->cap_nodes);
    int idx = p->nnodes++;
    p->nodes[idx] = n;
    if (g->ns == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 256;
        g->skey = xrealloc(g->skey, sizeof(*g->skey) * (size_t)g->cap); g->sctx = xrealloc(g->sctx, sizeof(*g->sctx) * (size_t)g->cap); g->sval = xrealloc(g->sval, sizeof(int) * (size_t)g->cap);
    }
    g->skey[g->ns] = s; g->sctx[g->ns] = cx; g->sval[g->ns++] = idx;
    if (s->k == K_CACHE && s->cid + 1 > p->ncache) p->ncache = s->cid + 1;
    return idx;
}
/* roots[i] (NULL допустим) → prog->root[i]; 0 — успех */
int gpu_export_new(const S *const *roots, int nroots, int bake_ctx, McgProg *out) {
    memset(out, 0, sizeof *out);
    GExp g; memset(&g, 0, sizeof g);
    g.p = out; out->old = 0; g.bake_ctx = bake_ctx;
    if (nroots > MCG_MAX_ROOTS) nroots = MCG_MAX_ROOTS;
    out->nroots = nroots;
    GCtx top = { 0, 0, 0, 0 };
    for (int i = 0; i < nroots; i++) out->root[i] = roots[i] ? gx_node(&g, roots[i], top) : -1;
    free(g.skey); free(g.sctx); free(g.sval); free(g.nkey); free(g.nval);
    return 0;
}

/* ---- кэши: номера cid в дереве сэмплеров (для ячеек, которые CPU должен получить от GPU) ---- */
typedef struct { const S **seen; int n, cap; int *ids; int nid, cid_cap; } GCol;
static void gcol_walk(GCol *c, const S *s) {
    if (!s) return;
    for (int i = 0; i < c->n; i++) if (c->seen[i] == s) return;
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->seen = xrealloc(c->seen, sizeof(*c->seen) * (size_t)c->cap); }
    c->seen[c->n++] = s;
    if (s->k == K_CACHE) {
        int i = 0; for (; i < c->nid; i++) if (c->ids[i] == s->cid) break;
        if (i == c->nid && c->nid < c->cid_cap) c->ids[c->nid++] = s->cid;
    }
    gcol_walk(c, s->a); gcol_walk(c, s->b); gcol_walk(c, s->c);
    for (int i = 0; i < s->narr; i++) gcol_walk(c, s->arr[i]);
    for (int i = 0; i < s->ncoord; i++) gcol_walk(c, s->coords[i]);
}
/* дописывает в ids (ёмкость cap, текущее число *n) номера кэшей дерева root; без дублей */
void gpu_collect_cache_ids(const S *root, int *ids, int cap, int *n) {
    GCol c; memset(&c, 0, sizeof c); c.ids = ids; c.nid = *n; c.cid_cap = cap;
    gcol_walk(&c, root);
    *n = c.nid; free(c.seen);
}
/* выставить ячейку кэша cid в контексте x (как после s_volume по объёму v): буфер values — vol_size(v) значений */
void gpu_sctx_set_cell(SCtx *x, int cid, const Vol *v, const float *values) {
    if (cid < 0 || cid >= x->ncells) return;
    Cell *cl = &x->cells[cid];
    int n = vol_size(v);
    if (cl->cap < n) { free(cl->buf); cl->buf = xmalloc(sizeof(float) * (size_t)n); cl->cap = n; }
    memcpy(cl->buf, values, sizeof(float) * (size_t)n);
    cl->vol = *v; cl->has_buf = 1;
}
int gpu_sctx_cache_count(const SCtx *x) { return x->ncells; }
/* тесты: прочитать ячейку кэша cid (объём и значения). 1 — ячейка выставлена */
int gpu_sctx_get_cell(const SCtx *x, int cid, Vol *v, const float **values) {
    if (cid < 0 || cid >= x->ncells || !x->cells[cid].has_buf) return 0;
    *v = x->cells[cid].vol; *values = x->cells[cid].buf; return 1;
}

/* gpu_export_old.inc — экспорт проведённого графа ON (26.1/26.2, double) в плоскую программу McgProg для libmcgen_cuda.
 * Включается в КОНЕЦ df_old.c (нужны приватные типы ON/OSp/O_*). Поток W7 (GPU). Вызывается из gpu_bridge.c.
 * Экспортируется только «точечное» поддерево роутера (без обёрток NoiseChunk): маркеры кэшей и ссылки прозрачны. */
#include "../gpu/mcgen_gpu_abi.h"

typedef struct {
    McgProg *p;
    const ON **skey; int *sval; int ns, cap;
    const OldNormal **nkey; int *nval; int nn, ncap;
    int cap_nodes, cap_noise, cap_layers, cap_oct, cap_sp, cap_splf, cap_spc, cap_arr, cap_thr;
    int fail;
} GExpO;

#define GXO_GROW(arr, n, cap) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 64; (arr) = xrealloc((arr), sizeof(*(arr)) * (size_t)(cap)); } } while (0)

static void gxo_perlin(GExpO *g, const OldPerlin *op, int *l0, int *n0) {
    McgProg *p = g->p;
    *l0 = p->nlayers; *n0 = op->n;
    double factor = op->lowest_in, vf = op->lowest_val;
    for (int i = 0; i < op->n; i++) {
        GXO_GROW(p->layers, p->nlayers, g->cap_layers);
        McgLayer *ml = &p->layers[p->nlayers++];
        memset(ml, 0, sizeof *ml);
        ml->a = factor; ml->b = vf; ml->amp_d = op->amp[i]; ml->oct = -1;
        if (op->has[i]) {
            GXO_GROW(p->octs, p->noct, g->cap_oct);
            McgOct *mo = &p->octs[p->noct];
            const GNoise *n = &op->lev[i];
            mo->xo = n->xo; mo->yo = n->yo; mo->zo = n->zo; memcpy(mo->p, n->p, 256);
            ml->oct = p->noct++;
        }
        factor *= 2.0; vf /= 2.0;
    }
}
static int gxo_noise(GExpO *g, const OldNormal *nn) {
    for (int i = 0; i < g->nn; i++) if (g->nkey[i] == nn) return g->nval[i];
    McgProg *p = g->p;
    GXO_GROW(p->noise, p->nnoise, g->cap_noise);
    int idx = p->nnoise++;
    McgNoise mn; memset(&mn, 0, sizeof mn);
    gxo_perlin(g, &nn->first, &mn.l0, &mn.n0);
    gxo_perlin(g, &nn->second, &mn.l1, &mn.n1);
    mn.value_factor = nn->value_factor;
    p->noise[idx] = mn;
    if (g->nn == g->ncap) { g->ncap = g->ncap ? g->ncap * 2 : 32; g->nkey = xrealloc(g->nkey, sizeof(*g->nkey) * (size_t)g->ncap); g->nval = xrealloc(g->nval, sizeof(int) * (size_t)g->ncap); }
    g->nkey[g->nn] = nn; g->nval[g->nn++] = idx;
    return idx;
}
static int gxo_node(GExpO *g, const ON *f);
static void gxo_collect(const OSp *s, const ON **cs, int *n) {
    if (s->is_const) return;
    int i = 0; for (; i < *n; i++) if (cs[i] == s->coord) break;
    if (i == *n && *n < 64) cs[(*n)++] = s->coord;
    for (int k = 0; k < s->n; k++) gxo_collect(s->val[k], cs, n);
}
static int gxo_spline(GExpO *g, const OSp *s, const ON **cs, int ncs) {
    McgProg *p = g->p;
    GXO_GROW(p->sp, p->nsp, g->cap_sp);
    int idx = p->nsp++;
    McgSpline ms; memset(&ms, 0, sizeof ms);
    if (s->is_const) { ms.is_const = 1; ms.value = s->value; p->sp[idx] = ms; return idx; }
    int slot = 0; for (; slot < ncs; slot++) if (cs[slot] == s->coord) break;
    ms.coord = slot; ms.n = s->n;
    ms.loc0 = p->nsplf;
    for (int i = 0; i < 2 * s->n; i++) { GXO_GROW(p->splf, p->nsplf, g->cap_splf); p->splf[p->nsplf++] = i < s->n ? s->loc[i] : s->der[i - s->n]; }
    int tmp[4096]; int *ch = s->n <= 4096 ? tmp : xmalloc(sizeof(int) * (size_t)s->n);
    for (int i = 0; i < s->n; i++) ch[i] = gxo_spline(g, s->val[i], cs, ncs);
    ms.val0 = p->nspc;
    for (int i = 0; i < s->n; i++) { GXO_GROW(p->spc, p->nspc, g->cap_spc); p->spc[p->nspc++] = ch[i]; }
    if (ch != tmp) free(ch);
    p->sp[idx] = ms;
    return idx;
}
static int gxo_kind(int t) {
    switch (t) {
    case DF_CONST: return MCGO_CONST;
    case O_HOLDER: case DF_INTERPOLATED: case DF_FLAT_CACHE: case DF_CACHE_2D: case DF_CACHE_ONCE: case DF_CACHE_ALL_IN_CELL: case DF_BLEND_DENSITY: return MCGO_PASS;
    case DF_NOISE: return MCGO_NOISE;
    case DF_SHIFTED_NOISE: return MCGO_SHIFTED_NOISE;
    case DF_SHIFT_A: return MCGO_SHIFT_A;
    case DF_SHIFT_B: return MCGO_SHIFT_B;
    case DF_SHIFT: return MCGO_SHIFT;
    case DF_END_ISLANDS: return MCGO_END_ISLANDS;
    case DF_Y_CLAMPED_GRADIENT: return MCGO_Y_GRADIENT;
    case DF_WEIRD_SCALED: return MCGO_WEIRD_SCALED;
    case DF_BLEND_ALPHA: return MCGO_BLEND_ALPHA;
    case DF_BLEND_OFFSET: case DF_BEARDIFIER: return MCGO_ZERO;
    case DF_ABS: return MCGO_ABS;
    case DF_SQUARE: return MCGO_SQUARE;
    case DF_CUBE: return MCGO_CUBE;
    case DF_HALF_NEGATIVE: return MCGO_HALF_NEG;
    case DF_QUARTER_NEGATIVE: return MCGO_QUARTER_NEG;
    case DF_RECIPROCAL: return MCGO_RECIP;
    case DF_SQUEEZE: return MCGO_SQUEEZE;
    case DF_CLAMP: return MCGO_CLAMP;
    case O_AP2_ADD: return MCGO_ADD;
    case O_AP2_MUL: return MCGO_MUL;
    case O_AP2_MIN: return MCGO_MIN;
    case O_AP2_MAX: return MCGO_MAX;
    case O_MOA_ADD: return MCGO_CADD;
    case O_MOA_MUL: return MCGO_CMUL;
    case DF_RANGE_CHOICE: return MCGO_RANGE;
    case DF_INTERVAL_SELECT: return MCGO_ISEL;
    case DF_SPLINE: return MCGO_SPLINE;
    default: return -1;
    }
}
static int gxo_node(GExpO *g, const ON *f) {
    if (!f) return -1;
    for (int i = 0; i < g->ns; i++) if (g->skey[i] == f) return g->sval[i];
    int k = gxo_kind(f->t);
    if (k < 0) { g->fail = 1; return -1; }
    McgProg *p = g->p;
    McgNode n; memset(&n, 0, sizeof n);
    n.k = k; n.a = n.b = n.c = -1; n.ns = -1; n.sp = -1; n.oct = -1;
    n.d0 = f->d0; n.d1 = f->d1; n.i0 = f->i0; n.i1 = f->i1;
    if (k == MCGO_PASS || k == MCGO_ABS || k == MCGO_SQUARE || k == MCGO_CUBE || k == MCGO_HALF_NEG || k == MCGO_QUARTER_NEG || k == MCGO_RECIP ||
        k == MCGO_SQUEEZE || k == MCGO_CLAMP || k == MCGO_CADD || k == MCGO_CMUL || k == MCGO_WEIRD_SCALED || k == MCGO_RANGE || k == MCGO_ISEL ||
        k == MCGO_ADD || k == MCGO_MUL || k == MCGO_MIN || k == MCGO_MAX || k == MCGO_SHIFTED_NOISE) {
        if (f->a) n.a = gxo_node(g, f->a);
        if (f->b) n.b = gxo_node(g, f->b);
        if (f->c) n.c = gxo_node(g, f->c);
    }
    if (g->fail) return -1;
    if (k == MCGO_MIN) n.d1 = f->b->minv;
    if (k == MCGO_MAX) n.d1 = f->b->maxv;
    if (f->nn) n.ns = gxo_noise(g, f->nn);
    if (k == MCGO_END_ISLANDS && f->end) {
        GXO_GROW(p->octs, p->noct, g->cap_oct);
        McgOct *mo = &p->octs[p->noct];
        mo->xo = f->end->s.xo; mo->yo = f->end->s.yo; mo->zo = f->end->s.zo; memcpy(mo->p, f->end->s.p, 256);
        n.oct = p->noct++;
    }
    if (k == MCGO_ISEL) {
        int tmp[256]; int *ids = f->nlist <= 256 ? tmp : xmalloc(sizeof(int) * (size_t)f->nlist);
        for (int i = 0; i < f->nlist; i++) ids[i] = gxo_node(g, f->list[i]);
        n.arr0 = p->narr; n.narr = f->nlist;
        for (int i = 0; i < f->nlist; i++) { GXO_GROW(p->arr, p->narr, g->cap_arr); p->arr[p->narr++] = ids[i]; }
        if (ids != tmp) free(ids);
        n.thr0 = p->nthr; n.nthr = f->nthr;
        for (int i = 0; i < f->nthr; i++) { GXO_GROW(p->thr, p->nthr, g->cap_thr); p->thr[p->nthr++] = f->thr[i]; }
    }
    if (k == MCGO_SPLINE) {
        const ON *cs[64]; int ncs = 0;
        gxo_collect(f->sp, cs, &ncs);
        int ids[64];
        for (int i = 0; i < ncs; i++) ids[i] = gxo_node(g, cs[i]);
        n.arr0 = p->narr; n.narr = ncs;
        for (int i = 0; i < ncs; i++) { GXO_GROW(p->arr, p->narr, g->cap_arr); p->arr[p->narr++] = ids[i]; }
        n.sp = gxo_spline(g, f->sp, cs, ncs);
    }
    if (g->fail) return -1;
    GXO_GROW(p->nodes, p->nnodes, g->cap_nodes);
    int idx = p->nnodes++;
    p->nodes[idx] = n;
    if (g->ns == g->cap) { g->cap = g->cap ? g->cap * 2 : 256; g->skey = xrealloc(g->skey, sizeof(*g->skey) * (size_t)g->cap); g->sval = xrealloc(g->sval, sizeof(int) * (size_t)g->cap); }
    g->skey[g->ns] = f; g->sval[g->ns++] = idx;
    return idx;
}
/* поля роутера rf[field] (0 — ok, −1 — есть неподдерживаемые узлы). o — OldWire. fields[i] — RF_*, −1 — пропустить */
int gpu_export_old(void *ov, const int *fields, int nroots, McgProg *out) {
    OldWire *o = ov;
    memset(out, 0, sizeof *out);
    GExpO g; memset(&g, 0, sizeof g);
    g.p = out; out->old = 1;
    if (nroots > MCG_MAX_ROOTS) nroots = MCG_MAX_ROOTS;
    out->nroots = nroots;
    for (int i = 0; i < nroots; i++) out->root[i] = (fields[i] >= 0 && o->rf[fields[i]]) ? gxo_node(&g, o->rf[fields[i]]) : -1;
    free(g.skey); free(g.sval); free(g.nkey); free(g.nval);
    return g.fail ? -1 : 0;
}

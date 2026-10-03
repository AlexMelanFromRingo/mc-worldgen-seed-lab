/* feature_sort.c — списки фич биомов (worldgen/biome/*.json → features) и FeatureSorter.buildFeaturesPerStep: глобальный индекс фич
 * по шагам декорации. Индекс фичи входит в seed её генератора (setFeatureSeed), поэтому порядок воспроизводится точно:
 * топологическая сортировка обходом в глубину по графу «фича → следующая в списке того же биома», вершины и рёбра — в порядке
 * (шаг, индекс первого обнаружения), результат — обратный порядок завершения обхода. */
#include "feature.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct BList { int nsteps; int *n; int **ids; } BList;   /* [шаг] → индексы placed[] */

static int only_match(const char *only, const char *id) {
    if (!only) return 1;
    const char *p = only;
    size_t il = strlen(id); const char *bare = !strncmp(id, "minecraft:", 10) ? id + 10 : id;
    size_t bl = strlen(bare);
    while (*p) {
        const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
        const char *q = p; if (l > 10 && !strncmp(q, "minecraft:", 10)) { q += 10; l -= 10; }
        if (l == bl && !strncmp(q, bare, l)) return 1;
        (void)il;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static int cmp_i(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

typedef struct Sorter {
    int NF;                      /* число возможных индексов фич (= placed) */
    int nnodes, cap;
    int *nstep, *nf, *npl;       /* узел: шаг, индекс первого обнаружения, индекс в placed[] */
    int *node_of;                /* [шаг*NF + fidx] → узел или −1 */
    int **succ; int *nsucc, *csucc;
    int *state;                  /* 0 не посещён, 1 в обходе, 2 завершён */
    int *post, npost;
    int cycle;
} Sorter;

static int node_key(const Sorter *s, int n) { return s->nstep[n] * s->NF + s->nf[n]; }
static int get_node(Sorter *s, int step, int fidx, int pl) {
    int k = step * s->NF + fidx;
    if (s->node_of[k] >= 0) return s->node_of[k];
    if (s->nnodes == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->nstep = xrealloc(s->nstep, sizeof(int) * (size_t)s->cap); s->nf = xrealloc(s->nf, sizeof(int) * (size_t)s->cap); s->npl = xrealloc(s->npl, sizeof(int) * (size_t)s->cap);
        s->succ = xrealloc(s->succ, sizeof(int *) * (size_t)s->cap); s->nsucc = xrealloc(s->nsucc, sizeof(int) * (size_t)s->cap); s->csucc = xrealloc(s->csucc, sizeof(int) * (size_t)s->cap);
    }
    int n = s->nnodes++;
    s->nstep[n] = step; s->nf[n] = fidx; s->npl[n] = pl; s->succ[n] = NULL; s->nsucc[n] = s->csucc[n] = 0;
    s->node_of[k] = n;
    return n;
}
static void add_edge(Sorter *s, int a, int b) {
    for (int i = 0; i < s->nsucc[a]; i++) if (s->succ[a][i] == b) return;
    if (s->nsucc[a] == s->csucc[a]) { s->csucc[a] = s->csucc[a] ? s->csucc[a] * 2 : 4; s->succ[a] = xrealloc(s->succ[a], sizeof(int) * (size_t)s->csucc[a]); }
    s->succ[a][s->nsucc[a]++] = b;
}
static Sorter *g_sort_cmp;
static int cmp_node(const void *a, const void *b) { return node_key(g_sort_cmp, *(const int *)a) - node_key(g_sort_cmp, *(const int *)b); }

static void dfs(Sorter *s, int n) {
    if (s->cycle) return;
    if (s->state[n] == 2) return;
    if (s->state[n] == 1) { s->cycle = 1; return; }
    s->state[n] = 1;
    if (s->nsucc[n] > 1) qsort(s->succ[n], (size_t)s->nsucc[n], sizeof(int), cmp_node);
    for (int i = 0; i < s->nsucc[n]; i++) { dfs(s, s->succ[n][i]); if (s->cycle) return; }
    s->state[n] = 2;
    s->post[s->npost++] = n;
}

int fsort_build(FWorld *fw, const int *src, int nsrc) { (void)fw; (void)src; (void)nsrc; return 0; }

int fworld_load(FWorld *fw, const int *src, int nsrc) {
    const McGen *g = fw->g;
    FParse p; memset(&p, 0, sizeof p); p.fw = fw; p.g = g; p.bs = fw->bs; p.w = fw->w; p.version = fw->version; p.newf = fw->newf;
    fw->nbiomes = g->nbiomes;
    BList *bl = xcalloc((size_t)nsrc, sizeof(BList));
    int cap = 0;
    /* 1. списки биомов источника */
    for (int bi = 0; bi < nsrc; bi++) {
        char *path = xsprintf("%s/data/minecraft/worldgen/biome/%s.json", g->pack, strchr(g->biome_names[src[bi]], ':') + 1);
        JsDoc *d = js_parse_file(path, NULL, 0);
        if (!d) { snprintf(fw->err, sizeof fw->err, "нет %s", path); free(path); return -1; }
        free(path);
        Js *fs = js_get(js_root(d), "features");
        int ns = js_is_arr(fs) ? fs->n : 0;
        bl[bi].nsteps = ns; bl[bi].n = xcalloc((size_t)(ns ? ns : 1), sizeof(int)); bl[bi].ids = xcalloc((size_t)(ns ? ns : 1), sizeof(int *));
        for (int s = 0; s < ns; s++) {
            Js *l = fs->items[s]; int m = js_is_arr(l) ? l->n : 0;
            bl[bi].ids[s] = xcalloc((size_t)(m ? m : 1), sizeof(int));
            for (int k = 0; k < m; k++) {
                const Js *e = l->items[k];
                if (!js_is_str(e)) continue;                       /* встроенные placed_feature в биомах не встречаются */
                if (!only_match(fw->only, e->s)) continue;
                Placed *pf = fp_placed(&p, e);
                if (!pf) {
                    if (fw->debug) fprintf(stderr, "libmcgen: placed_feature %s: %s\n", e->s, p.err);
                    p.err[0] = 0;
                    pf = fp_alloc(&p, sizeof *pf); pf->id = fp_strdup(&p, e->s); pf->index = -1; fw->n_unimpl++;
                    sm_put(&fw->placed_cache, e->s, pf);
                }
                if (pf->index < 0) {
                    if (fw->nplaced == cap) { cap = cap ? cap * 2 : 512; fw->placed = xrealloc(fw->placed, sizeof(Placed *) * (size_t)cap); }
                    pf->index = fw->nplaced; fw->placed[fw->nplaced++] = pf;
                }
                bl[bi].ids[s][bl[bi].n[s]++] = pf->index;
            }
        }
        js_free(d);
    }
    /* 2. FeatureSorter */
    Sorter S; memset(&S, 0, sizeof S);
    S.NF = fw->nplaced ? fw->nplaced : 1;
    int maxstep = 0;
    for (int bi = 0; bi < nsrc; bi++) if (bl[bi].nsteps > maxstep) maxstep = bl[bi].nsteps;
    S.node_of = xmalloc(sizeof(int) * (size_t)(S.NF * (maxstep ? maxstep : 1)));
    for (int i = 0; i < S.NF * (maxstep ? maxstep : 1); i++) S.node_of[i] = -1;
    int *fidx_of = xmalloc(sizeof(int) * (size_t)S.NF); for (int i = 0; i < S.NF; i++) fidx_of[i] = -1;
    int next_f = 0;
    for (int bi = 0; bi < nsrc; bi++) {
        int total = 0; for (int s = 0; s < bl[bi].nsteps; s++) total += bl[bi].n[s];
        int *list = xmalloc(sizeof(int) * (size_t)(total ? total : 1)); int ln = 0;
        for (int s = 0; s < bl[bi].nsteps; s++) for (int k = 0; k < bl[bi].n[s]; k++) {
            int pl = bl[bi].ids[s][k];
            if (fidx_of[pl] < 0) fidx_of[pl] = next_f++;
            list[ln++] = get_node(&S, s, fidx_of[pl], pl);
        }
        for (int i = 0; i < ln; i++) if (i < ln - 1) add_edge(&S, list[i], list[i + 1]);
        free(list);
    }
    int *order = xmalloc(sizeof(int) * (size_t)(S.nnodes ? S.nnodes : 1));
    for (int i = 0; i < S.nnodes; i++) order[i] = i;
    g_sort_cmp = &S; qsort(order, (size_t)S.nnodes, sizeof(int), cmp_node);
    S.state = xcalloc((size_t)(S.nnodes ? S.nnodes : 1), sizeof(int)); S.post = xmalloc(sizeof(int) * (size_t)(S.nnodes ? S.nnodes : 1));
    for (int i = 0; i < S.nnodes && !S.cycle; i++) if (S.state[order[i]] == 0) dfs(&S, order[i]);
    if (S.cycle) { snprintf(fw->err, sizeof fw->err, "Feature order cycle found"); return -1; }
    fw->nsteps = maxstep;
    fw->step_n = xcalloc((size_t)(maxstep ? maxstep : 1), sizeof(int)); fw->step_list = xcalloc((size_t)(maxstep ? maxstep : 1), sizeof(int *));
    fw->step_words = xcalloc((size_t)(maxstep ? maxstep : 1), sizeof(int));
    int *pos_in_step = xmalloc(sizeof(int) * (size_t)(S.nnodes ? S.nnodes : 1));
    for (int s = 0; s < maxstep; s++) {
        int cnt = 0;
        for (int i = S.npost - 1; i >= 0; i--) if (S.nstep[S.post[i]] == s) cnt++;
        fw->step_list[s] = xmalloc(sizeof(int) * (size_t)(cnt ? cnt : 1)); fw->step_n[s] = cnt; fw->step_words[s] = (cnt + 63) / 64;
        int k = 0;
        for (int i = S.npost - 1; i >= 0; i--) if (S.nstep[S.post[i]] == s) { pos_in_step[S.post[i]] = k; fw->step_list[s][k++] = S.npl[S.post[i]]; }
    }
    /* 3. маски биомов: по шагам (глобальный индекс шага) и по placed[] (BiomeGenerationSettings.hasFeature) */
    fw->biome_step = xcalloc((size_t)g->nbiomes, sizeof(u64 **)); fw->biome_has = xcalloc((size_t)g->nbiomes, sizeof(u64 *));
    int hw = (fw->nplaced + 63) / 64 + 1;
    for (int b = 0; b < g->nbiomes; b++) fw->biome_has[b] = xcalloc((size_t)hw, sizeof(u64));
    for (int bi = 0; bi < nsrc; bi++) {
        int b = src[bi];
        fw->biome_step[b] = xcalloc((size_t)(maxstep ? maxstep : 1), sizeof(u64 *));
        for (int s = 0; s < bl[bi].nsteps; s++) {
            if (bl[bi].n[s] == 0) continue;
            u64 *m = xcalloc((size_t)(fw->step_words[s] ? fw->step_words[s] : 1), sizeof(u64));
            for (int k = 0; k < bl[bi].n[s]; k++) {
                int pl = bl[bi].ids[s][k];
                int node = S.node_of[s * S.NF + fidx_of[pl]];
                int pos = pos_in_step[node];
                m[pos >> 6] |= 1ull << (pos & 63);
                fw->biome_has[b][pl >> 6] |= 1ull << (pl & 63);
            }
            fw->biome_step[b][s] = m;
        }
    }
    /* освобождение временного */
    for (int bi = 0; bi < nsrc; bi++) { for (int s = 0; s < bl[bi].nsteps; s++) free(bl[bi].ids[s]); free(bl[bi].ids); free(bl[bi].n); }
    free(bl); free(fidx_of); free(order); free(pos_in_step);
    for (int i = 0; i < S.nnodes; i++) free(S.succ[i]);
    free(S.succ); free(S.nsucc); free(S.csucc); free(S.nstep); free(S.nf); free(S.npl); free(S.node_of); free(S.state); free(S.post);
    (void)cmp_i;
    return 0;
}

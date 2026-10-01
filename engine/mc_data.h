/* mc_data.h — хостовый загрузчик данных: data/climate-<V>.txt, data/params/<V>/*.tsv → McClimateSpec / McBiomeTree.
 * Пути задаются относительно корня проекта (MC_ROOT, по умолчанию текущий каталог или переменная окружения MCGEN_ROOT).
 */
#ifndef MC_DATA_H
#define MC_DATA_H
#include "mc_biomes.h"
#include "gen/mc_biome_names.h"
#include <stdio.h>
#include <stdlib.h>

static const char *mc_version_name(int v) { return v == MC_26_1 ? "26.1" : v == MC_26_2 ? "26.2" : v == MC_26_3 ? "26.3" : "26.4-snapshot-2"; }
static int mc_version_from_name(const char *s) {
    if (!strncmp(s, "26.1", 4)) return MC_26_1;
    if (!strncmp(s, "26.2", 4)) return MC_26_2;
    if (!strncmp(s, "26.3", 4)) return MC_26_3;
    if (!strncmp(s, "26.4", 4)) return MC_26_4;
    return -1;
}
static const char *mc_root(void) { const char *r = getenv("MCGEN_ROOT"); return r ? r : "."; }

static int mc_biome_id(const char *name) {
    if (!strncmp(name, "minecraft:", 10)) name += 10;
    for (int i = 0; i < MC_BIOME_COUNT; i++) if (!strcmp(MC_BIOME_NAMES[i], name)) return i;
    return -1;
}
static const char *mc_biome_name(int id) { return (id >= 0 && id < MC_BIOME_COUNT) ? MC_BIOME_NAMES[id] : "?"; }

/* ------------------------- разбор climate-<V>.txt ------------------------- */
typedef struct { char name[40]; McNoiseParams p; } McNamedNoise;
typedef struct {
    int n; McNamedNoise noise[16];
} McNoiseTable;

static char *mc_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); if (len) *len = (size_t)n; return b;
}

static char *mc_next_tok(char **p) {
    char *s = *p; while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *p = s; return NULL; }
    char *e = s; while (*e && *e != ' ' && *e != '\t') e++;
    if (*e) { *e = 0; *p = e + 1; } else *p = e;
    return s;
}

/* рекурсивный разбор сплайна: возвращает индекс узла */
static int mc_parse_spline(McSpline *S, char **p, int is_new) {
    char *t = mc_next_tok(p);
    int node = S->n_nodes++;
    if (t[0] == 'C') {
        S->kind[node] = 0; S->val[node] = strtof(mc_next_tok(p), NULL); (void)is_new; return node;
    }
    /* 'S' */
    S->kind[node] = 1;
    S->coord[node] = (u8)atoi(mc_next_tok(p));
    int n = atoi(mc_next_tok(p));
    S->npts[node] = (u16)n; S->off[node] = (u16)S->n_pts;
    int base = S->n_pts; S->n_pts += n;
    for (int i = 0; i < n; i++) {
        S->loc[base + i] = strtof(mc_next_tok(p), NULL);
        S->der[base + i] = strtof(mc_next_tok(p), NULL);
        S->child[base + i] = (i16)mc_parse_spline(S, p, is_new);
    }
    return node;
}

static const McNoiseParams *mc_find_noise(const McNoiseTable *T, const char *name) {
    for (int i = 0; i < T->n; i++) if (!strcmp(T->noise[i].name, name)) return &T->noise[i].p;
    return NULL;
}

typedef struct {
    int version;
    McNoiseTable noises;
    McClimateSpec preset[3];         /* overworld, amplified, large_biomes */
    char preset_name[3][16];
    int n_presets;
    McNoiseSpec nether_temp, nether_veg;   /* legacy */
} McVersionData;

static int mc_load_version(McVersionData *V, int version) {
    memset(V, 0, sizeof(*V)); V->version = version;
    char path[512]; snprintf(path, sizeof path, "%s/data/climate-%s.txt", mc_root(), mc_version_name(version));
    char *txt = mc_read_file(path, NULL); if (!txt) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    int is_new = (version >= MC_26_3);
    char *line = txt;
    struct { char name[16]; char tn[40], vn[40], cn[40], en[40], rn[40]; } pr[3]; int npr = 0;
    double rf_d[3] = {0}; float rf_f[3] = {0};
    while (*line) {
        char *eol = strchr(line, '\n'); if (eol) *eol = 0;
        char *p = line; char *tok = mc_next_tok(&p);
        if (tok && tok[0] != '#') {
            if (!strcmp(tok, "noise")) {
                McNamedNoise *nn = &V->noises.noise[V->noises.n++];
                strncpy(nn->name, mc_next_tok(&p), 39);
                McNoiseParams *P = &nn->p;
                P->new_format = atoi(mc_next_tok(&p)); P->first_octave = atoi(mc_next_tok(&p)); P->n_amp = atoi(mc_next_tok(&p));
                P->base_amplitude = strtod(mc_next_tok(&p), NULL); P->normalize = atoi(mc_next_tok(&p)); P->has_mod = atoi(mc_next_tok(&p));
                for (int i = 0; i < P->n_amp; i++) P->amp[i] = strtod(mc_next_tok(&p), NULL);
            } else if (!strcmp(tok, "preset")) {
                strncpy(pr[npr].name, mc_next_tok(&p), 15);
                strncpy(pr[npr].tn, mc_next_tok(&p), 39); strncpy(pr[npr].vn, mc_next_tok(&p), 39);
                strncpy(pr[npr].cn, mc_next_tok(&p), 39); strncpy(pr[npr].en, mc_next_tok(&p), 39); strncpy(pr[npr].rn, mc_next_tok(&p), 39);
                npr++;
            } else if (!strcmp(tok, "depth")) {
                char *pn = mc_next_tok(&p); int k = -1; for (int i = 0; i < npr; i++) if (!strcmp(pr[i].name, pn)) k = i;
                McClimateSpec *S = &V->preset[k];
                S->grad_from_y = atoi(mc_next_tok(&p)); S->grad_to_y = atoi(mc_next_tok(&p));
                char *a = mc_next_tok(&p), *b = mc_next_tok(&p), *c = mc_next_tok(&p);
                S->grad_from_d = strtod(a, NULL); S->grad_to_d = strtod(b, NULL); S->off_const_d = strtod(c, NULL);
                S->grad_from_f = strtof(a, NULL); S->grad_to_f = strtof(b, NULL); S->off_const_f = strtof(c, NULL);
            } else if (!strcmp(tok, "offset_spline")) {
                char *pn = mc_next_tok(&p); int k = -1; for (int i = 0; i < npr; i++) if (!strcmp(pr[i].name, pn)) k = i;
                McSpline *S = &V->preset[k].offset; S->n_nodes = 0; S->n_pts = 0;
                S->root = mc_parse_spline(S, &p, is_new);
            } else if (!strcmp(tok, "ridges_folded")) {
                for (int i = 0; i < 3; i++) { char *c = mc_next_tok(&p); rf_d[i] = strtod(c, NULL); rf_f[i] = strtof(c, NULL); }
            }
        }
        if (!eol) break; line = eol + 1;
    }
    free(txt);
    V->n_presets = npr;
    for (int k = 0; k < npr; k++) {
        McClimateSpec *S = &V->preset[k]; strcpy(V->preset_name[k], pr[k].name);
        S->mode = is_new ? MC_NOISE_FLOAT : MC_NOISE_DOUBLE;
        S->rf_c1_d = rf_d[0]; S->rf_c2_d = rf_d[1]; S->rf_c3_d = rf_d[2];
        S->rf_c1_f = rf_f[0]; S->rf_c2_f = rf_f[1]; S->rf_c3_f = rf_f[2];
        const char *names[6] = {"offset", pr[k].tn, pr[k].vn, pr[k].cn, pr[k].en, pr[k].rn};
        McNoiseSpec *sp[6] = {&S->sp_offset, &S->sp_temp, &S->sp_veg, &S->sp_cont, &S->sp_eros, &S->sp_ridge};
        for (int i = 0; i < 6; i++) {
            const McNoiseParams *P = mc_find_noise(&V->noises, names[i]);
            if (!P) { fprintf(stderr, "noise %s not found\n", names[i]); return -1; }
            if (is_new) mc_spec_new(sp[i], P, 0); else mc_spec_old(sp[i], P, 0);
            char full[64]; snprintf(full, sizeof full, "minecraft:%s", names[i]);
            mc_md5_seed128(full, &S->hl[i], &S->hh[i]);
        }
    }
    const McNoiseParams *nt = mc_find_noise(&V->noises, "nether/temperature"), *nv = mc_find_noise(&V->noises, "nether/vegetation");
    if (is_new) { mc_spec_new(&V->nether_temp, nt, 1); mc_spec_new(&V->nether_veg, nv, 1); }
    else        { mc_spec_old(&V->nether_temp, nt, 1); mc_spec_old(&V->nether_veg, nv, 1); }
    return 0;
}
static McClimateSpec *mc_preset(McVersionData *V, const char *name) {
    for (int i = 0; i < V->n_presets; i++) if (!strcmp(V->preset_name[i], name)) return &V->preset[i];
    return NULL;
}

/* ------------------------- список параметров биомов (TSV) ------------------------- */
static McBiomeTree *mc_load_biome_tree(int version, const char *name /* "overworld" | "nether" */) {
    char path[512]; snprintf(path, sizeof path, "%s/data/params/%s/%s.tsv", mc_root(), mc_version_name(version), name);
    char *txt = mc_read_file(path, NULL); if (!txt) { fprintf(stderr, "cannot read %s\n", path); return NULL; }
    int cap = 9000, n = 0;
    McParamBox *boxes = (McParamBox *)malloc(sizeof(McParamBox) * (size_t)cap); int *ids = (int *)malloc(sizeof(int) * (size_t)cap);
    char *line = txt;
    while (*line) {
        char *eol = strchr(line, '\n'); if (eol) *eol = 0;
        if (line[0] != '#' && line[0]) {
            long long v[13]; char bn[80]; char *p = line; int ok = 1;
            for (int i = 0; i < 13; i++) { char *e; v[i] = strtoll(p, &e, 10); if (e == p) { ok = 0; break; } p = e; }
            if (ok) {
                while (*p == '\t' || *p == ' ') p++;
                strncpy(bn, p, 79); bn[79] = 0;
                McParamBox *b = &boxes[n];
                for (int d = 0; d < 6; d++) { b->lo[d] = v[2 * d]; b->hi[d] = v[2 * d + 1]; }
                b->lo[6] = b->hi[6] = v[12];
                ids[n] = mc_biome_id(bn); if (ids[n] < 0) fprintf(stderr, "unknown biome %s\n", bn);
                n++;
            }
        }
        if (!eol) break; line = eol + 1;
    }
    free(txt);
    McBiomeTree *T = mc_rt_create(boxes, ids, n, version >= MC_26_3 ? 19 : 6);   /* Climate.RTree.CHILDREN_PER_NODE: 6 (26.1/26.2), 19 (26.3) */
    /* 26.4-snapshot-2: Climate.Parameter.distance: above = target - max + 1 (верхняя граница ИСКЛЮЧАЮЩАЯ) —
     * эквивалентно старой формуле с hi' = hi - 1 во всех узлах (дерево строится по исходным значениям). */
    if (version >= MC_26_4) for (int i = 0; i < T->n_nodes; i++) for (int d = 0; d < MC_RT_DIM; d++) T->node[i].box.hi[d] -= 1;
    free(boxes); free(ids);
    return T;
}

#endif /* MC_DATA_H */

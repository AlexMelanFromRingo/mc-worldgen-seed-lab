/* structure.c — стадия STRUCTURES: реестры, размещение, старты, ссылки, Beardifier, рисование частей (см. structure.h). */
#include "structure.h"
#include "jigsaw.h"
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== ГСЧ-мелочи */
double rs_gauss(RS *r) {
    /* Random.nextGaussian (Marsaglia polar): состояние между вызовами не хранится (в структурах не используется «парой») */
    double v1, v2, s;
    do { v1 = 2.0 * rs_double(r) - 1.0; v2 = 2.0 * rs_double(r) - 1.0; s = v1 * v1 + v2 * v2; } while (s >= 1.0 || s == 0.0);
    double mul = sqrt(-2.0 * log(s) / s);
    return v1 * mul;
}

/* ====================================================================== части и старты */
StPiece *piece_new(const PieceVT *vt, BB bb, int orient, int depth, void *data) {
    StPiece *p = xcalloc(1, sizeof *p); p->vt = vt; p->bb = bb; p->orient = orient; p->depth = depth; p->data = data; return p;
}
void piece_free(StPiece *p) { if (!p) return; if (p->vt && p->vt->free_data) p->vt->free_data(p->data); free(p); }
void piece_move(StPiece *p, int dx, int dy, int dz) { p->bb = bb_moved(p->bb, dx, dy, dz); if (p->vt->move) p->vt->move(p, dx, dy, dz); }
void pvec_push(PieceVec *a, StPiece *p) { if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->v = xrealloc(a->v, (size_t)a->cap * sizeof *a->v); } a->v[a->n++] = p; }
BB pvec_bb(const PieceVec *a) { BB b = bb_empty(); for (int i = 0; i < a->n; i++) b = bb_union(b, a->v[i]->bb); return b; }
StPiece *pvec_collision(const PieceVec *a, const BB *box) { for (int i = 0; i < a->n; i++) if (bb_intersects(&a->v[i]->bb, box)) return a->v[i]; return NULL; }
/* StructurePiecesBuilder.offsetPiecesVertically / moveBelowSeaLevel / moveInsideHeights */
void pvec_offset_vertically(PieceVec *a, int dy) { for (int i = 0; i < a->n; i++) piece_move(a->v[i], 0, dy, 0); }
int pvec_move_below_sea_level(PieceVec *a, int sea_level, int min_y, RS *r, int offset) {
    int max_y = sea_level - offset;
    BB b = pvec_bb(a);
    int y1 = bb_yspan(&b) + min_y + 1;
    if (y1 < max_y) y1 += rs_bound(r, max_y - y1);
    int dy = y1 - b.y1;
    pvec_offset_vertically(a, dy);
    return dy;
}
void pvec_move_inside_heights(PieceVec *a, RS *r, int lowest, int highest) {
    BB b = pvec_bb(a);
    int span = highest - lowest + 1 - bb_yspan(&b);
    int y0 = span > 1 ? lowest + rs_bound(r, span) : lowest;
    pvec_offset_vertically(a, y0 - b.y0);
}

/* ====================================================================== реестр типов */
#define MAX_TYPES 64
static const StructType *g_types[MAX_TYPES]; static int g_ntypes;
static const PieceVT *g_pieces[256]; static int g_npieces;
void structure_register_type(const StructType *t) { for (int i = 0; i < g_ntypes; i++) if (g_types[i] == t) return; if (g_ntypes < MAX_TYPES) g_types[g_ntypes++] = t; }
void piece_register_type(const PieceVT *vt) { for (int i = 0; i < g_npieces; i++) if (g_pieces[i] == vt) return; if (g_npieces < 256) g_pieces[g_npieces++] = vt; }
const PieceVT *piece_find_type(const char *id) { for (int i = 0; i < g_npieces; i++) if (!strcmp(g_pieces[i]->id, id)) return g_pieces[i]; return NULL; }
static const StructType *find_type(const char *name) { for (int i = 0; i < g_ntypes; i++) if (!strcmp(g_types[i]->name, name)) return g_types[i]; return NULL; }

/* единственное место со списком модулей построек */
void structure_register_all(void) {
    static int done; if (done) return; done = 1;
    jigsaw_register();
    extern void structures_register_extra(void);      /* structures/register.c: модули Java-кодированных построек */
    structures_register_extra();
}

/* ====================================================================== HeightProvider/VerticalAnchor (LCG) */
struct HProv { int type; int a_kind[2], a_off[2]; };   /* type: 0 constant, 1 uniform */
static int anchor_parse(const Js *v, int *kind, int *off) {
    if (js_get(v, "absolute")) { *kind = 0; *off = js_int(js_get(v, "absolute"), 0); return 1; }
    if (js_get(v, "above_bottom")) { *kind = 1; *off = js_int(js_get(v, "above_bottom"), 0); return 1; }
    if (js_get(v, "below_top")) { *kind = 2; *off = js_int(js_get(v, "below_top"), 0); return 1; }
    return 0;
}
static int anchor_y(int kind, int off, int gen_min_y, int gen_depth) { return kind == 0 ? off : kind == 1 ? gen_min_y + off : gen_min_y + gen_depth - 1 - off; }
HProv *hprov_parse(const Js *v, char *err, size_t errlen) {
    HProv *h = xcalloc(1, sizeof *h);
    if (js_get(v, "absolute") || js_get(v, "above_bottom") || js_get(v, "below_top")) { anchor_parse(v, &h->a_kind[0], &h->a_off[0]); return h; }
    const char *t = js_str(js_get(v, "type"), "");
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    if (!strcmp(t, "constant")) { h->type = 0; if (!anchor_parse(js_get(v, "value"), &h->a_kind[0], &h->a_off[0])) goto bad; return h; }
    if (!strcmp(t, "uniform")) {
        h->type = 1;
        if (!anchor_parse(js_get(v, "min_inclusive"), &h->a_kind[0], &h->a_off[0]) || !anchor_parse(js_get(v, "max_inclusive"), &h->a_kind[1], &h->a_off[1])) goto bad;
        return h;
    }
bad:
    set_err(err, errlen, "неподдерживаемый HeightProvider '%s'", t);
    free(h); return NULL;
}
void hprov_free(HProv *h) { free(h); }
int hprov_sample(const HProv *h, RS *r, int gen_min_y, int gen_depth, int sea_level) {
    (void)sea_level;
    if (h->type == 0) return anchor_y(h->a_kind[0], h->a_off[0], gen_min_y, gen_depth);
    int lo = anchor_y(h->a_kind[0], h->a_off[0], gen_min_y, gen_depth), hi = anchor_y(h->a_kind[1], h->a_off[1], gen_min_y, gen_depth);
    if (lo > hi) return lo;
    return rs_bound(r, hi - lo + 1) + lo;           /* UniformHeight: Mth.randomBetweenInclusive */
}

/* ====================================================================== загрузка реестров */
static const char *step_names[ST__COUNT] = { "raw_generation", "lakes", "local_modifications", "underground_structures", "surface_structures", "strongholds",
                                              "underground_ores", "underground_decoration", "fluid_springs", "vegetal_decoration", "top_layer_modification" };
static const char *adapt_names[5] = { "none", "bury", "beard_thin", "beard_box", "encapsulate" };

static void tag_collect_biomes(const McGen *g, const char *tag_id, u8 *mask, int depth) {
    if (depth > 8) return;
    const char *c = strchr(tag_id, ':'); const char *ns = c ? tag_id : "minecraft", *path = c ? c + 1 : tag_id; int nsl = c ? (int)(c - tag_id) : 9;
    char file[1024]; snprintf(file, sizeof file, "%s/data/%.*s/tags/worldgen/biome/%s.json", g->pack, nsl, ns, path);
    char err[128]; JsDoc *d = js_parse_file(file, err, sizeof err); if (!d) return;
    const Js *vals = js_get(js_root(d), "values");
    for (int i = 0; js_is_arr(vals) && i < vals->n; i++) {
        const Js *e = vals->items[i]; const char *s = js_is_str(e) ? e->s : js_str(js_get(e, "id"), NULL);
        if (!s) continue;
        if (s[0] == '#') tag_collect_biomes(g, s + 1, mask, depth + 1);
        else { int b = gen_biome_id(g, s); if (b >= 0 && b < g->nbiomes) mask[b] = 1; }
    }
    js_free(d);
}
/* HolderSet<Biome>: «#tag», «biome», либо массив */
static u8 *holder_set_biomes(const McGen *g, const Js *v) {
    u8 *mask = xcalloc((size_t)(g->nbiomes ? g->nbiomes : 1), 1);
    if (js_is_str(v)) { if (v->s[0] == '#') tag_collect_biomes(g, v->s + 1, mask, 0); else { int b = gen_biome_id(g, v->s); if (b >= 0) mask[b] = 1; } }
    else if (js_is_arr(v)) for (int i = 0; i < v->n; i++) {
        const char *s = js_str(v->items[i], NULL); if (!s) continue;
        if (s[0] == '#') tag_collect_biomes(g, s + 1, mask, 0); else { int b = gen_biome_id(g, s); if (b >= 0) mask[b] = 1; }
    }
    return mask;
}

static int cmp_defs(const void *a, const void *b) { return strcmp((*(StructDef *const *)a)->id, (*(StructDef *const *)b)->id); }
static int cmp_sets(const void *a, const void *b) { return strcmp(((const StructSet *)a)->id, ((const StructSet *)b)->id); }

static int dim_biome_possible(const McWorld *w, int b) {
    const McGen *g = w->g;
    switch (w->preset->biome_source) {
    case BS_FIXED: return b == w->preset->fixed_biome;
    case BS_THE_END: {
        static const char *E[5] = { "minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands", "minecraft:small_end_islands", "minecraft:end_barrens" };
        for (int i = 0; i < 5; i++) if (gen_biome_id(g, E[i]) == b) return 1;
        return 0;
    }
    case BS_MULTI_NETHER: for (int i = 0; i < g->n_nether; i++) if (g->nether_points[i].biome == b) return 1; return 0;
    default: for (int i = 0; i < g->n_ow; i++) if (g->ow_points[i].biome == b) return 1; return 0;
    }
}

static StructWorld *sw_build(McWorld *w);
StructWorld *structures_world_get(McWorld *w) {
    mutex_lock(w->lock);
    StructWorld *sw = w->structures;
    if (!sw) { sw = sw_build(w); w->structures = sw; }
    mutex_unlock(w->lock);
    return sw && sw->ok ? sw : NULL;
}

static StructWorld *sw_build(McWorld *w) {
    structure_register_all();
    const McGen *g = w->g;
    StructWorld *sw = xcalloc(1, sizeof *sw);
    sw->w = w; sw->g = g; sw->bs = bs_get(g); sw->version = g->version; sw->newf = g->newf; sw->lock = mutex_new();
    sw->debug = getenv("MCGEN_STRUCTURES_DEBUG") != NULL;
    sw->procs = processors_store_new(); sw->templates = templates_store_new(); sw->pools = jigsaw_store_new();
    /* structure/*.json */
    char dir[1024]; snprintf(dir, sizeof dir, "%s/data/minecraft/worldgen/structure", g->pack);
    char **files; int nf = list_dir_recursive(dir, ".json", &files);
    sw->defs = xcalloc((size_t)(nf ? nf : 1), sizeof *sw->defs);
    for (int i = 0; i < nf; i++) {
        char path[1100]; snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        char err[256] = {0}; JsDoc *d = js_parse_file(path, err, sizeof err);
        if (!d) continue;
        const Js *r = js_root(d);
        StructDef *def = xcalloc(1, sizeof *def);
        char id[256]; snprintf(id, sizeof id, "minecraft:%.*s", (int)strlen(files[i]) - 5, files[i]); def->id = xstrdup(id);
        const char *tn = js_str(js_get(r, "type"), ""); def->type = find_type(tn);
        def->biome_ok = holder_set_biomes(g, js_get(r, "biomes"));
        const char *st = js_str(js_get(r, "step"), "surface_structures"); def->step = ST_SURFACE_STRUCTURES;
        for (int k = 0; k < ST__COUNT; k++) if (!strcmp(st, step_names[k])) def->step = k;
        const char *ta = js_str(js_get(r, "terrain_adaptation"), "none");
        for (int k = 0; k < 5; k++) if (!strcmp(ta, adapt_names[k])) def->adapt = k;
        if (def->type) { char e2[256] = {0}; def->cfg = def->type->parse(sw, r, e2, sizeof e2); if (!def->cfg) { if (sw->debug) fprintf(stderr, "structures: %s: %s\n", id, e2); def->type = NULL; } }
        else if (sw->debug) fprintf(stderr, "structures: тип %s (%s) не реализован\n", tn, id);
        js_free(d);
        sw->defs[sw->ndefs++] = def;
    }
    free_str_list(files, nf);
    qsort(sw->defs, (size_t)sw->ndefs, sizeof *sw->defs, cmp_defs);
    for (int i = 0; i < sw->ndefs; i++) {
        StructDef *d = sw->defs[i]; d->index = i; sm_put(&sw->def_by_id, d->id, d);
        d->index_in_step = sw->nstep_defs[d->step]++;
    }
    for (int s = 0; s < ST__COUNT; s++) { sw->step_defs[s] = xcalloc((size_t)(sw->nstep_defs[s] ? sw->nstep_defs[s] : 1), sizeof(StructDef *)); sw->nstep_defs[s] = 0; }
    for (int i = 0; i < sw->ndefs; i++) { StructDef *d = sw->defs[i]; sw->step_defs[d->step][sw->nstep_defs[d->step]++] = d; }
    /* structure_set/*.json */
    snprintf(dir, sizeof dir, "%s/data/minecraft/worldgen/structure_set", g->pack);
    nf = list_dir_recursive(dir, ".json", &files);
    sw->sets = xcalloc((size_t)(nf ? nf : 1), sizeof *sw->sets);
    for (int i = 0; i < nf; i++) {
        char path[1100]; snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        char err[256] = {0}; JsDoc *d = js_parse_file(path, err, sizeof err);
        if (!d) continue;
        const Js *r = js_root(d); StructSet *s = &sw->sets[sw->nsets];
        char id[256]; snprintf(id, sizeof id, "minecraft:%.*s", (int)strlen(files[i]) - 5, files[i]); s->id = xstrdup(id);
        const Js *pl = js_get(r, "placement");
        const char *pt = js_str(js_get(pl, "type"), ""); if (!strncmp(pt, "minecraft:", 10)) pt += 10;
        s->ptype = !strcmp(pt, "random_spread") ? 0 : !strcmp(pt, "concentric_rings") ? 1 : 2;
        s->spacing = js_int(js_get(pl, "spacing"), 1); s->separation = js_int(js_get(pl, "separation"), 0);
        const char *sp = js_str(js_get(pl, "spread_type"), "linear"); s->triangular = !strcmp(sp, "triangular");
        s->salt = js_int(js_get(pl, "salt"), 0); s->frequency = js_numf(js_get(pl, "frequency"), 1.0f);
        const char *fm = js_str(js_get(pl, "frequency_reduction_method"), "default");
        s->freq_method = !strcmp(fm, "legacy_type_1") ? 1 : !strcmp(fm, "legacy_type_2") ? 2 : !strcmp(fm, "legacy_type_3") ? 3 : 0;
        s->excl_set = -1;
        const Js *ez = js_get(pl, "exclusion_zone");
        if (ez) { s->excl_chunks = js_int(js_get(ez, "chunk_count"), 1); s->excl_set = -2; }
        s->distance = js_int(js_get(pl, "distance"), 0); s->spread = js_int(js_get(pl, "spread"), 0); s->count = js_int(js_get(pl, "count"), 0);
        if (s->ptype == 1) s->preferred = holder_set_biomes(g, js_get(pl, "preferred_biomes"));
        const Js *sts = js_get(r, "structures");
        for (int k = 0; js_is_arr(sts) && k < sts->n; k++) {
            const char *sid = js_str(js_get(sts->items[k], "structure"), ""); StructDef *def = sm_get(&sw->def_by_id, sid);
            if (!def) continue;
            s->e = xrealloc(s->e, (size_t)(s->n + 1) * sizeof *s->e); s->e[s->n].def = def; s->e[s->n].weight = js_int(js_get(sts->items[k], "weight"), 1); s->n++;
        }
        /* для exclusion_zone нужен id другого набора — сохраним строку во временном поле */
        if (ez) s->ring_x = (int *)xstrdup(js_str(js_get(ez, "other_set"), ""));
        js_free(d);
        sw->nsets++;
    }
    free_str_list(files, nf);
    qsort(sw->sets, (size_t)sw->nsets, sizeof *sw->sets, cmp_sets);
    for (int i = 0; i < sw->nsets; i++) sm_put(&sw->set_by_id, sw->sets[i].id, (void *)(intptr_t)(i + 1));
    const char *only = getenv("MCGEN_STRUCTURES_ONLY");
    for (int i = 0; i < sw->nsets; i++) {
        StructSet *s = &sw->sets[i];
        if (s->excl_set == -2) { intptr_t v = (intptr_t)sm_get(&sw->set_by_id, (const char *)s->ring_x); s->excl_set = v ? (int)v - 1 : -1; free(s->ring_x); s->ring_x = NULL; }
        s->possible = 0;
        if (only && !strstr(only, s->id)) continue;       /* MCGEN_STRUCTURES_ONLY=id[,id]: оставить только эти наборы (эталон «один набор») */
        for (int k = 0; k < s->n && !s->possible; k++) for (int b = 0; b < g->nbiomes; b++) if (s->e[k].def->biome_ok[b] && dim_biome_possible(w, b)) { s->possible = 1; break; }
    }
    sw->ok = 1;
    return sw;
}

/* ====================================================================== освобождение */
typedef struct ChunkStarts { int cx, cz, n; StStart **s; } ChunkStarts;
typedef struct SCache { ChunkStarts **tab; int cap, n; } SCache;
typedef struct HCEnt { u64 key; int val; int used; } HCEnt;
typedef struct HCache { HCEnt *tab; int cap, n; } HCache;
typedef struct WGEnt { int cx, cz; i16 hm[2][256]; } WGEnt;
typedef struct WGCache { WGEnt **tab; int cap, n; } WGCache;
static void start_free(StStart *s) { if (!s) return; for (int i = 0; i < s->n; i++) piece_free(s->pieces[i]); free(s->pieces); free(s); }
void structures_world_free(McWorld *w) {
    StructWorld *sw = w->structures; if (!sw) return;
    SCache *c = sw->start_cache;
    if (c) { for (int i = 0; i < c->cap; i++) if (c->tab[i]) { for (int k = 0; k < c->tab[i]->n; k++) start_free(c->tab[i]->s[k]); free(c->tab[i]->s); free(c->tab[i]); } free(c->tab); free(c); }
    HCache *h = sw->height_cache; if (h) { free(h->tab); free(h); }
    { WGCache *wc = sw->wg_cache; if (wc) { for (int i = 0; i < wc->cap; i++) free(wc->tab[i]); free(wc->tab); free(wc); } }
    for (int i = 0; i < sw->ntc; i++) terrain_ctx_free(sw->tcv[i]);
    free(sw->tcv);
    for (int i = 0; i < sw->ndefs; i++) { StructDef *d = sw->defs[i]; if (d->type && d->type->free_cfg) d->type->free_cfg(d->cfg); free(d->biome_ok); free(d->id); free(d); }
    free(sw->defs);
    for (int s = 0; s < ST__COUNT; s++) free(sw->step_defs[s]);
    for (int i = 0; i < sw->nsets; i++) { free(sw->sets[i].id); free(sw->sets[i].e); free(sw->sets[i].preferred); free(sw->sets[i].ring_x); free(sw->sets[i].ring_z); free(sw->sets[i].ring_seed); free(sw->sets[i].ring_done); }
    free(sw->sets);
    sm_free(&sw->def_by_id, NULL); sm_free(&sw->set_by_id, NULL);
    templates_world_free(sw->templates); jigsaw_world_free(sw->pools); processors_world_free(w, sw->procs);
    mutex_free(sw->lock); free(sw); w->structures = NULL;
}

/* ====================================================================== размещение */
int structures_ring_positions_near(StructWorld *sw, StructSet *s, int cx, int cz);       /* structure_place.c */

static int spread_eval(RS *r, int limit, int tri) {
    if (!tri) return rs_bound(r, limit);
    int a = rs_bound(r, limit), b = rs_bound(r, limit); return (a + b) / 2;
}
static int is_placement_chunk(StructWorld *sw, StructSet *s, int cx, int cz) {
    i64 seed = sw->w->seeds.structures;
    if (s->ptype == 0) {
        int gx = jm_floordiv(cx, s->spacing), gz = jm_floordiv(cz, s->spacing);
        RS r; rs_seed_lcg(&r, 0); rs_large_feature_salt(&r, seed, gx, gz, s->salt);
        int limit = s->spacing - s->separation;
        int sx = spread_eval(&r, limit, s->triangular), sz = spread_eval(&r, limit, s->triangular);
        return gx * s->spacing + sx == cx && gz * s->spacing + sz == cz;
    }
    if (s->ptype == 1) {
        if (structures_ring_positions_near(sw, s, cx, cz) <= 0) return 0;
        for (int i = 0; i < s->nring; i++) if (s->ring_done[i] && s->ring_x[i] == cx && s->ring_z[i] == cz) return 1;
        return 0;
    }
    return cx == 0 && cz == 0;      /* dimension_origin: чанк начала измерения (спавн-таргет не моделируем) */
}
static int freq_ok(StructWorld *sw, const StructSet *s, int cx, int cz) {
    if (!(s->frequency < 1.0f)) return 1;
    i64 seed = sw->w->seeds.structures; RS r; rs_seed_lcg(&r, 0);
    switch (s->freq_method) {
    case 0: rs_large_feature_salt(&r, seed, s->salt, cx, cz); return rs_float(&r) < s->frequency;
    case 1: {
        int ax = cx >> 4, az = cz >> 4;
        rs_set_seed(&r, (i64)(i32)(ax ^ (az << 4)) ^ seed); rs_int(&r);
        return rs_bound(&r, (int)(1.0f / s->frequency)) == 0;
    }
    case 2: rs_large_feature_salt(&r, seed, cx, cz, 10387320); return rs_float(&r) < s->frequency;
    default: rs_large_feature_seed(&r, seed, cx, cz); return rs_double(&r) < (double)s->frequency;
    }
}
int structure_is_start_chunk(StructWorld *sw, const StructSet *cs, int cx, int cz) {
    StructSet *s = (StructSet *)cs;
    if (!is_placement_chunk(sw, s, cx, cz)) return 0;
    if (!freq_ok(sw, s, cx, cz)) return 0;
    if (s->excl_set >= 0) {
        StructSet *o = &sw->sets[s->excl_set];
        int r = s->excl_chunks;
        for (int dx = -r; dx <= r; dx++) for (int dz = -r; dz <= r; dz++) if (structure_is_start_chunk(sw, o, cx + dx, cz + dz)) return 0;
    }
    return 1;
}

/* ====================================================================== генерация старта */
/* пул контекстов TerrainCtx для колонок высот (под замком мира построек) */
int terrain_column_height_ctx(McWorld *w, TerrainCtx *t, int x, int z, int hm_type);    /* terrain.c */
static TerrainCtx *tc_get(StructWorld *sw) {
    TerrainCtx *t = NULL;
    mutex_lock(sw->lock);
    if (sw->ntc) t = sw->tcv[--sw->ntc];
    mutex_unlock(sw->lock);
    return t ? t : terrain_ctx_new(sw->w);
}
static void tc_put(StructWorld *sw, TerrainCtx *t) {
    mutex_lock(sw->lock);
    if (sw->ntc == sw->ctc) { sw->ctc = sw->ctc ? sw->ctc * 2 : 8; sw->tcv = xrealloc(sw->tcv, (size_t)sw->ctc * sizeof *sw->tcv); }
    sw->tcv[sw->ntc++] = t;
    mutex_unlock(sw->lock);
}

int gen_first_free_height(GenCtx *c, int x, int z, int hm) {
    StructWorld *sw = c->sw;
    u64 key = ((u64)(u32)x << 32) | (u64)(u32)z; key = key * 8u + (u64)hm;
    mutex_lock(sw->lock);
    HCache *h = sw->height_cache;
    if (h && h->cap) { u64 hh = (key * 0x9E3779B97F4A7C15ULL) >> 20; int i = (int)(hh & (u64)(h->cap - 1)); while (h->tab[i].used) { if (h->tab[i].key == key) { int v = h->tab[i].val; mutex_unlock(sw->lock); return v; } i = (i + 1) & (h->cap - 1); } }
    mutex_unlock(sw->lock);
    TerrainCtx *tc = tc_get(sw);
    int v = terrain_column_height_ctx(c->w, tc, x, z, hm);
    tc_put(sw, tc);
    mutex_lock(sw->lock);
    h = sw->height_cache; if (!h) { h = xcalloc(1, sizeof *h); sw->height_cache = h; }
    if ((h->n + 1) * 2 > h->cap) {
        int oc = h->cap; HCEnt *ot = h->tab; h->cap = oc ? oc * 2 : 1024; h->tab = xcalloc((size_t)h->cap, sizeof(HCEnt)); h->n = 0;
        for (int i = 0; i < oc; i++) if (ot[i].used) { u64 hh = (ot[i].key * 0x9E3779B97F4A7C15ULL) >> 20; int j = (int)(hh & (u64)(h->cap - 1)); while (h->tab[j].used) j = (j + 1) & (h->cap - 1); h->tab[j] = ot[i]; h->n++; }
        free(ot);
    }
    u64 hh = (key * 0x9E3779B97F4A7C15ULL) >> 20; int j = (int)(hh & (u64)(h->cap - 1));
    while (h->tab[j].used && h->tab[j].key != key) j = (j + 1) & (h->cap - 1);
    if (!h->tab[j].used) { h->tab[j].used = 1; h->tab[j].key = key; h->n++; }
    h->tab[j].val = v;
    mutex_unlock(sw->lock);
    return v;
}
int gen_biome_quart(GenCtx *c, int qx, int qy, int qz) { return world_biome_noise(c->w, qx, qy, qz); }
int gen_biome_valid(GenCtx *c, int qx, int qy, int qz) { int b = world_biome_noise(c->w, qx, qy, qz); return b >= 0 && c->def->biome_ok[b]; }
int gen_could_exist_in_column(GenCtx *c, int bx, int bz, int min_y, int max_y) {
    int qx = bx >> 2, qz = bz >> 2, q0 = min_y >> 2, q1 = max_y >> 2;
    for (int q = q0; q <= q1; q++) if (gen_biome_valid(c, qx, q, qz)) return 1;
    return 0;
}
int gen_could_exist_on_chunk_center(GenCtx *c) {
    return gen_could_exist_in_column(c, c->cx * 16 + 8, c->cz * 16 + 8, c->min_y - 1, c->min_y + c->height - 1);
}
static void corner_heights(GenCtx *c, int minx, int sx, int minz, int sz, int h[4]) {
    h[0] = gen_first_occupied_height(c, minx, minz, HM_WORLD_SURFACE_WG); h[1] = gen_first_occupied_height(c, minx, minz + sz, HM_WORLD_SURFACE_WG);
    h[2] = gen_first_occupied_height(c, minx + sx, minz, HM_WORLD_SURFACE_WG); h[3] = gen_first_occupied_height(c, minx + sx, minz + sz, HM_WORLD_SURFACE_WG);
}
int gen_mean_first_occupied_height(GenCtx *c, int minx, int sx, int minz, int sz) { int h[4]; corner_heights(c, minx, sx, minz, sz, h); return (h[0] + h[1] + h[2] + h[3]) / 4; }
int gen_lowest_y(GenCtx *c, int minx, int minz, int sx, int sz) {
    int h[4]; corner_heights(c, minx, sx, minz, sz, h);
    int a = h[0] < h[1] ? h[0] : h[1], b = h[2] < h[3] ? h[2] : h[3]; return a < b ? a : b;
}

/* Structure.generate: старт или NULL (недействителен) */
static StStart *structure_generate(StructWorld *sw, const StructDef *def, int cx, int cz) {
    McWorld *w = sw->w;
    if (!def->type) return NULL;
    GenCtx c; memset(&c, 0, sizeof c);
    c.w = w; c.sw = sw; c.bs = sw->bs; c.def = def; c.seed = w->seeds.structures; c.cx = cx; c.cz = cz;
    rs_seed_lcg(&c.rs, 0); rs_large_feature_seed(&c.rs, c.seed, cx, cz);
    c.min_y = w->min_y; c.height = w->height;
    c.gen_min_y = w->ns->min_y > w->min_y ? w->ns->min_y : w->min_y;
    c.gen_depth = w->ns->height < w->height ? w->ns->height : w->height;
    Stub stub; memset(&stub, 0, sizeof stub);
    sw->n_starts_tried++;
    if (!def->type->find(&c, def->cfg, &stub)) return NULL;
    if (!gen_biome_valid(&c, stub.x >> 2, stub.y >> 2, stub.z >> 2)) { if (def->type->free_stub) def->type->free_stub(&stub); return NULL; }
    PieceVec pv; memset(&pv, 0, sizeof pv);
    if (!def->type->build(&c, def->cfg, &stub, &pv) || pv.n == 0) { for (int i = 0; i < pv.n; i++) piece_free(pv.v[i]); free(pv.v); return NULL; }
    StStart *s = xcalloc(1, sizeof *s);
    s->def = def; s->cx = cx; s->cz = cz; s->n = pv.n; s->pieces = pv.v;
    for (int i = 0; i < pv.n; i++) pv.v[i]->bb_init = pv.v[i]->bb;
    s->bb = pvec_bb(&pv); s->adj_bb = def->adapt != TA_NONE ? bb_inflated(s->bb, 12) : s->bb;
    sw->n_starts_valid++;
    return s;
}

/* ChunkGenerator.createStructures для одного чанка-источника (все наборы) */
static void create_structures(StructWorld *sw, int cx, int cz, ChunkStarts *out) {
    for (int si = 0; si < sw->nsets; si++) {
        StructSet *s = &sw->sets[si];
        if (!s->possible || s->n == 0) continue;
        if (!structure_is_start_chunk(sw, s, cx, cz)) continue;
        StStart *st = NULL;
        if (s->n == 1) st = structure_generate(sw, s->e[0].def, cx, cz);
        else {
            int n = s->n; StructEntry *opt = xmalloc((size_t)n * sizeof *opt); memcpy(opt, s->e, (size_t)n * sizeof *opt);
            RS r; rs_seed_lcg(&r, 0); rs_large_feature_seed(&r, sw->w->seeds.structures, cx, cz);
            int total = 0; for (int i = 0; i < n; i++) total += opt[i].weight;
            while (n > 0) {
                int choice = rs_bound(&r, total), idx = 0;
                for (int i = 0; i < n; i++) { choice -= opt[i].weight; if (choice < 0) break; idx++; }
                StructEntry sel = opt[idx];
                st = structure_generate(sw, sel.def, cx, cz);
                if (st) break;
                for (int i = idx; i < n - 1; i++) opt[i] = opt[i + 1];
                n--; total -= sel.weight;
            }
            free(opt);
        }
        if (st) { out->s = xrealloc(out->s, (size_t)(out->n + 1) * sizeof *out->s); out->s[out->n++] = st; }
    }
}

/* ====================================================================== кэш стартов */
static ChunkStarts *cache_lookup(SCache *c, int cx, int cz) {
    if (!c->cap) return NULL;
    u64 key = ((u64)(u32)cx << 32) | (u64)(u32)cz; u64 h = (key * 0x9E3779B97F4A7C15ULL) >> 20;
    int i = (int)(h & (u64)(c->cap - 1));
    while (c->tab[i]) { if (c->tab[i]->cx == cx && c->tab[i]->cz == cz) return c->tab[i]; i = (i + 1) & (c->cap - 1); }
    return NULL;
}
static void cache_insert(SCache *c, ChunkStarts *e) {
    if ((c->n + 1) * 2 > c->cap) {
        int oc = c->cap; ChunkStarts **ot = c->tab; c->cap = oc ? oc * 2 : 1024; c->tab = xcalloc((size_t)c->cap, sizeof *c->tab); c->n = 0;
        for (int i = 0; i < oc; i++) if (ot[i]) { u64 key = ((u64)(u32)ot[i]->cx << 32) | (u64)(u32)ot[i]->cz; u64 h = (key * 0x9E3779B97F4A7C15ULL) >> 20; int j = (int)(h & (u64)(c->cap - 1)); while (c->tab[j]) j = (j + 1) & (c->cap - 1); c->tab[j] = ot[i]; c->n++; }
        free(ot);
    }
    u64 key = ((u64)(u32)e->cx << 32) | (u64)(u32)e->cz; u64 h = (key * 0x9E3779B97F4A7C15ULL) >> 20; int j = (int)(h & (u64)(c->cap - 1));
    while (c->tab[j]) j = (j + 1) & (c->cap - 1);
    c->tab[j] = e; c->n++;
}

/* быстрая проверка «может ли чанк быть источником»: хотя бы один набор считает его стартовым (дёшево, без биомов) */
static int chunk_has_any_placement(StructWorld *sw, int cx, int cz) {
    for (int si = 0; si < sw->nsets; si++) { StructSet *s = &sw->sets[si]; if (s->possible && s->n && structure_is_start_chunk(sw, s, cx, cz)) return 1; }
    return 0;
}

int structure_starts_at(StructWorld *sw, int cx, int cz, StStart ***out) {
    mutex_lock(sw->lock);
    SCache *c = sw->start_cache; if (!c) { c = xcalloc(1, sizeof *c); sw->start_cache = c; }
    ChunkStarts *e = cache_lookup(c, cx, cz);
    if (!e) {
        e = xcalloc(1, sizeof *e); e->cx = cx; e->cz = cz;
        mutex_unlock(sw->lock);        /* сами вычисления без замка кэша: тяжёлые, независимые; дубликаты отбрасываем */
        ChunkStarts tmp; memset(&tmp, 0, sizeof tmp);
        if (chunk_has_any_placement(sw, cx, cz)) create_structures(sw, cx, cz, &tmp);
        mutex_lock(sw->lock);
        c = sw->start_cache;
        ChunkStarts *e2 = cache_lookup(c, cx, cz);
        if (e2) { for (int i = 0; i < tmp.n; i++) start_free(tmp.s[i]); free(tmp.s); free(e); e = e2; }
        else { e->n = tmp.n; e->s = tmp.s; cache_insert(c, e); }
    }
    *out = e->s; int n = e->n;
    mutex_unlock(sw->lock);
    return n;
}

/* ====================================================================== ссылки (structure_references) */
/* порядок итерации LongOpenHashSet (fastutil) для ключей, добавленных в заданном порядке: ключ 0 первым, затем по убыванию индекса таблицы */
static void longset_order(const i64 *keys, int n, int *order) {
    int cap = 32;                            /* DEFAULT_INITIAL_SIZE 16 / 0.75 → 32 */
    while (n > (int)(cap * 0.75f)) cap *= 2;
    i64 *tab = xcalloc((size_t)cap, sizeof(i64)); int *id = xmalloc((size_t)cap * sizeof(int)); int null_idx = -1;
    for (int i = 0; i < cap; i++) id[i] = -1;
    int mask = cap - 1;
    for (int i = 0; i < n; i++) {
        i64 k = keys[i];
        if (k == 0) { null_idx = i; continue; }
        u64 hh = (u64)k * 0x9E3779B97F4A7C15ULL; hh ^= hh >> 32; int pos = (int)((u32)(hh ^ (hh >> 16))) & mask;
        while (tab[pos] != 0) pos = (pos + 1) & mask;
        tab[pos] = k; id[pos] = i;
    }
    int m = 0; if (null_idx >= 0) order[m++] = null_idx;
    for (int pos = cap - 1; pos >= 0; pos--) if (tab[pos] != 0) order[m++] = id[pos];
    free(tab); free(id);
}

/* все старты, чьи (скорректированные) bounding box пересекают чанк; порядок: источники по x, затем z (как createReferences) */
int structure_refs_for_chunk(StructWorld *sw, int cx, int cz, StStart ***out) {
    StStart **res = NULL; int n = 0, cap = 0;
    int x0 = cx * 16, z0 = cz * 16;
    for (int sx = cx - 8; sx <= cx + 8; sx++) for (int sz = cz - 8; sz <= cz + 8; sz++) {
        StStart **st; int k = structure_starts_at(sw, sx, sz, &st);
        for (int i = 0; i < k; i++) {
            if (!bb_intersects_xz(&st[i]->adj_bb, x0, z0, x0 + 15, z0 + 15)) continue;
            if (n == cap) { cap = cap ? cap * 2 : 8; res = xrealloc(res, (size_t)cap * sizeof *res); }
            res[n++] = st[i];
        }
    }
    *out = res; return n;
}
void structure_free_refs(StStart **a) { free(a); }

/* ====================================================================== рисование частей (StructureStart.placeInChunk) */
static void start_place_in_chunk(StCtx *c, const StStart *s) {
    if (s->n == 0) return;
    const BB *cb = &s->pieces[0]->bb;
    int rx = bb_cx(cb), ry = cb->y0, rz = bb_cz(cb);
    for (int i = 0; i < s->n; i++) {
        StPiece *p = s->pieces[i];
        if (bb_intersects(&p->bb, &c->chunk) && p->vt->post) p->vt->post(c, p, rx, ry, rz);
    }
    if (s->def->type && s->def->type->after_place) s->def->type->after_place(c, s);
}

void structures_decorate_step(FCtx *fc, FRnd *rnd, i64 dec_seed, int step, int cx, int cz) {
    McWorld *w = fc->w;
    StructWorld *sw = structures_world_get(w);
    if (!sw || step < 0 || step >= ST__COUNT || sw->nstep_defs[step] == 0) return;
    StStart **refs; int nr = structure_refs_for_chunk(sw, cx, cz, &refs);
    if (nr == 0) { structure_free_refs(refs); return; }
    StCtx c; memset(&c, 0, sizeof c);
    c.w = w; c.sw = sw; c.fc = fc; c.cx = cx; c.cz = cz;
    c.chunk = bb_make(cx * 16, w->min_y + 1, cz * 16, cx * 16 + 15, w->min_y + w->height - 1, cz * 16 + 15);
    RS rs; rs.kind = 1; rs.f = *rnd; c.rs = &rs;
    for (int i = 0; i < sw->nstep_defs[step]; i++) {
        const StructDef *def = sw->step_defs[step][i];
        rs_set_seed(&rs, dec_seed + (i64)i + (i64)(10000 * step));
        /* старты этой структуры, ссылающиеся на чанк: порядок — итерация LongOpenHashSet ключей (ChunkPos.pack источника) */
        i64 keys[64]; const StStart *cand[64]; int m = 0;
        for (int k = 0; k < nr && m < 64; k++) if (refs[k]->def == def) { cand[m] = refs[k]; keys[m] = ((i64)(i32)refs[k]->cz << 32) | (i64)(u32)refs[k]->cx; m++; }
        if (m == 0) continue;
        int order[64]; if (m > 1) longset_order(keys, m, order); else order[0] = 0;
        for (int k = 0; k < m; k++) start_place_in_chunk(&c, cand[order[k]]);
    }
    *rnd = rs.f;
    structure_free_refs(refs);
}

/* ====================================================================== Beardifier */
typedef struct BRigid { BB box; int adapt, gld; } BRigid;
struct Beard { int nr; BRigid *r; int nj; JJunction *j; BB affected; int has; };

int beard_active(const Beard *b) { return b && b->has; }
Beard *beard_for_chunk(McWorld *w, int cx, int cz) {
    StructWorld *sw = structures_world_get(w); if (!sw) return NULL;
    StStart **refs; int nr = structure_refs_for_chunk(sw, cx, cz, &refs);
    Beard *b = NULL; BB any = bb_empty();
    int x0 = cx * 16, z0 = cz * 16;
    for (int i = 0; i < nr; i++) {
        const StStart *s = refs[i]; int ad = s->def->adapt;
        if (ad == TA_NONE) continue;
        for (int k = 0; k < s->n; k++) {
            const StPiece *p = s->pieces[k];
            if (!bb_intersects_xz(&p->bb, x0 - 12, z0 - 12, x0 + 15 + 12, z0 + 15 + 12)) continue;       /* isCloseToChunk(pos, 12) */
            if (!b) b = xcalloc(1, sizeof *b);
            const JPiece *jp = jigsaw_piece_data(p);
            if (jp) {
                if (jp->el && jigsaw_piece_projection(p) == 0) {
                    b->r = xrealloc(b->r, (size_t)(b->nr + 1) * sizeof *b->r); b->r[b->nr].box = p->bb; b->r[b->nr].adapt = ad; b->r[b->nr].gld = jp->ground_delta; b->nr++;
                    any = bb_union(any, p->bb);
                }
                for (int q = 0; q < jp->nj; q++) {
                    const JJunction *j = &jp->junc[q];
                    if (j->sx > x0 - 12 && j->sz > z0 - 12 && j->sx < x0 + 15 + 12 && j->sz < z0 + 15 + 12) {
                        b->j = xrealloc(b->j, (size_t)(b->nj + 1) * sizeof *b->j); b->j[b->nj++] = *j;
                        any = bb_encaps_pt(any, j->sx, j->sgy, j->sz);
                    }
                }
            } else {
                b->r = xrealloc(b->r, (size_t)(b->nr + 1) * sizeof *b->r); b->r[b->nr].box = p->bb; b->r[b->nr].adapt = ad; b->r[b->nr].gld = 0; b->nr++;
                any = bb_union(any, p->bb);
            }
        }
    }
    structure_free_refs(refs);
    if (!b || bb_is_empty(&any)) { free(b ? b->r : NULL); free(b ? b->j : NULL); free(b); return NULL; }
    b->affected = bb_inflated(any, 24); b->has = 1;
    return b;
}
void beard_free(Beard *b) { if (!b) return; free(b->r); free(b->j); free(b); }

static float BEARD_KERNEL[24 * 24 * 24];
static int kernel_ready;
static float compute_beard_contribution(int dx, int dy, int dz) {
    double dyo = dy + 0.5, d2 = (double)(dx * dx) + dyo * dyo + (double)(dz * dz);
    /* Math.pow(Math.E, −d²/16): значение приводится к float */
    return (float)exp(-d2 / 16.0);
}
static void kernel_init(void) {
    if (kernel_ready) return;
    for (int zi = 0; zi < 24; zi++) for (int xi = 0; xi < 24; xi++) for (int yi = 0; yi < 24; yi++)
        BEARD_KERNEL[zi * 24 * 24 + xi * 24 + yi] = compute_beard_contribution(xi - 12, yi - 12, zi - 12);
    kernel_ready = 1;
}
/* Mth.lengthSquared(float…) = x*x + y*y + z*z; Mth.sqrt(float) = (float)Math.sqrt(value); Mth.fastInvSqrt(double) */
static float len_sq3(float x, float y, float z) { return x * x + y * y + z * z; }
static double fast_inv_sqrt(double x) {
    double half = 0.5 * x; i64 i; memcpy(&i, &x, 8);
    i = 0x5fe6eb50c7b537aaLL - (i >> 1); memcpy(&x, &i, 8);
    return x * (1.5 - half * x * x);
}
static float bury_contribution(float dx, float dy, float dz) {
    float d2 = len_sq3(dx, dy, dz);
    return d2 >= 36.0f ? 0.0f : 1.0f - (float)sqrt((double)d2) / 6.0f;
}
static float beard_contribution(int dx, int dy, int dz, int y_to_ground) {
    int xi = dx + 12, yi = dy + 12, zi = dz + 12;
    if (xi < 0 || xi >= 24 || yi < 0 || yi >= 24 || zi < 0 || zi >= 24) return 0.0f;
    float dyo = (float)y_to_ground + 0.5f;
    float d2 = len_sq3((float)dx, dyo, (float)dz);
    float value = -dyo * (float)fast_inv_sqrt((double)(d2 / 2.0f)) / 2.0f;
    return value * BEARD_KERNEL[zi * 24 * 24 + xi * 24 + yi];
}
static float beard_sample(const Beard *b, int bx, int by, int bz) {
    float nv = 0.0f;
    for (int i = 0; i < b->nr; i++) {
        const BRigid *r = &b->r[i]; const BB *bx_ = &r->box;
        int ddx = bx_->x0 - bx; int t = bx - bx_->x1; if (t > ddx) ddx = t; if (ddx < 0) ddx = 0;
        int ddz = bx_->z0 - bz; t = bz - bx_->z1; if (t > ddz) ddz = t; if (ddz < 0) ddz = 0;
        int ground = bx_->y0 + r->gld, dy_ground = by - ground, dy;
        switch (r->adapt) {
        case TA_BURY: case TA_BEARD_THIN: dy = dy_ground; break;
        case TA_BEARD_BOX: { int a = ground - by, c2 = by - bx_->y1; dy = a > c2 ? a : c2; if (dy < 0) dy = 0; break; }
        case TA_ENCAPSULATE: { int a = bx_->y0 - by, c2 = by - bx_->y1; dy = a > c2 ? a : c2; if (dy < 0) dy = 0; break; }
        default: dy = 0;
        }
        switch (r->adapt) {
        case TA_BURY: nv += bury_contribution((float)ddx, (float)dy / 2.0f, (float)ddz); break;
        case TA_BEARD_THIN: case TA_BEARD_BOX: nv += beard_contribution(ddx, dy, ddz, dy_ground) * 0.8f; break;
        case TA_ENCAPSULATE: nv += bury_contribution((float)ddx / 2.0f, (float)dy / 2.0f, (float)ddz / 2.0f) * 0.8f; break;
        default: break;
        }
    }
    for (int i = 0; i < b->nj; i++) {
        const JJunction *j = &b->j[i];
        int dx = bx - j->sx, dy = by - j->sgy, dz = bz - j->sz;
        nv += beard_contribution(dx, dy, dz, dy) * 0.4f;
    }
    return nv;
}
float beard_value(const Beard *b, int x, int y, int z) {
    if (!b || !b->has || !bb_inside(&b->affected, x, y, z)) return 0.0f;
    kernel_init();
    return beard_sample(b, x, y, z);
}
double beard_value_d(const Beard *b, int x, int y, int z) { return (double)beard_value(b, x, y, z); }
void beard_volume(const Beard *b, float *out, const Vol *v) {
    int n = vol_size(v);
    for (int i = 0; i < n; i++) out[i] = 0.0f;
    if (!b || !b->has) return;
    kernel_init();
    BB vb = bb_make(v->x0, v->y0, v->z0, vol_max_x(v), vol_max_y(v), vol_max_z(v));
    if (!bb_intersects(&b->affected, &vb)) return;
    int minx = jm_floordiv(b->affected.x0 - v->x0 > 0 ? b->affected.x0 - v->x0 : 0, v->dx), miny = jm_floordiv(b->affected.y0 - v->y0 > 0 ? b->affected.y0 - v->y0 : 0, v->dy), minz = jm_floordiv(b->affected.z0 - v->z0 > 0 ? b->affected.z0 - v->z0 : 0, v->dz);
    int maxx = jm_floordiv(b->affected.x1 - v->x0, v->dx), maxy = jm_floordiv(b->affected.y1 - v->y0, v->dy), maxz = jm_floordiv(b->affected.z1 - v->z0, v->dz);
    if (maxx > v->sx - 1) maxx = v->sx - 1; if (maxy > v->sy - 1) maxy = v->sy - 1; if (maxz > v->sz - 1) maxz = v->sz - 1;
    for (int z = minz; z <= maxz; z++) for (int x = minx; x <= maxx; x++) for (int y = miny; y <= maxy; y++)
        out[vol_idx(v, x, y, z)] = beard_sample(b, vol_bx(v, x), vol_by(v, y), vol_bz(v, z));
}

/* ====================================================================== карты высот WG (WORLD_SURFACE_WG, OCEAN_FLOOR_WG) в 26.3
 * Игра создаёт эти карты в NoiseBasedChunkGenerator.doFill и обновляет только при заполнении шумом; блоки построек, фич, поверхности и карверов
 * в них не попадают (ProtoChunk.setBlockState обновляет лишь «финальные» карты). Поэтому значение = результат заполнения шумом (с Beardifier). */
static WGEnt *wg_find(WGCache *c, int cx, int cz) {
    if (!c || !c->cap) return NULL;
    u64 key = ((u64)(u32)cx << 32) | (u64)(u32)cz; int i = (int)(((key * 0x9E3779B97F4A7C15ULL) >> 20) & (u64)(c->cap - 1));
    while (c->tab[i]) { if (c->tab[i]->cx == cx && c->tab[i]->cz == cz) return c->tab[i]; i = (i + 1) & (c->cap - 1); }
    return NULL;
}
static void wg_insert(WGCache *c, WGEnt *e) {
    if ((c->n + 1) * 2 > c->cap) {
        int oc = c->cap; WGEnt **ot = c->tab; c->cap = oc ? oc * 2 : 256; c->tab = xcalloc((size_t)c->cap, sizeof *c->tab); c->n = 0;
        for (int i = 0; i < oc; i++) if (ot[i]) { WGEnt *o = ot[i]; u64 key = ((u64)(u32)o->cx << 32) | (u64)(u32)o->cz; int j = (int)(((key * 0x9E3779B97F4A7C15ULL) >> 20) & (u64)(c->cap - 1)); while (c->tab[j]) j = (j + 1) & (c->cap - 1); c->tab[j] = o; c->n++; }
        free(ot);
    }
    u64 key = ((u64)(u32)e->cx << 32) | (u64)(u32)e->cz; int j = (int)(((key * 0x9E3779B97F4A7C15ULL) >> 20) & (u64)(c->cap - 1));
    while (c->tab[j]) j = (j + 1) & (c->cap - 1);
    c->tab[j] = e; c->n++;
}
int structure_height_wg(McWorld *w, int type, int x, int z) {
    StructWorld *sw = structures_world_get(w);
    if (!sw) return w->min_y;
    int cx = x >> 4, cz = z >> 4, k = type == HM_OCEAN_FLOOR_WG ? 1 : 0;
    mutex_lock(sw->lock);
    WGCache *c = sw->wg_cache; WGEnt *e = wg_find(c, cx, cz);
    if (e) { int v = e->hm[k][(z & 15) * 16 + (x & 15)]; mutex_unlock(sw->lock); return v; }
    mutex_unlock(sw->lock);
    WGEnt *ne = xcalloc(1, sizeof *ne); ne->cx = cx; ne->cz = cz;
    size_t tot = (size_t)w->height * 256;
    u16 *blocks = xmalloc(tot * sizeof(u16)); char err[128] = {0};
    TerrainCtx *t = tc_get(sw);
    int rc = terrain_fill_chunk(w, t, cx, cz, blocks, err, sizeof err);
    tc_put(sw, t);
    const BsTab *bs = sw->bs;
    for (int i = 0; i < 256; i++) { ne->hm[0][i] = (i16)w->min_y; ne->hm[1][i] = (i16)w->min_y; }
    if (!rc) for (int i = 0; i < 256; i++) {
        int left = 2;
        for (int y = w->height - 1; y >= 0 && left; y--) {
            int st = blocks[(size_t)y * 256 + i];
            if (ne->hm[0][i] == w->min_y && ((bs->hmcls[st] >> HM_WORLD_SURFACE_WG) & 1)) { ne->hm[0][i] = (i16)(w->min_y + y + 1); left--; }
            if (ne->hm[1][i] == w->min_y && ((bs->hmcls[st] >> HM_OCEAN_FLOOR_WG) & 1)) { ne->hm[1][i] = (i16)(w->min_y + y + 1); left--; }
        }
    }
    free(blocks);
    mutex_lock(sw->lock);
    if (!sw->wg_cache) sw->wg_cache = xcalloc(1, sizeof(WGCache));
    c = sw->wg_cache; e = wg_find(c, cx, cz);
    if (e) { free(ne); ne = e; } else wg_insert(c, ne);
    int v = ne->hm[k][(z & 15) * 16 + (x & 15)];
    mutex_unlock(sw->lock);
    return v;
}

void structures_begin_region(McWorld *w) {
    w->struct_run++;
    StructWorld *sw = structures_world_get(w);
    if (!sw) return;
    mutex_lock(sw->lock);
    SCache *c = sw->start_cache;
    if (c) for (int i = 0; i < c->cap; i++) if (c->tab[i]) for (int k = 0; k < c->tab[i]->n; k++) {
        StStart *s = c->tab[i]->s[k];
        for (int q = 0; q < s->n; q++) { StPiece *p = s->pieces[q]; p->bb = p->bb_init; if (p->vt->reset) p->vt->reset(p); }
    }
    mutex_unlock(sw->lock);
}

/* ====================================================================== ABI: старты */
int mcgen_structure_starts(McWorld *w, int cx0, int cz0, int nx, int nz, McStructureStart *out, int cap) {
    StructWorld *sw = w ? structures_world_get(w) : NULL;
    if (!sw) return 0;
    int total = 0;
    for (int cz = cz0; cz < cz0 + nz; cz++) for (int cx = cx0; cx < cx0 + nx; cx++) {
        StStart **st; int n = structure_starts_at(sw, cx, cz, &st);
        for (int i = 0; i < n; i++) {
            if (out && total < cap) {
                McStructureStart *o = &out[total];
                o->id = st[i]->def->id; o->chunk_x = cx; o->chunk_z = cz; o->piece_count = st[i]->n;
                o->bb[0] = st[i]->bb.x0; o->bb[1] = st[i]->bb.y0; o->bb[2] = st[i]->bb.z0; o->bb[3] = st[i]->bb.x1; o->bb[4] = st[i]->bb.y1; o->bb[5] = st[i]->bb.z1;
            }
            total++;
        }
    }
    return total;
}
int mcgen_structure_piece_bb(McWorld *w, int cx, int cz, int index, int piece, int bb[6]) {
    StructWorld *sw = w ? structures_world_get(w) : NULL;
    if (!sw || !bb) return MCGEN_E_ARG;
    StStart **st; int n = structure_starts_at(sw, cx, cz, &st);
    if (index < 0 || index >= n || piece < 0 || piece >= st[index]->n) return MCGEN_E_ARG;
    const BB *b = &st[index]->pieces[piece]->bb;
    bb[0] = b->x0; bb[1] = b->y0; bb[2] = b->z0; bb[3] = b->x1; bb[4] = b->y1; bb[5] = b->z1;
    return MCGEN_OK;
}

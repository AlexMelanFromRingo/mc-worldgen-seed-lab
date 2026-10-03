/* feature.c — стадия FEATURES: реестр типов фич, разбор конфигураций из JSON датапака, мир фич (FWorld), окно чанков региона и цикл
 * ChunkGenerator.applyBiomeDecoration. Подробно — docs/blender/features.md. */
#include "feature.h"
#include "carver.h"
#include "surface.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>

/* ====================================================================== реестр типов фич */
#define MAX_TYPES 160
static const FeatType *g_types[MAX_TYPES]; static int g_ntypes; static int g_registered;
void feature_register_type(const FeatType *t) { if (g_ntypes < MAX_TYPES) g_types[g_ntypes++] = t; }
const FeatType *feature_find_type(const char *name) {
    if (!strncmp(name, "minecraft:", 10)) name += 10;
    for (int i = 0; i < g_ntypes; i++) { const char *n = g_types[i]->name; if (!strncmp(n, "minecraft:", 10)) n += 10; if (!strcmp(n, name)) return g_types[i]; }
    return NULL;
}
/* группы фич регистрируют типы здесь (единственное место со списком) */
void feature_register_ore(void);        /* feature_ore.c */
void feature_register_blobs(void);      /* feature_disk.c: disk, block_blob, spring_feature, lake */
void feature_register_simple(void);     /* feature_misc.c: simple_block, random/weighted selectors … */
void feature_register_all(void) {
    if (g_registered) return;
    g_registered = 1;
    fm_init();
    feature_register_ore();
    feature_register_blobs();
    feature_register_simple();
}

/* ====================================================================== арена и разбор */
typedef struct FArena { struct FArena *next; size_t used, cap; } FArena;
void *fp_alloc(FParse *p, size_t n) {
    FWorld *fw = p->fw;
    n = (n + 15) & ~(size_t)15;
    FArena *a = fw->arena;
    if (!a || a->used + n > a->cap) {
        size_t cap = n > 65536 ? n : 65536;
        FArena *na = xcalloc(1, sizeof(FArena) + cap);
        na->cap = cap; na->next = a; fw->arena = a = na;
    }
    void *r = (char *)(a + 1) + a->used; a->used += n;
    memset(r, 0, n);
    return r;
}
char *fp_strdup(FParse *p, const char *s) { size_t n = strlen(s) + 1; char *r = fp_alloc(p, n); memcpy(r, s, n); return r; }
int fp_fail(FParse *p, const char *fmt, ...) {
    if (p->err[0]) return 0;
    va_list ap; va_start(ap, fmt); vsnprintf(p->err, sizeof p->err, fmt, ap); va_end(ap);
    return 0;
}
const Js *fp_cfg(const Js *f) { Js *c = js_get(f, "config"); return js_is_obj(c) ? c : f; }

static int dir_exists(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }

const Js *fp_load_json(FParse *p, const char *kind, const char *id) {
    FWorld *fw = p->fw;
    const char *name = id; const char *ns = "minecraft";
    char nsbuf[64];
    const char *colon = strchr(id, ':');
    if (colon) { size_t l = (size_t)(colon - id); if (l >= sizeof nsbuf) return NULL; memcpy(nsbuf, id, l); nsbuf[l] = 0; ns = nsbuf; name = colon + 1; }
    const char *dir = !strcmp(kind, "feature") ? fw->cfg_dir : kind;
    char *key = xsprintf("%s/%s:%s", dir, ns, name);
    JsDoc *d = sm_get(&fw->docs, key);
    if (!d) {
        char *path = xsprintf("%s/data/%s/worldgen/%s/%s.json", fw->g->pack, ns, dir, name);
        d = js_parse_file(path, NULL, 0);
        free(path);
        if (!d) { free(key); return NULL; }
        sm_put(&fw->docs, key, d);
    }
    free(key);
    return js_root(d);
}

/* ---- настроенные фичи ---- */
static Feat *feat_from_obj(FParse *p, const Js *obj, const char *id) {
    Feat *f = fp_alloc(p, sizeof *f);
    f->id = id ? fp_strdup(p, id) : NULL;
    const char *t = js_str(js_get(obj, "type"), NULL);
    if (!t) { fp_fail(p, "фича %s: нет type", id ? id : "<inline>"); return NULL; }
    f->type_name = fp_strdup(p, t);
    f->t = feature_find_type(t);
    if (!f->t) { p->fw->n_unimpl++; p->nunimpl++; return f; }   /* тип ещё не реализован: фича молча пропускается (занимает индекс в порядке) */
    char saved[256]; snprintf(saved, sizeof saved, "%s", p->err); p->err[0] = 0;
    int un0 = p->nunimpl;
    void *cfg = f->t->parse(p, fp_cfg(obj));
    if (!cfg) {
        if (p->fw->debug) fprintf(stderr, "libmcgen: фича %s (%s): %s\n", id ? id : "<inline>", t, p->err[0] ? p->err : "ошибка разбора");
        f->t = NULL; p->fw->n_unimpl++; p->nunimpl++;      /* не удалось разобрать: пропускаем как нереализованную */
        p->err[0] = 0; snprintf(p->err, sizeof p->err, "%s", saved);
        return f;
    }
    f->cfg = cfg;
    f->partial = p->nunimpl > un0;
    snprintf(p->err, sizeof p->err, "%s", saved);
    return f;
}

Feat *fp_feature(FParse *p, const Js *v) {
    if (js_is_str(v)) {
        Feat *c = sm_get(&p->fw->feat_cache, v->s);
        if (c) return c;
        const Js *d = fp_load_json(p, "feature", v->s);
        if (!d) { fp_fail(p, "нет фичи %s", v->s); return NULL; }
        Feat *f = feat_from_obj(p, d, v->s);
        if (f) sm_put(&p->fw->feat_cache, v->s, f);
        return f;
    }
    if (js_is_obj(v)) return feat_from_obj(p, v, NULL);
    fp_fail(p, "фича: ожидалась ссылка или объект");
    return NULL;
}

Placed *fp_placed(FParse *p, const Js *v) {
    if (js_is_str(v)) {
        Placed *c = sm_get(&p->fw->placed_cache, v->s);
        if (c) return c;
        const Js *d = fp_load_json(p, "placed_feature", v->s);
        if (!d) { fp_fail(p, "нет placed_feature %s", v->s); return NULL; }
        Placed *pf = fp_placed(p, d);
        if (pf) { pf->id = fp_strdup(p, v->s); sm_put(&p->fw->placed_cache, v->s, pf); }
        return pf;
    }
    if (!js_is_obj(v)) { fp_fail(p, "placed_feature: ожидалась ссылка или объект"); return NULL; }
    Placed *pf = fp_alloc(p, sizeof *pf);
    pf->index = -1;
    pf->feat = fp_feature(p, js_get(v, "feature")); if (!pf->feat) return NULL;
    if (!placement_parse_list(p, js_get(v, "placement"), &pf->mods, &pf->nmods)) return NULL;
    return pf;
}

/* ====================================================================== мир фич */
double features_clock(void) { return now_sec(); }

static void free_arena(FArena *a) { while (a) { FArena *n = a->next; free(a); a = n; } }
static void doc_free(void *p) { js_free(p); }
void features_world_free(McWorld *w) {
    FWorld *fw = w->features;
    if (!fw) return;
    sm_free(&fw->docs, doc_free); sm_free(&fw->feat_cache, NULL); sm_free(&fw->placed_cache, NULL); sm_free(&fw->bsp_cache, NULL);
    free(fw->placed);
    for (int s = 0; s < fw->nsteps; s++) free(fw->step_list ? fw->step_list[s] : NULL);
    free(fw->step_list); free(fw->step_n); free(fw->step_words);
    for (int b = 0; b < fw->nbiomes; b++) {
        if (fw->biome_step && fw->biome_step[b]) { for (int s = 0; s < fw->nsteps; s++) free(fw->biome_step[b][s]); free(fw->biome_step[b]); }
        if (fw->biome_has) free(fw->biome_has[b]);
    }
    free(fw->biome_step); free(fw->biome_has); free(fw->only);
    free_arena(fw->arena);
    free(fw);
    w->features = NULL;
}

/* биомы источника в порядке BiomeSource.possibleBiomes() (первое вхождение) */
static int source_biomes(McWorld *w, int *out, int cap) {
    const McGen *g = w->g; int n = 0;
    #define ADD(b) do { int bb_ = (b); int dup_ = 0; for (int q_ = 0; q_ < n; q_++) if (out[q_] == bb_) dup_ = 1; if (!dup_ && n < cap && bb_ >= 0) out[n++] = bb_; } while (0)
    switch (w->preset->biome_source) {
    case BS_MULTI_OVERWORLD: for (int i = 0; i < g->n_ow; i++) ADD(g->ow_points[i].biome); break;
    case BS_MULTI_NETHER: for (int i = 0; i < g->n_nether; i++) ADD(g->nether_points[i].biome); break;
    case BS_THE_END: {
        static const char *E[5] = { "minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands", "minecraft:small_end_islands", "minecraft:end_barrens" };
        for (int i = 0; i < 5; i++) ADD(gen_biome_id(g, E[i]));
        break;
    }
    default: ADD(w->preset->fixed_biome); break;
    }
    #undef ADD
    return n;
}

FWorld *features_world_get(McWorld *w) {
    mutex_lock(w->lock);
    if (w->features) { mutex_unlock(w->lock); return ((FWorld *)w->features)->ok ? w->features : NULL; }
    feature_register_all();
    FWorld *fw = xcalloc(1, sizeof *fw);
    fw->w = w; fw->g = w->g; fw->bs = bs_get(w->g); fw->version = w->g->version; fw->newf = w->g->newf;
    fw->debug = getenv("MCGEN_FEATURES_DEBUG") != NULL;
    char *d1 = xsprintf("%s/data/minecraft/worldgen/feature", w->g->pack);
    fw->cfg_dir = dir_exists(d1) ? "feature" : "configured_feature";
    free(d1);
    const char *only = getenv("MCGEN_FEATURES_ONLY");
    if (only && *only) fw->only = xstrdup(only);
    int src[256]; int nsrc = source_biomes(w, src, 256);
    w->features = fw;
    if (nsrc <= 0 || fworld_load(fw, src, nsrc) != 0 || fsort_build(fw, src, nsrc) != 0) {
        if (fw->debug || !fw->err[0]) fprintf(stderr, "libmcgen: стадия FEATURES недоступна: %s\n", fw->err[0] ? fw->err : "нет биомов");
        fw->ok = 0; mutex_unlock(w->lock); return NULL;
    }
    fw->ok = 1;
    const char *dump = getenv("MCGEN_FEATURES_DUMP_ORDER");     /* тест FeatureSorter: JSON {"steps":[[id,…],…]} */
    if (dump && *dump) {
        FILE *f = fopen(dump, "w");
        if (f) {
            fprintf(f, "{\"steps\":[");
            for (int st = 0; st < fw->nsteps; st++) {
                fprintf(f, "%s[", st ? "," : "");
                for (int k = 0; k < fw->step_n[st]; k++) fprintf(f, "%s\"%s\"", k ? "," : "", fw->placed[fw->step_list[st][k]]->id);
                fprintf(f, "]");
            }
            fprintf(f, "],\"placed\":[");
            for (int i = 0; i < fw->nplaced; i++) {
                const Placed *pf = fw->placed[i];
                fprintf(f, "%s{\"id\":\"%s\",\"type\":\"%s\",\"impl\":%d}", i ? "," : "", pf->id, pf->feat && pf->feat->type_name ? pf->feat->type_name : "?",
                        pf->feat && pf->feat->t ? (pf->feat->partial ? 2 : 1) : 0);
            }
            fprintf(f, "]}\n"); fclose(f);
        }
    }
    if (fw->debug) fprintf(stderr, "libmcgen: FEATURES: %d placed_feature, %d шагов, нереализованных (по типу/ошибке разбора): %d\n", fw->nplaced, fw->nsteps, fw->n_unimpl);
    mutex_unlock(w->lock);
    return fw;
}

/* ====================================================================== цикл декорации одного чанка */
static FeatStats g_stats; static double g_dec_secs;
void features_get_stats(FeatStats *out) { *out = g_stats; }

static void decorate_chunk(FCtx *c, int cx, int cz) {
    FWorld *fw = c->fw; McWorld *w = c->w;
    c->ccx = cx; c->ccz = cz; c->n_chunks++;
    FRnd rnd; memset(&rnd, 0, sizeof rnd); c->rnd = &rnd;
    int ox = cx * 16, oz = cz * 16, oy = c->min_y;
    i64 seed = w->seeds.features;
    /* WorldgenRandom.setDecorationSeed(seed, minBlockX, minBlockZ) */
    frnd_seed(&rnd, seed);
    u64 xs = (u64)frnd_long(&rnd) | 1ull, zs = (u64)frnd_long(&rnd) | 1ull;
    i64 dec = (i64)(((u64)(i64)ox * xs + (u64)(i64)oz * zs) ^ (u64)seed);
    frnd_seed(&rnd, dec);
    /* биомы в окне 3×3 чанков */
    u8 mask[32]; memset(mask, 0, sizeof mask);
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
        FChunk *ch = c->grid[(cz + dz - c->gz0) * c->gnx + (cx + dx - c->gx0)];
        for (int k = 0; k < 32; k++) mask[k] |= ch->bio_mask[k];
    }
    int pb[256], npb = 0;
    for (int b = 0; b < fw->nbiomes; b++) if (((mask[b >> 3] >> (b & 7)) & 1) && fw->biome_step[b]) pb[npb++] = b;
    int nsteps_total = fw->nsteps > 11 ? fw->nsteps : 11;
    for (int step = 0; step < nsteps_total; step++) {
        /* структуры шага — стадия STRUCTURES (не реализована) */
        if (step >= fw->nsteps) continue;
        int words = fw->step_words[step];
        u64 acc[16]; memset(acc, 0, sizeof acc);
        for (int i = 0; i < npb; i++) {
            const u64 *m = fw->biome_step[pb[i]][step];
            if (m) for (int k = 0; k < words; k++) acc[k] |= m[k];
        }
        for (int k = 0; k < words; k++) {
            u64 bits = acc[k];
            while (bits) {
                int bit = __builtin_ctzll(bits); bits &= bits - 1;
                int gi = k * 64 + bit;
                const Placed *pf = fw->placed[fw->step_list[step][gi]];
                frnd_seed(&rnd, dec + (i64)gi + (i64)(10000 * step));       /* setFeatureSeed */
                c->n_calls++;
                if (!pf->feat || !pf->feat->t) { c->n_skipped++; continue; }
                placed_place(c, pf, ox, oy, oz, 1);
            }
        }
    }
}

/* ====================================================================== окно чанков региона */
typedef struct RingJob {
    McWorld *w; uint32_t stages; int total, next; McMutex *lock; FChunk *chunks; int height; int fail; char err[256];
} RingJob;

static void ring_worker(void *arg) {
    RingJob *j = arg; McWorld *w = j->w;
    TerrainCtx *t = terrain_ctx_new(w);
    SCtx *bx = w->nc ? sctx_new(w->nc, 1) : NULL;
    SurfCtx *sc = (j->stages & MC_STAGE_SURFACE) ? surface_ctx_new(w) : NULL;
    for (;;) {
        mutex_lock(j->lock); int i = j->fail ? j->total : j->next++; mutex_unlock(j->lock);
        if (i >= j->total) break;
        FChunk *ch = &j->chunks[i];
        char e[256] = {0};
        if (bx) sctx_reset_caches(bx);
        world_chunk_biomes(w, bx, ch->cx, ch->cz, ch->biomes);
        int rc = terrain_fill_chunk(w, t, ch->cx, ch->cz, ch->blocks, e, sizeof e);
        if (!rc && sc) rc = surface_apply_chunk(w, sc, ch->cx, ch->cz, ch->blocks, ch->biomes, terrain_marks_rw(t), e, sizeof e);
        if (!rc && (j->stages & MC_STAGE_CARVERS)) rc = carvers_apply_chunk(w, t, ch->cx, ch->cz, ch->blocks, terrain_marks_rw(t), e, sizeof e);
        if (rc) { mutex_lock(j->lock); if (!j->fail) { j->fail = 1; snprintf(j->err, sizeof j->err, "%s", e); } mutex_unlock(j->lock); break; }
    }
    terrain_ctx_free(t);
    if (bx) sctx_free(bx);
    if (sc) surface_ctx_free(sc);
}

typedef struct PrimeJob { FCtx *c; FChunk **list; int n, next; McMutex *lock; } PrimeJob;
static void prime_worker(void *arg) {
    PrimeJob *j = arg;
    for (;;) {
        mutex_lock(j->lock); int i = j->next++; mutex_unlock(j->lock);
        if (i >= j->n) break;
        FChunk *ch = j->list[i]; FCtx *c = j->c;
        fchunk_prime_final(c->g, c->bs, ch, c->min_y, c->height, c->lazy_wg);
        int H4 = (c->height / 4) * 16;
        for (int k = 0; k < H4; k++) { int b = ch->biomes[k]; ch->bio_mask[b >> 3] |= (u8)(1 << (b & 7)); }
    }
}

typedef struct DecJob { FCtx *c; McMutex *lock; int cx0, cz0, nx, nz, t, next, count; long done, calls, skipped; } DecJob;
static void dec_worker(void *arg) {
    DecJob *j = arg;
    FCtx c = *j->c;                       /* копия контекста потока: центр чанка и ГСЧ — свои */
    for (;;) {
        mutex_lock(j->lock); int k = j->next++; mutex_unlock(j->lock);
        if (k >= j->count) break;
        /* k-й чанк волны t: перебор iz по возрастанию (ix = t − 3·iz внутри [0, nx)) */
        int iz = 0, ix = 0, seen = -1;
        for (iz = 0; iz < j->nz; iz++) { ix = j->t - 3 * iz; if (ix >= 0 && ix < j->nx && ++seen == k) break; }
        decorate_chunk(&c, j->cx0 + ix, j->cz0 + iz);
    }
    mutex_lock(j->lock); j->done += c.n_chunks; j->calls += c.n_calls; j->skipped += c.n_skipped; mutex_unlock(j->lock);
}

int features_apply_region(McWorld *w, McRegion *r, int threads, McProgressFn cb, void *ud, char *err, size_t errlen) {
    FWorld *fw = features_world_get(w);
    if (!fw) return 0;                              /* стадия недоступна: регион остаётся без декораций */
    double t0 = now_sec();
    McRegionInfo info; mcgen_region_info(r, &info);
    uint32_t stages = region_stages(r);
    int gx0 = info.cx0 - 2, gz0 = info.cz0 - 2, gnx = info.nx + 4, gnz = info.nz + 4;
    size_t H = (size_t)w->height;
    FChunk *chunks = xcalloc((size_t)gnx * gnz, sizeof(FChunk));
    FChunk **grid = xcalloc((size_t)gnx * gnz, sizeof(FChunk *));
    int nring = 0;
    for (int iz = 0; iz < gnz; iz++) for (int ix = 0; ix < gnx; ix++) {
        FChunk *ch = &chunks[iz * gnx + ix]; grid[iz * gnx + ix] = ch;
        ch->cx = gx0 + ix; ch->cz = gz0 + iz;
        uint16_t *b = mcgen_region_blocks(r, ch->cx, ch->cz);
        if (b) { ch->blocks = b; ch->biomes = mcgen_region_biomes(r, ch->cx, ch->cz); ch->marks = region_chunk_marks(r, ch->cx, ch->cz); }
        else {
            ch->blocks = xmalloc(sizeof(uint16_t) * H * 256); ch->biomes = xcalloc((H / 4) * 16, 1); ch->marks = NULL; nring++;
        }
    }
    int nt = threads > 0 ? threads : cpu_count();
    /* внешние кольца: BIOMES → TERRAIN → SURFACE → CARVERS по запрошенным стадиям */
    int rc = 0; char e[256] = {0};
    if (nring > 0) {
        FChunk *rl = xcalloc((size_t)nring, sizeof(FChunk)); int k = 0;
        for (int i = 0; i < gnx * gnz; i++) if (!chunks[i].marks) rl[k++] = chunks[i];     /* копии указателей (блоки/биомы общие) */
        /* chunks[i].marks == NULL только у колец (у региона marks всегда не NULL) */
        RingJob j; memset(&j, 0, sizeof j);
        j.w = w; j.stages = stages; j.total = nring; j.lock = mutex_new(); j.chunks = rl; j.height = (int)H;
        int nth = nt < nring ? nt : nring; if (nth < 1) nth = 1;
        if (nth == 1) ring_worker(&j);
        else {
            McThread **th = xcalloc((size_t)nth, sizeof(McThread *));
            for (int i = 0; i < nth; i++) th[i] = thread_start(ring_worker, &j);
            for (int i = 0; i < nth; i++) thread_join(th[i]);
            free(th);
        }
        mutex_free(j.lock); free(rl);
        if (j.fail) { snprintf(e, sizeof e, "%s", j.err); rc = -1; }
    }
    if (!rc) {
        FCtx c; memset(&c, 0, sizeof c);
        c.w = w; c.g = w->g; c.bs = fw->bs; c.fw = fw;
        c.grid = grid; c.gx0 = gx0; c.gz0 = gz0; c.gnx = gnx; c.gnz = gnz;
        c.min_y = w->min_y; c.height = w->height; c.sea_level = w->sea_level;
        c.gen_min_y = w->ns->min_y > w->min_y ? w->ns->min_y : w->min_y;
        c.gen_depth = w->ns->height < w->height ? w->ns->height : w->height;
        c.lazy_wg = w->g->newf;
        c.st_air = w->g->st_air; c.st_cave_air = w->g->st_cave_air; c.st_void_air = fw->bs->st_void_air; c.st_water = w->g->st_water; c.st_lava = w->g->st_lava;
        c.bedrock_blk = bs_block_index(fw->bs, "minecraft:bedrock"); c.plains = gen_biome_id(w->g, "minecraft:plains");
        if (c.plains < 0) c.plains = 0;
        /* карты высот и биомы окна (параллельно) */
        PrimeJob pj; memset(&pj, 0, sizeof pj);
        pj.c = &c; pj.list = grid; pj.n = gnx * gnz; pj.lock = mutex_new();
        int nth = nt < pj.n ? nt : pj.n; if (nth < 1) nth = 1;
        if (nth == 1) prime_worker(&pj);
        else { McThread **th = xcalloc((size_t)nth, sizeof(McThread *)); for (int i = 0; i < nth; i++) th[i] = thread_start(prime_worker, &pj); for (int i = 0; i < nth; i++) thread_join(th[i]); free(th); }
        mutex_free(pj.lock);
        /* декорация: чанки региона и кольцо вокруг него (их фичи заходят в регион); порядок — построчно (z, затем x), параллельно волнами.
         * MCGEN_FEATURES_SEQ=<zx|xz|zx-|xz-|ring> — последовательный обход другим порядком (эксперимент). */
        int total = (info.nx + 2) * (info.nz + 2);
        double t_dec0 = now_sec();
        DecJob dj; memset(&dj, 0, sizeof dj);
        dj.c = &c; dj.lock = mutex_new(); dj.cx0 = info.cx0 - 1; dj.cz0 = info.cz0 - 1; dj.nx = info.nx + 2; dj.nz = info.nz + 2;
        int seq = getenv("MCGEN_FEATURES_SEQ") != NULL;
        if (seq) {
            const char *ord = getenv("MCGEN_FEATURES_SEQ");           /* порядок обхода (эксперимент): zx (по умолчанию), xz, zx-, xz-, ring */
            if (!strcmp(ord, "xz")) { for (int cx = dj.cx0; cx < dj.cx0 + dj.nx; cx++) for (int cz = dj.cz0; cz < dj.cz0 + dj.nz; cz++) decorate_chunk(&c, cx, cz); }
            else if (!strcmp(ord, "zx-")) { for (int cz = dj.cz0 + dj.nz - 1; cz >= dj.cz0; cz--) for (int cx = dj.cx0 + dj.nx - 1; cx >= dj.cx0; cx--) decorate_chunk(&c, cx, cz); }
            else if (!strcmp(ord, "xz-")) { for (int cx = dj.cx0 + dj.nx - 1; cx >= dj.cx0; cx--) for (int cz = dj.cz0 + dj.nz - 1; cz >= dj.cz0; cz--) decorate_chunk(&c, cx, cz); }
            else if (!strcmp(ord, "ring")) {      /* от центра наружу по кольцам */
                int mx = dj.cx0 + dj.nx / 2, mz = dj.cz0 + dj.nz / 2, rmax = dj.nx > dj.nz ? dj.nx : dj.nz;
                for (int rr = 0; rr <= rmax; rr++) for (int cz = dj.cz0; cz < dj.cz0 + dj.nz; cz++) for (int cx = dj.cx0; cx < dj.cx0 + dj.nx; cx++) {
                    int d = abs(cx - mx) > abs(cz - mz) ? abs(cx - mx) : abs(cz - mz);
                    if (d == rr) decorate_chunk(&c, cx, cz);
                }
            }
            else for (int cz = dj.cz0; cz < dj.cz0 + dj.nz; cz++) for (int cx = dj.cx0; cx < dj.cx0 + dj.nx; cx++) decorate_chunk(&c, cx, cz);
            g_stats.chunks += c.n_chunks; g_stats.placed_calls += c.n_calls; g_stats.unimpl_skipped += c.n_skipped;
        } else {
            /* волновой фронт: чанки с равным t = ix + 3·iz не пересекаются окнами 3×3 (|dx| ≥ 3), а все пересекающиеся «более ранние» чанки
             * порядка (z, затем x) имеют меньшее t — результат идентичен последовательному обходу при любом числе потоков */
            int tmax = (dj.nx - 1) + 3 * (dj.nz - 1);
            for (int t = 0; t <= tmax && !rc; t++) {
                dj.t = t; dj.next = 0; dj.count = 0;
                for (int iz = 0; iz < dj.nz; iz++) { int ix = t - 3 * iz; if (ix >= 0 && ix < dj.nx) dj.count++; }
                if (dj.count == 0) continue;
                int nthr = nt < dj.count ? nt : dj.count; if (nthr < 1) nthr = 1;
                if (nthr == 1) dec_worker(&dj);
                else { McThread **th = xcalloc((size_t)nthr, sizeof(McThread *)); for (int i = 0; i < nthr; i++) th[i] = thread_start(dec_worker, &dj); for (int i = 0; i < nthr; i++) thread_join(th[i]); free(th); }
                if ((t & 31) == 0 && cb && cb(ud, 0.9 + 0.08 * t / (tmax + 1.0), "features")) { snprintf(e, sizeof e, "отменено"); rc = 1; }
            }
            g_stats.chunks += dj.done; g_stats.placed_calls += dj.calls; g_stats.unimpl_skipped += dj.skipped;
        }
        mutex_free(dj.lock); (void)total;
        g_dec_secs = now_sec() - t_dec0;
    }
    for (int i = 0; i < gnx * gnz; i++) if (!chunks[i].marks) { free(chunks[i].blocks); free(chunks[i].biomes); }
    free(chunks); free(grid);
    g_stats.secs += now_sec() - t0;
    if (fw->debug) fprintf(stderr, "\nlibmcgen: FEATURES: чанков декорировано %d, всего %.2f с (из них декорация %.2f с = %.0f чанков/с, остальное — кольца и карты высот), вызовов placed_feature %ld\n", (info.nx + 2) * (info.nz + 2), now_sec() - t0, g_dec_secs, (info.nx + 2) * (info.nz + 2) / (g_dec_secs > 0 ? g_dec_secs : 1e-9), g_stats.placed_calls);
    if (rc) { set_err(err, errlen, "%s", e); return rc; }
    return 0;
}

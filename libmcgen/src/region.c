/* region.c — генерация области чанков (пул потоков), доступ к данным, карты высот, дамп MCR1, предпросмотр биомов. */
#include "mcgen_internal.h"
#include "mcgen_test.h"
#include "fluidpp.h"
#include "schedule.h"
#include "carver.h"
#include "surface.h"
#include "feature.h"
#include "gpu_bridge.h"
void structures_begin_region(McWorld *w);   /* structure.c */
void structure_shape_update(void *fw, int x, int y, int z);   /* structure_post.c */
void structure_neighbors_update(void *fw, int x, int y, int z);
void features_post_chunk(void *fw, McWorld *w, int cx, int cz);  /* feature_miscx.c: пузырьковые столбцы над магмой/песком душ */
#include "mcgen_tweaks_table.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif

struct McRegion {
    McRegionInfo info;
    uint32_t stages;
    uint16_t *blocks;      /* nx*nz*height*256 */
    uint8_t *biomes;       /* nx*nz*(height/4)*16 */
    int16_t *hm;           /* nx*nz*4*256 */
    PPMarks *marks;        /* пометки пост-обработки жидкостей по чанкам */
    char *bents;           /* блок-сущности построек (баннеры с узорами) в области — JSON-массив (malloc), см. mcgen_region_block_entities */
};

static int chunk_index(const McRegion *r, int cx, int cz);

/* Освобождает физическую память страниц массива блоков, целиком заполненных нулями: воздух над поверхностью — это больше половины региона (весь мир −64…320, поверхность
 * около 63–150), а стадии генерации записывают воздух явно и «прикладывают» страницы. Содержимое не меняется (читаются те же нули: Linux отдаёт нулевую страницу по
 * требованию; где MADV_DONTNEED лишь подсказка — данные остаются нулями так или иначе); запись правкой снова выделяет страницу. Только если воздух = id 0.
 * Linux/macOS; на Windows ничего не делается. MCGEN_NO_TRIM=1 отключает (для сверки). */
static void region_trim_zero_pages(void *base, size_t bytes) {
#if defined(__linux__) || defined(__APPLE__)
    if (getenv("MCGEN_NO_TRIM")) return;
    long psz = sysconf(_SC_PAGESIZE);
    if (psz < 4096 || (psz & (psz - 1))) return;
    uintptr_t a = ((uintptr_t)base + (uintptr_t)psz - 1) & ~((uintptr_t)psz - 1), e = ((uintptr_t)base + bytes) & ~((uintptr_t)psz - 1);
    uintptr_t run = 0;
    for (uintptr_t p = a; p + (uintptr_t)psz <= e; p += (uintptr_t)psz) {
        const uint64_t *q = (const uint64_t *)p;
        size_t k = 0, nw = (size_t)psz / 8;
        while (k < nw && q[k] == 0) k++;
        if (k == nw) { if (!run) run = p; }
        else if (run) { madvise((void *)run, p - run, MADV_DONTNEED); run = 0; }
    }
    if (run) madvise((void *)run, (e - run) / (uintptr_t)psz * (uintptr_t)psz, MADV_DONTNEED);
#else
    (void)base; (void)bytes;
#endif
}

/* ---------------- зум биомов (BiomeManager.getBiome) ---------------- */
static inline i64 zoom_lcg(i64 r, i64 c) { u64 v = (u64)r; v *= v * 6364136223846793005ULL + 1442695040888963407ULL; return (i64)(v + (u64)c); }
static inline double fiddle(i64 r) { i64 m = (r >> 24) % 1024; if (m < 0) m += 1024; return ((double)m / 1024.0 - 0.5) * 0.9; }
static double fiddled_distance(i64 seed, int x, int y, int z, double dx, double dy, double dz) {
    i64 r = seed;
    r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z); r = zoom_lcg(r, x); r = zoom_lcg(r, y); r = zoom_lcg(r, z);
    double fx = fiddle(r); r = zoom_lcg(r, seed);
    double fy = fiddle(r); r = zoom_lcg(r, seed);
    double fz = fiddle(r);
    return (dz + fz) * (dz + fz) + (dy + fy) * (dy + fy) + (dx + fx) * (dx + fx);
}
int mcgen_biome_at(const McWorld *w, int x, int y, int z) {
    if (!w) return -1;
    int ax = x - 2, ay = y - 2, az = z - 2;
    int px = ax >> 2, py = ay >> 2, pz = az >> 2;
    double fx = (ax & 3) / 4.0, fy = (ay & 3) / 4.0, fz = (az & 3) / 4.0;
    int mi = 0; double md = INFINITY;
    for (int i = 0; i < 8; i++) {
        int xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        double d = fiddled_distance(w->biome_zoom_seed, xe ? px : px + 1, ye ? py : py + 1, ze ? pz : pz + 1,
                                    xe ? fx : fx - 1.0, ye ? fy : fy - 1.0, ze ? fz : fz - 1.0);
        if (md > d) { mi = i; md = d; }
    }
    return world_biome_cell(w, (mi & 4) == 0 ? px : px + 1, (mi & 2) == 0 ? py : py + 1, (mi & 1) == 0 ? pz : pz + 1);
}
/* сетка биомов: строки раздаются потокам (McWorld только читается, контексты шумов — свои у потока) */
typedef struct { const McWorld *w; int x0, z0, nx, nz, step, y; uint8_t *out; McMutex *lock; int next; } GridJob;
static void grid_rows(GridJob *j, int iz) {
    for (int ix = 0; ix < j->nx; ix++) {
        int b = mcgen_biome_at(j->w, j->x0 + ix * j->step, j->y, j->z0 + iz * j->step);
        j->out[(size_t)iz * j->nx + ix] = (uint8_t)(b < 0 ? 0 : b);
    }
}
static void grid_worker(void *arg) {
    GridJob *j = arg;
    for (;;) {
        mutex_lock(j->lock); int iz = j->next++; mutex_unlock(j->lock);
        if (iz >= j->nz) return;
        grid_rows(j, iz);
    }
}
/* сетка биомов: GPU (libmcgen_cuda, если включена и доступна; результат ≡ CPU побитно), иначе CPU */
int gpu_try_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out, int force);   /* gpu_bridge.c */
int mcgen_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out) {
    if (!w || !out || nx <= 0 || nz <= 0 || step <= 0) return MCGEN_E_ARG;
    if (gpu_try_biome_grid(w, x0, z0, nx, nz, step, y, out, 0) == 0) return MCGEN_OK;
    return mcgen_biome_grid_cpu(w, x0, z0, nx, nz, step, y, out);
}
int mcgen_biome_grid_cpu(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out) {
    if (!w || !out || nx <= 0 || nz <= 0 || step <= 0) return MCGEN_E_ARG;
    GridJob j = { w, x0, z0, nx, nz, step, y, out, NULL, 0 };
    int nt = cpu_count();
    if ((long)nx * nz < 4096 || nt <= 1 || nz < 2) { for (int iz = 0; iz < nz; iz++) grid_rows(&j, iz); return MCGEN_OK; }
    if (nt > nz) nt = nz;
    if (nt > 64) nt = 64;
    j.lock = mutex_new();
    McThread *th[64];
    for (int i = 0; i < nt; i++) th[i] = thread_start(grid_worker, &j);
    for (int i = 0; i < nt; i++) thread_join(th[i]);
    mutex_free(j.lock);
    return MCGEN_OK;
}
int mcgen_x_noise_biomes(const McWorld *w, int qx0, int qz0, int nx, int nz, int qy, uint8_t *out) {
    for (int iz = 0; iz < nz; iz++) for (int ix = 0; ix < nx; ix++) out[iz * nx + ix] = (uint8_t)world_biome_noise(w, qx0 + ix, qy, qz0 + iz);
    return 0;
}
int mcgen_x_is_float(const McWorld *w) { return w->g->newf; }
int mcgen_x_df_eval(McWorld *w, const char *id, int n, const int *xyz, double *out, char *err, size_t errlen) {
    return world_df_point(w, id, n, xyz, out, err, errlen);
}
int mcgen_x_fill_chunk(McWorld *w, int cx, int cz, uint16_t *blocks, char *err, size_t errlen) {
    TerrainCtx *t = terrain_ctx_new(w);
    int rc = terrain_fill_chunk(w, t, cx, cz, blocks, err, errlen);
    terrain_ctx_free(t);
    return rc;
}

/* ---------------- классификация состояний для карт высот ----------------
 * Heightmap.Types: WORLD_SURFACE — не воздух; OCEAN_FLOOR — blocksMotion; MOTION_BLOCKING — blocksMotion или жидкость;
 * MOTION_BLOCKING_NO_LEAVES — то же без листвы. Для стадий TERRAIN (камень/вода/лава/воздух) этого достаточно;
 * точная таблица blocksMotion для всех блоков — у стадий декораций (здесь — эвристика по имени). */
enum { CL_AIR = 1, CL_MOTION = 2, CL_FLUID = 4, CL_LEAVES = 8 };
void gen_compute_state_classes(McGen *g) {
    u8 *c = xcalloc((size_t)g->nstates, 1);
    static const char *NONSOLID[] = { "grass", "fern", "flower", "sapling", "torch", "vine", "snow[", "carpet", "button", "lever", "rail", "sign", "banner",
        "mushroom[", "brown_mushroom", "red_mushroom", "dead_bush", "sugar_cane", "kelp", "seagrass", "coral_fan", "sea_pickle", "dandelion", "poppy",
        "orchid", "allium", "azure_bluet", "tulip", "oxeye", "cornflower", "lily_of_the_valley", "wither_rose", "sweet_berry", "cobweb", "pressure_plate",
        "tripwire", "redstone_wire", "nether_sprouts", "roots", "fungus", "glow_lichen", "hanging_roots", "spore_blossom", "small_dripleaf", "pink_petals",
        "light[", "structure_void", "short_", "tall_", "bush", "firefly", "leaf_litter", "wildflowers", "cactus_flower", "pale_hanging_moss", "eyeblossom", NULL };
    for (int i = 0; i < g->nstates; i++) {
        const char *n = g->state_names[i];
        const char *b = strchr(n, ':') ? strchr(n, ':') + 1 : n;
        if (!strncmp(b, "air", 3) || !strncmp(b, "cave_air", 8) || !strncmp(b, "void_air", 8)) { c[i] = CL_AIR; continue; }
        int fluid = !strncmp(b, "water", 5) || !strncmp(b, "lava", 4) || !strncmp(b, "bubble_column", 13) || strstr(n, "waterlogged=true");
        int motion = !(!strncmp(b, "water", 5) || !strncmp(b, "lava", 4) || !strncmp(b, "bubble_column", 13));
        for (int k = 0; NONSOLID[k] && motion; k++) if (strstr(b, NONSOLID[k])) motion = 0;
        if (!strncmp(b, "grass_block", 11) || !strncmp(b, "flowering_azalea", 16) || !strncmp(b, "azalea_leaves", 13)) motion = 1;   /* «grass»/«flower» — подстроки не про них */
        if (!strncmp(b, "powder_snow", 11) && strncmp(b, "powder_snow_cauldron", 20)) motion = 0;                         /* пустая форма коллизии */
        c[i] = (u8)((motion ? CL_MOTION : 0) | (fluid ? CL_FLUID : 0) | (strstr(b, "_leaves") ? CL_LEAVES : 0));
    }
    /* 26.3+: Heightmap.Types берёт состав из тегов датапака (blocks_motion_in_heightmap[_no_leaves]) — точнее эвристики по именам */
    {
        const u8 *tm = gen_block_tag(g, "minecraft:blocks_motion_in_heightmap"), *tn = gen_block_tag(g, "minecraft:blocks_motion_in_heightmap_no_leaves");
        int any = 0;
        for (int b = 0; b < g->nblocks && !any; b++) any = tm[b];
        if (any) for (int i = 0; i < g->nstates; i++) {
            if (c[i] & CL_AIR) continue;
            int b = g->state_block[i];
            c[i] = (u8)((c[i] & CL_FLUID) | (tm[b] ? CL_MOTION : 0) | (tm[b] && !tn[b] ? CL_LEAVES : 0));
        }
    }
    g->state_cls = c;
}
static void compute_heightmaps(const McGen *g, const u8 *cls, const uint16_t *blk, int min_y, int height, int16_t *hm) {
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
        int found[4] = {0, 0, 0, 0}; int16_t v[4];
        for (int k = 0; k < 4; k++) v[k] = (int16_t)min_y;
        for (int y = height - 1; y >= 0; y--) {
            uint16_t s = blk[((size_t)y * 16 + z) * 16 + x];
            u8 c = s < g->nstates ? cls[s] : 0;
            int pred[4] = { !(c & CL_AIR), (c & CL_MOTION) != 0, (c & (CL_MOTION | CL_FLUID)) != 0, ((c & (CL_MOTION | CL_FLUID)) != 0) && !(c & CL_LEAVES) };
            int done = 1;
            for (int k = 0; k < 4; k++) { if (!found[k] && pred[k]) { found[k] = 1; v[k] = (int16_t)(min_y + y + 1); } if (!found[k]) done = 0; }
            if (done) break;
        }
        for (int k = 0; k < 4; k++) hm[k * 256 + z * 16 + x] = v[k];
    }
}

/* ---------------- пул потоков ---------------- */
typedef struct {
    McWorld *w; McRegion *r; uint32_t stages;
    int next, done, total, cancel, failed;
    McMutex *lock;
    McProgressFn cb; void *ud;
    char err[512];
    const u8 *cls;
} Job;

static void worker(void *arg) {
    Job *j = arg;
    McWorld *w = j->w; McRegion *r = j->r;
    TerrainCtx *t = (j->stages & ~MC_STAGE_BIOMES) ? terrain_ctx_new(w) : NULL;
    SCtx *bx = w->nc ? sctx_new(w->nc, 1) : NULL;
    SurfCtx *sc = (j->stages & MC_STAGE_SURFACE) ? surface_ctx_new(w) : NULL;   /* стадия SURFACE (surface.c) */
    int H = r->info.height;
    size_t bsz = (size_t)H * 256, isz = (size_t)(H / 4) * 16;
    for (;;) {
        mutex_lock(j->lock);
        int i = (j->cancel || j->failed) ? j->total : j->next++;
        mutex_unlock(j->lock);
        if (i >= j->total) break;
        int cx = r->info.cx0 + i % r->info.nx, cz = r->info.cz0 + i / r->info.nx;
        uint16_t *blk = r->blocks + bsz * (size_t)i;
        uint8_t *bio = r->biomes + isz * (size_t)i;
        if (j->stages & MC_STAGE_BIOMES) { if (bx) sctx_reset_caches(bx); world_chunk_biomes(w, bx, cx, cz, bio); }
        if (j->stages & MC_STAGE_TERRAIN) {
            char e[512] = {0};
            if (terrain_fill_chunk(w, t, cx, cz, blk, e, sizeof e)) {
                mutex_lock(j->lock); if (!j->failed) { j->failed = 1; snprintf(j->err, sizeof j->err, "%s", e); } mutex_unlock(j->lock);
                break;
            }
            if (sc && surface_apply_chunk_ex(w, sc, cx, cz, blk, bio, terrain_marks_rw(t), t, (j->stages & MC_STAGE_CARVERS) != 0, e, sizeof e)) {
                mutex_lock(j->lock); if (!j->failed) { j->failed = 1; snprintf(j->err, sizeof j->err, "%s", e); } mutex_unlock(j->lock);
                break;
            }
            if ((j->stages & MC_STAGE_CARVERS) && !(sc && surface_carves_inside(w))) {   /* после TERRAIN (и SURFACE) на том же контексте: aquifer и кэши чанка (carver.h); 26.4 с SURFACE — карвинг внутри прохода поверхности (surface.c) */
                if (carvers_apply_chunk(w, t, cx, cz, blk, terrain_marks_rw(t), e, sizeof e)) {
                    mutex_lock(j->lock); if (!j->failed) { j->failed = 1; snprintf(j->err, sizeof j->err, "%s", e); } mutex_unlock(j->lock);
                    break;
                }
            }
            ppmarks_copy(&r->marks[i], terrain_marks(t));
        }
        mutex_lock(j->lock);
        j->done++;
        if (j->cb && (j->done == j->total || j->done * 100 / j->total != (j->done - 1) * 100 / j->total)) {
            /* what = "<стадия> <готово>/<всего>" (стадия — старшая из выполняемых в проходе по чанкам) */
            char what[64]; snprintf(what, sizeof what, "%s %d/%d", (j->stages & MC_STAGE_TERRAIN) ? "terrain" : "biomes", j->done, j->total);
            if (j->cb(j->ud, (j->stages & MC_STAGE_TERRAIN ? 0.9 : 0.99) * j->done / j->total, what)) j->cancel = 1;
        }
        mutex_unlock(j->lock);
    }
    if (t) terrain_ctx_free(t);
    if (bx) sctx_free(bx);
    if (sc) surface_ctx_free(sc);
}

/* ---------------- пост-обработка жидкостей по региону (LevelChunk.postProcessGeneration) ----------------
 * Блоки соседних чанков вне региона («гало») при необходимости генерируются лениво (TERRAIN); пометки гало-чанков,
 * лежащие у границы региона (могут растечься внутрь), тоже обрабатываются. Порядок — по чанкам (cz, затем cx). */
typedef struct HaloChunk { int cx, cz; uint16_t *blocks; PPMarks marks; struct HaloChunk *next; } HaloChunk;
typedef struct { McWorld *w; McRegion *r; HaloChunk *halo[256]; TerrainCtx *t; int fail; char err[256]; SurfCtx *sc; SCtx *bx; } View;
/* Гало-чанк целиком (TERRAIN → SURFACE → CARVERS на переданных контекстах потока): блоки + пометки. 0 — успех, иначе текст ошибки в e */
static int halo_generate(McWorld *w, McRegion *r, TerrainCtx *t, SurfCtx **sc, SCtx **bx, int cx, int cz, HaloChunk *h, char *e, size_t el) {
    h->blocks = xmalloc(sizeof(uint16_t) * (size_t)r->info.height * 256);
    if (terrain_fill_chunk(w, t, cx, cz, h->blocks, e, el)) return 1;
    if (r->stages & MC_STAGE_SURFACE) {
        /* SURFACE для гало-чанка: биомы чанка стадией BIOMES (пакетно, как у чанков региона), затем поверхность */
        if (!*sc) *sc = surface_ctx_new(w);
        if (!*bx && w->nc) *bx = sctx_new(w->nc, 1);
        uint8_t *hb = xmalloc((size_t)(w->height / 4) * 16);
        if (*bx) sctx_reset_caches(*bx);
        world_chunk_biomes(w, *bx, cx, cz, hb);
        int rc = surface_apply_chunk_ex(w, *sc, cx, cz, h->blocks, hb, terrain_marks_rw(t), t, (r->stages & MC_STAGE_CARVERS) != 0, e, el);
        free(hb);
        if (rc) return 1;
    }
    if ((r->stages & MC_STAGE_CARVERS) && !((r->stages & MC_STAGE_SURFACE) && surface_carves_inside(w)) && carvers_apply_chunk(w, t, cx, cz, h->blocks, terrain_marks_rw(t), e, el)) return 1;
    ppmarks_copy(&h->marks, terrain_marks(t));
    return 0;
}
static void halo_insert(View *v, HaloChunk *h) { int b = ((h->cx * 31 + h->cz) & 255); h->next = v->halo[b]; v->halo[b] = h; }
static uint16_t *view_chunk(View *v, int cx, int cz, PPMarks **marks) {
    McRegion *r = v->r;
    int i = chunk_index(r, cx, cz);
    if (i >= 0) { if (marks) *marks = &r->marks[i]; return r->blocks + (size_t)i * r->info.height * 256; }
    int b = ((cx * 31 + cz) & 255);
    for (HaloChunk *h = v->halo[b]; h; h = h->next) if (h->cx == cx && h->cz == cz) { if (marks) *marks = &h->marks; return h->blocks; }
    HaloChunk *h = xcalloc(1, sizeof *h);
    h->cx = cx; h->cz = cz;
    if (!v->t) v->t = terrain_ctx_new(v->w);
    char e[256] = {0};
    if (halo_generate(v->w, r, v->t, &v->sc, &v->bx, cx, cz, h, e, sizeof e)) { v->fail = 1; snprintf(v->err, sizeof v->err, "%s", e); }
    halo_insert(v, h);
    if (marks) *marks = &h->marks;
    return h->blocks;
}
static int view_get(void *ud, int x, int y, int z) {
    View *v = ud;
    int ly = y - v->w->min_y;
    if (ly < 0 || ly >= v->w->height) return v->w->g->st_air;   /* за пределами мира — void_air */
    uint16_t *b = view_chunk(v, x >> 4, z >> 4, NULL);
    return b[((size_t)ly * 16 + (z & 15)) * 16 + (x & 15)];
}
static void view_set(void *ud, int x, int y, int z, int st) {
    View *v = ud;
    int ly = y - v->w->min_y;
    if (ly < 0 || ly >= v->w->height) return;
    uint16_t *b = view_chunk(v, x >> 4, z >> 4, NULL);
    b[((size_t)ly * 16 + (z & 15)) * 16 + (x & 15)] = (uint16_t)st;
}
/* Кольцо гало (чанки на расстоянии 1 от региона) нужно растеканию жидкостей целиком — раньше оно генерировалось лениво одним потоком (для области 8×8 это 36 чанков
 * полного конвейера против 64 у региона: ≈ 75 % времени всей генерации). Теперь кольцо считается заранее всеми потоками; результат тот же (чанк детерминирован). */
typedef struct { View *v; int *cx, *cz; int n, next, done, cancel, fail; McMutex *lock; McProgressFn cb; void *ud; char err[256]; } HaloJob;
static void halo_worker(void *arg) {
    HaloJob *j = arg; View *v = j->v; McWorld *w = v->w;
    TerrainCtx *t = terrain_ctx_new(w); SurfCtx *sc = NULL; SCtx *bx = NULL;
    for (;;) {
        mutex_lock(j->lock);
        int k = (j->cancel || j->fail) ? j->n : j->next++;
        mutex_unlock(j->lock);
        if (k >= j->n) break;
        HaloChunk *h = xcalloc(1, sizeof *h); h->cx = j->cx[k]; h->cz = j->cz[k];
        char e[256] = {0};
        int bad = halo_generate(w, v->r, t, &sc, &bx, h->cx, h->cz, h, e, sizeof e);
        mutex_lock(j->lock);
        if (bad) { if (!j->fail) { j->fail = 1; snprintf(j->err, sizeof j->err, "%s", e); } free(h->blocks); free(h); }
        else {
            halo_insert(v, h);
            j->done++;
            if (j->cb) { char what[64]; snprintf(what, sizeof what, "fluids halo %d/%d", j->done, j->n); if (j->cb(j->ud, 0.95 + 0.01 * j->done / j->n, what)) j->cancel = 1; }
        }
        mutex_unlock(j->lock);
    }
    if (t) terrain_ctx_free(t);
    if (sc) surface_ctx_free(sc);
    if (bx) sctx_free(bx);
}
static int halo_pregenerate(View *v, int threads, McProgressFn cb, void *ud) {
    McRegion *r = v->r;
    int cx0 = r->info.cx0, cz0 = r->info.cz0, nx = r->info.nx, nz = r->info.nz;
    int cap = 2 * (nx + nz) + 8, n = 0;
    int *lx = xmalloc((size_t)cap * sizeof(int)), *lz = xmalloc((size_t)cap * sizeof(int));
    for (int cz = cz0 - 1; cz <= cz0 + nz; cz++) for (int cx = cx0 - 1; cx <= cx0 + nx; cx++) {
        if (cx >= cx0 && cx < cx0 + nx && cz >= cz0 && cz < cz0 + nz) continue;
        if (n < cap) { lx[n] = cx; lz[n] = cz; n++; }
    }
    HaloJob j; memset(&j, 0, sizeof j);
    j.v = v; j.cx = lx; j.cz = lz; j.n = n; j.lock = mutex_new(); j.cb = cb; j.ud = ud;
    int nt = threads > 0 ? threads : cpu_count();
    if (nt > n) nt = n;
    if (nt <= 1) halo_worker(&j);
    else {
        McThread **th = xcalloc((size_t)nt, sizeof(McThread *));
        for (int i = 0; i < nt; i++) th[i] = thread_start(halo_worker, &j);
        for (int i = 0; i < nt; i++) thread_join(th[i]);
        free(th);
    }
    mutex_free(j.lock); free(lx); free(lz);
    if (j.fail) { v->fail = 1; snprintf(v->err, sizeof v->err, "%s", j.err); }
    return j.cancel ? 1 : 0;
}
/* pp_margin < 0 — бесконечный мир: обрабатываются все чанки региона и пометки гало у границы (в игре все чанки рано или
 * поздно становятся «тикающими»); pp_margin = K >= 0 — только чанки на расстоянии >= K от края региона, без гало.
 * Так воспроизводится загруженная игрой область: postProcessGeneration вызывается в ChunkMap.prepareTickingChunk, т. е.
 * только для чанков уровня BLOCK_TICKING (все 8 соседей — FULL); внешнее кольцо FULL-чанков его не проходит. */
static int region_postprocess(McWorld *w, McRegion *r, int pp_margin, int threads, McProgressFn cb, void *ud, char *err, size_t errlen) {
    View v; memset(&v, 0, sizeof v); v.w = w; v.r = r;
    FluidWorld fw = { w->g, &v, view_get, view_set, w->preset->fast_lava, 1, 0 };
    fw.min_y = w->min_y; fw.height = w->height; fw.has_sky = w->dim_kind != 1; fw.world = w;
    fw.post_flags = ((r->stages & MC_STAGE_FEATURES) ? 1 : 0) | (w->struct_on ? 2 : 0);
    if (fw.post_flags) { fw.shape_update = structure_shape_update; fw.neighbors_update = structure_neighbors_update; }          /* пометки: грибы без света (FEATURES), заборы/факелы/лестницы построек */
    int cx0 = r->info.cx0, cz0 = r->info.cz0, nx = r->info.nx, nz = r->info.nz, cancel = 0;
    if (pp_margin < 0 && !getenv("MCGEN_HALO_LAZY")) {                 /* MCGEN_HALO_LAZY=1 — прежнее ленивое создание гало одним потоком (для сверки) */
        if (halo_pregenerate(&v, threads, cb, ud)) cancel = 1;
        if (cancel || v.fail) goto cleanup;
    }
    /* воспроизведение записанного порядка постобработки настоящего сервера (tools/gt/jfr_order.py --fluid-txt):
     * MCGEN_FLUID_ORDER=<файл> — строки «cx cz» в порядке, в котором чанки стали «тикающими» (все 8 соседей FULL); только при pp_margin >= 0 */
    int ordered = 0;
    if (w->sched && w->sched->np > 0) {            /* расписание записанного прогона: порядок «тикающих» чанков из файла (.mcsched, строки P) */
        const McSchedule *sc = w->sched; int pm = pp_margin < 0 ? 0 : pp_margin; ordered = 1;
        for (int i = 0; i < sc->np; i++) {
            int ox = sc->px[i], oz = sc->pz[i];
            if (!(ox >= cx0 && ox < cx0 + nx && oz >= cz0 && oz < cz0 + nz)) continue;
            int d = ox - cx0; if (cx0 + nx - 1 - ox < d) d = cx0 + nx - 1 - ox;
            if (oz - cz0 < d) d = oz - cz0;
            if (cz0 + nz - 1 - oz < d) d = cz0 + nz - 1 - oz;
            if (d >= pm) fluidpp_chunk(&fw, &r->marks[chunk_index(r, ox, oz)], ox, oz, w->min_y);
            if (r->stages & MC_STAGE_FEATURES) features_post_chunk(&fw, w, ox, oz);
        }
    } else
    { const char *fo = getenv("MCGEN_FLUID_ORDER"); FILE *fp = (fo && *fo && pp_margin >= 0) ? mc_fopen(fo, "r") : NULL;
      if (fp) { int ox, oz; ordered = 1;
          while (fscanf(fp, "%d %d", &ox, &oz) == 2) {
              if (!(ox >= cx0 && ox < cx0 + nx && oz >= cz0 && oz < cz0 + nz)) continue;
              int d = ox - cx0; if (cx0 + nx - 1 - ox < d) d = cx0 + nx - 1 - ox;
              if (oz - cz0 < d) d = oz - cz0;
              if (cz0 + nz - 1 - oz < d) d = cz0 + nz - 1 - oz;
              if (d >= pp_margin) fluidpp_chunk(&fw, &r->marks[chunk_index(r, ox, oz)], ox, oz, w->min_y);
              if (r->stages & MC_STAGE_FEATURES) features_post_chunk(&fw, w, ox, oz);
          }
          fclose(fp); } }
    for (int cz = cz0 - 1; cz <= cz0 + nz && !cancel && !ordered; cz++) {
        if (cb) {
            char what[64]; snprintf(what, sizeof what, "fluids %d/%d", cz - cz0 + 1, nz + 2);
            if (cb(ud, 0.96 + 0.02 * (cz - cz0 + 1) / (nz + 2), what)) { cancel = 1; break; }
        }
        for (int cx = cx0 - 1; cx <= cx0 + nx; cx++) {
            int inside = cx >= cx0 && cx < cx0 + nx && cz >= cz0 && cz < cz0 + nz;
            if (pp_margin >= 0) {
                if (!inside) continue;
                int d = cx - cx0; if (cx0 + nx - 1 - cx < d) d = cx0 + nx - 1 - cx;
                if (cz - cz0 < d) d = cz - cz0;
                if (cz0 + nz - 1 - cz < d) d = cz0 + nz - 1 - cz;
                if (d < pp_margin) { if (r->stages & MC_STAGE_FEATURES) features_post_chunk(&fw, w, cx, cz); continue; }   /* пузыри над магмой есть и у краевых чанков эталона */
            }
            if (inside) { fluidpp_chunk(&fw, &r->marks[chunk_index(r, cx, cz)], cx, cz, w->min_y); if (r->stages & MC_STAGE_FEATURES) features_post_chunk(&fw, w, cx, cz); continue; }
            /* гало: только пометки в одном блоке от региона */
            PPMarks *m = NULL; view_chunk(&v, cx, cz, &m);
            for (int s = 0; s < m->nsec; s++) for (int k = 0; k < m->n[s]; k++) {
                u16 p = m->pos[s][k];
                int x = cx * 16 + (p & 15), z = cz * 16 + ((p >> 8) & 15);
                int dx = x < cx0 * 16 ? cx0 * 16 - x : (x >= (cx0 + nx) * 16 ? x - (cx0 + nx) * 16 + 1 : 0);
                int dz = z < cz0 * 16 ? cz0 * 16 - z : (z >= (cz0 + nz) * 16 ? z - (cz0 + nz) * 16 + 1 : 0);
                if (dx > 1 || dz > 1) continue;
                fluidpp_tick(&fw, x, w->min_y + s * 16 + ((p >> 4) & 15), z);
            }
        }
    }
cleanup:
    for (int b = 0; b < 256; b++) { HaloChunk *h = v.halo[b]; while (h) { HaloChunk *nx2 = h->next; free(h->blocks); ppmarks_free(&h->marks); free(h); h = nx2; } }
    if (v.t) terrain_ctx_free(v.t);
    if (v.sc) surface_ctx_free(v.sc);
    if (v.bx) sctx_free(v.bx);
    if (v.fail) { set_err(err, errlen, "%s", v.err); return -1; }
    if (cancel) { set_err(err, errlen, "отменено"); return 1; }
    return 0;
}

/* Карты высот всех чанков региона: чанки независимы — считаются всеми потоками (раньше цикл шёл одним потоком: на области 2048×2048 это секунды) */
typedef struct { McWorld *w; McRegion *r; size_t n; size_t next; McMutex *lock; } HmJob;
static void hm_worker(void *arg) {
    HmJob *j = arg; McWorld *w = j->w; McRegion *r = j->r; size_t H = (size_t)w->height;
    for (;;) {
        mutex_lock(j->lock); size_t i0 = j->next; j->next += 16; mutex_unlock(j->lock);
        if (i0 >= j->n) break;
        size_t i1 = i0 + 16 < j->n ? i0 + 16 : j->n;
        for (size_t i = i0; i < i1; i++) compute_heightmaps(w->g, w->g->state_cls, r->blocks + H * 256 * i, w->min_y, (int)H, r->hm + i * 1024);
    }
}
static void heightmaps_all(McWorld *w, McRegion *r, int threads) {
    HmJob j; j.w = w; j.r = r; j.n = (size_t)r->info.nx * r->info.nz; j.next = 0; j.lock = mutex_new();
    int nt = threads > 0 ? threads : cpu_count();
    if ((size_t)nt > (j.n + 15) / 16) nt = (int)((j.n + 15) / 16);
    if (nt <= 1) hm_worker(&j);
    else {
        McThread **th = xcalloc((size_t)nt, sizeof(McThread *));
        for (int i = 0; i < nt; i++) th[i] = thread_start(hm_worker, &j);
        for (int i = 0; i < nt; i++) thread_join(th[i]);
        free(th);
    }
    mutex_free(j.lock);
}

static int generate_impl(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int threads, int pp_margin,
                         McProgressFn cb, void *ud, McRegion **out, char *err, size_t errlen) {
    if (!w || !out || nx <= 0 || nz <= 0 || (long)nx * nz > 1 << 20) { set_err(err, errlen, "mcgen_generate_region: аргументы"); return MCGEN_E_ARG; }
    *out = NULL;
    uint32_t sup = MC_STAGE_BIOMES | MC_STAGE_TERRAIN | MC_STAGE_SURFACE | MC_STAGE_CARVERS | MC_STAGE_FEATURES | MC_STAGE_STRUCTURES;
    if (stages & MC_STAGE_SURFACE) stages |= MC_STAGE_TERRAIN | MC_STAGE_BIOMES;   /* поверхность: заполненный чанк и биомы для правил */
    if (stages & MC_STAGE_CARVERS) stages |= MC_STAGE_TERRAIN;   /* карверы работают над заполненным чанком */
    if (stages & MC_STAGE_FEATURES) stages |= MC_STAGE_TERRAIN | MC_STAGE_BIOMES;   /* декорации: заполненные чанки и биомы (стадия features.c) */
    if (stages & MC_STAGE_STRUCTURES) stages |= MC_STAGE_TERRAIN | MC_STAGE_BIOMES;   /* постройки: Beardifier в заполнении, части — в цикле декорации (structure.c) */
    if (stages & MC_STAGE_STRUCTURES) structures_begin_region(w);                    /* части кэшированных стартов — в исходное состояние */
    w->struct_on = (stages & MC_STAGE_STRUCTURES) != 0;                              /* Beardifier учитывается в terrain_fill_chunk при любом числе потоков */
    /* GPU (поток W7): опережающий расчёт плотности/жил для чанков региона и кольца гало; закрывается в generate() */
    gpu_terrain_begin(w, cx0, cz0, nx, nz, stages, pp_margin < 0 && w->tweak[MCGEN_TWEAK_FLUID_FLOW] != 0.0);
    if (stages & ~sup & MC_STAGE_ALL) {
        /* стадии SURFACE и выше — другие потоки работ; пока считаем доступные */
        stages &= sup;
    }
    McRegion *r = xcalloc(1, sizeof *r);
    r->info.cx0 = cx0; r->info.cz0 = cz0; r->info.nx = nx; r->info.nz = nz; r->info.min_y = w->min_y; r->info.height = w->height;
    r->stages = stages;
    size_t n = (size_t)nx * nz, H = (size_t)w->height;
    r->blocks = calloc(n * H * 256, sizeof(uint16_t));
    r->biomes = calloc(n * (H / 4) * 16, 1);
    r->hm = calloc(n * 1024, sizeof(int16_t));
    r->marks = calloc(n, sizeof(PPMarks));
    if (!r->blocks || !r->biomes || !r->hm || !r->marks) { mcgen_region_free(r); set_err(err, errlen, "нет памяти под регион"); return MCGEN_E_NOMEM; }
    for (size_t i = 0; i < n * 1024; i++) r->hm[i] = (int16_t)w->min_y;
    if (!(stages & MC_STAGE_TERRAIN)) { /* блоки — воздух (id 0 не обязательно воздух) */
        uint16_t air = (uint16_t)w->g->st_air;
        if (air) for (size_t i = 0; i < n * H * 256; i++) r->blocks[i] = air;
    }
    world_bent_reset(w);
    Job j; memset(&j, 0, sizeof j);
    j.w = w; j.r = r; j.stages = stages; j.total = (int)n; j.lock = mutex_new(); j.cb = cb; j.ud = ud;
    j.cls = w->g->state_cls;
    int nt = threads > 0 ? threads : cpu_count();
    if (nt > (int)n) nt = (int)n;
    if (nt <= 1) worker(&j);
    else {
        McThread **th = xcalloc((size_t)nt, sizeof(McThread *));
        for (int i = 0; i < nt; i++) th[i] = thread_start(worker, &j);
        for (int i = 0; i < nt; i++) thread_join(th[i]);
        free(th);
    }
    mutex_free(j.lock);
    if (j.failed) { set_err(err, errlen, "%s", j.err); mcgen_region_free(r); return MCGEN_E_INTERNAL; }
    if (j.cancel) { set_err(err, errlen, "отменено"); mcgen_region_free(r); return MCGEN_E_CANCEL; }
    if (stages & MC_STAGE_TERRAIN) {
        /* Порядок конвейера: все стадии по чанкам (worker/view_chunk: TERRAIN → SURFACE → CARVERS → …) → растекание жидкостей
         * (в игре — при переходе чанка в FULL, т. е. после всех стадий генерации) → карты высот. Новые стадии вставлять до этого места. */
        if (stages & (MC_STAGE_FEATURES | MC_STAGE_STRUCTURES)) {   /* декорации и постройки (feature*.c + structure.c): после всех стадий чанков, до пост-обработки жидкостей */
            int frc = features_apply_region(w, r, threads, cb, ud, err, errlen);
            if (frc) { mcgen_region_free(r); return frc > 0 ? MCGEN_E_CANCEL : MCGEN_E_INTERNAL; }
        }
        /* fluid_flow = 0: «чистое» заполнение шумом (как чанк со статусом ниже full) — без растекания */
        int prc = w->tweak[MCGEN_TWEAK_FLUID_FLOW] != 0.0 ? region_postprocess(w, r, pp_margin, threads, cb, ud, err, errlen) : 0;
        if (prc) { mcgen_region_free(r); return prc > 0 ? MCGEN_E_CANCEL : MCGEN_E_INTERNAL; }
        if (cb) cb(ud, 0.99, "heightmaps");
        heightmaps_all(w, r, threads);
    }
    r->bents = world_bent_json(w, cx0 * 16, cz0 * 16, (cx0 + nx) * 16, (cz0 + nz) * 16);
    if (w->g->st_air == 0) region_trim_zero_pages(r->blocks, n * H * 256 * sizeof(uint16_t));      /* воздух над поверхностью не держит физическую память */
#if defined(__GLIBC__)
    if (!getenv("MCGEN_NO_TRIM")) malloc_trim(0);                                                  /* рабочие буферы стадий уже освобождены — вернуть кучу системе */
#endif
    if (cb) cb(ud, 1.0, "done");
    *out = r;
    return MCGEN_OK;
}

static int generate(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int threads, int pp_margin,
                    McProgressFn cb, void *ud, McRegion **out, char *err, size_t errlen) {
    int rc = generate_impl(w, cx0, cz0, nx, nz, stages, threads, pp_margin, cb, ud, out, err, errlen);
    if (w) gpu_terrain_end(w);
    return rc;
}
int mcgen_generate_region(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int threads,
                          McProgressFn cb, void *ud, McRegion **out, char *err, size_t errlen) {
    return generate(w, cx0, cz0, nx, nz, stages, threads, -1, cb, ud, out, err, errlen);
}
int mcgen_x_generate_region_pp(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int threads, int pp_margin,
                               McRegion **out, char *err, size_t errlen) {
    return generate(w, cx0, cz0, nx, nz, stages, threads, pp_margin, NULL, NULL, out, err, errlen);
}
uint32_t region_stages(const McRegion *r) { return r->stages; }                                 /* для feature.c */
PPMarks *region_chunk_marks(McRegion *r, int cx, int cz) { int i = chunk_index(r, cx, cz); return i < 0 ? NULL : &r->marks[i]; }
void mcgen_region_free(McRegion *r) {
    if (!r) return;
    if (r->marks) for (int i = 0; i < r->info.nx * r->info.nz; i++) ppmarks_free(&r->marks[i]);
    free(r->marks); free(r->blocks); free(r->biomes); free(r->hm); free(r->bents); free(r);
}
const char *mcgen_region_block_entities(const McRegion *r) { return r && r->bents ? r->bents : "[]"; }
void mcgen_region_info(const McRegion *r, McRegionInfo *info) { if (r && info) *info = r->info; }
static int chunk_index(const McRegion *r, int cx, int cz) {
    int ix = cx - r->info.cx0, iz = cz - r->info.cz0;
    if (ix < 0 || iz < 0 || ix >= r->info.nx || iz >= r->info.nz) return -1;
    return iz * r->info.nx + ix;
}
uint16_t *mcgen_region_blocks(McRegion *r, int cx, int cz) {
    int i = r ? chunk_index(r, cx, cz) : -1; return i < 0 ? NULL : r->blocks + (size_t)i * r->info.height * 256;
}
uint8_t *mcgen_region_biomes(McRegion *r, int cx, int cz) {
    int i = r ? chunk_index(r, cx, cz) : -1; return i < 0 ? NULL : r->biomes + (size_t)i * (r->info.height / 4) * 16;
}
int16_t *mcgen_region_heightmap(McRegion *r, int cx, int cz, int kind) {
    int i = r ? chunk_index(r, cx, cz) : -1; if (i < 0 || kind < 0 || kind > 3) return NULL;
    return r->hm + (size_t)i * 1024 + (size_t)kind * 256;
}

/* ---------------- MCR1 ---------------- */
static int wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }
static int wr_i32(FILE *f, int32_t v) { u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) }; return wr(f, b, 4); }
static int wr_u16(FILE *f, uint16_t v) { u8 b[2] = { (u8)v, (u8)(v >> 8) }; return wr(f, b, 2); }
static int wr_str(FILE *f, const char *s) { size_t n = strlen(s); return (n > 65535 || wr_u16(f, (uint16_t)n) || wr(f, s, n)) ? -1 : 0; }
int mcgen_region_write_mcr(const McRegion *r, const McGen *g, const char *path, char *err, size_t errlen) {
    if (!r || !g || !path) { set_err(err, errlen, "write_mcr: аргументы"); return MCGEN_E_ARG; }
    FILE *f = mc_fopen(path, "wb");
    if (!f) { set_err(err, errlen, "не открыть %s", path); return MCGEN_E_IO; }
    int bad = 0;
    bad |= wr(f, "MCR1", 4); bad |= wr_i32(f, MCGEN_ABI_VERSION);
    bad |= wr_i32(f, r->info.cx0); bad |= wr_i32(f, r->info.cz0); bad |= wr_i32(f, r->info.nx); bad |= wr_i32(f, r->info.nz);
    bad |= wr_i32(f, r->info.min_y); bad |= wr_i32(f, r->info.height); bad |= wr_i32(f, (int32_t)r->stages);
    size_t H = (size_t)r->info.height;
    u8 *tmp = xmalloc(H * 256 * 2);
    for (int i = 0; i < r->info.nx * r->info.nz && !bad; i++) {
        const uint16_t *b = r->blocks + (size_t)i * H * 256;
        for (size_t k = 0; k < H * 256; k++) { tmp[2 * k] = (u8)b[k]; tmp[2 * k + 1] = (u8)(b[k] >> 8); }
        bad |= wr(f, tmp, H * 256 * 2);
        bad |= wr(f, r->biomes + (size_t)i * (H / 4) * 16, (H / 4) * 16);
        const int16_t *hm = r->hm + (size_t)i * 1024;
        for (int k = 0; k < 1024; k++) { tmp[2 * k] = (u8)hm[k]; tmp[2 * k + 1] = (u8)((uint16_t)hm[k] >> 8); }
        bad |= wr(f, tmp, 2048);
    }
    free(tmp);
    bad |= wr_i32(f, g->nstates);
    for (int i = 0; i < g->nstates && !bad; i++) bad |= wr_str(f, g->state_names[i]);
    bad |= wr_i32(f, g->nbiomes);
    for (int i = 0; i < g->nbiomes && !bad; i++) bad |= wr_str(f, g->biome_names[i]);
    if (fclose(f) != 0) bad = 1;
    if (bad) { set_err(err, errlen, "ошибка записи %s", path); return MCGEN_E_IO; }
    return MCGEN_OK;
}

/* Заглушка libmcgen для проверки моста ctypes и кросс-сборки (НЕ генератор мира).
 * Реализует весь публичный ABI из libmcgen/include/mcgen.h с тривиальным содержимым: плоский мир с «ступенчатой» поверхностью,
 * 3 измерения, 6 состояний блоков, 3 биома, настоящая таблица тонких настроек (libmcgen/gen/mcgen_tweaks_table.h), прогресс/отмена,
 * дамп MCR1. Сборка: python3 libmcgen/build.py --stub (или tests/addon/run_tests.py).
 * Переменная окружения MCGEN_STUB_DELAY_MS задерживает каждый чанк (для проверки отмены и снятия GIL). */
#include "mcgen.h"
#include "mcgen_tweaks_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void msleep(int ms) { Sleep(ms); }
#else
#include <time.h>
static void msleep(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#endif

struct McGen { char pack[512]; char version[64]; };
struct McWorld { McGen *g; int dim; McSeeds seeds; int sea; };
typedef struct { uint16_t *blocks; uint8_t *biomes; int16_t *hm[4]; } Chunk;
struct McRegion { McRegionInfo info; uint32_t stages; Chunk *ch; };

static const char *DIMS[3] = { "minecraft:overworld", "minecraft:the_nether", "minecraft:the_end" };
static const char *PRESETS_OW[3] = { "normal", "large_biomes", "amplified" };
static const char *BLOCKS[6] = { "minecraft:air", "minecraft:stone", "minecraft:grass_block[snowy=false]", "minecraft:dirt",
                                 "minecraft:water[level=0]", "minecraft:bedrock" };
static const char *BIOMES[3] = { "minecraft:plains", "minecraft:desert", "minecraft:ocean" };

static void seterr(char *err, size_t n, const char *m) { if (err && n) { strncpy(err, m, n - 1); err[n - 1] = 0; } }
MCGEN_API const char *mcgen_version(void) { return "libmcgen 0.0.0-stub abi " "1"; }
MCGEN_API int mcgen_abi_version(void) { return MCGEN_ABI_VERSION; }

MCGEN_API int mcgen_open(const char *pack_dir, const char *version, McGen **out, char *err, size_t errlen) {
    if (!pack_dir || !version || !out) { seterr(err, errlen, "null argument"); return MCGEN_E_ARG; }
    if (strcmp(version, "26.1") && strcmp(version, "26.2") && strcmp(version, "26.3") && strcmp(version, "26.4-snapshot-2")) {
        seterr(err, errlen, "unsupported version"); return MCGEN_E_VERSION; }
    FILE *f = fopen(pack_dir, "rb");           /* каталог на Linux открывается fopen'ом (чтение не удастся, но не NULL), на Windows — нет */
    char probe[600]; snprintf(probe, sizeof probe, "%s/version.json", pack_dir);
    FILE *v = fopen(probe, "rb");
    if (f) fclose(f);
    if (!v && !getenv("MCGEN_STUB_NOPACK")) { seterr(err, errlen, "pack directory has no version.json"); return MCGEN_E_IO; }
    if (v) fclose(v);
    McGen *g = (McGen *)calloc(1, sizeof *g);
    strncpy(g->pack, pack_dir, sizeof g->pack - 1); strncpy(g->version, version, sizeof g->version - 1);
    *out = g; return MCGEN_OK;
}
MCGEN_API void mcgen_close(McGen *g) { free(g); }
MCGEN_API int mcgen_dimension_count(const McGen *g) { (void)g; return 3; }
MCGEN_API const char *mcgen_dimension_name(const McGen *g, int i) { (void)g; return i >= 0 && i < 3 ? DIMS[i] : NULL; }
MCGEN_API int mcgen_preset_count(const McGen *g, const char *dim) { (void)g; return !strcmp(dim, DIMS[0]) ? 3 : (!strcmp(dim, DIMS[1]) || !strcmp(dim, DIMS[2])) ? 1 : -1; }
MCGEN_API const char *mcgen_preset_name(const McGen *g, const char *dim, int i) {
    (void)g; if (!strcmp(dim, DIMS[0])) return i >= 0 && i < 3 ? PRESETS_OW[i] : NULL; return i == 0 ? "normal" : NULL; }
MCGEN_API int mcgen_block_state_count(const McGen *g) { (void)g; return 6; }
MCGEN_API const char *mcgen_block_state_name(const McGen *g, int id) { (void)g; return id >= 0 && id < 6 ? BLOCKS[id] : NULL; }
MCGEN_API int mcgen_block_state_from_name(const McGen *g, const char *n) { (void)g; for (int i = 0; i < 6; i++) if (!strcmp(n, BLOCKS[i])) return i; return -1; }
MCGEN_API int mcgen_biome_count(const McGen *g) { (void)g; return 3; }
MCGEN_API const char *mcgen_biome_name(const McGen *g, int id) { (void)g; return id >= 0 && id < 3 ? BIOMES[id] : NULL; }
MCGEN_API int mcgen_tweak_count(const McGen *g) { (void)g; return MCGEN_TWEAK_COUNT; }
MCGEN_API const McTweakInfo *mcgen_tweak_info(const McGen *g, int i) { (void)g; return i >= 0 && i < MCGEN_TWEAK_COUNT ? &MCGEN_TWEAK_TABLE[i] : NULL; }

MCGEN_API int mcgen_world_new(McGen *g, const char *dimension, const char *preset, const McSeeds *seeds, const McTweakValue *tw, int ntw,
                              McWorld **out, char *err, size_t errlen) {
    int d = -1; for (int i = 0; i < 3; i++) if (!strcmp(dimension, DIMS[i])) d = i;
    if (d < 0) { seterr(err, errlen, "unknown dimension"); return MCGEN_E_ARG; }
    int np = mcgen_preset_count(g, dimension), okp = 0;
    for (int i = 0; i < np; i++) if (!strcmp(preset, mcgen_preset_name(g, dimension, i))) okp = 1;
    if (!okp) { seterr(err, errlen, "unknown preset"); return MCGEN_E_ARG; }
    McWorld *w = (McWorld *)calloc(1, sizeof *w); w->g = g; w->dim = d; w->seeds = *seeds; w->sea = d == 0 ? 63 : d == 1 ? 32 : 0;
    for (int i = 0; i < ntw; i++) {
        int known = 0;
        for (int k = 0; k < MCGEN_TWEAK_COUNT; k++) if (!strcmp(tw[i].id, MCGEN_TWEAK_TABLE[k].id)) known = 1;
        if (!known) { free(w); seterr(err, errlen, "unknown tweak"); return MCGEN_E_ARG; }
        if (!strcmp(tw[i].id, "sea_level_offset")) w->sea += (int)tw[i].value;
    }
    *out = w; return MCGEN_OK;
}
MCGEN_API void mcgen_world_free(McWorld *w) { free(w); }
MCGEN_API int mcgen_world_min_y(const McWorld *w) { return w->dim == 0 ? -64 : 0; }
MCGEN_API int mcgen_world_height(const McWorld *w) { return w->dim == 0 ? 384 : 256; }
MCGEN_API int mcgen_world_sea_level(const McWorld *w) { return w->sea; }

static int fdiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
MCGEN_API int mcgen_biome_at(const McWorld *w, int x, int y, int z) { (void)w; (void)y; int s = fdiv(x, 64) + fdiv(z, 64); return ((s % 3) + 3) % 3; }
MCGEN_API int mcgen_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out) {
    if (!out || nx <= 0 || nz <= 0 || step <= 0) return MCGEN_E_ARG;
    for (int iz = 0; iz < nz; iz++) for (int ix = 0; ix < nx; ix++) out[iz * nx + ix] = (uint8_t)mcgen_biome_at(w, x0 + ix * step, y, z0 + iz * step);
    return MCGEN_OK;
}

static int top_y(int x, int z) { return 64 + ((x + 2 * z) & 7); }   /* высота верхнего твёрдого блока (абсолютная) */

MCGEN_API int mcgen_generate_region(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int threads, McProgressFn cb, void *ud,
                                    McRegion **out, char *err, size_t errlen) {
    (void)threads;
    if (nx <= 0 || nz <= 0 || nx > 4096 || nz > 4096) { seterr(err, errlen, "bad region size"); return MCGEN_E_ARG; }
    int miny = mcgen_world_min_y(w), H = mcgen_world_height(w);
    McRegion *r = (McRegion *)calloc(1, sizeof *r);
    r->info.cx0 = cx0; r->info.cz0 = cz0; r->info.nx = nx; r->info.nz = nz; r->info.min_y = miny; r->info.height = H; r->stages = stages;
    r->ch = (Chunk *)calloc((size_t)nx * nz, sizeof(Chunk));
    const char *dl = getenv("MCGEN_STUB_DELAY_MS"); int delay = dl ? atoi(dl) : 0;
    int total = nx * nz;
    for (int i = 0; i < total; i++) {
        if (cb) { if (cb(ud, (double)i / total, (stages & MC_STAGE_TERRAIN) ? "terrain" : "biomes")) { mcgen_region_free(r); seterr(err, errlen, "cancelled"); return MCGEN_E_CANCEL; } }
        if (delay) msleep(delay);
        Chunk *c = &r->ch[i]; int cx = cx0 + i % nx, cz = cz0 + i / nx;
        c->blocks = (uint16_t *)calloc((size_t)H * 256, 2); c->biomes = (uint8_t *)calloc((size_t)(H / 4) * 16, 1);
        for (int k = 0; k < 4; k++) c->hm[k] = (int16_t *)calloc(256, 2);
        for (int qy = 0; qy < H / 4; qy++) for (int qz = 0; qz < 4; qz++) for (int qx = 0; qx < 4; qx++)
            c->biomes[(qy * 4 + qz) * 4 + qx] = (uint8_t)mcgen_biome_at(w, cx * 16 + qx * 4, 0, cz * 16 + qz * 4);
        for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
            int top = top_y(cx * 16 + x, cz * 16 + z), sea = w->sea;
            int hmv = top + 1;
            if (stages & MC_STAGE_TERRAIN) for (int y = miny; y < miny + H; y++) {
                uint16_t b = 0;
                if (y <= top) b = 1; else if (y <= sea) b = 4;
                if ((stages & MC_STAGE_SURFACE) && b == 1) {
                    if (y == miny) b = 5; else if (y == top) b = 2; else if (y > top - 4) b = 3;
                }
                c->blocks[((size_t)(y - miny) * 16 + z) * 16 + x] = b;
            }
            c->hm[0][z * 16 + x] = (int16_t)(hmv > sea + 1 ? hmv : sea + 1); c->hm[1][z * 16 + x] = (int16_t)hmv;
            c->hm[2][z * 16 + x] = c->hm[0][z * 16 + x]; c->hm[3][z * 16 + x] = c->hm[0][z * 16 + x];
        }
    }
    if (cb) cb(ud, 1.0, "done");
    *out = r; return MCGEN_OK;
}
MCGEN_API void mcgen_region_free(McRegion *r) {
    if (!r) return;
    for (int i = 0; i < r->info.nx * r->info.nz; i++) { free(r->ch[i].blocks); free(r->ch[i].biomes); for (int k = 0; k < 4; k++) free(r->ch[i].hm[k]); }
    free(r->ch); free(r);
}
MCGEN_API void mcgen_region_info(const McRegion *r, McRegionInfo *info) { *info = r->info; }
static Chunk *at(McRegion *r, int cx, int cz) {
    int ix = cx - r->info.cx0, iz = cz - r->info.cz0;
    return (ix < 0 || iz < 0 || ix >= r->info.nx || iz >= r->info.nz) ? NULL : &r->ch[iz * r->info.nx + ix];
}
MCGEN_API uint16_t *mcgen_region_blocks(McRegion *r, int cx, int cz) { Chunk *c = at(r, cx, cz); return c ? c->blocks : NULL; }
MCGEN_API uint8_t *mcgen_region_biomes(McRegion *r, int cx, int cz) { Chunk *c = at(r, cx, cz); return c ? c->biomes : NULL; }
MCGEN_API int16_t *mcgen_region_heightmap(McRegion *r, int cx, int cz, int kind) { Chunk *c = at(r, cx, cz); return c && kind >= 0 && kind < 4 ? c->hm[kind] : NULL; }

static void wstr(FILE *f, const char *s) { uint16_t n = (uint16_t)strlen(s); fwrite(&n, 2, 1, f); fwrite(s, 1, n, f); }
MCGEN_API int mcgen_region_write_mcr(const McRegion *r, const McGen *g, const char *path, char *err, size_t errlen) {
    (void)g; FILE *f = fopen(path, "wb"); if (!f) { seterr(err, errlen, "cannot open output"); return MCGEN_E_IO; }
    int32_t hdr[8] = { MCGEN_ABI_VERSION, r->info.cx0, r->info.cz0, r->info.nx, r->info.nz, r->info.min_y, r->info.height, (int32_t)r->stages };
    fwrite("MCR1", 1, 4, f); fwrite(hdr, 4, 8, f);
    for (int i = 0; i < r->info.nx * r->info.nz; i++) {
        const Chunk *c = &r->ch[i];
        fwrite(c->blocks, 2, (size_t)r->info.height * 256, f); fwrite(c->biomes, 1, (size_t)(r->info.height / 4) * 16, f);
        for (int k = 0; k < 4; k++) fwrite(c->hm[k], 2, 256, f);
    }
    uint32_t n = 6; fwrite(&n, 4, 1, f); for (int i = 0; i < 6; i++) wstr(f, BLOCKS[i]);
    n = 3; fwrite(&n, 4, 1, f); for (int i = 0; i < 3; i++) wstr(f, BIOMES[i]);
    fclose(f); return MCGEN_OK;
}

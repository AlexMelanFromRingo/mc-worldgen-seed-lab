/* gpu_bridge.c — мост libmcgen ↔ необязательная libmcgen_cuda (динамическая загрузка: dlopen / LoadLibrary).
 *
 * Принципы (спека §3.6): библиотека работает без GPU; любая ошибка загрузки/устройства/самопроверки — тихий возврат на CPU
 * с записью причины в статус (mcgen_gpu_status); GPU используется только если результат ≡ CPU: при первом использовании мира
 * делается самопроверка «GPU = CPU» на небольшой выборке (кэшируется в мире), плюс отдельная mcgen_gpu_selftest.
 * Точки, которые GPU не смог решить строго как CPU (NaN в климате, длинная цепочка «ничьих» R-дерева), пересчитываются здесь на CPU. */
#define _GNU_SOURCE
#include "gpu_bridge.h"
#include "mc_biomes.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

/* ---------------------------------------------------------------- динамическая библиотека */
typedef struct {
    int (*abi)(void);
    int (*device_count)(void);
    int (*device_info)(int, char *, size_t, int *, int *, size_t *);
    int (*select)(int);
    const char *(*last_error)(void);
    int (*mem_info)(size_t *, size_t *);
    void *(*biome_new)(const McgBiomeWorld *, char *, size_t);
    void (*biome_free)(void *);
    int (*biome_grid)(void *, int, int, int, int, int, int, unsigned char *, int *);
    int (*biome_points)(void *, int, const int *, unsigned char *, int *);
} GpuApi;

static struct {
    atomic_flag lock;
    int mode;                      /* MCGEN_COMPUTE_* (по умолчанию AUTO) */
    int device;
    int load_state;                /* 0 — не пробовали, 1 — загружена, −1 — нет */
    char load_err[1000];
    char path_override[600];
    char path_used[700];
    void *lib;
    GpuApi api;
    int dev_selected;              /* 1 — устройство выбрано и инициализировано */
    char dev_name[160]; int dev_cc_major, dev_cc_minor; size_t dev_mem_mb;
    char last_fallback[300];       /* причина последнего возврата на CPU */
    int worlds_ok, worlds_failed;
    char selftest[400];            /* результат mcgen_gpu_selftest (пусто — не запускали) */
    int selftest_state;            /* 0 — нет, 1 — прошла, −1 — провалена */
} G = { ATOMIC_FLAG_INIT, MCGEN_COMPUTE_AUTO };

static void glock(void) { while (atomic_flag_test_and_set_explicit(&G.lock, memory_order_acquire)) { } }
static void gunlock(void) { atomic_flag_clear_explicit(&G.lock, memory_order_release); }

#ifdef _WIN32
#  define LIBNAME "mcgen_cuda.dll"
#else
#  define LIBNAME "libmcgen_cuda.so"
#endif

/* каталог, где лежит сам libmcgen (рядом ожидается libmcgen_cuda) */
static int module_dir(char *out, size_t n) {
#ifdef _WIN32
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&module_dir, &m)) return 0;
    DWORD len = GetModuleFileNameA(m, out, (DWORD)n);
    if (len == 0 || len >= n) return 0;
    char *s = strrchr(out, '\\'); char *s2 = strrchr(out, '/'); if (s2 > s) s = s2;
    if (!s) return 0; *s = 0; return 1;
#else
    Dl_info di;
    if (!dladdr((void *)&module_dir, &di) || !di.dli_fname) return 0;
    snprintf(out, n, "%s", di.dli_fname);
    char *s = strrchr(out, '/');
    if (!s) return 0; *s = 0; return 1;
#endif
}
static void *lib_open(const char *path) {
#ifdef _WIN32
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void *lib_sym(void *lib, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

/* загрузка (один раз; под замком). 1 — загружена */
static int gpu_load_locked(void) {
    if (G.load_state) return G.load_state > 0;
    G.load_state = -1;
    char cand[4][700]; int nc = 0;
    if (G.path_override[0]) snprintf(cand[nc++], 700, "%s", G.path_override);
    const char *env = getenv("MCGEN_CUDA_LIB");
    if (env && *env) snprintf(cand[nc++], 700, "%s", env);
    char dir[600];
    if (module_dir(dir, sizeof dir)) snprintf(cand[nc++], 700, "%s/%s", dir, LIBNAME);
    snprintf(cand[nc++], 700, "%s", LIBNAME);
    void *lib = NULL;
    for (int i = 0; i < nc && !lib; i++) { lib = lib_open(cand[i]); if (lib) snprintf(G.path_used, sizeof G.path_used, "%s", cand[i]); }
    if (!lib) { snprintf(G.load_err, sizeof G.load_err, "библиотека %s не найдена (CUDA-ускорение не установлено или нет драйвера NVIDIA)", LIBNAME); return 0; }
#define SYM(f, n) do { *(void **)&G.api.f = lib_sym(lib, n); if (!G.api.f) { snprintf(G.load_err, sizeof G.load_err, "в %s нет функции %s", G.path_used, n); return 0; } } while (0)
    SYM(abi, "mcgpu_abi"); SYM(device_count, "mcgpu_device_count"); SYM(device_info, "mcgpu_device_info"); SYM(select, "mcgpu_select");
    SYM(last_error, "mcgpu_last_error"); SYM(mem_info, "mcgpu_mem_info"); SYM(biome_new, "mcgpu_biome_new"); SYM(biome_free, "mcgpu_biome_free");
    SYM(biome_grid, "mcgpu_biome_grid"); SYM(biome_points, "mcgpu_biome_points");
#undef SYM
    if (G.api.abi() != MCGPU_ABI) { snprintf(G.load_err, sizeof G.load_err, "версия ABI libmcgen_cuda %d не совпадает с ожидаемой %d", G.api.abi(), MCGPU_ABI); return 0; }
    G.lib = lib; G.load_state = 1; G.load_err[0] = 0;
    return 1;
}
/* выбор устройства (один раз). 1 — готово */
static int gpu_device_locked(void) {
    if (G.dev_selected) return 1;
    if (!gpu_load_locked()) return 0;
    int n = G.api.device_count();
    if (n <= 0) { snprintf(G.load_err, sizeof G.load_err, "нет устройств NVIDIA CUDA (или драйвер не установлен)"); G.load_state = -1; return 0; }
    int dev = G.device >= 0 && G.device < n ? G.device : 0;
    if (G.api.select(dev)) { snprintf(G.load_err, sizeof G.load_err, "не удалось инициализировать устройство %d: %s", dev, G.api.last_error()); G.load_state = -1; return 0; }
    G.api.device_info(dev, G.dev_name, sizeof G.dev_name, &G.dev_cc_major, &G.dev_cc_minor, &G.dev_mem_mb);
    G.device = dev; G.dev_selected = 1;
    return 1;
}
static void set_fallback(const char *fmt, const char *a) { snprintf(G.last_fallback, sizeof G.last_fallback, fmt, a ? a : ""); }

/* ---------------------------------------------------------------- публичные функции управления */
int mcgen_gpu_set_library_path(const char *path) {
    glock();
    snprintf(G.path_override, sizeof G.path_override, "%s", path ? path : "");
    if (G.load_state < 0 || !G.lib) G.load_state = 0;     /* повторная попытка поиска */
    gunlock();
    return MCGEN_OK;
}
int mcgen_gpu_device_count(void) {
    glock();
    int n = 0;
    if (gpu_load_locked()) n = G.api.device_count();
    gunlock();
    return n > 0 ? n : 0;
}
int mcgen_gpu_device_info(int index, char *name, size_t namelen, int *cc_major, int *cc_minor, size_t *mem_mb) {
    glock();
    int rc = MCGEN_E_UNSUPPORTED;
    if (gpu_load_locked()) rc = G.api.device_info(index, name, namelen, cc_major, cc_minor, mem_mb) ? MCGEN_E_ARG : MCGEN_OK;
    gunlock();
    return rc;
}
int mcgen_gpu_set_compute(int mode, int device) {
    if (mode < MCGEN_COMPUTE_CPU || mode > MCGEN_COMPUTE_AUTO) return MCGEN_E_ARG;
    glock();
    G.mode = mode;
    if (device >= 0 && device != G.device) {
        if (G.dev_selected) { gunlock(); return MCGEN_E_UNSUPPORTED; }   /* устройство уже занято; смена — только до первого использования */
        G.device = device;
    }
    gunlock();
    return MCGEN_OK;
}
int mcgen_gpu_get_compute(void) { return G.mode; }

/* статус: строка из строк «ключ: значение»; возврат 1 — GPU готово к работе */
int mcgen_gpu_status(char *buf, size_t buflen) {
    glock();
    const char *mode = G.mode == MCGEN_COMPUTE_CPU ? "CPU" : G.mode == MCGEN_COMPUTE_GPU ? "GPU" : "Auto";
    int avail = 0;
    char line[2048]; line[0] = 0;
    int ok = G.mode == MCGEN_COMPUTE_CPU ? 0 : gpu_device_locked();
    if (G.mode == MCGEN_COMPUTE_CPU) {
        /* не трогаем драйвер, если пользователь выбрал CPU; только сообщаем, найдена ли библиотека */
        snprintf(line, sizeof line, "mode: CPU\nstate: GPU отключено пользователем");
    } else if (ok) {
        avail = 1;
        snprintf(line, sizeof line, "mode: %s\nstate: GPU готово\ndevice: %s (sm_%d%d, %zu МБ)\nlibrary: %s\nworlds_ok: %d\nworlds_failed: %d\nlast_fallback: %s\nselftest: %s",
                 mode, G.dev_name, G.dev_cc_major, G.dev_cc_minor, G.dev_mem_mb, G.path_used, G.worlds_ok, G.worlds_failed,
                 G.last_fallback[0] ? G.last_fallback : "-", G.selftest[0] ? G.selftest : "не запускалась");
    } else {
        snprintf(line, sizeof line, "mode: %s\nstate: GPU недоступно (расчёт на CPU)\nreason: %s", mode, G.load_err[0] ? G.load_err : "неизвестно");
    }
    if (buf && buflen) { snprintf(buf, buflen, "%s", line); }
    gunlock();
    return avail;
}

/* ---------------------------------------------------------------- программа и дерево мира */
void gpu_prog_free(McgProg *p) {
    free(p->nodes); free(p->noise); free(p->layers); free(p->octs); free(p->sp); free(p->splf); free(p->spc); free(p->arr); free(p->thr);
    memset(p, 0, sizeof *p);
}
static McgTreeNode *compress_tree(const McBiomeTree *T, int *n_out, int *root_out, char *why, size_t whylen) {
    int n = T->n_nodes;
    int *order = xmalloc(sizeof(int) * (size_t)n);
    McgTreeNode *out = xcalloc((size_t)n, sizeof(McgTreeNode));
    int head = 0, tail = 1; order[0] = T->root;
    while (head < tail) {
        const McRNode *nd = &T->node[order[head]];
        McgTreeNode *m = &out[head];
        for (int d = 0; d < 7; d++) {
            if (nd->box.lo[d] < INT32_MIN / 2 || nd->box.hi[d] > INT32_MAX / 2 || nd->box.lo[d] > INT32_MAX / 2 || nd->box.hi[d] < INT32_MIN / 2) {
                snprintf(why, whylen, "границы R-дерева вне диапазона int32"); free(order); free(out); return NULL;
            }
            m->lo[d] = (int32_t)nd->box.lo[d]; m->hi[d] = (int32_t)nd->box.hi[d];
        }
        m->biome = nd->biome;
        if (nd->count == 0) { m->count = 0; m->first = -1; }
        else {
            m->count = nd->count; m->first = tail;
            for (int k = 0; k < nd->count; k++) { if (tail >= n) { snprintf(why, whylen, "R-дерево не является деревом"); free(order); free(out); return NULL; } order[tail++] = T->child_idx[nd->first + k]; }
        }
        head++;
    }
    free(order);
    *n_out = tail; *root_out = 0;
    return out;
}

typedef struct {
    void *h;                       /* дескриптор GPU-мира (mcgpu_biome_new) */
    int state;                     /* 1 — проверен, годен; −1 — отказ (причина в why) */
    char why[300];
} GpuWorld;

static int g_climate_fields_new[6] = { RF_TEMPERATURE, RF_VEGETATION, RF_CONTINENTS, RF_EROSION, RF_DEPTH, RF_RIDGES };

/* собрать McgBiomeWorld; программа и дерево — в *prog и *tree (освобождает вызывающий) */
static int build_biome_world(const McWorld *w, McgBiomeWorld *B, McgProg *prog, McgTreeNode **tree, char *why, size_t whylen) {
    memset(B, 0, sizeof *B); memset(prog, 0, sizeof *prog); *tree = NULL;
    const McGen *g = w->g;
    int bs = w->preset->biome_source;
    B->newf = g->newf; B->min_qy = w->min_y >> 2; B->qh = w->height >> 2; B->zoom_seed = w->biome_zoom_seed;
    if (g->nbiomes >= 255) { snprintf(why, whylen, "слишком много биомов для u8-сетки"); return -1; }
    if (bs == BS_FIXED) { B->source = MCG_BS_FIXED; B->fixed_biome = w->preset->fixed_biome; return 0; }
    int fields_old[6] = { -1, -1, -1, -1, -1, -1 };
    const S *roots[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    if (bs == BS_THE_END) {
        B->source = MCG_BS_END;
        static const char *N[5] = { "minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands", "minecraft:small_end_islands", "minecraft:end_barrens" };
        for (int i = 0; i < 5; i++) B->end_ids[i] = gen_biome_id(g, N[i]);
        fields_old[3] = RF_EROSION; roots[3] = w->s_rf[RF_EROSION];
        if (g->newf ? !roots[3] : 0) { snprintf(why, whylen, "нет поля erosion"); return -1; }
    } else {
        B->source = MCG_BS_MULTI;
        for (int k = 0; k < 6; k++) { fields_old[k] = g_climate_fields_new[k]; roots[k] = w->s_rf[g_climate_fields_new[k]]; if (g->newf && !roots[k]) { snprintf(why, whylen, "нет поля климата %d", k); return -1; } }
        const McBiomeTree *T = bs == BS_MULTI_NETHER ? g->nether_tree : g->ow_tree;
        if (!T) { snprintf(why, whylen, "нет R-дерева биомов"); return -1; }
        int nt, root;
        *tree = compress_tree(T, &nt, &root, why, whylen);
        if (!*tree) return -1;
        B->ntree = nt; B->tree = *tree; B->tree_root = root;
    }
    if (g->newf) { if (gpu_export_new(roots, 6, prog)) { snprintf(why, whylen, "экспорт программы (26.3+) не удался"); return -1; } }
    else {
        if (!w->old) { snprintf(why, whylen, "нет проводки 26.1/26.2"); return -1; }
        if (gpu_export_old(w->old, fields_old, 6, prog)) { snprintf(why, whylen, "в роутере есть узлы, не поддержанные GPU"); return -1; }
    }
    if (getenv("MCGEN_GPU_DEBUG")) {
        int hist[80] = {0}; for (int i = 0; i < prog->nnodes; i++) { int k = prog->nodes[i].k; if (k >= 0 && k < 80) hist[k]++; }
        fprintf(stderr, "[gpu] программа: %d узлов, %d шумов, %d октав, %d сплайнов; виды:", prog->nnodes, prog->nnoise, prog->noct, prog->nsp);
        for (int k = 0; k < 80; k++) if (hist[k]) fprintf(stderr, " %d×%d", k, hist[k]);
        fprintf(stderr, "\n[gpu]   порядок:"); for (int i = 0; i < prog->nnodes && i < 80; i++) fprintf(stderr, " %d", prog->nodes[i].k);
        fprintf(stderr, "\n[gpu]   корни:"); for (int i = 0; i < 6; i++) fprintf(stderr, " %d", prog->root[i]);
        fprintf(stderr, " (%s/%s)\n", w->preset->name, w->g->newf ? "новая" : "старая");
    }
    B->prog = prog;
    return 0;
}

/* ---------------------------------------------------------------- самопроверка мира: GPU = CPU */
static uint64_t rng_next(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x; }

/* сравнить n случайных точек GPU↔CPU; возврат: число расхождений (−1 — ошибка GPU). *unres — число точек, отданных на CPU */
static int compare_points(const McWorld *w, void *h, int n, uint64_t seed, int *unres, char *why, size_t whylen) {
    int *xyz = xmalloc(sizeof(int) * 3 * (size_t)n);
    uint8_t *gpu = xmalloc((size_t)n);
    uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < n; i++) {
        uint64_t r = rng_next(&s);
        int span = (r & 3) == 0 ? 30000000 : ((r & 3) == 1 ? 200000 : ((r & 3) == 2 ? 4000 : 300));
        xyz[3 * i] = (int)(rng_next(&s) % (2 * (uint64_t)span + 1)) - span;
        xyz[3 * i + 2] = (int)(rng_next(&s) % (2 * (uint64_t)span + 1)) - span;
        int ylo = w->min_y - 16, yh = w->height + 32;
        xyz[3 * i + 1] = ylo + (int)(rng_next(&s) % (uint64_t)yh);
    }
    int bad_gpu = 0, mism = 0;
    int rc = G.api.biome_points(h, n, xyz, gpu, &bad_gpu);
    if (rc) { snprintf(why, whylen, "ошибка GPU: %s", G.api.last_error()); free(xyz); free(gpu); return -1; }
    for (int i = 0; i < n; i++) {
        if (gpu[i] == 255) continue;      /* нерешённые — CPU, сравнивать нечего */
        if ((int)gpu[i] != mcgen_biome_at(w, xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2])) {
            if (!mism) snprintf(why, whylen, "расхождение GPU и CPU в точке (%d,%d,%d): GPU %d, CPU %d", xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2], gpu[i],
                                mcgen_biome_at(w, xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]));
            mism++;
        }
    }
    if (unres) *unres = bad_gpu;
    free(xyz); free(gpu);
    return mism;
}

enum { SELFCHECK_POINTS = 384 };

/* состояние GPU-мира для биомов: создаётся при первом использовании (под глобальным замком) */
static GpuWorld *gpu_world_get(const McWorld *cw) {
    McWorld *w = (McWorld *)cw;
    glock();
    GpuWorld *gw = w->gpu;
    if (gw) { gunlock(); return gw; }
    gw = xcalloc(1, sizeof *gw);
    w->gpu = gw;
    McgBiomeWorld B; McgProg prog; McgTreeNode *tree = NULL;
    char why[300] = {0};
    int rc = build_biome_world(w, &B, &prog, &tree, why, sizeof why);
    if (rc == 0) {
        char err[300] = {0};
        gw->h = G.api.biome_new(&B, err, sizeof err);
        if (!gw->h) { snprintf(why, sizeof why, "GPU: %s", err); rc = -1; }
    }
    gpu_prog_free(&prog); free(tree);
    if (rc == 0) {
        int unres = 0;
        int mism = compare_points(w, gw->h, SELFCHECK_POINTS, 0xC0FFEE123ULL ^ (uint64_t)w->seeds.climate, &unres, why, sizeof why);
        if (mism != 0) { G.api.biome_free(gw->h); gw->h = NULL; if (mism > 0) { char t[300]; snprintf(t, sizeof t, "самопроверка GPU=CPU провалена (%d из %d): %s", mism, SELFCHECK_POINTS, why); snprintf(why, sizeof why, "%s", t); } rc = -1; }
    }
    if (rc == 0) { gw->state = 1; G.worlds_ok++; }
    else { gw->state = -1; snprintf(gw->why, sizeof gw->why, "%s", why); G.worlds_failed++; snprintf(G.last_fallback, sizeof G.last_fallback, "мир: %s", why); }
    gunlock();
    return gw;
}
void gpu_world_free(McWorld *w) {
    GpuWorld *gw = w->gpu;
    if (!gw) return;
    glock();
    if (gw->h && G.lib) G.api.biome_free(gw->h);
    gunlock();
    free(gw); w->gpu = NULL;
}

/* ---------------------------------------------------------------- сетка биомов */
enum { AUTO_MIN_SAMPLES = 16384 };

int gpu_try_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out, int force) {
    int mode = G.mode;
    if (mode == MCGEN_COMPUTE_CPU && !force) return -1;
    if (!force && mode == MCGEN_COMPUTE_AUTO && (long)nx * (long)nz < AUTO_MIN_SAMPLES) return -1;
    glock(); int ok = gpu_device_locked(); if (!ok) set_fallback("%s", G.load_err); gunlock();
    if (!ok) return -1;
    GpuWorld *gw = gpu_world_get(w);
    if (gw->state <= 0) return -1;
    int unres = 0;
    if (G.api.biome_grid(gw->h, x0, z0, nx, nz, step, y, out, &unres)) {
        glock(); set_fallback("ошибка GPU: %s", G.api.last_error()); gunlock();
        return -1;
    }
    if (unres > 0) {
        for (int iz = 0; iz < nz; iz++) for (int ix = 0; ix < nx; ix++) {
            size_t i = (size_t)iz * nx + ix;
            if (out[i] == 255) { int b = mcgen_biome_at(w, x0 + ix * step, y, z0 + iz * step); out[i] = (uint8_t)(b < 0 ? 0 : b); }
        }
    }
    return 0;
}
int mcgen_gpu_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out) {
    if (!w || !out || nx <= 0 || nz <= 0 || step <= 0) return MCGEN_E_ARG;
    return gpu_try_biome_grid(w, x0, z0, nx, nz, step, y, out, 1) == 0 ? MCGEN_OK : MCGEN_E_UNSUPPORTED;
}
/* точки: GPU, недостающие — CPU. *n_cpu — сколько точек GPU отдал на CPU */
int mcgen_gpu_biome_points(const McWorld *w, int n, const int *xyz, uint8_t *out, int *n_cpu) {
    if (!w || !out || n < 0 || (n && !xyz)) return MCGEN_E_ARG;
    glock(); int ok = gpu_device_locked(); gunlock();
    if (!ok) return MCGEN_E_UNSUPPORTED;
    GpuWorld *gw = gpu_world_get(w);
    if (gw->state <= 0) return MCGEN_E_UNSUPPORTED;
    int unres = 0;
    if (G.api.biome_points(gw->h, n, xyz, out, &unres)) return MCGEN_E_INTERNAL;
    if (n_cpu) *n_cpu = unres;
    if (unres) for (int i = 0; i < n; i++) if (out[i] == 255) { int b = mcgen_biome_at(w, xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]); out[i] = (uint8_t)(b < 0 ? 0 : b); }
    return MCGEN_OK;
}
/* причина отказа GPU для мира (для тестов/аддона); пустая строка — мир годен */
const char *gpu_world_why(const McWorld *w) { GpuWorld *gw = w->gpu; return gw && gw->state < 0 ? gw->why : ""; }

/* ---------------------------------------------------------------- самопроверка по версии */
/* Создаёт тестовые миры (Overworld normal/large_biomes/amplified, Nether, End) и сравнивает npoints случайных точек каждого
 * на GPU и CPU. 0 — совпало везде (результат запоминается), иначе MCGEN_E_INTERNAL; report — подробности. */
int mcgen_gpu_selftest(McGen *g, int npoints, char *report, size_t replen) {
    if (!g) return MCGEN_E_ARG;
    if (npoints < 256) npoints = 256;
    char rep[1500]; rep[0] = 0; size_t rl = 0;
    glock(); int ok = gpu_device_locked(); gunlock();
    if (!ok) { snprintf(rep, sizeof rep, "GPU недоступно: %s", G.load_err); if (report && replen) snprintf(report, replen, "%s", rep); return MCGEN_E_UNSUPPORTED; }
    static const char *DIMS[5][2] = { { "minecraft:overworld", "normal" }, { "minecraft:overworld", "large_biomes" }, { "minecraft:overworld", "amplified" },
                                      { "minecraft:the_nether", "normal" }, { "minecraft:the_end", "normal" } };
    static const int64_t SEEDS[2] = { 12345, -4172144997902289642LL };
    int total_bad = 0, total_pts = 0, tested = 0;
    for (int d = 0; d < 5; d++) {
        for (int si = 0; si < 2; si++) {
            McWorld *w; char err[300];
            McSeeds sd = mcgen_seeds_unified(SEEDS[si]);
            if (mcgen_world_new(g, DIMS[d][0], DIMS[d][1], &sd, NULL, 0, &w, err, sizeof err)) continue;   /* нет такого пресета в этой версии */
            GpuWorld *gw = gpu_world_get(w);
            int bad = 0, unres = 0;
            char why[300] = {0};
            if (gw->state <= 0) { bad = -1; snprintf(why, sizeof why, "%s", gw->why); }
            else bad = compare_points(w, gw->h, npoints, 0x1234567ULL + (uint64_t)(d * 7 + si), &unres, why, sizeof why);
            tested++;
            if (bad != 0) total_bad += bad < 0 ? 1 : bad; else total_pts += npoints;
            if (bad != 0 && rl + 200 < sizeof rep) rl += (size_t)snprintf(rep + rl, sizeof rep - rl, "%s/%s seed %lld: %s\n", DIMS[d][0] + 10, DIMS[d][1], (long long)SEEDS[si], why[0] ? why : "расхождения");
            mcgen_world_free(w);
        }
    }
    glock();
    if (tested == 0) { snprintf(G.selftest, sizeof G.selftest, "нет тестовых миров"); G.selftest_state = -1; }
    else if (total_bad == 0) { snprintf(G.selftest, sizeof G.selftest, "пройдена: %d миров, %d точек, 0 расхождений", tested, total_pts); G.selftest_state = 1; }
    else { snprintf(G.selftest, sizeof G.selftest, "ПРОВАЛЕНА: %d расхождений/отказов", total_bad); G.selftest_state = -1; }
    snprintf(rep + rl, sizeof rep - rl, "%s", G.selftest);
    gunlock();
    if (report && replen) snprintf(report, replen, "%s", rep);
    return G.selftest_state > 0 ? MCGEN_OK : MCGEN_E_INTERNAL;
}

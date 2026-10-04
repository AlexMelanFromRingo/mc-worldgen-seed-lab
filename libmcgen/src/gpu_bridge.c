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
#include "mcgen_tweaks_table.h"
#ifdef _WIN32
#  define SHORT_SLEEP() Sleep(1)
#else
#  include <time.h>
#  define SHORT_SLEEP() do { struct timespec ts_ = { 0, 150000 }; nanosleep(&ts_, NULL); } while (0)
#endif
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
    /* TERRAIN (необязательные символы) */
    void *(*terrain_new)(const McgTerrainWorld *, int, char *, size_t);
    void (*terrain_free)(void *);
    int (*terrain_max_chunks)(void *);
    size_t (*terrain_cell_floats)(void *);
    int (*terrain_cell_info)(void *, int, int *, int *, size_t *, size_t *);
    int (*terrain_cell_cand)(void *, int, int, int *, int *, int *, int *);
    void *(*host_alloc)(size_t);
    void (*host_free)(void *);
    int (*terrain_batch)(void *, int, const int *, const int *, McgBeardFn, void *, float *, unsigned short *, float *, signed char *);
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

static atomic_int g_env_done = 0;
/* режим вычислений: переменная окружения MCGEN_COMPUTE=cpu|gpu|auto (для CLI и тестов) задаёт значение по умолчанию, пока не вызван mcgen_gpu_set_compute */
static int gmode(void) {
    if (!atomic_load(&g_env_done)) {
        atomic_store(&g_env_done, 1);
        const char *e = getenv("MCGEN_COMPUTE");
        if (e) G.mode = !strcmp(e, "cpu") ? MCGEN_COMPUTE_CPU : !strcmp(e, "gpu") ? MCGEN_COMPUTE_GPU : MCGEN_COMPUTE_AUTO;
    }
    return G.mode;
}
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
    *(void **)&G.api.terrain_new = lib_sym(lib, "mcgpu_terrain_new"); *(void **)&G.api.terrain_free = lib_sym(lib, "mcgpu_terrain_free");
    *(void **)&G.api.terrain_max_chunks = lib_sym(lib, "mcgpu_terrain_max_chunks"); *(void **)&G.api.terrain_cell_floats = lib_sym(lib, "mcgpu_terrain_cell_floats");
    *(void **)&G.api.terrain_cell_info = lib_sym(lib, "mcgpu_terrain_cell_info"); *(void **)&G.api.terrain_cell_cand = lib_sym(lib, "mcgpu_terrain_cell_cand"); *(void **)&G.api.host_alloc = lib_sym(lib, "mcgpu_host_alloc");
    *(void **)&G.api.host_free = lib_sym(lib, "mcgpu_host_free"); *(void **)&G.api.terrain_batch = lib_sym(lib, "mcgpu_terrain_batch");
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
    atomic_store(&g_env_done, 1); G.mode = mode;
    if (device >= 0 && device != G.device) {
        if (G.dev_selected) { gunlock(); return MCGEN_E_UNSUPPORTED; }   /* устройство уже занято; смена — только до первого использования */
        G.device = device;
    }
    gunlock();
    return MCGEN_OK;
}
int mcgen_gpu_get_compute(void) { return gmode(); }

/* статус: строка из строк «ключ: значение»; возврат 1 — GPU готово к работе */
int mcgen_gpu_status(char *buf, size_t buflen) {
    glock();
    int gm = gmode();
    const char *mode = gm == MCGEN_COMPUTE_CPU ? "CPU" : gm == MCGEN_COMPUTE_GPU ? "GPU" : "Auto";
    int avail = 0;
    char line[2048]; line[0] = 0;
    int ok = gm == MCGEN_COMPUTE_CPU ? 0 : gpu_device_locked();
    if (gm == MCGEN_COMPUTE_CPU) {
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

typedef struct GpuTerrainWorld GpuTerrainWorld;
typedef struct TSvc TSvc;
typedef struct {
    void *h;                       /* дескриптор GPU-мира (mcgpu_biome_new) */
    int state;                     /* 1 — проверен, годен; −1 — отказ (причина в why) */
    char why[300];
    GpuTerrainWorld *tw;           /* рельеф (создаётся лениво при первом регионе с TERRAIN) */
    int tstate;                    /* 0 — не пробовали, 1 — готов, −1 — отказ (twhy) */
    char twhy[300];
    TSvc *svc;                     /* служба опережающих пакетов активного региона */
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
    if (g->newf) { if (gpu_export_new(roots, 6, 1, prog)) { snprintf(why, whylen, "экспорт программы (26.3+) не удался"); return -1; } }
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
static GpuWorld *gpu_world_shell(McWorld *w) {     /* оболочка состояния (под замком вызывающего) */
    if (!w->gpu) w->gpu = xcalloc(1, sizeof(GpuWorld));
    return w->gpu;
}
static GpuWorld *gpu_world_get(const McWorld *cw) {
    McWorld *w = (McWorld *)cw;
    glock();
    GpuWorld *gw = gpu_world_shell(w);
    if (gw->h || gw->state) { gunlock(); return gw; }
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
static void terrain_world_free(GpuWorld *gw);
void gpu_world_free(McWorld *w) {
    GpuWorld *gw = w->gpu;
    if (!gw) return;
    gpu_terrain_end(w);
    glock();
    terrain_world_free(gw);
    if (gw->h && G.lib) G.api.biome_free(gw->h);
    gunlock();
    free(gw); w->gpu = NULL;
}

/* ---------------------------------------------------------------- сетка биомов */
enum { AUTO_MIN_SAMPLES = 16384 };

int gpu_try_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out, int force) {
    int mode = gmode();
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

/* ======================================================================================================================
 *  TERRAIN на GPU (26.3+): плотность final_density и жилы руд считаются пакетами на видеокарте с опережением; CPU продолжает
 *  aquifer, раскладку блоков и всё остальное. Результат побитово равен CPU (см. gpu/mcgpu_terrain.cu и tests/g9_terrain.c).
 * ==================================================================================================================== */
typedef struct Beard Beard;
Beard *beard_for_chunk(McWorld *w, int cx, int cz);
void beard_free(Beard *b);
void beard_volume(const Beard *b, float *out, const Vol *v);
int veins_gpu_info(const McWorld *w, const S **dens, const S **rich, const S **gap, int *ore, int *raw, int *filler, float *raw_chance);

struct GpuTerrainWorld {
    void *h; int nmin, nh, nd; int veins_on; int Bmax; size_t cell_floats;
    int ncell;
    struct { int cid; int ncand; size_t cap, off; struct { int vol[9]; int xrel, zrel, n; } cand[8]; } cell[32];
};
static void terrain_world_free(GpuWorld *gw) {
    GpuTerrainWorld *tw = gw->tw;
    if (!tw) return;
    if (tw->h && G.lib && G.api.terrain_free) G.api.terrain_free(tw->h);
    free(tw); gw->tw = NULL;
}
/* создать GPU-описание рельефа мира (под замком вызывающего). 0 — готово; иначе −1, причина в why */
static int terrain_prepare(McWorld *w, GpuWorld *gw, char *why, size_t wl) {
    const McGen *g = w->g;
    if (!g->newf) { snprintf(why, wl, "рельеф 26.1/26.2 на GPU не реализован (считается на CPU)"); return -1; }
    if (!G.api.terrain_new || !G.api.terrain_batch || !G.api.host_alloc) { snprintf(why, wl, "libmcgen_cuda без поддержки рельефа"); return -1; }
    if (!w->s_rf[RF_FINAL_DENSITY]) { snprintf(why, wl, "нет final_density"); return -1; }
    int nmin = w->ns->min_y > w->min_y ? w->ns->min_y : w->min_y;
    int ntop = w->ns->min_y + w->ns->height; if (ntop > w->min_y + w->height) ntop = w->min_y + w->height;
    int nh = ntop - nmin;
    if (nh <= 0) { snprintf(why, wl, "пустой объём шума"); return -1; }
    const S *vd[8], *vr[8], *vg[8]; int ore[8], raw[8], fil[8]; float rc[8];
    int nv = veins_gpu_info(w, vd, vr, vg, ore, raw, fil, rc);
    int veins_on = nv > 0 && nv <= MCG_MAX_VEINS && w->tweak[MCGEN_TWEAK_ORE_VEINS] != 0.0 && nmin == w->min_y && !w->ore_pos.legacy && g->nstates < 65535;
    const S *roots[MCG_MAX_ROOTS]; int nr = 0;
    roots[nr++] = w->s_rf[RF_FINAL_DENSITY];
    if (veins_on) for (int i = 0; i < nv; i++) { roots[nr++] = vd[i]; roots[nr++] = vr[i]; }
    int pre_root = -1;
    if (w->ns->has_aquifers && w->tweak[MCGEN_TWEAK_AQUIFERS] != 0.0 && w->s_aq[AQ_SURFACE_LEVEL]) { pre_root = nr; roots[nr++] = w->s_aq[AQ_SURFACE_LEVEL]; }
    McgProg vol, gap; memset(&gap, 0, sizeof gap);
    gpu_export_new(roots, nr, 0, &vol);
    if (veins_on) gpu_export_new(vg, nv, 1, &gap);
    McgTerrainWorld T; memset(&T, 0, sizeof T);
    T.vol = &vol; T.gap = veins_on ? &gap : NULL; T.nmin = nmin; T.nh = nh; T.root_density = 0; T.veins_on = veins_on; T.nveins = veins_on ? nv : 0;
    for (int i = 0; veins_on && i < nv; i++) {
        T.vein[i].density_root = 1 + 2 * i; T.vein[i].richness_root = 2 + 2 * i; T.vein[i].gap_root = i;
        T.vein[i].ore = ore[i]; T.vein[i].raw = raw[i]; T.vein[i].filler = fil[i]; T.vein[i].raw_chance = rc[i];
    }
    T.ore_lo = w->ore_pos.lo; T.ore_hi = w->ore_pos.hi;
    for (int k = 0; k < AQ__COUNT; k++) if (w->s_aq[k]) gpu_collect_cache_ids(w->s_aq[k], T.cache_ids, 32, &T.ncache_ids);
    T.pre_root = pre_root;
    if (pre_root >= 0) { T.pre_sx = 11; T.pre_sy = 1; T.pre_sz = 11; T.pre_dx = 4; T.pre_dy = 1; T.pre_dz = 4; T.pre_y0 = 0; T.pre_xoff = -16; T.pre_zoff = -16; }
    char err[300] = {0};
    void *h = G.api.terrain_new(&T, 64, err, sizeof err);
    gpu_prog_free(&vol); gpu_prog_free(&gap);
    if (!h) { snprintf(why, wl, "GPU: %s", err); return -1; }
    GpuTerrainWorld *tw = xcalloc(1, sizeof *tw);
    tw->h = h; tw->nmin = nmin; tw->nh = nh; tw->nd = 16 * nh * 16; tw->veins_on = veins_on; tw->Bmax = G.api.terrain_max_chunks(h);
    tw->cell_floats = G.api.terrain_cell_floats(h);
    for (int k = 0; k < T.ncache_ids; k++) {
        int cid, nc; size_t cap, off;
        if (G.api.terrain_cell_info(h, k, &cid, &nc, &cap, &off)) continue;
        if (nc > 8) { G.api.terrain_free(h); free(tw); snprintf(why, wl, "у кэша слишком много объёмов-кандидатов (%d)", nc); return -1; }
        tw->cell[k].cid = cid; tw->cell[k].ncand = nc; tw->cell[k].cap = cap; tw->cell[k].off = off;
        for (int j = 0; j < nc; j++) G.api.terrain_cell_cand(h, k, j, tw->cell[k].cand[j].vol, &tw->cell[k].cand[j].xrel, &tw->cell[k].cand[j].zrel, &tw->cell[k].cand[j].n);
    }
    tw->ncell = T.ncache_ids;
    gw->tw = tw;
    return 0;
}
/* состояние рельефа мира (под замком) */
static GpuTerrainWorld *terrain_world(McWorld *w, GpuWorld *gw) {
    if (gw->tstate > 0) return gw->tw;
    if (gw->tstate < 0) return NULL;
    char why[300] = {0};
    if (terrain_prepare(w, gw, why, sizeof why) == 0) { gw->tstate = 1; return gw->tw; }
    gw->tstate = -1; snprintf(gw->twhy, sizeof gw->twhy, "%s", why);
    snprintf(G.last_fallback, sizeof G.last_fallback, "рельеф: %s", why);
    return NULL;
}
int gpu_terrain_dims(McWorld *w, int *nmin, int *nh, int *max_chunks) {
    glock();
    int ok = gpu_device_locked();
    GpuTerrainWorld *tw = NULL;
    if (ok) { GpuWorld *gw = gpu_world_shell(w); tw = terrain_world(w, gw); }
    if (tw) { if (nmin) *nmin = tw->nmin; if (nh) *nh = tw->nh; if (max_chunks) *max_chunks = tw->Bmax; }
    gunlock();
    return tw ? 0 : -1;
}

typedef struct { Beard **bd; } BeardCtx;
static int beard_cb(void *ud, int ci, const int vol[9], float *out) {
    BeardCtx *c = ud;
    if (!c->bd[ci]) return 0;
    Vol v = { vol[0], vol[1], vol[2], vol[3], vol[4], vol[5], vol[6], vol[7], vol[8] };
    beard_volume(c->bd[ci], out, &v);
    return 1;
}
/* пакет напрямую (блокирующий): тесты и разовые запросы */
int gpu_terrain_batch_raw(McWorld *w, int n, const int *cx, const int *cz, float *dens, uint16_t *veins, float *cells, signed char *which, char *why, size_t whylen) {
    glock();
    int ok = gpu_device_locked();
    GpuTerrainWorld *tw = NULL;
    if (ok) { GpuWorld *gw = gpu_world_shell(w); tw = terrain_world(w, gw); if (!tw) snprintf(why, whylen, "%s", gw->twhy); }
    else snprintf(why, whylen, "%s", G.load_err);
    gunlock();
    if (!tw) return -1;
    Beard **bd = xcalloc((size_t)n, sizeof(Beard *));
    if (w->struct_on) for (int i = 0; i < n; i++) bd[i] = beard_for_chunk(w, cx[i], cz[i]);
    BeardCtx bc = { bd };
    int rc = 0;
    for (int o = 0; o < n && !rc; o += tw->Bmax) {
        int m = n - o < tw->Bmax ? n - o : tw->Bmax;
        BeardCtx bc2 = { bd + o };
        rc = G.api.terrain_batch(tw->h, m, cx + o, cz + o, beard_cb, &bc2, dens + (size_t)o * tw->nd, veins ? veins + (size_t)o * tw->nd : NULL, cells ? cells + (size_t)o * tw->cell_floats : NULL, which ? which + (size_t)o * tw->ncell : NULL);
        if (rc) snprintf(why, whylen, "ошибка GPU: %s", G.api.last_error());
    }
    (void)bc;
    for (int i = 0; i < n; i++) if (bd[i]) beard_free(bd[i]);
    free(bd);
    return rc ? -1 : 0;
}

/* ---- служба опережающих пакетов региона ---- */
typedef struct {
    atomic_int batch_id;                /* какой пакет лежит в слоте (−1 — свободен) */
    atomic_int state;                   /* 0 свободен, 1 считается, 2 готов, 3 отказ */
    atomic_int consumed;
    int count;
    float *dens; uint16_t *veins; float *cells; signed char *which;
} TSlot;
struct TSvc {
    McWorld *w; GpuTerrainWorld *tw;
    int total, nreg;                    /* всего чанков плана; из них — внутри региона */
    int *pcx, *pcz;                     /* план: индекс → чанк */
    int *hkey_x, *hkey_z, *hidx; int hcap;
    int B, nslots; TSlot *slot;
    McThread *thr; atomic_int stop; atomic_int failed;
    double t_begin, prod_busy, prod_wait; atomic_long wait_us; atomic_int nacq;      /* статистика (MCGEN_GPU_DEBUG) */
};
static int svc_find(const TSvc *s, int cx, int cz) {
    unsigned h = ((unsigned)cx * 73856093u) ^ ((unsigned)cz * 19349663u);
    for (int k = 0; k < s->hcap; k++) {
        int i = (int)((h + (unsigned)k) % (unsigned)s->hcap);
        if (s->hidx[i] < 0) return -1;
        if (s->hkey_x[i] == cx && s->hkey_z[i] == cz) return s->hidx[i];
    }
    return -1;
}
static void svc_producer(void *arg) {
    TSvc *s = arg;
    McWorld *w = s->w; GpuTerrainWorld *tw = s->tw;
    int nb = (s->total + s->B - 1) / s->B;
    int *cx = xmalloc(sizeof(int) * (size_t)s->B), *cz = xmalloc(sizeof(int) * (size_t)s->B);
    Beard **bd = xcalloc((size_t)s->B, sizeof(Beard *));
    for (int b = 0; b < nb && !atomic_load(&s->stop); b++) {
        TSlot *sl = &s->slot[b % s->nslots];
        double tw0 = now_sec();
        while (!atomic_load(&s->stop) && atomic_load(&sl->state) != 0) SHORT_SLEEP();      /* ждём освобождения слота предыдущим пакетом */
        s->prod_wait += now_sec() - tw0;
        if (atomic_load(&s->stop)) break;
        int cnt = s->total - b * s->B < s->B ? s->total - b * s->B : s->B;
        for (int i = 0; i < cnt; i++) { cx[i] = s->pcx[b * s->B + i]; cz[i] = s->pcz[b * s->B + i]; bd[i] = w->struct_on ? beard_for_chunk(w, cx[i], cz[i]) : NULL; }
        atomic_store(&sl->batch_id, b); atomic_store(&sl->consumed, 0); sl->count = cnt; atomic_store(&sl->state, 1);
        BeardCtx bc = { bd };
        double tb0 = now_sec();
        int rc = G.api.terrain_batch(tw->h, cnt, cx, cz, beard_cb, &bc, sl->dens, tw->veins_on ? sl->veins : NULL, tw->cell_floats ? sl->cells : NULL, tw->cell_floats ? sl->which : NULL);
        s->prod_busy += now_sec() - tb0;
        for (int i = 0; i < cnt; i++) if (bd[i]) { beard_free(bd[i]); bd[i] = NULL; }
        if (rc) { atomic_store(&s->failed, 1); snprintf(G.last_fallback, sizeof G.last_fallback, "рельеф: ошибка GPU: %s", G.api.last_error()); }
        atomic_store(&sl->state, rc ? 3 : 2);
    }
    free(cx); free(cz); free(bd);
}
int gpu_terrain_begin(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int with_ring) {
    if (!(stages & MC_STAGE_TERRAIN) || gmode() == MCGEN_COMPUTE_CPU) return 0;
    if (w->g->newf == 0) return 0;
    glock();
    int ok = gpu_device_locked();
    GpuWorld *gw = NULL; GpuTerrainWorld *tw = NULL;
    if (ok) { gw = gpu_world_shell(w); tw = terrain_world(w, gw); }
    else set_fallback("%s", G.load_err);
    int already = gw && gw->svc;
    gunlock();
    if (!tw || already) return 0;
    long nreg = (long)nx * nz;
    if (gmode() == MCGEN_COMPUTE_AUTO && nreg < 64) return 0;            /* мелкие области — на CPU (накладные расходы) */
    TSvc *s = xcalloc(1, sizeof *s);
    s->w = w; s->tw = tw; s->nreg = (int)nreg;
    int ring = with_ring ? 2 * (nx + 2) + 2 * nz : 0;
    s->total = (int)nreg + ring;
    s->pcx = xmalloc(sizeof(int) * (size_t)s->total); s->pcz = xmalloc(sizeof(int) * (size_t)s->total);
    int k = 0;
    for (int iz = 0; iz < nz; iz++) for (int ix = 0; ix < nx; ix++) { s->pcx[k] = cx0 + ix; s->pcz[k] = cz0 + iz; k++; }
    if (with_ring) for (int cz = cz0 - 1; cz <= cz0 + nz; cz++) for (int cx = cx0 - 1; cx <= cx0 + nx; cx++) {
        if (cx >= cx0 && cx < cx0 + nx && cz >= cz0 && cz < cz0 + nz) continue;
        s->pcx[k] = cx; s->pcz[k] = cz; k++;
    }
    s->total = k;
    s->hcap = s->total * 2 + 7;
    s->hkey_x = xmalloc(sizeof(int) * (size_t)s->hcap); s->hkey_z = xmalloc(sizeof(int) * (size_t)s->hcap); s->hidx = xmalloc(sizeof(int) * (size_t)s->hcap);
    for (int i = 0; i < s->hcap; i++) s->hidx[i] = -1;
    for (int i = 0; i < s->total; i++) {
        unsigned h = ((unsigned)s->pcx[i] * 73856093u) ^ ((unsigned)s->pcz[i] * 19349663u);
        for (int t = 0; t < s->hcap; t++) { int j = (int)((h + (unsigned)t) % (unsigned)s->hcap); if (s->hidx[j] < 0) { s->hidx[j] = i; s->hkey_x[j] = s->pcx[i]; s->hkey_z[j] = s->pcz[i]; break; } }
    }
    s->B = tw->Bmax < 32 ? tw->Bmax : 32; if (s->B < 1) s->B = 1;
    s->nslots = 4;
    s->slot = xcalloc((size_t)s->nslots, sizeof(TSlot));
    for (int i = 0; i < s->nslots; i++) {
        atomic_init(&s->slot[i].batch_id, -1); atomic_init(&s->slot[i].state, 0); atomic_init(&s->slot[i].consumed, 0);
        s->slot[i].dens = G.api.host_alloc(sizeof(float) * (size_t)s->B * (size_t)tw->nd);
        s->slot[i].veins = tw->veins_on ? G.api.host_alloc(sizeof(uint16_t) * (size_t)s->B * (size_t)tw->nd) : NULL;
        s->slot[i].cells = tw->cell_floats ? G.api.host_alloc(sizeof(float) * (size_t)s->B * tw->cell_floats) : NULL;
        s->slot[i].which = xcalloc((size_t)s->B * (size_t)(tw->ncell ? tw->ncell : 1), 1);
        if (!s->slot[i].dens || (tw->veins_on && !s->slot[i].veins) || (tw->cell_floats && !s->slot[i].cells)) {
            for (int j = 0; j <= i; j++) { G.api.host_free(s->slot[j].dens); G.api.host_free(s->slot[j].veins); G.api.host_free(s->slot[j].cells); free(s->slot[j].which); }
            free(s->slot); free(s->pcx); free(s->pcz); free(s->hkey_x); free(s->hkey_z); free(s->hidx); free(s);
            glock(); set_fallback("%s", "не удалось выделить закреплённую память"); gunlock();
            return 0;
        }
    }
    atomic_init(&s->stop, 0); atomic_init(&s->failed, 0); atomic_init(&s->wait_us, 0); atomic_init(&s->nacq, 0); s->t_begin = now_sec();
    glock(); gw->svc = s; gunlock();
    s->thr = thread_start(svc_producer, s);
    return 1;
}
void gpu_terrain_end(McWorld *w) {
    GpuWorld *gw = w->gpu;
    if (!gw || !gw->svc) return;
    TSvc *s = gw->svc;
    atomic_store(&s->stop, 1);
    thread_join(s->thr);
    if (getenv("MCGEN_GPU_DEBUG")) fprintf(stderr, "[gpu] регион: %d чанков в плане, %.2f с всего; GPU занято %.2f с, ожидание слота %.2f с; ожидание рабочих потоков данных GPU %.2f с (%d запросов)\n", s->total, now_sec() - s->t_begin, s->prod_busy, s->prod_wait, atomic_load(&s->wait_us) * 1e-6, atomic_load(&s->nacq));
    for (int i = 0; i < s->nslots; i++) { G.api.host_free(s->slot[i].dens); G.api.host_free(s->slot[i].veins); G.api.host_free(s->slot[i].cells); free(s->slot[i].which); }
    free(s->slot); free(s->pcx); free(s->pcz); free(s->hkey_x); free(s->hkey_z); free(s->hidx); free(s);
    glock(); gw->svc = NULL; gunlock();
}
int gpu_terrain_acquire(McWorld *w, int cx, int cz, GpuChunk *out) {
    GpuWorld *gw = w->gpu;
    TSvc *s = gw ? gw->svc : NULL;
    if (!s || atomic_load(&s->failed)) return 0;
    int i = svc_find(s, cx, cz);
    if (i < 0) return 0;
    int b = i / s->B;
    TSlot *sl = &s->slot[b % s->nslots];
    long long tw0 = (long long)(now_sec() * 1e6); int waited = 0;
    atomic_fetch_add(&s->nacq, 1);
    for (;;) {
        if (atomic_load(&s->stop)) return 0;
        int bid = atomic_load(&sl->batch_id), st = atomic_load(&sl->state);
        if (bid == b && st >= 2) {
            if (waited) atomic_fetch_add(&s->wait_us, (long)((long long)(now_sec() * 1e6) - tw0));
            if (st == 3) return 0;
            int k = i - b * s->B;
            GpuTerrainWorld *tw = s->tw;
            out->dens = sl->dens + (size_t)k * tw->nd;
            out->veins = sl->veins ? sl->veins + (size_t)k * tw->nd : NULL;
            out->cells = sl->cells ? sl->cells + (size_t)k * tw->cell_floats : NULL;
            out->which = sl->which ? sl->which + (size_t)k * (size_t)tw->ncell : NULL;
            out->slot = sl;
            for (int q = 0; q < tw->ncell; q++) if (out->which[q] == -2) {       /* состояние кэшей CPU неизвестно: чанк считает CPU */
                GpuChunk tmp = *out; gpu_terrain_release(w, &tmp); out->slot = NULL; return 0;
            }
            return 1;
        }
        waited = 1; SHORT_SLEEP();
    }
}
void gpu_terrain_release(McWorld *w, GpuChunk *c) {
    (void)w;
    if (!c || !c->slot) return;
    TSlot *sl = c->slot; c->slot = NULL;
    if (atomic_fetch_add(&sl->consumed, 1) + 1 == sl->count) { atomic_store(&sl->batch_id, -1); atomic_store(&sl->state, 0); }
}
void gpu_terrain_apply_veins(McWorld *w, const GpuChunk *c, uint16_t *blocks) {
    GpuTerrainWorld *tw = w->gpu ? ((GpuWorld *)w->gpu)->tw : NULL;
    if (!tw || !c->veins) return;
    const McGen *g = w->g;
    for (int yy = 0; yy < tw->nh; yy++) {
        const uint16_t *pr = c->veins + (size_t)yy * 256;
        uint16_t *br = blocks + (size_t)(tw->nmin - w->min_y + yy) * 256;
        for (int k = 0; k < 256; k++) {
            uint16_t p = pr[k];
            if (p == 0xFFFF) continue;
            int st = br[k];
            if (gen_is_air(g, st) || (g->state_cls[st] & 4)) continue;     /* воздух или жидкость */
            br[k] = p;
        }
    }
}
void gpu_terrain_inject_cells(McWorld *w, SCtx *x, int cx, int cz, const GpuChunk *c) {
    GpuTerrainWorld *tw = w->gpu ? ((GpuWorld *)w->gpu)->tw : NULL;
    if (!tw || !c->cells || !c->which) return;
    for (int k = 0; k < tw->ncell; k++) {
        int j = c->which[k];
        if (j < 0) continue;
        const int *vo = tw->cell[k].cand[j].vol;
        Vol v = { vo[0], vo[1], vo[2], tw->cell[k].cand[j].xrel ? cx * 16 + vo[3] : vo[3], vo[4], tw->cell[k].cand[j].zrel ? cz * 16 + vo[5] : vo[5], vo[6], vo[7], vo[8] };
        gpu_sctx_set_cell(x, tw->cell[k].cid, &v, c->cells + tw->cell[k].off);
    }
}

/* тесты: ячейка кэша k — номер и объёмы-кандидаты */
int gpu_terrain_cells_info(McWorld *w, int *n, size_t *cell_floats) {
    GpuTerrainWorld *tw = w->gpu ? ((GpuWorld *)w->gpu)->tw : NULL;
    if (!tw) return -1;
    *n = tw->ncell; *cell_floats = tw->cell_floats;
    return 0;
}
int gpu_terrain_cell_cand(McWorld *w, int k, int j, int *cid, int vol9[9], int *xrel, int *zrel, int *cnt, size_t *off) {
    GpuTerrainWorld *tw = w->gpu ? ((GpuWorld *)w->gpu)->tw : NULL;
    if (!tw || k < 0 || k >= tw->ncell || j < 0 || j >= tw->cell[k].ncand) return -1;
    *cid = tw->cell[k].cid; memcpy(vol9, tw->cell[k].cand[j].vol, 9 * sizeof(int)); *xrel = tw->cell[k].cand[j].xrel; *zrel = tw->cell[k].cand[j].zrel; *cnt = tw->cell[k].cand[j].n; *off = tw->cell[k].off;
    return 0;
}

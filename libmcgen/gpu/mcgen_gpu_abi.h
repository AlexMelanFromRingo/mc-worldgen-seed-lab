/* mcgen_gpu_abi.h — ABI между libmcgen (src/gpu_bridge.c) и необязательной библиотекой libmcgen_cuda (gpu/*.cu).
 *
 * libmcgen_cuda НЕ линкуется с libmcgen: ядро «выгружает» в неё плоские POD-описания (программы density-функций,
 * таблицы октав шума, R-дерево биомов), а библиотека считает их на видеокарте. Всё численное воспроизводит CPU-код
 * побитно (nvcc --fmad=false, строгий IEEE; те же ветки float/double, что в libmcgen).
 *
 * Чистый C11 (include и из C, и из CUDA). Менять — только с повышением MCGPU_ABI.
 */
#ifndef MCGEN_GPU_ABI_H
#define MCGEN_GPU_ABI_H
#include <stdint.h>
#include <stddef.h>

#define MCGPU_ABI 2

/* ---- виды узлов «новой» ветки (26.3+, float): те же значения, что K_* в src/df_new.c (проверяется static_assert там) ---- */
enum {
    MCGK_CONST, MCGK_NOISE, MCGK_NOISE_XZ, MCGK_NOISE_XYZ, MCGK_SHIFT_B, MCGK_END, MCGK_DIST, MCGK_GRAD_CLAMP, MCGK_GRAD_REPEAT, MCGK_GRAD_MIRROR,
    MCGK_CTX_ALPHA, MCGK_CTX_OFFSET, MCGK_CTX_BEARD,
    MCGK_ABS, MCGK_SQUARE, MCGK_CUBE, MCGK_SQRT, MCGK_LEAKY, MCGK_RECIP, MCGK_NEG, MCGK_SQUEEZE, MCGK_LOG, MCGK_SIGN,
    MCGK_ROUND_INT, MCGK_ROUND,
    MCGK_ADD, MCGK_CADD, MCGK_CSUB, MCGK_SUB, MCGK_MUL, MCGK_CMUL, MCGK_DIV, MCGK_CDIV, MCGK_MIN, MCGK_CMIN, MCGK_MAX, MCGK_CMAX,
    MCGK_POW_CB, MCGK_POW_CE, MCGK_POW,
    MCGK_SPLINE, MCGK_LERP, MCGK_LERP_CF, MCGK_LERP_CS, MCGK_CLAMP, MCGK_RANGE_C, MCGK_RANGE, MCGK_ISEL1, MCGK_ISEL,
    MCGK_CACHE, MCGK_BLEND_DENSITY, MCGK_INTERP, MCGK_SLICE_X, MCGK_SLICE_Y, MCGK_SLICE_Z, MCGK_SLICE_XZ, MCGK_FTS,
    MCGK__COUNT
};
/* ---- виды узлов «старой» ветки (26.1/26.2, double): собственная нумерация (экспортёр в df_old.c переводит) ---- */
enum {
    MCGO_CONST, MCGO_NOISE, MCGO_SHIFTED_NOISE, MCGO_SHIFT_A, MCGO_SHIFT_B, MCGO_SHIFT, MCGO_END_ISLANDS, MCGO_Y_GRADIENT,
    MCGO_WEIRD_SCALED, MCGO_BLEND_ALPHA, MCGO_ZERO /* blend_offset, beardifier */,
    MCGO_ABS, MCGO_SQUARE, MCGO_CUBE, MCGO_HALF_NEG, MCGO_QUARTER_NEG, MCGO_RECIP, MCGO_SQUEEZE,
    MCGO_CLAMP, MCGO_ADD, MCGO_MUL, MCGO_MIN, MCGO_MAX, MCGO_CADD, MCGO_CMUL,
    MCGO_RANGE, MCGO_ISEL, MCGO_SPLINE, MCGO_PASS /* HOLDER и маркеры кэшей в точечном режиме */,
    MCGO__COUNT
};

/* одна октава шума: смещения + таблица перестановок (GNoise / ImprovedNoise) */
typedef struct McgOct { double xo, yo, zo; uint8_t p[256]; } McgOct;

/* слой шума. «Новая» ветка (NStack): a = множитель частоты, amp_f = амплитуда, kind: 0 perlin / 1 smeared, b = fudge.
 * «Старая» (OldPerlin, уровень i): oct = -1, если уровень пуст; a = factor (lowest_in·2^i), b = vf (lowest_val/2^i), amp_d = амплитуда. */
typedef struct McgLayer { double a, b, amp_d; float amp_f; int oct; int kind; int pad; } McgLayer;
typedef struct McgNoise {
    int l0, n0;          /* слои: NStack целиком / первый OldPerlin */
    int l1, n1;          /* старая ветка: второй OldPerlin (NormalNoise = first + second) */
    double value_factor; /* старая ветка */
} McgNoise;

/* сплайн: узел дерева. coord — номер слота координаты (arr[node.arr0 + coord] — узел-координата) */
typedef struct McgSpline {
    int is_const; float value;
    int coord, n;
    int loc0;            /* splf[loc0 .. loc0+n) — loc, splf[loc0+n .. loc0+2n) — der */
    int val0;            /* spc[val0 .. val0+n) — индексы дочерних сплайнов */
} McgSpline;

typedef struct McgNode {
    int k;
    int a, b, c;         /* дочерние узлы (−1 — нет) */
    int ns;              /* индекс McgNoise */
    int sp;              /* индекс корня сплайна */
    int arr0, narr;      /* список узлов (interval_select, координаты сплайна) */
    int thr0, nthr;      /* пороги (в массиве thr, double) */
    int i0, i1, i2, i3;
    float f0, f1, f2, f3;
    double d0, d1;
    int cm, cx, cy, cz;  /* контекст координат (срезы slice): cm — маска зафиксированных осей (1 x, 2 y, 4 z), cx/cy/cz — их значения */
    int oct;             /* END: индекс октавы simplex */
    int cid;             /* номер кэша (CACHE) */
} McgNode;

#define MCG_MAX_ROOTS 16
typedef struct McgProg {
    int old;             /* 0 — float (26.3+), 1 — double (26.1/26.2) */
    int nnodes;          McgNode *nodes;
    int nnoise;          McgNoise *noise;
    int nlayers;         McgLayer *layers;
    int noct;            McgOct *octs;
    int nsp;             McgSpline *sp;
    int nsplf;           float *splf;
    int nspc;            int *spc;
    int narr;            int *arr;
    int nthr;            double *thr;
    int nroots;          int root[MCG_MAX_ROOTS];
    int ncache;
} McgProg;

/* узел R-дерева биомов: дети лежат подряд [first, first+count); лист: count == 0 */
typedef struct McgTreeNode { int32_t lo[7], hi[7]; int32_t first, count, biome, pad; } McgTreeNode;

/* ---- описание мира для сетки биомов ---- */
enum { MCG_BS_MULTI = 0, MCG_BS_END = 1, MCG_BS_FIXED = 2 };
typedef struct McgBiomeWorld {
    int source;              /* MCG_BS_* */
    int fixed_biome;
    int end_ids[5];          /* the_end, end_highlands, end_midlands, small_end_islands, end_barrens (id биомов libmcgen) */
    int newf;                /* 1: float-ветка (26.3+) */
    int min_qy, qh;          /* world_biome_cell: зажим qy в [min_qy, min_qy+qh) */
    int64_t zoom_seed;       /* BiomeManager.obfuscateSeed */
    const McgProg *prog;     /* корни: 0 temperature, 1 vegetation, 2 continents, 3 erosion, 4 depth, 5 ridges */
    int ntree; const McgTreeNode *tree; int tree_root;
} McgBiomeWorld;

/* ---- описание мира для TERRAIN (26.3+): объёмный исполнитель плотности и жилы руд ---- */
#define MCG_MAX_VEINS 4
typedef struct McgVein { int density_root, richness_root, gap_root; int ore, raw, filler; float raw_chance; } McgVein;
typedef struct McgTerrainWorld {
    const McgProg *vol;       /* объёмная программа (bake_ctx = 0): root[0] = final_density, корни жил — индексами в root[] */
    const McgProg *gap;       /* точечная программа «щели» жил (bake_ctx = 1): корни gap_root */
    int nmin, nh;             /* объём шума по y: [nmin, nmin+nh) */
    int root_density;         /* индекс в vol->root[] */
    int nveins; McgVein vein[MCG_MAX_VEINS];
    uint64_t ore_lo, ore_hi;  /* PositionalRandomFactory "minecraft:ore" (Xoroshiro) */
    int veins_on;             /* 1 — считать жилы на GPU */
    int cache_ids[32]; int ncache_ids;   /* номера кэшей, ячейки которых нужны CPU после плотности (aquifer) */
    int pre_root;             /* индекс в vol->root[] функции, объём которой CPU считает ДО плотности (aq surface_level; −1 — нет): нужна для трассы состояния кэшей */
    int pre_sx, pre_sy, pre_sz, pre_dx, pre_dy, pre_dz, pre_y0, pre_xoff, pre_zoff;   /* её объём (x, z — относительно чанка) */
} McgTerrainWorld;
/* колбэк Beardifier: заполняет out объёмом (vol = {sx,sy,sz,x0,y0,z0,dx,dy,dz}, блоковые координаты) для чанка chunk_index батча; 0 — у чанка нет Beardifier */
typedef int (*McgBeardFn)(void *ud, int chunk_index, const int vol[9], float *out);

#endif /* MCGEN_GPU_ABI_H */

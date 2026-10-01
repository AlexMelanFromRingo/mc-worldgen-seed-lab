/* gpu_biomes.h — хостовый API GPU/CPU проверки seed'ов по биомам (над engine/cuda/mc_gpu.cuh). */
#ifndef GPU_BIOMES_H
#define GPU_BIOMES_H
#include "../../../engine/cuda/mcg_host.h"
#include "../../../engine/cuda/mc_gpu.cuh"
#include <vector>
#include <string>

struct GbCtx {
    int version = 0, dim = 0, mode = 0;
    char preset[16] = {0};
    McgHost H;                        /* данные engine (хост) */
    std::vector<McgNode> tree;        /* компактное дерево (хост) */
    McNoiseSpec sp_host[6];           /* спецификации шумов в виде массива (хост) */
    McgParams Ph;                     /* указатели на хост-данные (CPU-режим) */
    McgParams Pd;                     /* указатели на данные устройства (GPU-режим) */
    McClimateSpec *d_clim = nullptr; McNoiseSpec *d_sp = nullptr; McgNode *d_tree = nullptr;
    bool gpu_ready = false;
};

/* version: MC_26_1.., dim: MC_OVERWORLD.., preset: "normal|overworld|amplified|large_biomes". 0 = ок */
int gb_open(GbCtx &c, int version, int dim, const char *preset, bool gpu = true);
void gb_close(GbCtx &c);

/* Режим ties: наблюдение выполнено, если ЛЮБОЙ лист R-дерева с минимальным fitness даёт биом из маски (учёт недетерминизма игры). */
void gb_set_ties(GbCtx &c, bool on);

/* --- точки: out[s*np+p] (u8), out_tg (опц.) 6 i32 на точку. seeds[ns], pts: 3 i32 на точку (qx,qy,qz).
 *     GPU: поток = (seed, кусок точек длиной chunk). Возврат: время ядра, мс. --- */
double gb_points_gpu(GbCtx &c, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out, i32 *out_tg = nullptr, int chunk = 64);
double gb_points_cpu(GbCtx &c, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out, i32 *out_tg = nullptr);   /* тот же код, OpenMP */

extern int gb_block;         /* потоков в блоке ядер (кратно 32, 32..256; GB_BLOCK в окружении переопределяет) */
extern bool gb_progress;     /* печатать прогресс длинных поисков (stderr) */

/* Маски биомов листьев с минимальным fitness (тай-набор) для целей tg (6 i32 на точку), хостовым кодом mcg_rt_ties. masks: 2 u64 на точку. */
void gb_ties_host(GbCtx &c, const i32 *tg, int n, u64 *masks);

/* --- поиск --- */
struct GbSearch {
    std::vector<i64> found;           /* прошедшие все наблюдения */
    std::vector<u64> reached;         /* reached[j] = число seed'ов, прошедших ровно j наблюдений (j=0..nobs) */
    u64 total = 0;
    double ms = 0;                    /* время ядра (GPU) / общее (CPU), мс */
};
/* Источник seed'ов: kind 0 — список (hostList, n); 1 — структурный seed base (48 бит) | i<<48, i<2^16; 2 — base+i, i<n */
struct GbSource { int kind = 0; const i64 *list = nullptr; u64 base = 0; u64 n = 0; };
int gb_search(GbCtx &c, bool gpu, const GbSource &src, const std::vector<McgObs> &obs, GbSearch &res, u32 maxfound = 1 << 20);

/* Оценка селективности наблюдений: доля случайных seed'ов (nsamp штук), дающих биом из маски. Используется для упорядочения. */
void gb_estimate_selectivity(GbCtx &c, std::vector<McgObs> &obs, int nsamp, std::vector<double> &p_out);

/* Разбор наблюдения "qx qy qz biome[|biome...]" -> McgObs (rad для End ставится отдельно: gb_finalize_obs). block=true: координаты блоковые
 * (как в F3; биом по BiomeManager c hashed seed). false при ошибке. */
bool gb_parse_obs_line(const char *line, McgObs &o, std::string &err, bool block = false);
void gb_finalize_obs(const GbCtx &c, std::vector<McgObs> &obs);   /* rad для End */
std::string gb_mask_str(const McgObs &o);

#endif

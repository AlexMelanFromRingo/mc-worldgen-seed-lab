/* gpu_bridge.h — мост libmcgen ↔ необязательная библиотека libmcgen_cuda (поток W7, спека §3.6, ворота G9).
 * Внутренний заголовок (не публичный контракт). Публичные функции mcgen_gpu_* объявлены в include/mcgen.h. */
#ifndef MCGEN_GPU_BRIDGE_H
#define MCGEN_GPU_BRIDGE_H
#include "mcgen_internal.h"
#include "../gpu/mcgen_gpu_abi.h"

/* экспорт программ (реализации включены в конец df_new.c / df_old.c: gpu_export_new.inc / gpu_export_old.inc) */
int gpu_export_new(const S *const *roots, int nroots, int bake_ctx, McgProg *out);
int gpu_export_old(void *oldwire, const int *fields, int nroots, McgProg *out);
void gpu_prog_free(McgProg *p);
void gpu_collect_cache_ids(const S *root, int *ids, int cap, int *n);
void gpu_sctx_set_cell(SCtx *x, int cid, const Vol *v, const float *values);
int gpu_sctx_cache_count(const SCtx *x);
int gpu_sctx_get_cell(const SCtx *x, int cid, Vol *v, const float **values);
int gpu_terrain_cells_info(McWorld *w, int *n, size_t *cell_floats);
int gpu_terrain_cell_cand(McWorld *w, int k, int j, int *cid, int vol9[9], int *xrel, int *zrel, int *cnt, size_t *off);

/* ---- TERRAIN на GPU (26.3+): предвычисление плотности и жил пакетами с опережающей загрузкой (gpu_bridge.c) ---- */
typedef struct GpuChunk {
    const float *dens;           /* final_density: 16 × nh × 16, раскладка Vol (iy + (ix + iz*16)*nh) */
    const uint16_t *veins;       /* заплатки жил по раскладке блоков от nmin: ((y*16+z)*16+x), 0xFFFF — нет (NULL — жилы считает CPU) */
    const float *cells;          /* значения ячеек кэшей */
    const signed char *which;    /* по кэшам: номер объёма-кандидата, оставленного плотностью (−1 — ячейка не выставлена) */
    void *slot;                  /* внутреннее */
} GpuChunk;
/* регион начат: запускает опережающие пакеты, если GPU включено, мир годен и ветка поддержана; возвращает 1, если служба запущена */
int gpu_terrain_begin(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int with_ring);
void gpu_terrain_end(McWorld *w);
/* данные чанка от службы (ждёт готовности пакета); 0 — нет данных (считать на CPU) */
int gpu_terrain_acquire(McWorld *w, int cx, int cz, GpuChunk *out);
void gpu_terrain_release(McWorld *w, GpuChunk *c);
/* заплатки жил → блоки; ячейки кэшей → контекст сэмплеров */
void gpu_terrain_apply_veins(McWorld *w, const GpuChunk *c, uint16_t *blocks);
void gpu_terrain_inject_cells(McWorld *w, SCtx *x, int cx, int cz, const GpuChunk *c);
/* прямой пакетный расчёт (тесты): n чанков → плотность (n × 16·nh·16), заплатки (n × nh·256, может быть NULL), ячейки (может быть NULL); 0 — ок */
int gpu_terrain_batch_raw(McWorld *w, int n, const int *cx, const int *cz, float *dens, uint16_t *veins, float *cells, signed char *which, char *why, size_t whylen);
int gpu_terrain_dims(McWorld *w, int *nmin, int *nh, int *max_chunks);


/* вызывается из mcgen_biome_grid: 0 — сетка посчитана на GPU (недостающие точки — на CPU), иначе −1 → считать на CPU.
 * force: 1 — режим «только GPU» вызван явно (без порога размера). */
int gpu_try_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out, int force);
void gpu_world_free(McWorld *w);
/* сетка/точки на CPU (без GPU) — реализация в region.c */
int mcgen_biome_grid_cpu(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out);

#endif

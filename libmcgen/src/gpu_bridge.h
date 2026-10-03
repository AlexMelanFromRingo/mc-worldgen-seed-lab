/* gpu_bridge.h — мост libmcgen ↔ необязательная библиотека libmcgen_cuda (поток W7, спека §3.6, ворота G9).
 * Внутренний заголовок (не публичный контракт). Публичные функции mcgen_gpu_* объявлены в include/mcgen.h. */
#ifndef MCGEN_GPU_BRIDGE_H
#define MCGEN_GPU_BRIDGE_H
#include "mcgen_internal.h"
#include "../gpu/mcgen_gpu_abi.h"

/* экспорт программ (реализации включены в конец df_new.c / df_old.c: gpu_export_new.inc / gpu_export_old.inc) */
int gpu_export_new(const S *const *roots, int nroots, McgProg *out);
int gpu_export_old(void *oldwire, const int *fields, int nroots, McgProg *out);
void gpu_prog_free(McgProg *p);

/* вызывается из mcgen_biome_grid: 0 — сетка посчитана на GPU (недостающие точки — на CPU), иначе −1 → считать на CPU.
 * force: 1 — режим «только GPU» вызван явно (без порога размера). */
int gpu_try_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out, int force);
void gpu_world_free(McWorld *w);
/* сетка/точки на CPU (без GPU) — реализация в region.c */
int mcgen_biome_grid_cpu(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out);

#endif

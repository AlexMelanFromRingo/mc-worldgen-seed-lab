/* df_old.h — density-функции 26.1/26.2 (DensityFunctions: compute/fillArray на double) и NoiseChunk.
 *
 * «Проводка» (RandomState.NoiseWiringHelper): дерево из JSON → граф с экземплярами шумов; ссылки — прозрачные «держатели»
 * (HolderHolder), маркеры (interpolated, flat_cache, cache_2d, cache_once, cache_all_in_cell, blend_density) в точечном
 * режиме прозрачны. Для заполнения чанка граф «оборачивается» как NoiseChunk.wrap: маркеры становятся обёртками со
 * своим состоянием (интерполяция по ячейкам, плоский кэш по квартам, кэш по 2D-позиции, кэш «один раз», кэш ячейки),
 * одинаковые (структурно) поддеревья — одна обёртка (как HashMap<DensityFunction,…> игры).
 */
#ifndef MCGEN_DF_OLD_H
#define MCGEN_DF_OLD_H
#include "mcgen_internal.h"

typedef struct OldWire OldWire;
typedef struct ON ON;
typedef struct NChunk NChunk;

OldWire *old_wire_new(McWorld *w, char *err, size_t errlen);
void old_wire_free(OldWire *o);
/* поле роутера в точке (compute(SinglePointContext)) — климат для биомов */
double old_router_point(void *o, int field, int x, int y, int z);
int old_df_point(OldWire *o, const Df *f, int n, const int *xyz, double *out, char *err, size_t errlen);

/* ---- NoiseChunk (заполнение чанка) ---- */
NChunk *nchunk_new(OldWire *o);                     /* состояние обёрток (по потоку) */
void nchunk_free(NChunk *c);
/* начать чанк (cellCountXZ = 16 / cellWidth): заполняет плоские кэши */
void nchunk_begin(NChunk *c, int chunk_min_x, int chunk_min_z, int min_y, int height);
void nchunk_begin_cells(NChunk *c, int chunk_min_x, int chunk_min_z, int min_y, int height, int cells_xz);   /* cells_xz: 1 — столбец (NoiseChunk.forColumn) */
int nchunk_cell_width(const NChunk *c);
int nchunk_cell_height(const NChunk *c);
void nchunk_init_first_cell_x(NChunk *c);
void nchunk_advance_cell_x(NChunk *c, int cell_x_index);
void nchunk_select_cell_yz(NChunk *c, int cell_y_index, int cell_z_index);
void nchunk_update_y(NChunk *c, int pos_y, double fy);
void nchunk_update_x(NChunk *c, int pos_x, double fx);
void nchunk_update_z(NChunk *c, int pos_z, double fz);
void nchunk_swap_slices(NChunk *c);
void nchunk_stop(NChunk *c);
void nchunk_set_beard(NChunk *c, const void *beard);   /* Beardifier чанка для W_BEARD (NULL — ноль) */
/* значения в текущей позиции интерполяции (контекст = NoiseChunk) */
double nchunk_full_density(NChunk *c);              /* cache_all_in_cell(add(final_density, beardifier)) */
double nchunk_router_here(NChunk *c, int field);   /* поле обёрнутого роутера в текущей позиции */
/* поле обёрнутого роутера в точке (SinglePointContext), с учётом плоских кэшей чанка */
double nchunk_router_point(NChunk *c, int field, int x, int y, int z);
int nchunk_block_x(const NChunk *c);
int nchunk_block_y(const NChunk *c);
int nchunk_block_z(const NChunk *c);
/* NoiseChunk.preliminarySurfaceLevel (кэш по колонке кварты) */
int nchunk_prelim_surface(NChunk *c, int x, int z);
int nchunk_max_prelim_surface(NChunk *c, int minx, int minz, int maxx, int maxz);

#endif

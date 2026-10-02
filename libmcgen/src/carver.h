/* carver.h — стадия CARVERS (пещеры, каньоны, карверы Нижнего мира): внутренний интерфейс (поток W3).
 *
 * Положение в цепочке: NoiseBasedChunkGenerator.buildTerrain = doFill (TERRAIN) → buildSurface (SURFACE) → generateCarvers (CARVERS).
 * Карверы работают над УЖЕ заполненным/покрытым поверхностью чанком и используют тот же aquifer (NoiseChunk) и те же кэши
 * сэмплеров, что и заполнение, поэтому стадию нужно вызывать на том же TerrainCtx сразу после terrain_fill_chunk
 * (и стадии SURFACE, если она запрошена), до копирования пометок пост-обработки жидкостей в регион.
 *
 * Чанк зависит только от СВОИХ блоков и от биомов исходных чанков (обход (2·8+1)² соседей по seed карвера) — блоки соседей не нужны.
 */
#ifndef MCGEN_CARVER_H
#define MCGEN_CARVER_H
#include "mcgen_internal.h"
#include "fluidpp.h"

/* terrain.c (расширение W1 для стадии CARVERS) */
PPMarks *terrain_marks_rw(TerrainCtx *t);
int terrain_carve_sched(TerrainCtx *t);   /* shouldScheduleFluidUpdate() последнего вызова aquifer (26.1/26.2 читают «старое» значение) */
int terrain_carve_substance(TerrainCtx *t, int x, int y, int z, int *sched);   /* Aquifer.computeSubstance(x,y,z,0.0): состояние / −1 (null) / −2 (нет чанка) */

/* Применить карверы к чанку (cx, cz): blocks — [y][z][x] (height мира), marks — пометки пост-обработки (дописываются).
 * Данные карверов мира строятся лениво при первом вызове (потокобезопасно). 0 — успех, иначе код MCGEN_E_*, текст в err. */
int carvers_apply_chunk(McWorld *w, TerrainCtx *t, int cx, int cz, uint16_t *blocks, PPMarks *marks, char *err, size_t errlen);
void carvers_world_free(McWorld *w);

/* Контракт со стадией SURFACE (W2): SurfaceSystem.topMaterial / MaterialSystem.topMaterial — материал верхнего слоя для позиции (x,y,z)
 * (блок под вырезанной травой: dirt → grass_block/…): правило поверхности в одной точке со stoneDepthAbove = stoneDepthBelow = 1,
 * waterHeight = under_fluid ? y + 1 : Integer.MIN_VALUE, градиентами поверхности по карте WORLD_SURFACE_WG чанка.
 * Возвращает id состояния или −1 (Optional.empty). blocks — [y][z][x] текущего чанка (в момент вызова уже с вырезанной частью).
 * Слабый символ: пока W2 его не определила, замена блока под травой не выполняется. */
int mcgen_surface_top_material(McWorld *w, TerrainCtx *t, int cx, int cz, const uint16_t *blocks, int x, int y, int z, int under_fluid)
    __attribute__((weak));

/* Тестовый экспорт: маска вырезания чанка без применения (для сверки с Java): mask[(x*16+z)*H + (y-miny)], H = высота маски.
 * Возвращает число отмеченных блоков или −1; *miny, *h — геометрия маски. 26.3+ */
void carvers_x_stats(long *calls, long *changed);   /* тест: вызовы topMaterial / изменённые им блоки */
void carvers_x_set_eager(int on);   /* тест: «жадный» режим 26.1/26.2 для 26.3+ */
int carvers_x_mask(McWorld *w, int cx, int cz, uint8_t *mask, size_t cap, int *miny, int *h);

#endif

/* light.h — небесный свет (LightLayer.SKY) полностью освещённого мира: тот же результат, что у SkyLightEngine после шага LIGHT и применения всех обновлений.
 * Модель проверена по SkyLight, сохранённому игрой в чанках эталонных миров (tools/gt/light_check.py: 0 расхождений на 60 чанках 5 миров). */
#ifndef MCGEN_LIGHT_H
#define MCGEN_LIGHT_H
#include "blockstate.h"

typedef int (*LightGetFn)(void *ud, int x, int y, int z);      /* состояние блока (id) в клетке; вне мира — воздух */

/* Уровень небесного света клетки (0..15): клетки с y ≥ lowestSourceY(колонка) — источники (15), где lowestSourceY = y самого высокого блока с затуханием ≠ 0, плюс 1
 * (ChunkSkyLightSources.isEdgeOccluded без учёта форм); соседний уровень = уровень − max(1, затухание клетки-приёмника). Поиск идёт от клетки назад по цене (алгоритм Дейкстры
 * по ведёрку стоимости ≤ 14), чтение блоков — через get; запрос дорогой (~25 тыс. клеток), использовать для редких проверок. */
int light_sky_final(const BsTab *bs, LightGetFn get, void *ud, int min_y, int height, int x, int y, int z);

/* Блочный свет (LightLayer.BLOCK): источники — блоки со свечением (BSF_LIGHT), уровень = свечение − стоимость пути (та же цена шага max(1, затухание клетки-приёмника)). */
int light_block_final(const BsTab *bs, LightGetFn get, void *ud, int min_y, int height, int x, int y, int z);
/* getRawBrightness(pos, 0) = max(блочный, небесный); небесный учитывается при has_sky (Nether — нет) */
int light_raw_final(const BsTab *bs, LightGetFn get, void *ud, int min_y, int height, int has_sky, int x, int y, int z);

#endif

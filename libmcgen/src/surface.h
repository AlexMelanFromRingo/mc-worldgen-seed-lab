/* surface.h — стадия SURFACE (поток W2): интерпретатор правил поверхности/материала и логика buildSurface.
 *
 * 26.3+ : MaterialSystem.buildSurface + material_rule (noise_settings.material_rule → worldgen/material_rule/*.json, material_condition/*.json);
 * 26.1/26.2 : SurfaceSystem.buildSurface + surface_rule (встроенное в noise_settings дерево).
 * Положение в цепочке: doFill (TERRAIN) → SURFACE → CARVERS. Жилы руд 26.3 («ore_vein» в material_rule) расставляет стадия TERRAIN
 * (terrain_veins.c) — правило ore_vein здесь «узнаёт» уже поставленный блок жилы и сохраняет его, как это сделала бы игра.
 */
#ifndef MCGEN_SURFACE_H
#define MCGEN_SURFACE_H
#include "mcgen_internal.h"
#include "fluidpp.h"

typedef struct SurfCtx SurfCtx;

/* Разбор правил и создание шумов мира (из mcgen_world_new); 0 — успех. */
int surface_world_init(McWorld *w, char *err, size_t errlen);
void surface_world_free(McWorld *w);

/* Контекст потока (кэши условий, сэмплеры, кэш биомов). */
SurfCtx *surface_ctx_new(McWorld *w);
void surface_ctx_free(SurfCtx *c);

/* Применить SURFACE к чанку (cx, cz): blocks — [y][z][x] (height мира), уже заполненный стадией TERRAIN;
 * chunk_biomes — «сырые» биомы этого чанка (клетки 4×4×4, [qy][qz][qx], как mcgen_region_biomes), соседние чанки
 * запрашиваются через world_biome_cell; marks — пометки пост-обработки жидкостей (дописываются, может быть NULL).
 * 26.4-snapshot-2: карвинг встроен в проход поверхности (ChunkTerrainBuilder.fillColumn). carve != 0 (запрошена стадия CARVERS) —
 * проход сам вырезает по маске карверов (carvers_mask_alloc) через aquifer последнего заполненного чанка t (terrain_carve_substance),
 * с логикой carvedTopBlock; отдельный carvers_apply_chunk для 26.4 при SURFACE не вызывается. В остальных версиях carve игнорируется. */
int surface_apply_chunk(McWorld *w, SurfCtx *c, int cx, int cz, uint16_t *blocks, const uint8_t *chunk_biomes, PPMarks *marks,
                        char *err, size_t errlen);
/* то же + карвинг внутри прохода (26.4-snapshot-2): t — TerrainCtx, на котором только что заполнен этот чанк; carve — запрошена стадия CARVERS.
 * Для 26.4 при SURFACE вызывающий НЕ вызывает carvers_apply_chunk (surface_carves_inside); для остальных версий t/carve игнорируются. */
int surface_apply_chunk_ex(McWorld *w, SurfCtx *c, int cx, int cz, uint16_t *blocks, const uint8_t *chunk_biomes, PPMarks *marks,
                           TerrainCtx *t, int carve, char *err, size_t errlen);
static inline int surface_carves_inside(const McWorld *w) { return w->g->version >= V26_4; }

#endif

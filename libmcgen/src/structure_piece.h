/* structure_piece.h — вспомогательный слой для Java-кодированных построек (аналог StructurePiece/ScatteredFeaturePiece.* игры).
 *
 * Как написать постройку (modules structures/<имя>.c, регистрация — structures/register.c):
 *   1. StructType: parse(cfg) разбирает structure/*.json; find(c, cfg, stub) — findGenerationPoint (точка для проверки биома; ГСЧ c->rs — как в игре);
 *      build(c, cfg, stub, out) — создаёт части (piece_new) после проверки биома.
 *   2. PieceVT: post(c, p, ref_x, ref_y, ref_z) — postProcess: рисует часть в чанке c->cx/c->cz, пользуясь sp_* (координаты части локальные,
 *      с учётом ориентации/отражения/поворота, запись только внутри c->chunk); ГСЧ — c->rs (WorldgenRandom на Xoroshiro, общий на структуру-шаг).
 *   3. Порядок и число вызовов ГСЧ — как в игре, ДАЖЕ для позиций вне текущего чанка (generateBox с селектором, generateMaybeBox вызывают ГСЧ для всех позиций).
 * Все sp_* принимают состояния блоков как id libmcgen (sp_st(c, "minecraft:stone[...]")).
 */
#ifndef MCGEN_STRUCTURE_PIECE_H
#define MCGEN_STRUCTURE_PIECE_H
#include "structure.h"
#include "template.h"

/* состояние блока по имени (кэшируется в мире построек); −1 если нет */
int sp_st(StCtx *c, const char *name);
int sp_st_gen(GenCtx *c, const char *name);
int sp_with(StCtx *c, int state, const char *prop, const char *value);     /* BlockState.setValue (по имени свойства); при ошибке — исходное */
int sp_air(StCtx *c);

/* ориентация: StructurePiece.setOrientation(dir) — dir_* из feature.h (DIR_NORTH…) или −1 (null) */
void sp_set_orientation(StPiece *p, int dir);
int sp_orientation_dir(const StPiece *p);               /* DIR_* или −1 */
BB sp_make_bb(int x, int y, int z, int dir, int width, int height, int depth);    /* StructurePiece.makeBoundingBox */

/* локальные координаты → мировые */
int sp_wx(const StPiece *p, int x, int z);
int sp_wy(const StPiece *p, int y);
int sp_wz(const StPiece *p, int x, int z);

/* чтение/запись (WorldGenRegion в окне 3×3; запись только в chunkBB = c->chunk) */
int sp_get(StCtx *c, const StPiece *p, int x, int y, int z);                  /* getBlock: воздух вне chunkBB */
int sp_get_world(StCtx *c, int wx, int wy, int wz);                           /* level.getBlockState(world pos) */
void sp_place(StCtx *c, const StPiece *p, int state, int x, int y, int z);    /* placeBlock: mirror → rotate состояния, setBlock(…, 2), пометки SHAPE_CHECK_BLOCKS */
void sp_set_world(StCtx *c, int wx, int wy, int wz, int state);              /* level.setBlock(pos, state, 2) для мировой позиции внутри c->chunk */
int sp_inside(StCtx *c, int wx, int wy, int wz);                              /* chunkBB.isInside */
void sp_air_box(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1);
void sp_box(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1, int edge, int fill, int skip_air);
typedef struct SPSel SPSel;
struct SPSel { void *ud; int (*next)(SPSel *s, RS *r, int wx_local, int wy_local, int wz_local, int is_edge); };   /* BlockSelector: возвращает состояние */
void sp_box_sel(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1, int skip_air, SPSel *sel);
void sp_maybe_box(StCtx *c, const StPiece *p, float prob, int x0, int y0, int z0, int x1, int y1, int z1, int edge, int fill, int skip_air, int has_to_be_inside);
void sp_maybe_block(StCtx *c, const StPiece *p, float prob, int x, int y, int z, int state);
void sp_upper_half_sphere(StCtx *c, const StPiece *p, int x0, int y0, int z0, int x1, int y1, int z1, int fill, int skip_air);
void sp_fill_column_down(StCtx *c, const StPiece *p, int state, int x, int start_y, int z);
int sp_is_interior(StCtx *c, const StPiece *p, int x, int y, int z);
int sp_replaceable_by_structures(StCtx *c, int state);                       /* StructurePiece.isReplaceableByStructures */
int sp_reorient(StCtx *c, int wx, int wy, int wz, int chest_state);         /* StructurePiece.reorient (сундук) */
int sp_create_chest(StCtx *c, const StPiece *p, int x, int y, int z, int state /* −1: reorient(CHEST) */);   /* ставит блок и тратит random.nextLong() как setLootTable */
int sp_create_dispenser(StCtx *c, const StPiece *p, int x, int y, int z, int facing_dir);

/* карты высот: WorldGenRegion.getHeight(type, x, z) = «первая свободная» y; WG-типы 26.3+ — по заполнению шумом (structure_height_wg) */
int sp_height(StCtx *c, int hm_type, int wx, int wz);
/* ScatteredFeaturePiece.updateAverageGroundHeight / updateHeightPositionToLowestGroundHeight: heightPosition хранится в *hpos (−1 — не задан) */
int sp_update_avg_ground(StCtx *c, StPiece *p, int *hpos, int offset);
int sp_update_lowest_ground(StCtx *c, StPiece *p, int *hpos, int offset);

/* WorldGenRegion.getRandom(): RandomSource региона (worldgen_region_random, позиционный по минимальному углу центрального чанка) */
Rnd *sp_region_random(StCtx *c);       /* один и тот же объект на весь чанк прогона (состояние сохраняется между вызовами) */
/* RandomSource.createThreadLocalInstance(level.getSeed()).forkPositional().at(x, y, z) на LCG */
RS sp_seed_positional(StCtx *c, int x, int y, int z);

#endif

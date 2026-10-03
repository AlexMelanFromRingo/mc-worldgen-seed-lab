/* feature_veg.h — общие утилиты фич растительности (поток W10): порядок обхода java.util.HashSet<BlockPos>, выживание растений. */
#ifndef MCGEN_FEATURE_VEG_H
#define MCGEN_FEATURE_VEG_H
#include "feature.h"

/* позиция блока в наборе (замена Set<BlockPos>) */
typedef struct VPos { int x, y, z; } VPos;

/* Переставляет a[0..n) (порядок вставки в HashSet) в порядок обхода java.util.HashSet<BlockPos> (HashMap с хэшем Vec3i.hashCode =
 * (y + z·31)·31 + x, ёмкость 16 → удвоение при size > 0,75·ёмкость; внутри корзины — порядок вставки, перестройка таблицы его сохраняет;
 * деревья-корзины не эмулируются: при размере ≤ 400 и таких координатах цепочек длиной ≥ 9 не бывает). Повторов в a быть не должно. */
void veg_hashset_order(VPos *a, int n);

/* BlockState.canSurvive для классов растений группы W10 (feature_veg.c). Возвращает 1, если класс обработан (результат — в *res). */
int veg_survive(FCtx *c, int state, int x, int y, int z, int *res);

/* тег блоков по имени (кэш по указателю на строковый литерал, на поток) */
const u8 *veg_tag(const FCtx *c, const char *tag);
static inline int veg_fluid(const FCtx *c, int x, int y, int z) { return BS_FL_TYPE(c->bs->fluid[fc_get(c, x, y, z)]); }
static inline int veg_is_water(int fl) { return fl == FL_WATER || fl == FL_FLOWING_WATER; }
/* Block.is(tag) для состояния */
static inline int veg_in_tag(const FCtx *c, const u8 *tag, int st) { return tag && tag[c->g->state_block[st]] != 0; }
/* BlockState.isFaceSturdy(level, pos, dir) (FULL) */
static inline int veg_sturdy(const FCtx *c, int st, int dir) { return (c->bs->sturdy[st] >> dir) & 1; }

#endif

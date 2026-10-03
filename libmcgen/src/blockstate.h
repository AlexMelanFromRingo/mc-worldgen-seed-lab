/* blockstate.h — утилиты состояний блоков для стадий декораций (FEATURES): свойства блока (set_value по имени), состояние по
 * умолчанию, разбор состояния из JSON датапака (все форматы 26.1…26.4), свойства состояний, закодированные в Java (reports/block_flags.json).
 *
 * Данные о блоках, лежащие в датапаке (теги, blocks.json), берутся оттуда; то, что в игре закодировано в Java (isSolid, canBeReplaced,
 * жидкость состояния, «прочные грани», карты высот по blocksMotion в 26.1/26.2), читается из reports/block_flags.json — файл создаёт
 * libmcgen/tests/g5_blockflags.py из настоящих классов игры (как reports/blocks.json). Без него включаются приближённые эвристики.
 */
#ifndef MCGEN_BLOCKSTATE_H
#define MCGEN_BLOCKSTATE_H
#include "mcgen_internal.h"

/* биты flags[состояние] (совпадают с BlockFlags.java) */
enum { BSF_AIR = 1, BSF_SOLID = 2, BSF_LIQUID = 4, BSF_REPLACEABLE = 8, BSF_OCCLUDE = 16, BSF_SOLID_RENDER = 32, BSF_FULL_COLL = 64,
       BSF_LEAVES = 128, BSF_BE = 256, BSF_MOTION = 512, BSF_SKY = 1024 };
#define BSF_PP(f) (((f) >> 16) & 3)        /* getPostProcessPos: 1 — позиция самого блока, 2 — над ним */
#define BSF_LIGHT(f) (((f) >> 20) & 15)
/* тип жидкости состояния (fluid[состояние] >> 8) */
enum { FL_NONE = 0, FL_WATER = 1, FL_FLOWING_WATER = 2, FL_LAVA = 3, FL_FLOWING_LAVA = 4, FL_OTHER = 5 };
#define BS_FL_TYPE(f) ((f) >> 8)
#define BS_FL_AMOUNT(f) (((f) >> 4) & 15)
#define BS_FL_FALLING(f) (((f) >> 3) & 1)
#define BS_FL_SOURCE(f) (((f) >> 2) & 1)

/* карты высот (Heightmap.Types): порядок как в игре */
enum { HM_WORLD_SURFACE_WG = 0, HM_WORLD_SURFACE = 1, HM_OCEAN_FLOOR_WG = 2, HM_OCEAN_FLOOR = 3, HM_MOTION_BLOCKING = 4, HM_MOTION_BLOCKING_NO_LEAVES = 5, HM__COUNT = 6 };
int hm_type_from_name(const char *s);           /* "WORLD_SURFACE_WG" … → тип, −1 если нет */

typedef struct BsBlock {
    char *name;
    int first, count;           /* состояния блока — непрерывный диапазон id [first, first+count) */
    int nprops;
    char **pname;               /* имена свойств в порядке blocks.json (=порядок игры) */
    int *pn;                    /* число значений свойства */
    char ***pval;               /* имена значений */
    int *stride;                /* вклад индекса значения в id (смешанная система счисления, последнее свойство — самое «быстрое») */
    int radix_ok;               /* id состояний подтверждены формулой first + Σ idx·stride */
    int def;                    /* состояние по умолчанию */
    char *cls;                  /* класс блока в Java (из block_flags.json) или NULL */
    char *chain;                /* «|Класс|Родитель|…|» — для проверок instanceof (bs_is_a) */
} BsBlock;

typedef struct BsTab {
    const McGen *g;
    int nblocks; BsBlock *blk;
    int nstates;
    u32 *flags;                 /* BSF_* и пр. */
    u16 *fluid;
    u8 *sturdy;                 /* биты: DOWN, UP, NORTH, SOUTH, WEST, EAST — isFaceSturdy(FULL) */
    int exported;               /* 1 — данные из block_flags.json, 0 — эвристика */
    u8 *hmcls;                  /* [состояние] биты (1<<HM_*) — «непрозрачность» для карты высот */
    StrMap block_ids;           /* имя блока → индекс+1 */
    int st_air, st_cave_air, st_void_air;
} BsTab;

/* таблица для McGen (создаётся лениво, потокобезопасно, освобождается в mcgen_close) */
const BsTab *bs_get(const McGen *g);
void bs_free(void *tab);

int bs_block_index(const BsTab *t, const char *name);                    /* «minecraft:stone» или «stone»; −1 */
static inline int bs_block_of(const BsTab *t, int st) { return t->g->state_block[st]; }
static inline int bs_default(const BsTab *t, int blk) { return t->blk[blk].def; }
int bs_prop_index(const BsTab *t, int blk, const char *prop);            /* индекс свойства в блоке или −1 */
int bs_has_prop(const BsTab *t, int st, const char *prop);
int bs_get_prop(const BsTab *t, int st, const char *prop, const char **val);   /* 1 — есть; val — имя значения */
int bs_with(const BsTab *t, int st, const char *prop, const char *val);        /* BlockState.setValue; −1 если свойства/значения нет */
int bs_try_with(const BsTab *t, int st, const char *prop, const char *val);    /* trySetValue: нет свойства → то же состояние */
/* состояние из JSON: строка «minecraft:x» (+ «[k=v,…]»), {"Name","Properties"} (26.1/26.2), {"id"|"Name","properties"|"Properties"} (26.3); −1 при ошибке */
int bs_from_json(const BsTab *t, const Js *v);
/* состояние жидкости из JSON FluidState: {"id":"minecraft:water","properties":{"falling":"true","level":"3"}} → состояние блока-жидкости
 * (FluidState.createLegacyBlock: level = 8 − min(amount,8) + (falling ? 8 : 0)); −1 при ошибке */
int bs_fluid_block_from_json(const BsTab *t, const Js *v);
int bs_is_block(const BsTab *t, int st, const char *name);               /* BlockState.is(Block) по имени (медленно — только при разборе) */
/* набор блоков из JSON HolderSet<Block>: «minecraft:x», «#minecraft:tag», массив таких; результат — u8[nblocks] (malloc) или NULL */
u8 *bs_block_set_from_json(const BsTab *t, const Js *v);
int bs_flag(const BsTab *t, int st, int bit);
int bs_is_a(const BsTab *t, int st, const char *cls);      /* блок состояния — экземпляр Java-класса cls (по простому имени) */

#endif

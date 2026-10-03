/* template.h — StructureTemplate: шаблоны построек из data/<ns>/structure/**.nbt (поток W9).
 *
 * Загрузка (nbt.h), палитры, порядок блоков как в игре (полные кубы → прочие → блок-сущности, внутри — по y, x, z),
 * повороты и отражения блоков вместе со свойствами (facing, axis, rotation, north/east/south/west, shape, orientation, hinge),
 * размещение шаблона в мире фич (FCtx) с процессорами.
 *
 * Использование другими потоками (фичи fossil, desert_well и т. п.):
 *     const Template *t = template_get(w, "minecraft:fossils/fossil_spine_01");
 *     TSettings s; tsettings_init(&s); s.rot = ROT_CW90; s.bounds = &bb; s.procs…
 *     template_place(fc, w, t, x, y, z, x, y, z, &s, seed, 2);
 */
#ifndef MCGEN_TEMPLATE_H
#define MCGEN_TEMPLATE_H
#include "structure.h"
#include "processor.h"

typedef struct TJig {
    int x, y, z;                 /* позиция в шаблоне */
    int state;                   /* состояние блока jigsaw (orientation) */
    int rollable;                /* JointType.ROLLABLE */
    const char *name, *pool, *target, *final_state;   /* строки «ns:id»; name/target — Identifier */
    int place_prio, sel_prio;
} TJig;

typedef struct TPal { int nb; TInfo *b; int nj; TJig *j; } TPal;

typedef struct Template {
    char *id;
    int sx, sy, sz;
    int npal; TPal *pal;
    NbtDoc *doc;
} Template;

const Template *template_get(McWorld *w, const char *id);      /* «minecraft:village/plains/houses/…» → structure/….nbt; NULL если нет */

/* StructureTemplate.transform: отражение, затем поворот вокруг pivot (px, pz) */
void tpl_transform(int x, int z, int mir, int rot, int px, int pz, int *ox, int *oz);
BB tpl_bounding_box(const Template *t, int x, int y, int z, int rot, int mir, int px, int pz);
void tpl_size(const Template *t, int rot, int *sx, int *sy, int *sz);
/* StructurePlaceSettings.getRandomPalette(palettes, pos): палитра по RandomSource.create(Mth.getSeed(pos)) */
const TPal *tpl_palette_at(const Template *t, int x, int y, int z);

/* BlockState.rotate / mirror (по свойствам блока; кэшируются) */
int bsx_rotate(const BsTab *bs, int state, int rot);
int bsx_mirror(const BsTab *bs, int state, int mir);

typedef struct TSettings {
    int rot, mir, px, pz;            /* вращение/отражение вокруг pivot (по умолчанию 0) */
    const BB *bounds;                /* StructurePlaceSettings.boundingBox: писать только внутри (NULL — без ограничения) */
    int waterlog;                    /* LiquidSettings.APPLY_WATERLOGGING */
    const Proc *const *procs; int nprocs;   /* процессоры в порядке применения */
    int known_shape;                 /* StructurePlaceSettings.knownShape: 1 — без обновления форм после размещения (jigsaw-элементы); 0 (Java по умолчанию) — updateShapeAtEdge + updateFromNeighbourShapes */
    RS *rnd;                         /* ГСЧ записи (placeInWorld(…, random, …)): nextLong() на каждый блок-контейнер с NBT (LootTableSeed); NULL — не тратить */
} TSettings;
static inline void tsettings_init(TSettings *s) { memset(s, 0, sizeof *s); s->waterlog = 1; }
void structure_update_after_template(FCtx *fc, const int *placed, int n, int flags);   /* structure_post.c */

/* StructureTemplate.placeInWorld(level, position, referencePos, settings, random, flags): блоки через fc_set (flags 2/18).
 * level_seed — для capped (ServerLevel.getSeed). Возвращает 1, если шаблон не пуст. */
int template_place(FCtx *fc, McWorld *w, const Template *t, int x, int y, int z, int rx, int ry, int rz,
                   const TSettings *s, i64 level_seed, int flags);
/* StructureTemplate.processBlockInfos + filterBlocks (для маркеров данных): применённые процессоры без записи */
int template_process(FCtx *fc, McWorld *w, const Template *t, int x, int y, int z, int rx, int ry, int rz,
                     const TSettings *s, i64 level_seed, TInfo **out, TInfo **orig_out);

void templates_world_free(void *store);
void *templates_store_new(void);
#endif

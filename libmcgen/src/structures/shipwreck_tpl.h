/* structures/shipwreck_tpl.h — общая часть шаблонных частей (TemplateStructurePiece) для модулей shipwreck, ocean_ruin, ruined_portal,
 * nether_fossil, end_city (поток «шаблонные постройки»).
 *
 * TemplateStructurePiece игры: шаблон + StructurePlaceSettings (поворот/отражение/опорная точка, процессоры) + templatePosition.
 * Рисование (postProcess) = StructureTemplate.placeInWorld(level, templatePosition, referencePos, settings, random, 2) с границей chunkBB:
 * процессоры (template_process), запись блоков, расход random.nextLong() на контейнеры с NBT (setLootTable «LootTableSeed»), заливка
 * водой (LiquidSettings.APPLY_WATERLOGGING), обновление форм (knownShape == false: updateShapeAtEdge + updateFromNeighbourShapes),
 * затем маркеры данных (structure_block mode=DATA → handleDataMarker) и jigsaw-блоки (final_state). */
#ifndef MCGEN_STRUCTURES_TPL_H
#define MCGEN_STRUCTURES_TPL_H
#include "../structure_piece.h"

typedef struct TplPiece {
    const Template *t;
    char name[112];                 /* templateName (поле «Template» в NBT) */
    int x, y, z;                    /* templatePosition */
    int rot, mir, px, pz;           /* StructurePlaceSettings: поворот, отражение, опорная точка */
    const Proc *procs[8]; int nprocs;
    int waterlog;                   /* LiquidSettings.APPLY_WATERLOGGING (по умолчанию да) */
    int known_shape;                /* StructurePlaceSettings.knownShape (по умолчанию false) */
} TplPiece;

/* заполняет tp: шаблон loc («minecraft:shipwreck/with_mast»), имя для NBT name, позиция и настройки; 0 — шаблона нет */
int tplp_init(McWorld *w, TplPiece *tp, const char *loc, const char *name, int x, int y, int z, int rot, int mir, int px, int pz);
void tplp_add_proc(TplPiece *tp, const Proc *p);
/* template.getBoundingBox(placeSettings, templatePosition) */
BB tplp_bb(const TplPiece *tp);
/* StructureTemplate.calculateConnectedPosition(settings1, pos1, settings2, pos2) при нулевой опорной точке и без отражения у второго */
void tplp_connected(const TplPiece *parent, int ox, int oy, int oz, const TplPiece *child, int *dx, int *dy, int *dz);

/* handleDataMarker(markerId, pos, level, random, chunkBB) */
typedef void (*TplMarkerFn)(StCtx *c, StPiece *p, void *ud, const char *marker, int x, int y, int z, const BB *bounds);
/* TemplateStructurePiece.postProcess (без переопределений): p->bb пересчитывается; bounds — chunkBB (может быть расширен);
 * возвращает 1, если шаблон размещён (placeInWorld вернул true) */
int tplp_post(StCtx *c, StPiece *p, TplPiece *tp, const BB *bounds, int rx, int ry, int rz, TplMarkerFn marker, void *ud);
/* RandomizableContainer.setBlockEntityLootTable(level, random, pos, table): random.nextLong(), если там контейнер с лутом (внутри окна) */
void tplp_loot(StCtx *c, int x, int y, int z);
/* блок — контейнер с RandomizableContainer (сундук, бочка, раздатчик, воронка, шалкер, горшок, крафтер) */
int tplp_is_container(StCtx *c, int st);

void tplp_dump(const TplPiece *tp, StrBuf *o);
static inline void tplp_move(TplPiece *tp, int dx, int dy, int dz) { tp->x += dx; tp->y += dy; tp->z += dz; }

static const char *const TPLP_ROTN[4] = { "NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90" };
#endif

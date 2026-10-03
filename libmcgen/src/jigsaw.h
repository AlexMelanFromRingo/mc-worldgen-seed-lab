/* jigsaw.h — JigsawStructure / JigsawPlacement / StructureTemplatePool / StructurePoolElement (поток W9). */
#ifndef MCGEN_JIGSAW_H
#define MCGEN_JIGSAW_H
#include "template.h"

typedef struct PoolElem PoolElem;
typedef struct JJunction { int sx, sgy, sz, dy, dest_proj; } JJunction;            /* JigsawJunction (dest_proj: 0 rigid, 1 terrain_matching) */
typedef struct JPiece {                                                              /* PoolElementStructurePiece */
    PoolElem *el;
    int x, y, z;                                                                     /* position */
    int ground_delta;
    int rot;                                                                         /* ROT_* */
    int liquid_apply;                                                                /* LiquidSettings.APPLY_WATERLOGGING */
    BB bb0;                                                                          /* bounding box элемента без «expansion hack» (так часть восстанавливается из NBT) */
    JJunction *junc; int nj, cj;
} JPiece;

extern const PieceVT PIECE_JIGSAW;
extern const StructType STRUCT_JIGSAW;
void jigsaw_register(void);
void *jigsaw_store_new(void);
void jigsaw_world_free(void *store);
int jigsaw_piece_projection(const StPiece *p);            /* 0 rigid, 1 terrain_matching */
const JPiece *jigsaw_piece_data(const StPiece *p);
#endif

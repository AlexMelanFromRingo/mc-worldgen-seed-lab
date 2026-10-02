/* fluidpp.h — пост-обработка жидкостей при переходе чанка в FULL (см. fluidpp.c) и пометки позиций. */
#ifndef MCGEN_FLUIDPP_H
#define MCGEN_FLUIDPP_H
#include "mcgen_internal.h"

/* пометки ProtoChunk.markPosForPostProcessing: по секциям (индекс от min_y мира), в порядке добавления */
typedef struct PPMarks { int nsec; u16 **pos; int *n, *cap; } PPMarks;
void ppmarks_clear(PPMarks *m);
void ppmarks_add(PPMarks *m, int sec, int lx, int ly, int lz);
void ppmarks_copy(PPMarks *dst, const PPMarks *src);
void ppmarks_free(PPMarks *m);

/* доступ к миру (блоки региона и соседей) */
typedef struct FluidWorld {
    const McGen *g;
    void *ud;
    int (*get)(void *ud, int x, int y, int z);
    void (*set)(void *ud, int x, int y, int z, int st);
    int fast_lava;            /* dimension_type: gameplay/fast_lava (Незер) */
    int water_conversion;     /* gamerule waterSourceConversion (по умолчанию да) */
    int lava_conversion;      /* gamerule lavaSourceConversion (по умолчанию нет) */
} FluidWorld;

void fluidpp_tick(FluidWorld *w, int x, int y, int z);
void fluidpp_chunk(FluidWorld *w, const PPMarks *m, int cx, int cz, int min_y);

#endif

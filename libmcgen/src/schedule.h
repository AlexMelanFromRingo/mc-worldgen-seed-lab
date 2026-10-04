/* schedule.h — расписание настоящего сервера (запись одного прогона): порядок шагов FEATURES с масками видимости света и порядок пост-обработки.
 * Формат .mcsched (текст, tools/gt/jfr_order.py --sched): «# …» комментарии; «# dim <overworld|the_nether|the_end>»; «F cx cz маска(hex)» — шаг FEATURES
 * (маска 5×5 вокруг чанка: чанки, уже прошедшие INITIALIZE_LIGHT); «P cx cz» — чанк стал «тикающим» (пост-обработка). Воспроизведение даёт мир записанного прогона
 * (0 расхождений на 16,6 млн блоков в измерениях docs/blender/nondeterminism.md). */
#ifndef MCGEN_SCHEDULE_H
#define MCGEN_SCHEDULE_H
#include "mcgen_internal.h"

typedef struct McSchedule {
    int nf, np;
    int *fx, *fz; unsigned *fmask;     /* шаги FEATURES */
    int *px, *pz;                      /* порядок пост-обработки */
    int dim_kind;                      /* -1 — не указано; 0 Overworld, 1 Nether, 2 End */
} McSchedule;

McSchedule *sched_load(const char *path, char *err, size_t errlen);     /* NULL при ошибке */
void sched_free(McSchedule *s);

#endif

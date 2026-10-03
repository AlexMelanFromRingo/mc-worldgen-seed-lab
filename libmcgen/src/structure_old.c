/* structure_old.c — ChunkGenerator.getBaseHeight для 26.1/26.2 (NoiseChunk с интерполяцией ячеек). Пока не реализовано: возвращает min_y. */
#include "structure.h"
int terrain_old_column_height(McWorld *w, void *octx, int bx, int bz, int hm_type) { (void)octx; (void)bx; (void)bz; (void)hm_type; return w->min_y; }

/* mcgen_test.h — тестовые экспорты libmcgen (не входят в публичный контракт mcgen.h; для CLI и тестов). */
#ifndef MCGEN_TEST_H
#define MCGEN_TEST_H
#include "mcgen.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Значения зарегистрированной density-функции id в n точках xyz[3n] — без кэшей, как oracle `df`
 * (26.3+: RandomState.sampleBlockValueUncached; 26.1/26.2: compute(SinglePointContext) после «проводки»). */
MCGEN_API int mcgen_x_df_eval(McWorld *w, const char *id, int n, const int *xyz, double *out, char *err, size_t errlen);
/* 1 — численная ветка float (26.3+), 0 — double (26.1/26.2) */
MCGEN_API int mcgen_x_is_float(const McWorld *w);
/* «сырые» биомы по квартам (как oracle `biome`): nx*nz, индекс iz*nx+ix */
MCGEN_API int mcgen_x_noise_biomes(const McWorld *w, int qx0, int qz0, int nx, int nz, int qy, uint8_t *out);
/* климат (temperature, humidity, continentalness, erosion, depth, weirdness) в квартовой точке, float */
MCGEN_API int mcgen_x_climate(const McWorld *w, int qx, int qy, int qz, float out[6]);
/* 1 — в клетке (qx,qy,qz) биомы a и b имеют равный минимальный fitness (ничья R-дерева), 0 — нет, −1 — ошибка */
MCGEN_API int mcgen_x_biome_tie(const McWorld *w, int qx, int qy, int qz, int a, int b);
/* «сырое» заполнение шумом одного чанка (TERRAIN без поверхности): height*256 u16 [y][z][x] */
MCGEN_API int mcgen_x_fill_chunk(McWorld *w, int cx, int cz, uint16_t *blocks, char *err, size_t errlen);
#ifdef __cplusplus
}
#endif
#endif

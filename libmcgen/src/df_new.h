/* df_new.h — вычислитель density-функций 26.3+ (пакет levelgen/densityfunction игры).
 *
 * Конвейер как в DensityFunctionCompiler:
 *   1) «оптимизация»: подстановка ссылок, дедупликация cache по структурному равенству входа (PreparedCache с id),
 *      затем правило SliceUniformAxes (2D-функции внутри 3D оборачиваются в slice по y=0 и т. п.);
 *   2) компиляция в дерево сэмплеров — те же специализации, что выбирает игра (ConstAdd, ConstMul, MinSampler с range…);
 *   3) вычисление: sampleValue (точка) и sampleVolume (объём) — это РАЗНЫЕ численные пути, оба воспроизведены.
 * Контекст (SamplerContext): ячейки кэшей по id (последний объём + последняя точка), буферы, поля Beardifier/Blender.
 */
#ifndef MCGEN_DF_NEW_H
#define MCGEN_DF_NEW_H
#include "df.h"

typedef struct S S;
typedef struct NComp NComp;
typedef struct SCtx SCtx;

/* Источник шумов для компиляции (CompileContext игры) */
typedef struct {
    void *ud;
    const NStack *(*noise)(void *ud, const char *name, char *err, size_t errlen);   /* createNoiseSampler */
    const BlendFbm *(*blended)(void *ud, const Df *f);                              /* old_blended_noise: createRandom(terrain) */
    const GNoise *(*end_islands)(void *ud);                                         /* createEndIslandRandom → Simplex */
    const Df *(*ref)(void *ud, const char *id);                                     /* реестр density_function */
    Ival (*noise_range)(void *ud, const char *name);                                /* NormalNoise.range() из параметров */
    int v264;    /* 26.4+: ClampFunction упрощается по range входа; SliceUniformAxes режет и gradient */
    void (*noise_scale)(void *ud, const char *name, double *mxz, double *my);       /* тонкие настройки (NULL — нет) */
} NEnv;

NComp *nc_new(const NEnv *env);
void nc_free(NComp *c);
/* RandomState.getSampler(function): оптимизация + компиляция (с общими для всего компилятора кэшами) */
const S *nc_get(NComp *c, const Df *f, char *err, size_t errlen);
int nc_cache_count(const NComp *c);
Ival nc_range_of(NComp *c, const Df *f);   /* range() исходной функции (для отладки/тестов) */

/* Контекст вычисления (по потоку) */
SCtx *sctx_new(const NComp *c, int caches);
void sctx_free(SCtx *x);
void sctx_reset_caches(SCtx *x);          /* новый SamplerContext (новый чанк) */
float s_value(SCtx *x, const S *s, int bx, int by, int bz);
void s_volume(SCtx *x, const S *s, float *out, const Vol *v);
/* буферы контекста (ScopedDensityBuffer) */
float *sctx_acquire(SCtx *x, int n);
void sctx_release(SCtx *x, float *p);

/* Поле контекста Beardifier (ContextBoundSampler): NULL — константа 0 */
typedef struct {
    void *ud;
    float (*value)(void *ud, int x, int y, int z);
    void (*volume)(void *ud, float *out, const Vol *v);
} SBeard;
void sctx_set_beardifier(SCtx *x, const SBeard *b);

#endif

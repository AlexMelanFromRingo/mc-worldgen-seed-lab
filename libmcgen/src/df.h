/* df.h — дерево density-функций, разобранное из JSON датапака (общее для всех версий),
 * и интерфейсы двух вычислителей:
 *   df_new.c — 26.3+ (densityfunction/*: DensityFunctionCompiler, сэмплеры с точечным и объёмным путём, float);
 *   df_old.c — 26.1/26.2 (DensityFunctions: compute/fillArray на double, маркеры NoiseChunk).
 */
#ifndef MCGEN_DF_H
#define MCGEN_DF_H
#include "util.h"
#include "noise.h"

typedef enum {
    DF_CONST = 0, DF_REF,
    /* генераторы */
    DF_NOISE, DF_SHIFTED_NOISE /*old*/, DF_SHIFT_A, DF_SHIFT_B, DF_SHIFT, DF_END_ISLANDS /*old*/, DF_END_OUTER_ISLANDS /*new*/,
    DF_DISTANCE_TO_POINT, DF_GRADIENT /*new*/, DF_Y_CLAMPED_GRADIENT /*old*/, DF_OLD_BLENDED_NOISE, DF_WEIRD_SCALED /*26.1*/,
    DF_BLEND_ALPHA, DF_BLEND_OFFSET, DF_BEARDIFIER,
    /* унарные */
    DF_ABS, DF_SQUARE, DF_CUBE, DF_SQRT, DF_HALF_NEGATIVE, DF_QUARTER_NEGATIVE, DF_RECIPROCAL /* new reciprocal / old invert */,
    DF_NEGATE, DF_SQUEEZE, DF_LOG, DF_SIGN,
    DF_FLOOR, DF_ROUND, DF_CEIL, DF_TRUNCATE,
    /* бинарные */
    DF_ADD, DF_SUB, DF_MUL, DF_DIV, DF_MIN, DF_MAX, DF_POW,
    /* прочие */
    DF_SPLINE, DF_LERP, DF_CLAMP, DF_RANGE_CHOICE, DF_INTERVAL_SELECT,
    DF_CACHE /*new*/, DF_INTERPOLATED, DF_FLAT_CACHE, DF_CACHE_2D, DF_CACHE_ONCE, DF_CACHE_ALL_IN_CELL, DF_BLEND_DENSITY,
    DF_SLICE, DF_FIND_TOP_SURFACE,
    DF__COUNT
} DfType;

typedef struct DfSpline DfSpline;
typedef struct Df Df;

/* Число из JSON: оба разбора (Float.parseFloat и Double.parseDouble) */
typedef struct { float f; double d; } DNum;

struct Df {
    DfType t;
    Df *a, *b, *c;          /* input/left/alpha …, right/first/when_in …, second/when_out … */
    Df *d;                  /* shift_z (шум) */
    Df **list; int nlist;   /* interval_select.functions */
    DNum *thr; int nthr;    /* interval_select.thresholds */
    DNum n0, n1, n2, n3, n4;/* числовые параметры (см. парсер) */
    int i0, i1, i2, i3;     /* целые параметры */
    char *name;             /* шум (полный id) или ссылка на функцию (полный id) */
    DfSpline *spline;
    u64 hash;               /* структурный хэш (для дедупликации кэшей, как record.equals в Java) */
};

struct DfSpline {
    int is_const; DNum value;
    Df *coord;
    int n; DNum *loc, *der; DfSpline **val;
};

/* Разбор: версия задаёт семейство имён полей (old: argument/argument1..; new: input/left/right). */
typedef struct DfParser DfParser;
Df *df_parse(const void *js /* Js* */, int new_family, char *err, size_t errlen);
void df_free(Df *f);
int df_equal(const Df *a, const Df *b);    /* структурное равенство (ссылки — по имени) */
u64 df_hash(const Df *f);
const char *df_type_name(DfType t);
char *df_full_id(const char *s);            /* "foo" → "minecraft:foo" */

#endif

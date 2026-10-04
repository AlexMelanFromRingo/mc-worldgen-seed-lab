/* util.h — общие утилиты libmcgen: память, строки, файлы, хэш-таблица, ошибки, «Java-математика», потоки.
 *
 * Всё численное, что должно совпадать с Java побитно, собрано в разделе «jm_*»: семантика Math.min/max/abs/floor/clamp
 * Java отличается от libm C в краевых случаях (NaN, −0.0, переполнение приведения к int).
 */
#ifndef MCGEN_UTIL_H
#define MCGEN_UTIL_H
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

typedef uint64_t u64;
typedef int64_t  i64;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint16_t u16;
typedef int16_t  i16;
typedef uint8_t  u8;

/* ---- память ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xsprintf(const char *fmt, ...);

/* динамический массив указателей */
typedef struct { void **v; int n, cap; } PtrVec;
void pv_push(PtrVec *a, void *p);
void pv_free(PtrVec *a);

/* строковый буфер */
typedef struct { char *s; size_t n, cap; } StrBuf;
void sb_putc(StrBuf *b, char c);
void sb_puts(StrBuf *b, const char *s);
void sb_printf(StrBuf *b, const char *fmt, ...);
char *sb_take(StrBuf *b);

/* ---- хэш-таблица строка → указатель (открытая адресация; ключи копируются) ---- */
typedef struct { char *key; void *val; u64 h; } StrMapEnt;
typedef struct { StrMapEnt *e; int cap, n; } StrMap;
u64 str_hash(const char *s);
void *sm_get(const StrMap *m, const char *key);
int sm_has(const StrMap *m, const char *key);
void sm_put(StrMap *m, const char *key, void *val);   /* перезаписывает */
void sm_free(StrMap *m, void (*free_val)(void *));

/* ---- файлы ---- */
char *read_file(const char *path, size_t *len);       /* NUL-терминировано; NULL при ошибке */
int file_exists(const char *path);
/* Пути — UTF-8 на всех платформах: в Windows (имя пользователя с кириллицей, длинные пути > 259 символов) — через широкие API (_wfopen, FindFirstFileW,
 * префикс \\?\ у длинных абсолютных путей); ANSI-вариант fopen там читает только системную кодовую страницу и MAX_PATH. */
FILE *mc_fopen(const char *path, const char *mode);
int mc_is_dir(const char *path);
/* список имён файлов каталога (рекурсивно, относительные пути с '/', отсортировано); возвращает число */
int list_dir_recursive(const char *dir, const char *suffix, char ***out);
void free_str_list(char **l, int n);

/* ---- числа без зависимости от локали (Blender может поменять LC_NUMERIC) ---- */
double c_strtod(const char *s, char **end);
float c_strtof(const char *s, char **end);

/* ---- ошибки ---- */
void set_err(char *err, size_t errlen, const char *fmt, ...);

/* ---- «Java-математика» (float/double) ---- */
/* Math.max/min ровно как в JDK: NaN в любом аргументе → NaN; max(-0,0)=0; min(0,-0)=-0 */
static inline float jm_maxf(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && signbit(a)) return b;
    return a >= b ? a : b;
}
static inline float jm_minf(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && signbit(b)) return b;
    return a <= b ? a : b;
}
static inline double jm_max(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && signbit(a)) return b;
    return a >= b ? a : b;
}
static inline double jm_min(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && signbit(b)) return b;
    return a <= b ? a : b;
}
/* Mth.clamp: value < min ? min : Math.min(value, max) */
static inline float jm_clampf(float v, float lo, float hi) { return v < lo ? lo : jm_minf(v, hi); }
static inline double jm_clamp(double v, double lo, double hi) { return v < lo ? lo : jm_min(v, hi); }
/* (int) приведение Java: насыщение, NaN → 0 */
static inline i32 jm_d2i(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return 2147483647;
    if (v <= -2147483648.0) return (i32)0x80000000u;
    return (i32)v;
}
static inline i64 jm_d2l(double v) {
    if (v != v) return 0;
    if (v >= 9223372036854775807.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return (i64)v;
}
static inline i32 jm_floor_d(double v) { return jm_d2i(floor(v)); }          /* Mth.floor(double) */
static inline i32 jm_floor_f(float v) { return jm_d2i(floor((double)v)); }   /* Mth.floor(float) = (int)Math.floor(v) */
static inline i64 jm_lfloor(double v) { return jm_d2l(floor(v)); }
static inline i32 jm_floordiv(i32 a, i32 b) { i32 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static inline i32 jm_floormod(i32 a, i32 b) { i32 m = a % b; if (m != 0 && ((m < 0) != (b < 0))) m += b; return m; }
static inline float jm_lerpf(float a, float p0, float p1) { return p0 + a * (p1 - p0); }
static inline double jm_lerp(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
static inline float jm_signumf(float v) { if (v != v || v == 0.0f) return v; return v > 0.0f ? 1.0f : -1.0f; }
/* Math.round(float) → int (округление к +inf на половинах) */
static inline i32 jm_roundf(float a) {
    if (a != a) return 0;
    double f = floor((double)a);
    double r = ((double)a - f >= 0.5) ? f + 1.0 : f;
    return jm_d2i(r);
}
/* Float.compare(a,b)==0 — равенство компонент record (NaN==NaN, 0.0 != -0.0) */
static inline int jm_feq(float a, float b) { u32 x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); if (a != a && b != b) return 1; return x == y; }
static inline int jm_deq(double a, double b) { u64 x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); if (a != a && b != b) return 1; return x == y; }

/* Java String.hashCode() для ASCII/UTF-8 строк из BMP (имена ресурсов — ASCII) */
static inline i32 java_str_hash(const char *s) { u32 h = 0; for (; *s; s++) h = 31u * h + (u32)(u8)*s; return (i32)h; }

/* ---- потоки ---- */
typedef struct McThread McThread;
typedef void (*McThreadFn)(void *arg);
McThread *thread_start(McThreadFn fn, void *arg);
void thread_join(McThread *t);
int cpu_count(void);
typedef struct McMutex McMutex;
McMutex *mutex_new(void);
void mutex_lock(McMutex *m);
void mutex_unlock(McMutex *m);
void mutex_free(McMutex *m);
double now_sec(void);

#endif

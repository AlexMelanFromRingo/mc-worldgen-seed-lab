/* util.c — реализация общих утилит (см. util.h). */
#define _GNU_SOURCE
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  include <windows.h>
#  include <locale.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <pthread.h>
#  include <unistd.h>
#  include <locale.h>
#  if defined(__APPLE__)
#    include <xlocale.h>
#  endif
#endif

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fprintf(stderr, "libmcgen: нет памяти (%zu)\n", n); abort(); } return p; }
void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); if (!p) { fprintf(stderr, "libmcgen: нет памяти\n"); abort(); } return p; }
void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) { fprintf(stderr, "libmcgen: нет памяти\n"); abort(); } return q; }
char *xstrdup(const char *s) { size_t n = strlen(s); char *d = xmalloc(n + 1); memcpy(d, s, n + 1); return d; }
char *xstrndup(const char *s, size_t n) { char *d = xmalloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }
char *xsprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2); va_end(ap2);
    return s;
}

void pv_push(PtrVec *a, void *p) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->v = xrealloc(a->v, sizeof(void *) * (size_t)a->cap); }
    a->v[a->n++] = p;
}
void pv_free(PtrVec *a) { free(a->v); a->v = NULL; a->n = a->cap = 0; }

void sb_putc(StrBuf *b, char c) {
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->s = xrealloc(b->s, b->cap); }
    b->s[b->n++] = c; b->s[b->n] = 0;
}
void sb_puts(StrBuf *b, const char *s) { while (*s) sb_putc(b, *s++); }
void sb_printf(StrBuf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[512]; int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < (int)sizeof tmp) { sb_puts(b, tmp); return; }
    va_start(ap, fmt); char *big = xmalloc((size_t)n + 1); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
    sb_puts(b, big); free(big);
}
char *sb_take(StrBuf *b) { char *s = b->s ? b->s : xstrdup(""); b->s = NULL; b->n = b->cap = 0; return s; }

/* ---- хэш-таблица ---- */
u64 str_hash(const char *s) { u64 h = 1469598103934665603ULL; for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ULL; } return h ? h : 1; }
static int sm_find(const StrMap *m, const char *key, u64 h) {
    if (!m->cap) return -1;
    int i = (int)(h & (u64)(m->cap - 1));
    for (;;) {
        if (!m->e[i].key) return -1 - i;
        if (m->e[i].h == h && !strcmp(m->e[i].key, key)) return i;
        i = (i + 1) & (m->cap - 1);
    }
}
void *sm_get(const StrMap *m, const char *key) { int i = sm_find(m, key, str_hash(key)); return i >= 0 ? m->e[i].val : NULL; }
int sm_has(const StrMap *m, const char *key) { return sm_find(m, key, str_hash(key)) >= 0; }
static void sm_grow(StrMap *m) {
    int oc = m->cap; StrMapEnt *oe = m->e;
    m->cap = oc ? oc * 2 : 64; m->e = xcalloc((size_t)m->cap, sizeof(StrMapEnt)); m->n = 0;
    for (int i = 0; i < oc; i++) if (oe[i].key) {
        int j = (int)(oe[i].h & (u64)(m->cap - 1));
        while (m->e[j].key) j = (j + 1) & (m->cap - 1);
        m->e[j] = oe[i]; m->n++;
    }
    free(oe);
}
void sm_put(StrMap *m, const char *key, void *val) {
    if ((m->n + 1) * 2 > m->cap) sm_grow(m);
    u64 h = str_hash(key);
    int i = sm_find(m, key, h);
    if (i >= 0) { m->e[i].val = val; return; }
    i = -1 - i;
    m->e[i].key = xstrdup(key); m->e[i].val = val; m->e[i].h = h; m->n++;
}
void sm_free(StrMap *m, void (*free_val)(void *)) {
    for (int i = 0; i < m->cap; i++) if (m->e[i].key) { free(m->e[i].key); if (free_val) free_val(m->e[i].val); }
    free(m->e); m->e = NULL; m->cap = m->n = 0;
}

/* ---- файлы ---- */
#if defined(_WIN32)
/* UTF-8 -> UTF-16 для API Windows; абсолютные пути длиннее 247 символов получают префикс \\?\ (тогда '/' обязан быть '\\'). Освобождать free(). */
static wchar_t *w_path(const char *s, int dir_pattern) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(((size_t)n + 8 + (dir_pattern ? 2 : 0)) * sizeof(wchar_t));
    if (!w) return NULL;
    wchar_t *d = w;
    int absolute = ((s[0] && s[1] == ':' && (s[2] == '\\' || s[2] == '/')) && n > 248);
    if (absolute) { memcpy(d, L"\\\\?\\", 4 * sizeof(wchar_t)); d += 4; }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, d, n) <= 0) { free(w); return NULL; }
    if (absolute) for (wchar_t *p = d; *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}
FILE *mc_fopen(const char *path, const char *mode) {
    wchar_t *wp = w_path(path, 0), wm[8]; int i = 0;
    for (; mode[i] && i < 7; i++) wm[i] = (wchar_t)(unsigned char)mode[i];
    wm[i] = 0;
    FILE *f = wp ? _wfopen(wp, wm) : fopen(path, mode);
    free(wp); return f;
}
int mc_is_dir(const char *path) {
    wchar_t *wp = w_path(path, 0); if (!wp) return 0;
    DWORD a = GetFileAttributesW(wp); free(wp);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
#else
FILE *mc_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int mc_is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
#endif
char *read_file(const char *path, size_t *len) {
    FILE *f = mc_fopen(path, "rb"); if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f); if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *b = xmalloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); if (len) *len = (size_t)n; return b;
}
int file_exists(const char *path) { FILE *f = mc_fopen(path, "rb"); if (!f) return 0; fclose(f); return 1; }

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void list_rec(const char *base, const char *rel, const char *suffix, PtrVec *out) {
    char *path = rel[0] ? xsprintf("%s/%s", base, rel) : xstrdup(base);
#if defined(_WIN32)
    char *pat = xsprintf("%s\\*", path);
    wchar_t *wpat = w_path(pat, 1); free(pat);
    WIN32_FIND_DATAW fd; HANDLE h = wpat ? FindFirstFileW(wpat, &fd) : INVALID_HANDLE_VALUE; free(wpat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char nm[3 * MAX_PATH + 4];
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, (int)sizeof nm, NULL, NULL)) continue;
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            char *r = rel[0] ? xsprintf("%s/%s", rel, nm) : xstrdup(nm);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { list_rec(base, r, suffix, out); free(r); }
            else { size_t ln = strlen(r), ls = strlen(suffix); if (ln >= ls && !strcmp(r + ln - ls, suffix)) pv_push(out, r); else free(r); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            const char *nm = de->d_name; if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            char *r = rel[0] ? xsprintf("%s/%s", rel, nm) : xstrdup(nm);
            char *full = xsprintf("%s/%s", base, r);
            struct stat st; int isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)); free(full);
            if (isdir) { list_rec(base, r, suffix, out); free(r); }
            else { size_t ln = strlen(r), ls = strlen(suffix); if (ln >= ls && !strcmp(r + ln - ls, suffix)) pv_push(out, r); else free(r); }
        }
        closedir(d);
    }
#endif
    free(path);
}
int list_dir_recursive(const char *dir, const char *suffix, char ***out) {
    PtrVec v = {0};
    list_rec(dir, "", suffix, &v);
    qsort(v.v, (size_t)v.n, sizeof(char *), cmp_str);
    *out = (char **)v.v; return v.n;
}
void free_str_list(char **l, int n) { for (int i = 0; i < n; i++) free(l[i]); free(l); }

/* ---- числа в "C"-локали ---- */
#if defined(_WIN32)
static _locale_t c_loc(void) { static _locale_t L; if (!L) L = _create_locale(LC_NUMERIC, "C"); return L; }
double c_strtod(const char *s, char **end) { return _strtod_l(s, end, c_loc()); }
float c_strtof(const char *s, char **end) { return _strtof_l(s, end, c_loc()); }
#else
static locale_t c_loc(void) { static locale_t L; if (!L) L = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0); return L; }
double c_strtod(const char *s, char **end) { return strtod_l(s, end, c_loc()); }
float c_strtof(const char *s, char **end) { return strtof_l(s, end, c_loc()); }
#endif

void set_err(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, errlen, fmt, ap); va_end(ap);
}

/* ---- потоки ---- */
#if defined(_WIN32)
struct McThread { HANDLE h; McThreadFn fn; void *arg; };
static DWORD WINAPI thr_main(LPVOID p) { McThread *t = p; t->fn(t->arg); return 0; }
McThread *thread_start(McThreadFn fn, void *arg) { McThread *t = xcalloc(1, sizeof *t); t->fn = fn; t->arg = arg; t->h = CreateThread(NULL, 0, thr_main, t, 0, NULL); return t; }
void thread_join(McThread *t) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); free(t); }
int cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
struct McMutex { CRITICAL_SECTION cs; };
McMutex *mutex_new(void) { McMutex *m = xcalloc(1, sizeof *m); InitializeCriticalSection(&m->cs); return m; }
void mutex_lock(McMutex *m) { EnterCriticalSection(&m->cs); }
void mutex_unlock(McMutex *m) { LeaveCriticalSection(&m->cs); }
void mutex_free(McMutex *m) { DeleteCriticalSection(&m->cs); free(m); }
double now_sec(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
#else
struct McThread { pthread_t t; McThreadFn fn; void *arg; };
static void *thr_main(void *p) { McThread *t = p; t->fn(t->arg); return NULL; }
McThread *thread_start(McThreadFn fn, void *arg) {
    McThread *t = xcalloc(1, sizeof *t); t->fn = fn; t->arg = arg;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20);   /* рекурсия по графу функций */
    pthread_create(&t->t, &at, thr_main, t); pthread_attr_destroy(&at);
    return t;
}
void thread_join(McThread *t) { pthread_join(t->t, NULL); free(t); }
int cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
struct McMutex { pthread_mutex_t m; };
McMutex *mutex_new(void) { McMutex *m = xcalloc(1, sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void mutex_lock(McMutex *m) { pthread_mutex_lock(&m->m); }
void mutex_unlock(McMutex *m) { pthread_mutex_unlock(&m->m); }
void mutex_free(McMutex *m) { pthread_mutex_destroy(&m->m); free(m); }
double now_sec(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + ts.tv_nsec * 1e-9; }
#endif

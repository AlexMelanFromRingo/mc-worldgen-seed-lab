/* json.c — рекурсивный разбор JSON (RFC 8259, плюс допуск комментариев не нужен — датапак строгий). */
#include "json.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* арена: блоки по 64 КБ */
typedef struct Blk { struct Blk *next; size_t used, cap; char data[]; } Blk;
struct JsDoc { Blk *blk; Js *root; };

static void *ar_alloc(JsDoc *d, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!d->blk || d->blk->used + n > d->blk->cap) {
        size_t cap = n > 65536 ? n : 65536;
        Blk *b = xmalloc(sizeof(Blk) + cap); b->next = d->blk; b->used = 0; b->cap = cap; d->blk = b;
    }
    void *p = d->blk->data + d->blk->used; d->blk->used += n; return p;
}

typedef struct { const char *p, *end; JsDoc *d; const char *err; int depth; } P;

static void ws(P *p) { while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')) p->p++; }

static Js *new_node(P *p, JsType t) { Js *v = ar_alloc(p->d, sizeof(Js)); memset(v, 0, sizeof *v); v->t = t; return v; }

static void put_utf8(StrBuf *b, unsigned cp) {
    if (cp < 0x80) sb_putc(b, (char)cp);
    else if (cp < 0x800) { sb_putc(b, (char)(0xC0 | (cp >> 6))); sb_putc(b, (char)(0x80 | (cp & 63))); }
    else if (cp < 0x10000) { sb_putc(b, (char)(0xE0 | (cp >> 12))); sb_putc(b, (char)(0x80 | ((cp >> 6) & 63))); sb_putc(b, (char)(0x80 | (cp & 63))); }
    else { sb_putc(b, (char)(0xF0 | (cp >> 18))); sb_putc(b, (char)(0x80 | ((cp >> 12) & 63))); sb_putc(b, (char)(0x80 | ((cp >> 6) & 63))); sb_putc(b, (char)(0x80 | (cp & 63))); }
}
static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i]; v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v; return 1;
}

static const char *parse_string(P *p) {
    /* p->p указывает на открывающую кавычку */
    p->p++;
    const char *start = p->p;
    int simple = 1;
    while (p->p < p->end && *p->p != '"') { if (*p->p == '\\') { simple = 0; p->p++; } p->p++; }
    if (p->p >= p->end) { p->err = "незакрытая строка"; return NULL; }
    size_t n = (size_t)(p->p - start);
    p->p++;
    if (simple) { char *s = ar_alloc(p->d, n + 1); memcpy(s, start, n); s[n] = 0; return s; }
    StrBuf b = {0};
    for (const char *q = start; q < start + n; q++) {
        if (*q != '\\') { sb_putc(&b, *q); continue; }
        q++;
        switch (*q) {
            case '"': sb_putc(&b, '"'); break; case '\\': sb_putc(&b, '\\'); break; case '/': sb_putc(&b, '/'); break;
            case 'b': sb_putc(&b, '\b'); break; case 'f': sb_putc(&b, '\f'); break; case 'n': sb_putc(&b, '\n'); break;
            case 'r': sb_putc(&b, '\r'); break; case 't': sb_putc(&b, '\t'); break;
            case 'u': {
                unsigned cp; if (!hex4(q + 1, &cp)) { free(b.s); p->err = "плохой \\u"; return NULL; }
                q += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && q[1] == '\\' && q[2] == 'u') {
                    unsigned lo; if (hex4(q + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
                }
                put_utf8(&b, cp); break;
            }
            default: free(b.s); p->err = "плохой escape"; return NULL;
        }
    }
    size_t len = b.n; char *s = ar_alloc(p->d, len + 1); if (len) memcpy(s, b.s, len); s[len] = 0; free(b.s);
    return s;
}

static Js *parse_value(P *p);

static Js *parse_array(P *p) {
    p->p++;
    Js *a = new_node(p, JS_ARR);
    PtrVec tmp = {0};
    ws(p);
    if (p->p < p->end && *p->p == ']') { p->p++; return a; }
    for (;;) {
        Js *v = parse_value(p); if (!v) { pv_free(&tmp); return NULL; }
        pv_push(&tmp, v);
        ws(p);
        if (p->p >= p->end) { p->err = "конец в массиве"; pv_free(&tmp); return NULL; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == ']') { p->p++; break; }
        p->err = "ожидалась , или ]"; pv_free(&tmp); return NULL;
    }
    a->n = tmp.n; a->items = ar_alloc(p->d, sizeof(Js *) * (size_t)tmp.n); memcpy(a->items, tmp.v, sizeof(Js *) * (size_t)tmp.n);
    pv_free(&tmp);
    return a;
}

static Js *parse_object(P *p) {
    p->p++;
    Js *o = new_node(p, JS_OBJ);
    PtrVec keys = {0}, vals = {0};
    ws(p);
    if (p->p < p->end && *p->p == '}') { p->p++; return o; }
    for (;;) {
        ws(p);
        if (p->p >= p->end || *p->p != '"') { p->err = "ожидался ключ"; goto fail; }
        const char *k = parse_string(p); if (!k) goto fail;
        ws(p);
        if (p->p >= p->end || *p->p != ':') { p->err = "ожидалось :"; goto fail; }
        p->p++;
        Js *v = parse_value(p); if (!v) goto fail;
        pv_push(&keys, (void *)k); pv_push(&vals, v);
        ws(p);
        if (p->p >= p->end) { p->err = "конец в объекте"; goto fail; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == '}') { p->p++; break; }
        p->err = "ожидалась , или }"; goto fail;
    }
    o->n = keys.n;
    o->keys = ar_alloc(p->d, sizeof(char *) * (size_t)keys.n); memcpy(o->keys, keys.v, sizeof(char *) * (size_t)keys.n);
    o->items = ar_alloc(p->d, sizeof(Js *) * (size_t)vals.n); memcpy(o->items, vals.v, sizeof(Js *) * (size_t)vals.n);
    pv_free(&keys); pv_free(&vals);
    return o;
fail:
    pv_free(&keys); pv_free(&vals); return NULL;
}

static Js *parse_value(P *p) {
    ws(p);
    if (p->p >= p->end) { p->err = "неожиданный конец"; return NULL; }
    if (++p->depth > 512) { p->err = "слишком глубоко"; return NULL; }
    Js *r = NULL;
    char c = *p->p;
    if (c == '{') r = parse_object(p);
    else if (c == '[') r = parse_array(p);
    else if (c == '"') { const char *s = parse_string(p); if (s) { r = new_node(p, JS_STR); r->s = s; } }
    else if (c == 't' && p->end - p->p >= 4 && !strncmp(p->p, "true", 4)) { p->p += 4; r = new_node(p, JS_BOOL); r->d = 1; }
    else if (c == 'f' && p->end - p->p >= 5 && !strncmp(p->p, "false", 5)) { p->p += 5; r = new_node(p, JS_BOOL); r->d = 0; }
    else if (c == 'n' && p->end - p->p >= 4 && !strncmp(p->p, "null", 4)) { p->p += 4; r = new_node(p, JS_NULL); }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        const char *s = p->p;
        if (*p->p == '-') p->p++;
        while (p->p < p->end && ((*p->p >= '0' && *p->p <= '9') || *p->p == '.' || *p->p == 'e' || *p->p == 'E' || *p->p == '+' || *p->p == '-')) p->p++;
        size_t n = (size_t)(p->p - s);
        char *t = ar_alloc(p->d, n + 1); memcpy(t, s, n); t[n] = 0;
        r = new_node(p, JS_NUM); r->s = t; r->d = c_strtod(t, NULL);
    } else p->err = "неизвестный символ";
    p->depth--;
    return r;
}

JsDoc *js_parse(const char *text, size_t len, char *err, size_t errlen) {
    JsDoc *d = xcalloc(1, sizeof *d);
    P p = { text, text + len, d, NULL, 0 };
    /* BOM */
    if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) p.p += 3;
    d->root = parse_value(&p);
    if (d->root) { ws(&p); if (p.p != p.end) { p.err = "мусор после значения"; d->root = NULL; } }
    if (!d->root) {
        set_err(err, errlen, "JSON: %s (смещение %ld)", p.err ? p.err : "?", (long)(p.p - text));
        js_free(d); return NULL;
    }
    return d;
}
JsDoc *js_parse_file(const char *path, char *err, size_t errlen) {
    size_t n; char *t = read_file(path, &n);
    if (!t) { set_err(err, errlen, "не открыть %s", path); return NULL; }
    char e2[256] = {0};
    JsDoc *d = js_parse(t, n, e2, sizeof e2);
    free(t);
    if (!d) set_err(err, errlen, "%s: %s", path, e2);
    return d;
}
Js *js_root(JsDoc *d) { return d ? d->root : NULL; }
void js_free(JsDoc *d) {
    if (!d) return;
    Blk *b = d->blk; while (b) { Blk *n = b->next; free(b); b = n; }
    free(d);
}

Js *js_get(const Js *o, const char *key) {
    if (!o || o->t != JS_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}
const char *js_str(const Js *v, const char *def) { return (v && v->t == JS_STR) ? v->s : def; }
double js_num(const Js *v, double def) { return (v && v->t == JS_NUM) ? v->d : def; }
float js_numf(const Js *v, float def) { return (v && v->t == JS_NUM) ? c_strtof(v->s, NULL) : def; }
int js_int(const Js *v, int def) { return (v && v->t == JS_NUM) ? (int)v->d : def; }
int js_bool(const Js *v, int def) { return (v && v->t == JS_BOOL) ? (int)v->d : def; }
int js_is_num(const Js *v) { return v && v->t == JS_NUM; }
int js_is_str(const Js *v) { return v && v->t == JS_STR; }
int js_is_obj(const Js *v) { return v && v->t == JS_OBJ; }
int js_is_arr(const Js *v) { return v && v->t == JS_ARR; }

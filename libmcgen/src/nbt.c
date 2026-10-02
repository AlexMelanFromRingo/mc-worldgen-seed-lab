/* nbt.c — разбор NBT Java-издания (NbtIo/NbtAccounter: big-endian, глубина вложенности ≤ 512, строки — модифицированный
 * UTF-8, переводятся в обычный UTF-8). Узлы и данные — в арене документа. */
#include "nbt.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Blk { struct Blk *next; size_t used, cap; } Blk;
struct NbtDoc { Blk *blk; Nbt root; };

static void *aalloc(NbtDoc *d, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!d->blk || d->blk->used + n > d->blk->cap) {
        size_t cap = n > 65536 ? n : 65536;
        Blk *b = malloc(sizeof(Blk) + 16 + cap);
        if (!b) return NULL;
        b->next = d->blk; b->used = 0; b->cap = cap; d->blk = b;
    }
    void *p = (char *)(d->blk + 1) + 16 + d->blk->used;
    d->blk->used += n;
    return p;
}

typedef struct { const uint8_t *p; size_t n, i; NbtDoc *d; int fail; char *err; size_t errlen; } Rd;
static int need(Rd *r, size_t k) {
    if (r->fail) return 0;
    if (r->n - r->i < k) { r->fail = 1; set_err(r->err, r->errlen, "NBT: неожиданный конец данных (смещение %zu)", r->i); return 0; }
    return 1;
}
static uint64_t be(Rd *r, int k) {
    if (!need(r, (size_t)k)) return 0;
    uint64_t v = 0;
    for (int j = 0; j < k; j++) v = v << 8 | r->p[r->i++];
    return v;
}
/* модифицированный UTF-8 (DataInput.readUTF) -> UTF-8; *olen — байт без завершающего NUL */
static const char *rd_str(Rd *r, int *olen) {
    size_t len = (size_t)be(r, 2);
    if (!need(r, len)) return NULL;
    const uint8_t *s = r->p + r->i;
    char *o = aalloc(r->d, len + 2);
    if (!o) { r->fail = 1; return NULL; }
    size_t k = 0;
    for (size_t j = 0; j < len;) {
        uint8_t c = s[j];
        if (c < 0x80) { o[k++] = (char)c; j++; continue; }
        if ((c & 0xe0) == 0xc0 && j + 1 < len) {
            unsigned cp = (unsigned)(c & 0x1f) << 6 | (s[j + 1] & 0x3f);
            if (cp == 0) o[k++] = 0; else { o[k++] = (char)c; o[k++] = (char)s[j + 1]; }
            j += 2; continue;
        }
        if ((c & 0xf0) == 0xe0 && j + 2 < len) {
            unsigned cp = (unsigned)(c & 0x0f) << 12 | (unsigned)(s[j + 1] & 0x3f) << 6 | (s[j + 2] & 0x3f);
            if (cp >= 0xd800 && cp <= 0xdbff && j + 5 < len && s[j + 3] == 0xed && (s[j + 4] & 0xf0) == 0xb0) {
                unsigned lo = (unsigned)(s[j + 4] & 0x0f) << 6 | (s[j + 5] & 0x3f);
                lo |= 0xdc00;
                unsigned u = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                o[k++] = (char)(0xf0 | u >> 18); o[k++] = (char)(0x80 | ((u >> 12) & 0x3f));
                o[k++] = (char)(0x80 | ((u >> 6) & 0x3f)); o[k++] = (char)(0x80 | (u & 0x3f));
                j += 6; continue;
            }
            o[k++] = (char)c; o[k++] = (char)s[j + 1]; o[k++] = (char)s[j + 2];
            j += 3; continue;
        }
        r->fail = 1; set_err(r->err, r->errlen, "NBT: неверная строка (смещение %zu)", r->i + j);
        return NULL;
    }
    o[k] = 0;
    r->i += len;
    if (olen) *olen = (int)k;
    return o;
}

static void rd_payload(Rd *r, Nbt *x, int type, int depth);
static void rd_list_elems(Rd *r, Nbt *x, int depth) {
    x->elem = (uint8_t)be(r, 1);
    int64_t n = (int32_t)be(r, 4);
    if (r->fail) return;
    /* как ListTag.TYPE.load: тип END при n > 0 — ошибка («Missing type on ListTag»); элемент занимает ≥ 1 байта */
    if (n < 0 || x->elem > NBT_LONG_ARRAY || (x->elem == NBT_END && n > 0) || (size_t)n > r->n - r->i) {
        r->fail = 1; set_err(r->err, r->errlen, "NBT: неверный список (смещение %zu)", r->i); return;
    }
    x->n = (int)n;
    x->v.kids = n ? aalloc(r->d, sizeof(Nbt) * (size_t)n) : NULL;
    for (int64_t k = 0; k < n && !r->fail; k++) {
        Nbt *e = &x->v.kids[k];
        memset(e, 0, sizeof *e); e->name = ""; e->type = x->elem;
        rd_payload(r, e, x->elem, depth + 1);
    }
}
static void rd_payload(Rd *r, Nbt *x, int type, int depth) {
    if (depth > 512) { r->fail = 1; set_err(r->err, r->errlen, "NBT: вложенность > 512"); return; }
    x->type = (uint8_t)type;
    switch (type) {
    case NBT_BYTE: x->v.i = (int8_t)be(r, 1); break;
    case NBT_SHORT: x->v.i = (int16_t)be(r, 2); break;
    case NBT_INT: x->v.i = (int32_t)be(r, 4); break;
    case NBT_LONG: x->v.i = (int64_t)be(r, 8); break;
    case NBT_FLOAT: { uint32_t b = (uint32_t)be(r, 4); memcpy(&x->v.f, &b, 4); break; }
    case NBT_DOUBLE: { uint64_t b = be(r, 8); memcpy(&x->v.d, &b, 8); break; }
    case NBT_STRING: x->v.s = rd_str(r, &x->n); break;
    case NBT_BYTE_ARRAY: case NBT_INT_ARRAY: case NBT_LONG_ARRAY: {
        int64_t n = (int32_t)be(r, 4);
        int es = type == NBT_BYTE_ARRAY ? 1 : type == NBT_INT_ARRAY ? 4 : 8;
        if (r->fail) return;
        if (n < 0 || !need(r, (size_t)n * (size_t)es)) { r->fail = 1; set_err(r->err, r->errlen, "NBT: неверный массив (смещение %zu)", r->i); return; }
        x->n = (int)n;
        void *a = aalloc(r->d, (size_t)n * (size_t)es + 1);
        if (!a) { r->fail = 1; return; }
        if (es == 1) memcpy(a, r->p + r->i, (size_t)n), r->i += (size_t)n;
        else if (es == 4) { int32_t *q = a; for (int64_t k = 0; k < n; k++) q[k] = (int32_t)be(r, 4); }
        else { int64_t *q = a; for (int64_t k = 0; k < n; k++) q[k] = (int64_t)be(r, 8); }
        x->v.b = a;
        break;
    }
    case NBT_LIST: rd_list_elems(r, x, depth); break;
    case NBT_COMPOUND: {
        PtrVec tmp = {0};   /* поля собираем во временный список, потом — одним массивом в арену */
        Nbt *one;
        for (;;) {
            int t = (int)be(r, 1);
            if (r->fail || t == NBT_END) break;
            if (t > NBT_LONG_ARRAY) { r->fail = 1; set_err(r->err, r->errlen, "NBT: неизвестный тег %d (смещение %zu)", t, r->i - 1); break; }
            one = malloc(sizeof(Nbt));
            if (!one) { r->fail = 1; break; }
            memset(one, 0, sizeof *one);
            one->name = rd_str(r, &one->name_len);
            pv_push(&tmp, one);
            if (r->fail) break;
            rd_payload(r, one, t, depth + 1);
        }
        x->n = tmp.n;
        x->v.kids = tmp.n ? aalloc(r->d, sizeof(Nbt) * (size_t)tmp.n) : NULL;
        for (int k = 0; k < tmp.n; k++) { if (x->v.kids) x->v.kids[k] = *(Nbt *)tmp.v[k]; free(tmp.v[k]); }
        free(tmp.v);
        break;
    }
    default: r->fail = 1; set_err(r->err, r->errlen, "NBT: неизвестный тег %d", type);
    }
}

NbtDoc *nbt_parse(const uint8_t *data, size_t n, char *err, size_t errlen) {
    NbtDoc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    Rd r = { data, n, 0, d, 0, err, errlen };
    int t = (int)be(&r, 1);
    if (!r.fail && (t == NBT_END || t > NBT_LONG_ARRAY)) { r.fail = 1; set_err(err, errlen, "NBT: неверный корневой тег %d", t); }
    if (!r.fail) {
        d->root.name = rd_str(&r, &d->root.name_len);
        if (!r.fail) rd_payload(&r, &d->root, t, 0);
    }
    if (r.fail) { nbt_free(d); return NULL; }
    return d;
}
NbtDoc *nbt_read_file(const char *path, char *err, size_t errlen) {
    size_t n; char *raw = read_file(path, &n);
    if (!raw) { set_err(err, errlen, "не прочитать %s", path); return NULL; }
    uint8_t *u; size_t un;
    if (mc_decompress_auto((const uint8_t *)raw, n, &u, &un)) { free(raw); set_err(err, errlen, "%s: ошибка распаковки", path); return NULL; }
    free(raw);
    NbtDoc *d = nbt_parse(u, un, err, errlen);
    free(u);
    return d;
}
const Nbt *nbt_root(const NbtDoc *d) { return d ? &d->root : NULL; }
void nbt_free(NbtDoc *d) {
    if (!d) return;
    for (Blk *b = d->blk; b;) { Blk *nx = b->next; free(b); b = nx; }
    free(d);
}
const Nbt *nbt_get(const Nbt *c, const char *name) {
    if (!c || c->type != NBT_COMPOUND) return NULL;
    for (int k = 0; k < c->n; k++) if (!strcmp(c->v.kids[k].name, name)) return &c->v.kids[k];
    return NULL;
}
const Nbt *nbt_at(const Nbt *l, int i) { return l && l->type == NBT_LIST && i >= 0 && i < l->n ? &l->v.kids[i] : NULL; }
int64_t nbt_int(const Nbt *x, int64_t def) {
    if (!x) return def;
    switch (x->type) {
    case NBT_BYTE: case NBT_SHORT: case NBT_INT: case NBT_LONG: return x->v.i;
    case NBT_FLOAT: return (int64_t)x->v.f;
    case NBT_DOUBLE: return (int64_t)x->v.d;
    default: return def;
    }
}
double nbt_num(const Nbt *x, double def) {
    if (!x) return def;
    switch (x->type) {
    case NBT_BYTE: case NBT_SHORT: case NBT_INT: case NBT_LONG: return (double)x->v.i;
    case NBT_FLOAT: return x->v.f;
    case NBT_DOUBLE: return x->v.d;
    default: return def;
    }
}
const char *nbt_str(const Nbt *x, const char *def) { return x && x->type == NBT_STRING && x->v.s ? x->v.s : def; }

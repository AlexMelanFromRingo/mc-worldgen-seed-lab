/* nbt_dump — каноническая запись NBT-файлов (для сверки с независимым разбором на Python: tests/nbt_check.py).
 *   nbt_dump <файл.nbt> [...]   → для каждого: "<путь>\t<длина распакованного>\t<каноническая запись>\n"
 * Формат: compound {имя:значение;…}, list [тип|a,b,…], b/s/i/l<десятичное>, f<8 hex бит>, d<16 hex бит>, '<строка UTF-8>',
 * B/I/L<n>:a,b,… (массивы). */
#include "nbt.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void dump(const Nbt *x) {
    switch (x->type) {
    case NBT_BYTE: printf("b%" PRId64, x->v.i); break;
    case NBT_SHORT: printf("s%" PRId64, x->v.i); break;
    case NBT_INT: printf("i%" PRId64, x->v.i); break;
    case NBT_LONG: printf("l%" PRId64, x->v.i); break;
    case NBT_FLOAT: { uint32_t b; memcpy(&b, &x->v.f, 4); printf("f%08" PRIx32, b); break; }
    case NBT_DOUBLE: { uint64_t b; memcpy(&b, &x->v.d, 8); printf("d%016" PRIx64, b); break; }
    case NBT_STRING: putchar('\''); fwrite(x->v.s, 1, (size_t)x->n, stdout); putchar('\''); break;
    case NBT_BYTE_ARRAY: printf("B%d:", x->n); for (int k = 0; k < x->n; k++) printf(k ? ",%d" : "%d", x->v.b[k]); break;
    case NBT_INT_ARRAY: printf("I%d:", x->n); for (int k = 0; k < x->n; k++) printf(k ? ",%" PRId32 : "%" PRId32, x->v.ia[k]); break;
    case NBT_LONG_ARRAY: printf("L%d:", x->n); for (int k = 0; k < x->n; k++) printf(k ? ",%" PRId64 : "%" PRId64, x->v.la[k]); break;
    case NBT_LIST: printf("[%d|", x->elem); for (int k = 0; k < x->n; k++) { if (k) putchar(','); dump(&x->v.kids[k]); } putchar(']'); break;
    case NBT_COMPOUND:
        putchar('{');
        for (int k = 0; k < x->n; k++) { fwrite(x->v.kids[k].name, 1, (size_t)x->v.kids[k].name_len, stdout); putchar(':'); dump(&x->v.kids[k]); putchar(';'); }
        putchar('}'); break;
    }
}
int main(int argc, char **argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        char err[256] = {0};
        NbtDoc *d = nbt_read_file(argv[i], err, sizeof err);
        if (!d) { printf("%s\tERR\t%s\n", argv[i], err); bad++; continue; }
        printf("%s\t-\t", argv[i]); dump(nbt_root(d)); putchar('\n');
        nbt_free(d);
    }
    return bad != 0;
}

/* nbt.h — распаковка DEFLATE (gzip/zlib/сырой поток) и разбор NBT (формат Java-издания: big-endian, строки в
 * «модифицированном UTF-8»). Нужны для шаблонов построек (data/<ns>/structure/**.nbt — gzip) и других бинарных
 * данных игры без внешних библиотек (zlib не берём: библиотека собирается zig под 4 платформы без зависимостей).
 *
 * Документ NBT — дерево узлов в одной арене; строки переведены в обычный UTF-8 (с длиной: в NBT допустим U+0000).
 */
#ifndef MCGEN_NBT_H
#define MCGEN_NBT_H
#include <stddef.h>
#include <stdint.h>

/* RFC 1951 (сырой DEFLATE). 0 — успех; *out — malloc, освобождает вызывающий. */
int mc_inflate_raw(const uint8_t *in, size_t n, uint8_t **out, size_t *outn, size_t *used);
/* gzip (1f 8b; несколько членов подряд склеиваются), zlib (78 xx) или несжатые данные — по сигнатуре. 0 — успех. */
int mc_decompress_auto(const uint8_t *in, size_t n, uint8_t **out, size_t *outn);

typedef enum {
    NBT_END = 0, NBT_BYTE, NBT_SHORT, NBT_INT, NBT_LONG, NBT_FLOAT, NBT_DOUBLE, NBT_BYTE_ARRAY, NBT_STRING, NBT_LIST,
    NBT_COMPOUND, NBT_INT_ARRAY, NBT_LONG_ARRAY
} NbtType;

typedef struct Nbt Nbt;
struct Nbt {
    uint8_t type;            /* NbtType */
    uint8_t elem;            /* NBT_LIST: тип элементов */
    const char *name;        /* имя в родительском compound (UTF-8, NUL-терминировано), иначе "" */
    int name_len;
    int n;                   /* compound: число полей; list: элементов; массивы: длина; строка: байт UTF-8 */
    union {
        int64_t i;           /* BYTE/SHORT/INT/LONG (со знаком) */
        float f;
        double d;
        const char *s;       /* STRING (UTF-8, NUL-терминировано, но может содержать U+0000 — см. n) */
        const int8_t *b;     /* BYTE_ARRAY */
        const int32_t *ia;   /* INT_ARRAY (уже в порядке байт машины) */
        const int64_t *la;   /* LONG_ARRAY */
        Nbt *kids;           /* COMPOUND (поля в порядке файла), LIST (элементы) */
    } v;
};

typedef struct NbtDoc NbtDoc;
/* data — несжатый NBT (корень — именованный тег, обычно compound). NULL при ошибке (текст в err). */
NbtDoc *nbt_parse(const uint8_t *data, size_t n, char *err, size_t errlen);
/* файл: распаковка по сигнатуре + разбор */
NbtDoc *nbt_read_file(const char *path, char *err, size_t errlen);
const Nbt *nbt_root(const NbtDoc *d);
void nbt_free(NbtDoc *d);

const Nbt *nbt_get(const Nbt *compound, const char *name);       /* поле compound по имени или NULL */
const Nbt *nbt_at(const Nbt *list, int i);                        /* элемент списка или NULL */
int64_t nbt_int(const Nbt *x, int64_t def);                       /* любое целое -> int64 */
double nbt_num(const Nbt *x, double def);                         /* любое число -> double */
const char *nbt_str(const Nbt *x, const char *def);

#endif

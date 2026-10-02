/* json.h — маленький JSON-парсер (DOM) для данных датапака.
 *
 * Числа хранятся вместе с исходным текстом: Mojang читает поля float через Float.parseFloat(текст)
 * (Gson LazilyParsedNumber), а double — через Double.parseDouble(текст). Двойное округление
 * (double → float) дало бы расхождения в последнем бите, поэтому для float используется strtof(текст).
 */
#ifndef MCGEN_JSON_H
#define MCGEN_JSON_H
#include <stddef.h>

typedef enum { JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_ARR, JS_OBJ } JsType;

typedef struct Js Js;
struct Js {
    JsType t;
    int n;              /* число элементов массива / пар объекта */
    const char *s;      /* JS_STR: строка; JS_NUM: исходный текст числа */
    double d;           /* JS_NUM: значение double (strtod), JS_BOOL: 0/1 */
    Js **items;         /* JS_ARR: элементы; JS_OBJ: значения */
    const char **keys;  /* JS_OBJ: ключи (в порядке файла) */
};

typedef struct JsDoc JsDoc;   /* владелец памяти дерева (арена) */

/* Разбор текста; при ошибке NULL и сообщение в err. */
JsDoc *js_parse(const char *text, size_t len, char *err, size_t errlen);
JsDoc *js_parse_file(const char *path, char *err, size_t errlen);
Js *js_root(JsDoc *d);
void js_free(JsDoc *d);

Js *js_get(const Js *o, const char *key);                  /* NULL, если нет или не объект */
const char *js_str(const Js *v, const char *def);          /* строка или def */
double js_num(const Js *v, double def);                    /* double (Double.parseDouble) */
float js_numf(const Js *v, float def);                     /* float (Float.parseFloat) */
int js_int(const Js *v, int def);
int js_bool(const Js *v, int def);
int js_is_num(const Js *v);
int js_is_str(const Js *v);
int js_is_obj(const Js *v);
int js_is_arr(const Js *v);

#endif

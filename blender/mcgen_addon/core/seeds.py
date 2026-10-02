"""Сиды как в игре: число или произвольная строка (Java String.hashCode), «случайный» сид, раздача по доменам.

Игра (WorldOptions.parseSeed): пустая строка -> случайный сид; если строка разбирается как long (Long.parseLong) — это и есть сид;
иначе сид = (long) строка.hashCode(), где hashCode — 32-битная знаковая свёртка s[0]*31^(n-1) + … по UTF-16 кодовым единицам.
"""
import random
import unicodedata

INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1


def java_string_hash(s):
    """String.hashCode() по UTF-16 кодовым единицам (суррогатные пары считаются как две единицы), результат — знаковый int32."""
    h = 0
    data = s.encode('utf-16-le', 'surrogatepass')
    for i in range(0, len(data), 2):
        h = (31 * h + (data[i] | (data[i + 1] << 8))) & 0xFFFFFFFF
    return h - (1 << 32) if h & 0x80000000 else h


def _parse_long(s):
    """Long.parseLong(s, 10): необязательный знак, затем ≥1 десятичных цифр Unicode (Nd); без пробелов и подчёркиваний. None — не число."""
    if not s:
        return None
    sign = 1
    body = s
    if s[0] in '+-':
        sign = -1 if s[0] == '-' else 1
        body = s[1:]
    if not body:
        return None
    v = 0
    for ch in body:
        if unicodedata.category(ch) != 'Nd':
            return None
        v = v * 10 + unicodedata.digit(ch)
    v *= sign
    return v if INT64_MIN <= v <= INT64_MAX else None


def parse_seed(text):
    """text -> (seed:int64 | None, kind). kind: 'empty' (пусто — «случайный»), 'number', 'string' (хэш строки)."""
    if text is None or text == '':
        return None, 'empty'
    v = _parse_long(text)
    if v is not None:
        return v, 'number'
    return java_string_hash(text), 'string'


def random_seed(rng=None):
    """Случайный знаковый 64-битный сид (как `new Random().nextLong()`-подобный)."""
    r = rng or random.SystemRandom()
    return r.getrandbits(64) - (1 << 63)


def resolve(text, rng=None):
    """Сид из текстового поля; пустое поле = новый случайный."""
    v, kind = parse_seed(text)
    return random_seed(rng) if v is None else v


def describe(text):
    """Подсказка для интерфейса: во что превратится введённый текст."""
    v, kind = parse_seed(text)
    if kind == 'empty':
        return None, kind
    return v, kind


def domains(settings_seed, seed_mode, split_texts, rng=None):
    """Собирает McSeeds (climate, terrain, structures, features) как кортеж int64.

    seed_mode 'UNIFIED': все четыре равны одному (побитово ваниль). 'SPLIT': каждое поле из своего текста; пустое поле — случайное
    (одно значение на домен), чтобы «раздельные сиды» не превращались молча в единый.
    """
    if seed_mode == 'UNIFIED':
        s = resolve(settings_seed, rng)
        return (s, s, s, s)
    return tuple(resolve(t, rng) for t in split_texts)

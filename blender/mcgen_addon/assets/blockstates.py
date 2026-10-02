"""Разбор blockstates (assets/minecraft/blockstates/*.json): variants (веса, повороты x/y/z, uvlock) и multipart
(условия AND/OR, перечисление `a|b`, отрицание `!a`) — по поведению игры 26.x. Чистый Python, без bpy.

Результат выбора для состояния блока — список «частей» (групп); часть — взвешенный список вариантов (`Variant`).
Игра для каждой части берёт один вариант (если вариантов несколько — по случайному числу позиции), см. `state_table`.
"""
import json
import os

__all__ = ['Variant', 'BlockStateDef', 'load_blockstate', 'parse_blockstate']


class Variant:
    """Один вариант: модель + поворот модели (x, y, z — кратны 90) + uvlock + вес."""
    __slots__ = ('model', 'x', 'y', 'z', 'uvlock', 'weight')

    def __init__(self, model, x=0, y=0, z=0, uvlock=False, weight=1):
        self.model, self.x, self.y, self.z, self.uvlock, self.weight = model, x % 360, y % 360, z % 360, bool(uvlock), int(weight)

    def key(self):
        return (self.model, self.x, self.y, self.z, self.uvlock)

    def __repr__(self):
        return 'Variant(%s x=%d y=%d z=%d uvlock=%s w=%d)' % (self.model, self.x, self.y, self.z, self.uvlock, self.weight)

    def __eq__(self, o):
        return isinstance(o, Variant) and self.key() == o.key() and self.weight == o.weight

    def __hash__(self):
        return hash(self.key() + (self.weight,))


def _norm_model(name):
    """'minecraft:block/stone' / 'block/stone' / 'stone' -> 'minecraft:block/stone' (в игре без пространства имён = minecraft)."""
    return name if ':' in name else 'minecraft:' + name


def _variant_from_json(d):
    if not isinstance(d, dict) or 'model' not in d:
        raise ValueError('вариант без model: %r' % (d,))
    for r in ('x', 'y', 'z'):
        v = int(d.get(r, 0))
        if v % 90 != 0:
            raise ValueError('поворот %s=%d не кратен 90' % (r, v))
    return Variant(_norm_model(d['model']), int(d.get('x', 0)), int(d.get('y', 0)), int(d.get('z', 0)),
                   d.get('uvlock', False), int(d.get('weight', 1)))


def _entry(v):
    """Значение variants/apply: объект или список объектов -> список Variant (взвешенный список; один элемент = SingleVariant)."""
    if isinstance(v, list):
        out = [_variant_from_json(x) for x in v]
        if not out:
            raise ValueError('пустой список вариантов')
        return out
    return [_variant_from_json(v)]


def _val_str(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    return str(v)


class _Terms:
    """Условие одного свойства: `a|b|!c`. Совпадает, если значение подходит под любой терм (терм с `!` — не равно)."""
    __slots__ = ('terms',)

    def __init__(self, raw):
        s = _val_str(raw)
        self.terms = []
        for t in s.split('|'):
            if not t:
                raise ValueError('пустой терм в %r' % s)
            neg = t.startswith('!')
            self.terms.append((t[1:] if neg else t, neg))

    def match(self, value):
        for t, neg in self.terms:
            if neg:
                if value != t:
                    return True
            elif value == t:
                return True
        return False


class _Cond:
    """Условие multipart: пара-словарь свойств (все AND) или комбинация {'AND': [...]} / {'OR': [...]}."""
    __slots__ = ('kind', 'tests', 'subs')

    def __init__(self, raw):
        if not isinstance(raw, dict) or not raw:
            raise ValueError('условие multipart должно быть непустым объектом')
        if len(raw) == 1 and next(iter(raw)) in ('AND', 'OR'):
            self.kind = next(iter(raw))
            self.tests = None
            self.subs = [_Cond(x) for x in raw[self.kind]]
        else:
            self.kind = 'KV'
            self.tests = {k: _Terms(v) for k, v in raw.items()}
            self.subs = None

    def match(self, props):
        if self.kind == 'KV':
            for k, t in self.tests.items():
                if k not in props or not t.match(props[k]):
                    return False
            return True
        if self.kind == 'AND':
            return all(s.match(props) for s in self.subs)
        return any(s.match(props) for s in self.subs)


def _parse_key(key):
    """'facing=north,half=top' -> {'facing': 'north', 'half': 'top'}; '' -> {} (подходит любому состоянию)."""
    d = {}
    for kv in key.split(','):
        if not kv:
            continue
        if '=' in kv:
            k, v = kv.split('=', 1)
            d[k] = v
        else:
            raise ValueError('неизвестное свойство в ключе варианта: %r' % kv)
    return d


class BlockStateDef:
    """Определение blockstate одного блока."""
    __slots__ = ('name', 'variants', 'multipart', 'is_multipart')

    def __init__(self, name):
        self.name = name
        self.variants = []   # [(dict match, [Variant])]
        self.multipart = []  # [(_Cond | None, [Variant])]
        self.is_multipart = False

    def select(self, props):
        """props: {имя: строка} -> (части, multipart) либо (None, False), если модели нет (игра рисует «missing»).
        Часть = список Variant (взвешенный). variants: подходит запись, все свойства ключа которой совпали; при нескольких
        совпадениях побеждает последняя (как put в игре). multipart: все части с выполненным условием, в порядке файла;
        пустой список — у состояния нет геометрии. Если есть и variants, и multipart — multipart только для несовпавших."""
        if self.variants:
            chosen = None
            for match, entry in self.variants:
                ok = True
                for k, v in match.items():
                    if props.get(k) != v:
                        ok = False
                        break
                if ok:
                    chosen = entry
            if chosen is not None:
                return [chosen], False
        if self.multipart:
            return [e for c, e in self.multipart if c is None or c.match(props)], True
        return None, False

    def all_variants(self):
        for _, e in self.variants:
            yield from e
        for _, e in self.multipart:
            yield from e


def parse_blockstate(name, data):
    """data — загруженный JSON (dict) -> BlockStateDef."""
    bs = BlockStateDef(name)
    has_v = 'variants' in data
    has_m = 'multipart' in data
    if not has_v and not has_m:
        raise ValueError("blockstate %s: нет ни 'variants', ни 'multipart'" % name)
    if has_v:
        for key, v in data['variants'].items():
            bs.variants.append((_parse_key(key), _entry(v)))
    if has_m:
        bs.is_multipart = True
        for part in data['multipart']:
            cond = _Cond(part['when']) if 'when' in part else None
            bs.multipart.append((cond, _entry(part['apply'])))
    return bs


def load_blockstate(assets_dir, name):
    """assets_dir — каталог `.../assets/minecraft`; name — 'stone' или 'minecraft:stone'."""
    n = name.split(':', 1)[-1]
    with open(os.path.join(assets_dir, 'blockstates', n + '.json'), encoding='utf-8') as f:
        return parse_blockstate('minecraft:' + n, json.load(f))

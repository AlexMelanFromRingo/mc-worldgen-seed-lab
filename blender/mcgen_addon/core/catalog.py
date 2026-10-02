"""Каталог версий/измерений/пресетов для выпадающих списков интерфейса: быстрые ответы из кэша без открытия библиотеки.

До первого открытия McGen версии используются статические значения (как у макета); после открытия настоящей библиотеки
update_from_gen() подменяет их списками из libmcgen (mcgen_dimension_*/mcgen_preset_*).
"""
from . import pack

DEFAULT_DIMENSIONS = ('minecraft:overworld', 'minecraft:the_nether', 'minecraft:the_end')
DEFAULT_PRESETS = {
    'minecraft:overworld': ('normal', 'large_biomes', 'amplified'),
    'minecraft:the_nether': ('normal',),
    'minecraft:the_end': ('normal',),
}
_cache = {}                      # версия -> {'dims': [...], 'presets': {dim: [...]}}

DIM_LABEL = {'minecraft:overworld': 'Overworld', 'minecraft:the_nether': 'Nether', 'minecraft:the_end': 'The End'}
PRESET_LABEL = {'normal': 'Default', 'large_biomes': 'Large Biomes', 'amplified': 'Amplified', 'floating_islands': 'Floating Islands', 'caves': 'Caves'}


def versions():
    return pack.available_versions()


def dimensions(version):
    return list(_cache.get(version, {}).get('dims') or DEFAULT_DIMENSIONS)


def presets(version, dimension):
    c = _cache.get(version, {}).get('presets', {})
    return list(c.get(dimension) or DEFAULT_PRESETS.get(dimension, ('normal',)))


def update_from_gen(version, gen):
    dims = gen.dimensions()
    _cache[version] = {'dims': dims, 'presets': {d: gen.presets(d) for d in dims}}


def has(version):
    return version in _cache


def refresh(version, pack_dir):
    """Открывает McGen выбранной версии (кэшируется backend.open_gen) и берёт из него измерения/пресеты. Ошибки не пробрасываются."""
    from . import backend
    try:
        update_from_gen(version, backend.open_gen(pack_dir, version))
        return True
    except Exception:       # noqa: BLE001 - нет ресурсов/библиотеки: остаются статические значения
        return False


def clear():
    _cache.clear()


def pretty_dim(d):
    return DIM_LABEL.get(d, d.split(':', 1)[-1].replace('_', ' ').title())


def pretty_preset(p):
    return PRESET_LABEL.get(p, p.replace('_', ' ').title())

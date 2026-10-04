"""Параметры генерации как простой словарь/датакласс (без bpy) + анализ «что изменилось» для Update Layers."""
from dataclasses import dataclass, field, replace
import json
import math

from . import paths

STAGE_ORDER = ('biomes', 'terrain', 'surface', 'carvers', 'features', 'structures')
STAGE_BIT = {'biomes': 1, 'terrain': 2, 'surface': 4, 'carvers': 8, 'features': 16, 'structures': 32}
STAGE_LABEL = {'biomes': 'Biomes', 'terrain': 'Terrain', 'surface': 'Surface', 'carvers': 'Caves', 'features': 'Features', 'structures': 'Structures'}
SEED_STAGE = ('biomes', 'terrain', 'structures', 'features')      # сид домена climate/terrain/structures/features -> нижняя стадия, на которую влияет
VIEW_KEYS = ('tint_biomes', 'water_style', 'chunks_per_object', 'greedy_merge', 'lod_mode', 'lod_near', 'pixel_style', 'y_min', 'y_max')

_tweaks_doc = None


def tweaks_doc():
    global _tweaks_doc
    if _tweaks_doc is None:
        with open(paths.tweaks_json_path(), encoding='utf-8') as f:
            _tweaks_doc = json.load(f)
    return _tweaks_doc


def tweak_stage(tid):
    for t in tweaks_doc()['tweaks']:
        if t['id'] == tid:
            return t['stage']
    return 'biomes'


@dataclass(frozen=True)
class GenParams:
    version: str = '26.3'
    dimension: str = 'minecraft:overworld'
    preset: str = 'normal'
    seeds: tuple = (12345, 12345, 12345, 12345)           # climate, terrain, structures, features
    tweaks: tuple = ()                                    # ((id, value), …) отличные от умолчания; пусто = ваниль
    cx0: int = 0
    cz0: int = 0
    nx: int = 8
    nz: int = 8
    stages: int = 7                                       # маска MC_STAGE_* (BIOMES всегда включена)
    threads: int = 0
    schedule: str = ''                                    # путь к .mcsched — расписание записанного прогона сервера (воспроизведение декораций); пусто = модель планировщика
    view: tuple = ()                                      # ((ключ, значение), …) — только отображение, не влияет на данные

    def tweaks_dict(self):
        return dict(self.tweaks)

    def view_dict(self):
        return dict(self.view)

    def world_key(self):
        return (self.version, self.dimension, self.preset, self.seeds, self.tweaks, self.schedule)

    def area(self):
        return (self.cx0, self.cz0, self.nx, self.nz)

    def chunk_count(self):
        return self.nx * self.nz


def stage_mask(terrain=True, surface=True, carvers=True, features=False, structures=False):
    m = STAGE_BIT['biomes']
    for flag, name in ((terrain, 'terrain'), (surface, 'surface'), (carvers, 'carvers'), (features, 'features'), (structures, 'structures')):
        if flag:
            m |= STAGE_BIT[name]
    # зависимости: поверхность/пещеры/фичи/постройки без рельефа бессмысленны — рельеф включается молча
    if m & ~(STAGE_BIT['biomes']) and not m & STAGE_BIT['terrain']:
        m = STAGE_BIT['biomes']
    return m


def lowest_stage(names):
    for s in STAGE_ORDER:
        if s in names:
            return s
    return None


def diff(old, new):
    """Что изменилось между двумя GenParams: {'world': bool, 'area': bool, 'stages': bool, 'view': bool, 'from_stage': str|None, 'reasons': [...]}.

    from_stage — самая НИЖНЯЯ затронутая стадия (biomes < terrain < surface < carvers < features < structures): всё выше пересчитывается.
    Пересчёт данных (world/area/stages) требует вызова libmcgen; view — только перестройка мешей.
    """
    r = {'world': False, 'area': False, 'stages': False, 'view': False, 'from_stage': None, 'reasons': []}
    if old is None:
        r.update(world=True, area=True, from_stage='biomes')
        r['reasons'].append('first run')
        return r
    touched = set()
    if (old.version, old.dimension, old.preset) != (new.version, new.dimension, new.preset):
        touched.add('biomes')
        r['reasons'].append('version/dimension/preset')
    for i, (a, b) in enumerate(zip(old.seeds, new.seeds)):
        if a != b:
            touched.add(SEED_STAGE[i])
            r['reasons'].append('seed ' + ('climate', 'terrain', 'structures', 'features')[i])
    od, nd = old.tweaks_dict(), new.tweaks_dict()
    for k in set(od) | set(nd):
        if od.get(k) != nd.get(k):
            touched.add(tweak_stage(k))
            r['reasons'].append('tweak ' + k)
    if old.schedule != new.schedule:
        touched.add('features')
        r['reasons'].append('schedule')
    if touched:
        r['world'] = True
    if old.stages != new.stages:
        r['stages'] = True
        changed = old.stages ^ new.stages
        for name, bit in STAGE_BIT.items():
            if changed & bit:
                touched.add(name)
        r['reasons'].append('layers')
    if old.area() != new.area():
        r['area'] = True
        r['reasons'].append('area')
    if old.view != new.view:
        r['view'] = True
        r['reasons'].append('view')
    r['from_stage'] = lowest_stage(touched)
    return r


def area_inside(inner, outer):
    """Область inner (cx0, cz0, nx, nz) целиком внутри outer."""
    ix, iz, inx, inz = inner
    ox, oz, onx, onz = outer
    return ix >= ox and iz >= oz and ix + inx <= ox + onx and iz + inz <= oz + onz


def block_to_chunk(v):
    return math.floor(v / 16)

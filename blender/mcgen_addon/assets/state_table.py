"""Таблица «состояние блока → квады» для C-ядра меширования и Python-слоя.

    table = state_table.load(assets_dir, pack_dir, cache_dir)

Для КАЖДОГО состояния из reports/blocks.json: список частей (групп) → взвешенные варианты → квады (4 вершины, UV в атласе, cullface,
тонировка, слой прозрачности, направление затенения), классификация (воздух / полный непрозрачный куб / частичный / cutout / translucent /
жидкость / особый блок без модели), маски окклюзии по 6 граням для отсечения, смещение модели, признаки жидкости, пропуска граней.
Всё выводится из blockstates, моделей, альфа-канала текстур, тегов датапака и небольшой таблицы блок-уровневых фактов игры
(`blockprops_data`, сгенерирована из кода игры). Результат кэшируется на диск (npz) по хэшу ресурсов. Чистый Python (numpy), без bpy.
"""
import json
import os
import time

import numpy as np

from . import atlas as atlas_mod
from . import blockprops_data as bp
from . import blockstates
from . import cache as cache_mod
from . import entity_models
from . import fluids
from . import models
from . import tint as tint_mod

__all__ = ['StateTable', 'load', 'build', 'F', 'CLASS_NAMES', 'SKIP_NONE', 'SKIP_SAME', 'SKIP_LEAVES', 'SKIP_BARS', 'SKIP_POWDER',
           'SKIP_ROOTS', 'SKIP_LIQUID', 'assets_root']


class F:
    """Биты флагов состояния (state_flags, uint32)."""
    GEOM = 1 << 0          # у состояния есть квады модели
    AIR = 1 << 1           # воздух (ни геометрии, ни жидкости)
    OPAQUE = 1 << 2        # полный непрозрачный куб (все 6 граней закрывают целиком) — isSolidRender
    SOLID = 1 << 3         # BlockState.isSolid()
    WATER = 1 << 4         # в состоянии есть вода
    LAVA = 1 << 5          # в состоянии есть лава
    FALLING = 1 << 6       # падающая жидкость
    RANDOM = 1 << 7        # хотя бы у одной части несколько вариантов (выбор по позиции)
    MULTIPART = 1 << 8     # модель multipart (случайное число берётся через nextLong, как в игре)
    OFF_XZ = 1 << 9
    OFF_XYZ = 1 << 10
    LEAVES = 1 << 11
    BARS = 1 << 12         # тег #minecraft:bars
    BLOCKS_FLOW = 1 << 13  # тег #minecraft:blocks_fluid_flow
    TINT_BELOW = 1 << 14   # оттенок брать на блок ниже (верхняя половина двойной травы)
    FLUID_AMOUNT_SHIFT = 16   # 4 бита: FluidState.getAmount() 0..8
    SKIP_SHIFT = 20           # 3 бита: класс skipRendering
    CONN_SHIFT = 24           # 4 бита: связи панелей/решёток N, S, W, E (для SKIP_BARS)
    SPECIAL = 1 << 28      # блок рисуется «особой» моделью (сундук, баннер, …) — в ресурсах геометрии нет
    NOMODEL = 1 << 29      # нет blockstate/модели (игра рисует missing; здесь пропуск)


SKIP_NONE, SKIP_SAME, SKIP_LEAVES, SKIP_BARS, SKIP_POWDER, SKIP_ROOTS, SKIP_LIQUID = range(7)
_SKIP_CODE = {'H': SKIP_SAME, 'L': SKIP_LEAVES, 'B': SKIP_BARS, 'P': SKIP_POWDER, 'M': SKIP_ROOTS, 'W': SKIP_LIQUID}

CLASS_NAMES = ('air', 'cube', 'partial', 'cutout', 'translucent', 'fluid', 'special', 'nomodel')
C_AIR, C_CUBE, C_PARTIAL, C_CUTOUT, C_TRANSLUCENT, C_FLUID, C_SPECIAL, C_NOMODEL = range(8)

_SPECIAL_CLASSES = ('AbstractChestBlock', 'ShulkerBoxBlock', 'AbstractBannerBlock', 'AbstractSkullBlock', 'BellBlock', 'ConduitBlock',
                    'DecoratedPotBlock', 'EnchantingTableBlock', 'EndPortalBlock', 'EndGatewayBlock', 'CopperGolemStatueBlock')
SPECIAL_BLOCKS = frozenset().union(*[bp.CLASS_MEMBERS.get(c, frozenset()) for c in _SPECIAL_CLASSES])
AIR_BLOCKS = frozenset(('air', 'cave_air', 'void_air'))

DIRS_UV = ((0, 2), (0, 2), (0, 1), (0, 1), (1, 2), (1, 2))   # оси (u, v) маски грани: DOWN/UP: x,z; NORTH/SOUTH: x,y; WEST/EAST: y,z
FULL_MASK = b'\xff' * 32


def _mergeable(q):
    """Грань годится для слияния (жадное слияние + повтор тайла): единичный квад ровно на границе блока, вершины — углы единичного
    квадрата, UV — углы всего спрайта (0..1) в любой ориентации."""
    d = q.dir
    ax = 1 if d < 2 else (2 if d < 4 else 0)
    target = 1.0 if d in (1, 3, 5) else 0.0
    if any(abs(p[ax] - target) > 1e-6 for p in q.pos):
        return 0
    ia, ib = [i for i in range(3) if i != ax]
    corners = set()
    for p in q.pos:
        a, b = p[ia], p[ib]
        if min(abs(a), abs(a - 1)) > 1e-6 or min(abs(b), abs(b - 1)) > 1e-6:
            return 0
        corners.add((round(a), round(b)))
    if len(corners) != 4:
        return 0
    uvs = set()
    for u, v in q.uv:
        if min(abs(u), abs(u - 1)) > 1e-6 or min(abs(v), abs(v - 1)) > 1e-6:
            return 0
        uvs.add((round(u), round(v)))
    return 1 if len(uvs) == 4 else 0


COPLANAR_EPS = 0.002   # на сколько блока приподнимается накладка, лежащая в одной плоскости с нижней гранью (оверлей травы и т. п.)


def _coplanar_offsets(quads):
    """Смещения (в блоках вдоль нормали) для квадов, копланарных с более ранним квадом той же модели и перекрывающихся с ним.
    В игре порядок отрисовки решает, кто сверху; в Blender копланарные грани дают z-fighting и чёрные пятна у прозрачных текселей."""
    n = len(quads)
    out = [0.0] * n
    info = []
    for q in quads:
        d = q.dir
        ax = 1 if d < 2 else (2 if d < 4 else 0)
        c = [p[ax] for p in q.pos]
        if max(c) - min(c) > 1e-5:
            info.append(None)
            continue
        ua, va = DIRS_UV[d]
        us = [p[ua] for p in q.pos]
        vs = [p[va] for p in q.pos]
        info.append((d, c[0], min(us), max(us), min(vs), max(vs)))
    for i in range(n):
        a = info[i]
        if a is None:
            continue
        k = 0
        for j in range(i):
            b = info[j]
            if b is None or b[0] != a[0] or abs(b[1] - a[1]) > 1e-4:
                continue
            if min(a[3], b[3]) - max(a[2], b[2]) > 1e-5 and min(a[5], b[5]) - max(a[4], b[4]) > 1e-5:
                k += 1
        out[i] = k * COPLANAR_EPS
    return out


def _mul_rgb(a, b):
    return (((a >> 16) & 255) * ((b >> 16) & 255) // 255 << 16) | (((a >> 8) & 255) * ((b >> 8) & 255) // 255 << 8) | ((a & 255) * (b & 255) // 255)


def assets_root(assets_dir):
    """Принимает `run/assets-26.3` или `.../assets/minecraft` -> `.../assets/minecraft`."""
    for cand in (assets_dir, os.path.join(assets_dir, 'minecraft'), os.path.join(assets_dir, 'assets', 'minecraft')):
        if os.path.isdir(os.path.join(cand, 'blockstates')):
            return cand
    raise FileNotFoundError('не найден каталог blockstates в %s (ожидается .../assets/minecraft)' % assets_dir)


# ----------------------------------------------------------------------------------------------------------------------
#                                                      теги
# ----------------------------------------------------------------------------------------------------------------------

def load_block_tag(pack_dir, tag, _seen=None):
    """Раскрывает тег блоков `#minecraft:name` (рекурсивно по ссылкам #) -> множество коротких имён блоков."""
    name = tag[1:] if tag.startswith('#') else tag
    name = name.split(':', 1)[-1]
    seen = _seen if _seen is not None else set()
    if name in seen:
        return set()
    seen.add(name)
    p = os.path.join(pack_dir, 'data', 'minecraft', 'tags', 'block', name + '.json')
    out = set()
    if not os.path.isfile(p):
        return out
    with open(p, encoding='utf-8') as f:
        d = json.load(f)
    for v in d.get('values', []):
        v = v['id'] if isinstance(v, dict) else v
        if v.startswith('#'):
            out |= load_block_tag(pack_dir, v, seen)
        else:
            out.add(v.split(':', 1)[-1])
    return out


# ----------------------------------------------------------------------------------------------------------------------
#                                                      маски граней
# ----------------------------------------------------------------------------------------------------------------------

_CELL = (np.arange(16) + 0.5) / 16.0


def _quad_face_rect(q):
    """Если квад лежит на границе блока в плоскости своего направления и осево-выровнен — прямоугольник (u0, u1, v0, v1) в долях блока."""
    d = q.dir
    ax = 1 if d < 2 else (2 if d < 4 else 0)
    target = 1.0 if d in (1, 3, 5) else 0.0
    for p in q.pos:
        if abs(p[ax] - target) > 1e-4:
            return None
    ua, va = DIRS_UV[d]
    us = [p[ua] for p in q.pos]
    vs = [p[va] for p in q.pos]
    u0, u1, v0, v1 = min(us), max(us), min(vs), max(vs)
    # прямоугольник ли (4 вершины — углы bbox)
    for p in q.pos:
        if not ((abs(p[ua] - u0) < 1e-4 or abs(p[ua] - u1) < 1e-4) and (abs(p[va] - v0) < 1e-4 or abs(p[va] - v1) < 1e-4)):
            return None
    return u0, u1, v0, v1


def _rect_mask(u0, u1, v0, v1):
    """bool[16(u), 16(v)] — клетки, центры которых внутри прямоугольника."""
    eps = 1e-6
    mu = (_CELL >= u0 - eps) & (_CELL <= u1 + eps)
    mv = (_CELL >= v0 - eps) & (_CELL <= v1 + eps)
    return np.outer(mu, mv)


def _pack_mask(m):
    return np.packbits(m.reshape(-1)).tobytes()


# ----------------------------------------------------------------------------------------------------------------------
#                                                    таблица
# ----------------------------------------------------------------------------------------------------------------------

class StateTable:
    """Результат `load`. Массивы NumPy — это таблицы для C-ядра (см. mesh/mesher.py), плюс служебные поля."""

    ARRAYS = ('st_flags', 'st_block', 'st_occ', 'st_sturdy', 'st_grp_off', 'st_off_h', 'st_off_v', 'st_class', 'grp_list', 'grp_var_off',
              'grp_total', 'var_baked', 'var_weight', 'baked_q_off', 'q_pos', 'q_uv', 'q_cull', 'q_dir', 'q_shade', 'q_tint', 'q_rgb',
              'q_layer', 'q_merge', 'occ_masks', 'occ_cover', 'fluid_rect', 'fluid_layer', 'atlas_image', 'atlas_rect', 'swamp_perm', 'block_default', 'st_lod_rgb', 'st_lod_tint', 'st_lod_kind')

    def __init__(self):
        self.version = None
        self.names = []          # имя состояния с свойствами: 'minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]'
        self.block_names = []    # индекс блока -> 'minecraft:oak_stairs'
        self.atlas_names = []
        self.atlas_info = {}
        self.stats = {}
        self.build_seconds = 0.0
        self.source_hash = ''
        self._name_index = None
        self._block_index = None
        self.biome_colors = None
        for a in self.ARRAYS:
            setattr(self, a, None)

    # --- сведения ---
    @property
    def n_states(self):
        return len(self.names)

    @property
    def n_quads(self):
        return int(self.q_pos.shape[0])

    @staticmethod
    def _canon(name):
        """Каноническое имя: свойства по алфавиту, без пустых скобок."""
        if '[' not in name:
            return name
        base, rest = name.split('[', 1)
        props = [p for p in rest.rstrip(']').split(',') if p]
        return base + ('[' + ','.join(sorted(props)) + ']' if props else '')

    def state_id(self, name):
        """Имя состояния (`minecraft:oak_stairs[facing=north,half=bottom,…]`, свойства в любом порядке; `minecraft:stone` или
        `minecraft:stone[]`) -> id либо -1."""
        if self._name_index is None:
            self._name_index = {self._canon(n): i for i, n in enumerate(self.names)}
        return self._name_index.get(self._canon(name), -1)

    def default_state(self, block_name):
        """id состояния блока по умолчанию (`minecraft:oak_stairs` -> ...) либо -1."""
        if self._block_index is None:
            self._block_index = {n: i for i, n in enumerate(self.block_names)}
        bi = self._block_index.get(block_name if ':' in block_name else 'minecraft:' + block_name)
        return -1 if bi is None else int(self.block_default[bi])

    def state_from_props(self, block_name, props=None):
        """Состояние по имени блока и (возможно неполным) словарю свойств; недостающие берутся из состояния по умолчанию."""
        d = self.default_state(block_name)
        if d < 0:
            return -1
        if not props:
            return d
        name = self.names[d]
        base, _, rest = name.partition('[')
        cur = {}
        for kv in rest.rstrip(']').split(','):
            if kv:
                k, v = kv.split('=', 1)
                cur[k] = v
        cur.update({str(k): str(v) for k, v in props.items()})
        return self.state_id(base + '[' + ','.join('%s=%s' % kv for kv in cur.items()) + ']')

    def state_props(self, sid):
        """Свойства состояния: (имя блока, {свойство: значение})."""
        name = self.names[sid]
        base, _, rest = name.partition('[')
        d = {}
        for kv in rest.rstrip(']').split(','):
            if kv:
                k, v = kv.split('=', 1)
                d[k] = v
        return base, d

    def classify(self, sid):
        """Класс состояния: 'air' | 'cube' | 'partial' | 'cutout' | 'translucent' | 'fluid' | 'special' | 'nomodel'."""
        return CLASS_NAMES[int(self.st_class[sid])]

    def flags(self, sid):
        return int(self.st_flags[sid])

    def is_opaque_cube(self, sid):
        return bool(self.st_flags[sid] & F.OPAQUE)

    def block_name(self, sid):
        return self.block_names[int(self.st_block[sid])]

    def describe(self, sid):
        """Текстовая сводка по состоянию (для отладки и отчёта)."""
        s = self.names[sid]
        fl = int(self.st_flags[sid])
        groups = []
        for gi in range(int(self.st_grp_off[sid]), int(self.st_grp_off[sid + 1])):
            g = int(self.grp_list[gi])
            vs = []
            for vi in range(int(self.grp_var_off[g]), int(self.grp_var_off[g + 1])):
                b = int(self.var_baked[vi])
                vs.append('w%d:%dq' % (int(self.var_weight[vi]), int(self.baked_q_off[b + 1] - self.baked_q_off[b])))
            groups.append('[' + ' '.join(vs) + ']')
        return '%s class=%s flags=0x%x occ=%s groups=%s' % (s, self.classify(sid), fl, list(map(int, self.st_occ[sid])), ' '.join(groups))

    def set_biomes(self, biome_names, assets_dir, pack_dir):
        """Строит таблицу цветов по id биомов (порядок = порядок `biome_names`)."""
        a = assets_root(assets_dir)
        cm = tint_mod.load_colormaps(a)
        self.biome_colors = tint_mod.BiomeColors(biome_names, tint_mod.load_biomes(pack_dir), cm)
        return self.biome_colors

    # --- сохранение ---
    def save(self, path):
        os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
        meta = {
            'version': self.version, 'block_names': self.block_names, 'atlas_names': self.atlas_names,
            'atlas_info': self.atlas_info, 'stats': self.stats, 'build_seconds': self.build_seconds, 'source_hash': self.source_hash,
            'names': '\n'.join(self.names),
        }
        arrs = {a: getattr(self, a) for a in self.ARRAYS}
        arrs['meta'] = np.frombuffer(json.dumps(meta).encode('utf-8'), dtype=np.uint8)
        tmp = path + '.tmp.npz'
        np.savez_compressed(tmp, **arrs)
        os.replace(tmp, path)

    @classmethod
    def from_npz(cls, path):
        t = cls()
        with np.load(path) as z:
            for a in cls.ARRAYS:
                setattr(t, a, np.ascontiguousarray(z[a]))
            meta = json.loads(bytes(z['meta']).decode('utf-8'))
        t.version = meta['version']
        t.block_names = meta['block_names']
        t.atlas_names = meta['atlas_names']
        t.atlas_info = meta['atlas_info']
        t.stats = meta['stats']
        t.build_seconds = meta['build_seconds']
        t.source_hash = meta['source_hash']
        t.names = meta['names'].split('\n')
        return t


def load(assets_dir, pack_dir, cache_dir, version=None, progress=None, force=False):
    """Основная точка входа: таблица из кэша (по хэшу ресурсов) или построение с сохранением в кэш.

    assets_dir — каталог ресурсов клиента (`run/assets-26.3` либо `.../assets/minecraft`), pack_dir — pack-каталог (`run/pack-26.3`:
    reports/blocks.json, data/minecraft/**), cache_dir — каталог кэша аддона."""
    root = assets_root(assets_dir)
    sha = cache_mod.resource_hash(root, pack_dir, cache_dir)
    path = cache_mod.cache_path(cache_dir, sha)
    if not force and os.path.isfile(path):
        try:
            t = StateTable.from_npz(path)
            if version:
                t.version = version
            return t
        except Exception:   # битый кэш — пересобираем
            pass
    t = build(assets_dir, pack_dir, version=version, progress=progress)
    t.source_hash = sha
    t.save(path)
    return t


# ----------------------------------------------------------------------------------------------------------------------
#                                                    сборка
# ----------------------------------------------------------------------------------------------------------------------

def _pack_state_name(block, props):
    if not props:
        return block
    return block + '[' + ','.join('%s=%s' % (k, props[k]) for k in props) + ']'


def build(assets_dir, pack_dir, version=None, progress=None):
    t0 = time.time()
    root = assets_root(assets_dir)
    textures_dir = os.path.join(root, 'textures')
    with open(os.path.join(pack_dir, 'reports', 'blocks.json'), encoding='utf-8') as f:
        blocks = json.load(f)
    say = progress or (lambda frac, what: None)

    # --- состояния ---
    S = 0
    for info in blocks.values():
        for st in info['states']:
            S = max(S, st['id'] + 1)
    names = [''] * S
    block_names = list(blocks.keys())
    state_block = np.zeros(S, dtype=np.uint16)
    state_props = [None] * S
    block_default = np.zeros(len(block_names), dtype=np.uint32)
    for bi, (bname, info) in enumerate(blocks.items()):
        block_default[bi] = info['states'][0]['id']
        for st in info['states']:
            if st.get('default'):
                block_default[bi] = st['id']
            sid = st['id']
            props = st.get('properties') or {}
            state_props[sid] = props
            state_block[sid] = bi
            names[sid] = _pack_state_name(bname, props)
    if any(n == '' for n in names):
        raise ValueError('blocks.json: id состояний не непрерывны')

    tag_bars = load_block_tag(pack_dir, 'minecraft:bars')
    tag_flow = load_block_tag(pack_dir, 'minecraft:blocks_fluid_flow')
    store = models.ModelStore(root)
    errors = {}

    # --- разбор blockstates -> части состояний ---
    say(0.02, 'blockstates')
    state_parts = [None] * S           # список частей (каждая — список Variant), [] — нет геометрии
    state_multipart = np.zeros(S, dtype=bool)
    state_kind = np.zeros(S, dtype=np.uint8)   # 0 обычный, 1 special, 2 nomodel, 3 invisible/air
    for bi, (bname, info) in enumerate(blocks.items()):
        short = bname.split(':', 1)[-1]
        sids = [st['id'] for st in info['states']]
        ent = entity_models.is_entity_block(short)
        if short in AIR_BLOCKS or (short in bp.INVISIBLE and short != 'end_portal'):
            for sid in sids:
                state_parts[sid] = []
                state_kind[sid] = 3
            continue
        if short == 'end_portal':
            for sid in sids:
                state_parts[sid] = [[blockstates.Variant('entity:' + short + '|')]]
            continue
        try:
            bsd = blockstates.load_blockstate(root, short)
        except (OSError, ValueError, KeyError) as e:
            if not ent:
                errors[bname] = 'blockstate: %s' % e
            bsd = None
        for sid in sids:
            if bsd is not None:
                parts, mp = bsd.select(state_props[sid])
            else:
                parts, mp = None, False
            if parts is None:
                parts = []
                state_kind[sid] = 1 if short in SPECIAL_BLOCKS else 2
            else:
                state_parts[sid] = parts
                state_multipart[sid] = mp
            if ent:
                pk = ','.join('%s=%s' % kv for kv in sorted(state_props[sid].items()))
                state_parts[sid] = list(parts) + [[blockstates.Variant('entity:' + short + '|' + pk)]]
                state_kind[sid] = 0
            elif short in SPECIAL_BLOCKS and not parts:
                state_kind[sid] = 1
            if state_parts[sid] is None:
                state_parts[sid] = parts

    # --- запекание уникальных вариантов ---
    say(0.10, 'модели')
    vkeys = {}
    for sid in range(S):
        for part in state_parts[sid]:
            for v in part:
                vkeys[v.key()] = v
    baked = {}
    for k in vkeys:
        if k[0].startswith('entity:'):
            short, _, pk = k[0][7:].partition('|')
            props = dict(kv.split('=', 1) for kv in pk.split(',') if kv)
            baked[k] = entity_models.entity_quads(short, props)
        else:
            baked[k] = store.bake(*k)
    for name, e in store.errors.items():
        errors['model:' + name] = e
    for k in vkeys:
        if k[0].startswith('entity:'):
            continue
        m = store.resolve(k[0])
        if m.error:
            errors['model:' + k[0]] = m.error

    # --- атлас ---
    say(0.45, 'атлас')
    tex_names = set()
    for qs in baked.values():
        for q in qs:
            tex_names.add(q.tex)
    for tt in fluids.FLUID_TEXTURES.values():
        for n in tt:
            if n:
                tex_names.add(n)
    atl = atlas_mod.build_atlas(textures_dir, sorted(tex_names), progress=lambda f, w: say(0.45 + 0.1 * f, w))
    for n in atl.missing_names:
        errors['texture:' + n] = 'нет файла текстуры'

    # --- квады в массивы (по уникальным (вариант, оттенок)) ---
    say(0.60, 'квады')
    q_pos, q_uv, q_cull, q_dir, q_shade, q_tint, q_rgb, q_layer, q_merge = [], [], [], [], [], [], [], [], []
    baked_arr = {}          # (vkey, tintsig) -> (id в таблице, число квадов)
    baked_q_off = [0]
    baked_quads_py = []     # для расчёта масок: список Quad (с их слоями)
    baked_layers = []
    nq = 0

    def add_baked(vkey, tsig):
        nonlocal nq
        key = (vkey, tsig)
        r = baked_arr.get(key)
        if r is not None:
            return r
        quads = baked[vkey]
        layers = []
        offs = _coplanar_offsets(quads)
        for qi, q in enumerate(quads):
            si = atl.sprite(q.tex)
            us = [u for u, _ in q.uv]
            vs = [v for _, v in q.uv]
            lay = atl.transparency(si, min(us), min(vs), max(us), max(vs), q.force_translucent)
            layers.append(lay)
            u0, v0, u1, v1 = atl.rect[si]
            pa = np.asarray(q.pos, dtype=np.float32)
            if offs[qi]:
                pa = pa + np.asarray(models.DIR_VEC[q.dir], dtype=np.float32) * np.float32(offs[qi])
            q_pos.append(pa)
            q_uv.append(np.asarray([(u0 + (u1 - u0) * u, v0 + (v1 - v0) * v) for u, v in q.uv], dtype=np.float32))
            q_cull.append(q.cull)
            q_dir.append(q.dir)
            q_shade.append(q.shade)
            if q.tint >= 0 and q.tint < len(tsig):
                kind, rgb = tsig[q.tint]
                q_tint.append(kind)
                q_rgb.append(rgb if kind == tint_mod.CONST else 0xFFFFFF)
            else:
                q_tint.append(tint_mod.NONE)
                q_rgb.append(0xFFFFFF)
            q_layer.append(lay)
            q_merge.append(_mergeable(q) if not offs[qi] else 0)
        baked_q_off.append(baked_q_off[-1] + len(quads))
        bid = len(baked_arr)
        baked_arr[key] = bid
        baked_quads_py.append(quads)
        baked_layers.append(layers)
        return bid

    var_has_tint = {k: any(q.tint >= 0 for q in qs) for k, qs in baked.items()}

    # группы и варианты (дедупликация)
    group_key_to_id = {}
    grp_var_off = [0]
    grp_total = []
    var_baked = []
    var_weight = []
    st_grp_off = [0]
    grp_list = []
    state_geom_key = [None] * S
    state_tsig = [()] * S
    cache_state_groups = {}

    for sid in range(S):
        parts = state_parts[sid]
        if not parts:
            st_grp_off.append(len(grp_list))
            continue
        bname = block_names[state_block[sid]]
        tsig_full = tint_mod.tint_sources(bname, state_props[sid])
        gk = (tuple(tuple((v.key(), v.weight) for v in part) for part in parts), tsig_full)
        ids = cache_state_groups.get(gk)
        if ids is None:
            ids = []
            for part in parts:
                keyg = []
                for v in part:
                    ts = tsig_full if var_has_tint[v.key()] else ()
                    keyg.append((add_baked(v.key(), ts), v.weight))
                keyg = tuple(keyg)
                gid = group_key_to_id.get(keyg)
                if gid is None:
                    gid = len(grp_total)
                    group_key_to_id[keyg] = gid
                    for b, w in keyg:
                        var_baked.append(b)
                        var_weight.append(w)
                    grp_var_off.append(len(var_baked))
                    grp_total.append(sum(w for _, w in keyg))
                ids.append(gid)
            cache_state_groups[gk] = ids
        grp_list.extend(ids)
        st_grp_off.append(len(grp_list))
        state_geom_key[sid] = (tuple(ids), state_multipart[sid])

    # --- маски окклюзии и «прочная» грань ---
    say(0.75, 'окклюзия')
    mask_ids = {b'\0' * 32: 0, FULL_MASK: 1}
    mask_list = [b'\0' * 32, FULL_MASK]
    geom_mask_cache = {}

    def masks_for(ids, can_occlude):
        key = (ids, can_occlude)
        r = geom_mask_cache.get(key)
        if r is not None:
            return r
        occ = [np.zeros((16, 16), dtype=bool) for _ in range(6)]
        stu = [np.zeros((16, 16), dtype=bool) for _ in range(6)]
        for gid in ids:
            vi = grp_var_off[gid]          # геометрия по первому варианту части
            b = var_baked[vi]
            quads, layers = baked_quads_py[b], baked_layers[b]
            for q, lay in zip(quads, layers):
                rc = _quad_face_rect(q)
                if rc is None:
                    continue
                m = _rect_mask(*rc)
                stu[q.dir] |= m
                if lay == 0:
                    occ[q.dir] |= m
        om = []
        for d in range(6):
            mk = _pack_mask(occ[d]) if can_occlude else b'\0' * 32
            i = mask_ids.get(mk)
            if i is None:
                i = len(mask_list)
                mask_ids[mk] = i
                mask_list.append(mk)
            om.append(i)
        sb = 0
        for d in range(6):
            if stu[d].all():
                sb |= 1 << d
        r = (tuple(om), sb)
        geom_mask_cache[key] = r
        return r

    st_flags = np.zeros(S, dtype=np.uint32)
    st_occ = np.zeros((S, 6), dtype=np.uint16)
    st_sturdy = np.zeros(S, dtype=np.uint8)
    st_off_h = np.zeros(S, dtype=np.float32)
    st_off_v = np.zeros(S, dtype=np.float32)
    st_class = np.zeros(S, dtype=np.uint8)
    n_nomodel = n_special = n_invisible = 0

    for sid in range(S):
        bname = block_names[state_block[sid]]
        short = bname.split(':', 1)[-1]
        props = state_props[sid]
        fl = 0
        if short in AIR_BLOCKS:
            fl |= F.AIR
        fk, famount, falling = fluids.fluid_state(bname, props)
        if fk == fluids.FLUID_WATER:
            fl |= F.WATER
        elif fk == fluids.FLUID_LAVA:
            fl |= F.LAVA
        if falling:
            fl |= F.FALLING
        fl |= famount << F.FLUID_AMOUNT_SHIFT
        if short not in bp.NOT_SOLID:
            fl |= F.SOLID
        if state_multipart[sid]:
            fl |= F.MULTIPART
        if short in bp.OFFSET:
            ot, mh, mv = bp.OFFSET[short]
            fl |= F.OFF_XZ if ot == 1 else F.OFF_XYZ
            st_off_h[sid] = mh
            st_off_v[sid] = mv
        sk = bp.SKIP.get(short, '')
        if sk:
            skip = _SKIP_CODE[sk[0]]
            fl |= skip << F.SKIP_SHIFT
            if skip == SKIP_LEAVES:
                fl |= F.LEAVES
        if short in tag_bars:
            fl |= F.BARS
        if sk and _SKIP_CODE[sk[0]] == SKIP_BARS:
            conn = 0
            for i, dname in enumerate(('north', 'south', 'west', 'east')):
                if props.get(dname) == 'true':
                    conn |= 1 << i
            fl |= conn << F.CONN_SHIFT
        if short in ('tall_grass', 'large_fern') and props.get('half') == 'upper':
            fl |= F.TINT_BELOW
        if short in tag_flow:
            fl |= F.BLOCKS_FLOW
        kind = state_kind[sid]
        if short in SPECIAL_BLOCKS or entity_models.is_entity_block(short):
            fl |= F.SPECIAL
            n_special += 1
        elif kind == 2:
            fl |= F.NOMODEL
            n_nomodel += 1
        elif kind == 3:
            n_invisible += 1
        gk = state_geom_key[sid]
        has_geom = gk is not None and any(baked_q_off[var_baked[grp_var_off[g]] + 1] > baked_q_off[var_baked[grp_var_off[g]]]
                                          for g in gk[0])
        # у частей без квадов (пустая модель) геометрии нет
        if has_geom:
            fl |= F.GEOM
            can_occ = short not in bp.NO_OCCLUDE
            om, sb = masks_for(gk[0], can_occ)
            st_occ[sid] = om
            st_sturdy[sid] = sb
            if all(o == 1 for o in om):
                fl |= F.OPAQUE
            # класс
            lays = set()
            for g in gk[0]:
                for vi in range(grp_var_off[g], grp_var_off[g + 1]):
                    for lay in baked_layers[var_baked[vi]]:
                        lays.add(lay)
            if fl & F.OPAQUE:
                cl = C_CUBE
            elif 2 in lays:
                cl = C_TRANSLUCENT
            elif 1 in lays:
                cl = C_CUTOUT
            else:
                cl = C_PARTIAL
            if any(grp_total[g] > 1 for g in gk[0]):
                fl |= F.RANDOM
            st_class[sid] = cl
        else:
            if fk != fluids.FLUID_NONE:
                st_class[sid] = C_FLUID
            elif kind == 1 or (fl & F.SPECIAL):
                st_class[sid] = C_SPECIAL
            elif kind == 2:
                st_class[sid] = C_NOMODEL
            elif short in AIR_BLOCKS:
                st_class[sid] = C_AIR
            else:
                st_class[sid] = C_AIR
        st_flags[sid] = fl

    # --- LOD: средний цвет верхней грани, вид оттенка и признак «поверхность» по состояниям ---
    st_lod_rgb = np.zeros(S, dtype=np.uint32)
    st_lod_tint = np.zeros(S, dtype=np.uint8)
    st_lod_kind = np.zeros(S, dtype=np.uint8)
    img = atl.image
    Hh, Ww = img.shape[:2]
    sprite_avg = {}

    def avg_color(si):
        r = sprite_avg.get(si)
        if r is None:
            u0, v0, u1, v1 = atl.rect[si]
            x0, y0, x1, y1 = int(round(u0 * Ww)), int(round(v0 * Hh)), int(round(u1 * Ww)), int(round(v1 * Hh))
            px = img[y0:y1, x0:x1].reshape(-1, 4).astype(np.float64)
            w = px[:, 3]
            if w.sum() <= 0:
                r = 0x808080
            else:
                c = (px[:, :3] * w[:, None]).sum(axis=0) / w.sum()
                r = (int(c[0]) << 16) | (int(c[1]) << 8) | int(c[2])
            sprite_avg[si] = r
        return r

    for sid in range(S):
        cl = int(st_class[sid])
        fl = int(st_flags[sid])
        fk = (fl & F.WATER) != 0 or (fl & F.LAVA) != 0
        if fk and not (fl & F.GEOM):
            n_still = fluids.FLUID_TEXTURES[fluids.FLUID_WATER if fl & F.WATER else fluids.FLUID_LAVA][0]
            st_lod_rgb[sid] = avg_color(atl.sprite(n_still))
            st_lod_tint[sid] = tint_mod.WATER if fl & F.WATER else tint_mod.NONE
            st_lod_kind[sid] = 1
            continue
        if cl in (C_CUBE, C_PARTIAL, C_TRANSLUCENT) or (cl == C_CUTOUT and fl & F.LEAVES):
            gk = state_geom_key[sid]
            if gk is None:
                continue
            best = None
            for g in gk[0]:
                b = var_baked[grp_var_off[g]]
                for q in baked_quads_py[b]:
                    if q.dir == 1:
                        best = q
                        break
                if best is not None:
                    break
            if best is None:
                g = gk[0][0]
                qs = baked_quads_py[var_baked[grp_var_off[g]]]
                best = qs[0] if qs else None
            if best is None:
                continue
            st_lod_rgb[sid] = avg_color(atl.sprite(best.tex))
            bname = block_names[state_block[sid]]
            ts = tint_mod.tint_sources(bname, state_props[sid])
            kind = tint_mod.NONE
            if 0 <= best.tint < len(ts):
                kind, rgbc = ts[best.tint]
                if kind == tint_mod.CONST:
                    st_lod_rgb[sid] = _mul_rgb(st_lod_rgb[sid], rgbc)
                    kind = tint_mod.NONE
            st_lod_tint[sid] = kind
            st_lod_kind[sid] = 1

    # маски: покрытие cover[a][b] = a ⊆ b
    M = len(mask_list)
    mk = np.frombuffer(b''.join(mask_list), dtype=np.uint8).reshape(M, 32)
    cover = np.zeros((M, M), dtype=np.uint8)
    for a in range(M):
        cover[a] = ((mk[a][None, :] & ~mk) == 0).all(axis=1)

    # --- жидкости: прямоугольники текстур ---
    fluid_rect = np.zeros((5, 4), dtype=np.float32)
    names_f = [fluids.FLUID_TEXTURES[fluids.FLUID_WATER][0], fluids.FLUID_TEXTURES[fluids.FLUID_WATER][1],
               fluids.FLUID_TEXTURES[fluids.FLUID_WATER][2], fluids.FLUID_TEXTURES[fluids.FLUID_LAVA][0],
               fluids.FLUID_TEXTURES[fluids.FLUID_LAVA][1]]
    for i, n in enumerate(names_f):
        fluid_rect[i] = atl.rect[atl.sprite(n)]
    fluid_layer = np.zeros(2, dtype=np.uint8)
    for k, (a, b, c) in ((0, fluids.FLUID_TEXTURES[fluids.FLUID_WATER]), (1, fluids.FLUID_TEXTURES[fluids.FLUID_LAVA])):
        lay = 0
        for n in (a, b, c):
            if n:
                si = atl.sprite(n)
                lay = max(lay, atl.transparency(si, 0.0, 0.0, 1.0, 1.0))
        fluid_layer[k] = lay

    # --- итоговые массивы ---
    t = StateTable()
    t.version = version
    t.names = names
    t.block_names = block_names
    t.st_flags = st_flags
    t.st_block = state_block
    t.st_occ = st_occ
    t.st_sturdy = st_sturdy
    t.st_grp_off = np.asarray(st_grp_off, dtype=np.int32)
    t.st_off_h = st_off_h
    t.st_off_v = st_off_v
    t.st_class = st_class
    t.grp_list = np.asarray(grp_list, dtype=np.int32)
    t.grp_var_off = np.asarray(grp_var_off, dtype=np.int32)
    t.grp_total = np.asarray(grp_total, dtype=np.int32)
    t.var_baked = np.asarray(var_baked, dtype=np.int32)
    t.var_weight = np.asarray(var_weight, dtype=np.int32)
    t.baked_q_off = np.asarray(baked_q_off, dtype=np.int32)
    t.q_pos = np.ascontiguousarray(np.asarray(q_pos, dtype=np.float32).reshape(-1, 4, 3))
    t.q_uv = np.ascontiguousarray(np.asarray(q_uv, dtype=np.float32).reshape(-1, 4, 2))
    t.q_cull = np.asarray(q_cull, dtype=np.int8)
    t.q_dir = np.asarray(q_dir, dtype=np.uint8)
    t.q_shade = np.asarray(q_shade, dtype=np.uint8)
    t.q_tint = np.asarray(q_tint, dtype=np.uint8)
    t.q_rgb = np.asarray(q_rgb, dtype=np.uint32)
    t.q_layer = np.asarray(q_layer, dtype=np.uint8)
    t.q_merge = np.asarray(q_merge, dtype=np.uint8)
    t.occ_masks = mk.copy()
    t.occ_cover = cover
    t.fluid_rect = fluid_rect
    t.fluid_layer = fluid_layer
    t.atlas_image = atl.image
    t.atlas_rect = atl.rect
    t.atlas_names = atl.names
    t.atlas_info = atl.to_dict()
    t.swamp_perm = tint_mod.swamp_perm()
    t.block_default = block_default
    t.st_lod_rgb = st_lod_rgb
    t.st_lod_tint = st_lod_tint
    t.st_lod_kind = st_lod_kind
    cls_counts = {CLASS_NAMES[i]: int((st_class == i).sum()) for i in range(len(CLASS_NAMES))}
    t.stats = {
        'states': S, 'blocks': len(block_names), 'quads': int(t.q_pos.shape[0]), 'baked_models': len(baked_arr),
        'variant_keys': len(vkeys), 'groups': len(grp_total), 'masks': M, 'sprites': len(atl.names),
        'atlas': [int(atl.image.shape[1]), int(atl.image.shape[0])], 'tile': atl.tile, 'classes': cls_counts,
        'special': n_special, 'nomodel': n_nomodel, 'invisible_or_air': n_invisible, 'errors': errors,
        'missing_textures': atl.missing_names, 'random_states': int(((st_flags & F.RANDOM) != 0).sum()),
        'multipart_states': int(((st_flags & F.MULTIPART) != 0).sum()), 'fluid_states': int(((st_flags & (F.WATER | F.LAVA)) != 0).sum()),
    }
    t.build_seconds = time.time() - t0
    say(1.0, 'готово')
    return t

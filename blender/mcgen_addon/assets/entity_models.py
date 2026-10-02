"""Заглушки «особых» блоков, которые игра рисует не JSON-моделью, а моделью сущности: сундуки (обычный, ловушка, эндер, медные),
шулкеры, баннеры (цвет флага), головы/черепа, корпус колокола, плоскость портала Края. Геометрия и раскладка текстур — как в классах
ModelPart.Cube / ChestModel / ShulkerModel / BannerModel / SkullModel / BellModel игры 26.x (коробки с их `texOffs`), текстуры — из
каталога ресурсов пользователя (`textures/entity/...`). Не воспроизводится: анимация (крышки, колокол), узоры баннеров, ушки/шляпы голов,
драконья голова, оверлей порталов. Чистый Python, без bpy.
"""
import math

from .models import DIR_VEC, Quad, _closest_dir

__all__ = ['entity_quads', 'is_entity_block', 'DYE_RGB', 'banner_color']

DYE_RGB = {
    'white': 16383998, 'orange': 16351261, 'magenta': 13061821, 'light_blue': 3847130, 'yellow': 16701501, 'lime': 8439583, 'pink': 15961002,
    'gray': 4673362, 'light_gray': 10329495, 'cyan': 1481884, 'purple': 8991416, 'blue': 3949738, 'brown': 8606770, 'green': 6192150,
    'red': 11546150, 'black': 1908001,
}
_COLORS = tuple(DYE_RGB)

_FACING_YROT = {'south': 0, 'west': 90, 'north': 180, 'east': 270}      # Direction.toYRot()
_STEP = {'north': (0, -1), 'south': (0, 1), 'west': (-1, 0), 'east': (1, 0)}


# ----------------------------------------------------------------------------------------------------------------------
#                                           матрицы 4×4 (список строк)
# ----------------------------------------------------------------------------------------------------------------------
def _ident():
    return [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0], [0, 0, 0, 1.0]]


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _trans(x, y, z):
    m = _ident()
    m[0][3], m[1][3], m[2][3] = x, y, z
    return m


def _scale(x, y, z):
    m = _ident()
    m[0][0], m[1][1], m[2][2] = x, y, z
    return m


def _roty(deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    m = _ident()
    m[0][0], m[0][2], m[2][0], m[2][2] = c, s, -s, c
    return m


def _rotx(deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    m = _ident()
    m[1][1], m[1][2], m[2][1], m[2][2] = c, -s, s, c
    return m


def _rotz(deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    m = _ident()
    m[0][0], m[0][1], m[1][0], m[1][1] = c, -s, s, c
    return m


def _apply(m, p):
    x, y, z = p
    return (m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3], m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
            m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3])


# ----------------------------------------------------------------------------------------------------------------------
#                                           коробка ModelPart.Cube
# ----------------------------------------------------------------------------------------------------------------------
ALL_FACES = ('down', 'up', 'west', 'north', 'east', 'south')


def _cube_polygons(origin, size, texoffs, faces=ALL_FACES):
    """Полигоны коробки как в ModelPart.Cube: список (вершины[4] (x, y, z) в пикселях, uv[4] (u, v) в пикселях текстуры)."""
    x0, y0, z0 = origin
    w, h, d = size
    x1, y1, z1 = x0 + w, y0 + h, z0 + d
    t0, t1, t2, t3 = (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)
    l0, l1, l2, l3 = (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)
    tu, tv = texoffs
    u0 = tu
    u1 = tu + d
    u2 = tu + d + w
    u22 = tu + d + w + w
    u3 = tu + d + w + d
    u4 = tu + d + w + d + w
    v0 = tv
    v1 = tv + d
    v2 = tv + d + h
    spec = {
        'down': ((l1, l0, t0, t1), (u1, v0, u2, v1)),
        'up': ((t2, t3, l3, l2), (u2, v1, u22, v0)),
        'west': ((t0, l0, l3, t3), (u0, v1, u1, v2)),
        'north': ((t1, t0, t3, t2), (u1, v1, u2, v2)),
        'east': ((l1, t1, t2, l2), (u2, v1, u3, v2)),
        'south': ((l0, l1, l2, l3), (u3, v1, u4, v2)),
    }
    out = []
    for f in faces:
        verts, (a0, b0, a1, b1) = spec[f]
        uv = ((a1, b0), (a0, b0), (a0, b1), (a1, b1))      # Polygon: вершины 0..3 <- (u1,v0), (u0,v0), (u0,v1), (u1,v1)
        out.append((verts, uv))
    return out


def _quads(polys, pose_px, mat, tex, texw, texh, tint=-1):
    """Полигоны -> Quad. pose_px — смещение части (пиксели), mat — матрица из долей блока (после /16) в блок."""
    out = []
    for verts, uv in polys:
        pos = []
        for v in verts:
            p = ((v[0] + pose_px[0]) / 16.0, (v[1] + pose_px[1]) / 16.0, (v[2] + pose_px[2]) / 16.0)
            pos.append(_apply(mat, p))
        d = _closest_dir(pos[0], pos[1], pos[2])
        if d is None:
            d = 1
        out.append(Quad(tuple(pos), tuple((u / texw, vv / texh) for u, vv in uv), tex, False, -1, tint, d, d, 0))
    return out


# ----------------------------------------------------------------------------------------------------------------------
#                                                   модели
# ----------------------------------------------------------------------------------------------------------------------
_CHEST_TEX = {'chest': 'normal', 'trapped_chest': 'trapped', 'ender_chest': 'ender', 'copper_chest': 'copper', 'exposed_copper_chest': 'copper_exposed',
              'weathered_copper_chest': 'copper_weathered', 'oxidized_copper_chest': 'copper_oxidized'}


def _chest(block, props):
    base = block[6:] if block.startswith('waxed_') else block
    name = _CHEST_TEX.get(base)
    if name is None:
        return []
    typ = props.get('type', 'single')
    suffix = {'single': '', 'left': '_left', 'right': '_right'}.get(typ, '')
    if base == 'ender_chest':
        suffix = ''
    tex = 'minecraft:entity/chest/' + name + suffix
    # модели: одинарная, левая и правая половинки двойного сундука
    if typ == 'right':
        faces = tuple(f for f in ALL_FACES if f != 'east')
        bw, lockx, lockw, ox = 15, 15, 1, 1
    elif typ == 'left':
        faces = tuple(f for f in ALL_FACES if f != 'west')
        bw, lockx, lockw, ox = 15, 0, 1, 0
    else:
        faces = ALL_FACES
        bw, lockx, lockw, ox = 14, 7, 2, 1
    mat = _mul(_trans(0.5, 0, 0.5), _mul(_roty(-_FACING_YROT.get(props.get('facing', 'north'), 180)), _trans(-0.5, 0, -0.5)))
    q = []
    q += _quads(_cube_polygons((ox, 0, 1), (bw, 10, 14), (0, 19), faces), (0, 0, 0), mat, tex, 64, 64)
    q += _quads(_cube_polygons((ox, 0, 0), (bw, 5, 14), (0, 0), faces), (0, 9, 1), mat, tex, 64, 64)
    q += _quads(_cube_polygons((lockx, -2, 14), (lockw, 4, 1), (0, 0), faces), (0, 9, 1), mat, tex, 64, 64)
    return q


def _shulker(block, props):
    color = block[:-len('_shulker_box')] if block != 'shulker_box' else None
    tex = 'minecraft:entity/shulker/shulker' + ('_' + color if color else '')
    f = props.get('facing', 'up')
    rot = {'up': _ident(), 'down': _rotx(180), 'north': _rotx(-90), 'south': _rotx(90), 'east': _rotz(-90), 'west': _rotz(90)}[f]
    mat = _mul(_trans(0.5, 0.5, 0.5), _mul(_scale(0.9995, 0.9995, 0.9995), _mul(rot, _mul(_scale(1, -1, -1), _trans(0, -1, 0)))))
    q = _quads(_cube_polygons((-8, -16, -8), (16, 12, 16), (0, 0)), (0, 24, 0), mat, tex, 64, 64)
    q += _quads(_cube_polygons((-8, -8, -8), (16, 8, 16), (0, 28)), (0, 24, 0), mat, tex, 64, 64)
    return q


def banner_color(block):
    for c in _COLORS:
        if block.startswith(c + '_'):
            return c
    return 'white'


def _banner(block, props):
    wall = '_wall_banner' in block
    ang = _FACING_YROT.get(props.get('facing', 'north'), 180) if wall else int(props.get('rotation', '0')) * 22.5
    mat = _mul(_trans(0.5, 0, 0.5), _mul(_roty(-ang), _scale(2.0 / 3.0, -2.0 / 3.0, -2.0 / 3.0)))
    tex = 'minecraft:entity/banner/banner_base'
    q = []
    if not wall:
        q += _quads(_cube_polygons((-1, -42, -1), (2, 42, 2), (44, 0)), (0, 0, 0), mat, tex, 64, 64)
    q += _quads(_cube_polygons((-10, -44 if not wall else -20.5, -1 if not wall else 9.5), (20, 2, 2), (0, 42)), (0, 0, 0), mat, tex, 64, 64)
    q += _quads(_cube_polygons((-10, 0, -2), (20, 40, 1), (0, 0)), (0, -44.0 if not wall else -20.5, 0.0 if not wall else 10.5), mat, tex, 64, 64, tint=0)
    return q


_SKULL_TEX = {
    'skeleton_skull': ('minecraft:entity/skeleton/skeleton', 64, 32), 'wither_skeleton_skull': ('minecraft:entity/skeleton/wither_skeleton', 64, 32),
    'zombie_head': ('minecraft:entity/zombie/zombie', 64, 64), 'creeper_head': ('minecraft:entity/creeper/creeper', 64, 32),
    'piglin_head': ('minecraft:entity/piglin/piglin', 64, 64), 'player_head': ('minecraft:entity/player/wide/steve', 64, 64),
}


def _skull(block, props):
    wall = '_wall_' in block
    key = block.replace('_wall_', '_')
    t = _SKULL_TEX.get(key)
    if t is None:
        return []
    tex, tw, th = t
    if wall:
        f = props.get('facing', 'north')
        sx, sz = _STEP[f]
        opposite = {'north': 'south', 'south': 'north', 'east': 'west', 'west': 'east'}[f]
        mat = _mul(_trans(0.5 - sx * 0.25, 0.25, 0.5 - sz * 0.25), _mul(_roty(-_FACING_YROT[opposite]), _scale(-1, -1, 1)))
    else:
        mat = _mul(_trans(0.5, 0, 0.5), _mul(_roty(-int(props.get('rotation', '0')) * 22.5), _scale(-1, -1, 1)))
    return _quads(_cube_polygons((-4, -8, -4), (8, 8, 8), (0, 0)), (0, 0, 0), mat, tex, tw, th)


def _bell(block, props):
    tex = 'minecraft:entity/bell/bell_body'
    mat = _ident()
    q = _quads(_cube_polygons((-3, -6, -3), (6, 7, 6), (0, 0)), (8, 12, 8), mat, tex, 32, 32)
    q += _quads(_cube_polygons((4, 4, 4), (8, 2, 8), (0, 13)), (0, 0, 0), mat, tex, 32, 32)
    return q


def _end_portal(block, props):
    pos = ((0.0, 0.75, 0.0), (0.0, 0.75, 1.0), (1.0, 0.75, 1.0), (1.0, 0.75, 0.0))
    return [Quad(pos, ((0.0, 0.0), (0.0, 1.0), (1.0, 1.0), (1.0, 0.0)), 'minecraft:block/black_concrete', False, -1, -1, 1, 1, 0)]


def is_entity_block(block):
    """Короткое имя блока (без minecraft:) -> есть ли заглушка."""
    if block in _CHEST_TEX or (block.startswith('waxed_') and block[6:] in _CHEST_TEX):
        return True
    return block.endswith('shulker_box') or block.endswith('_banner') or block in ('bell', 'end_portal') or \
        block.replace('_wall_', '_') in _SKULL_TEX


def entity_quads(block, props):
    """Квады заглушки блока `block` (короткое имя) с состоянием `props` — в долях блока, UV в долях текстуры. [] если нет."""
    if block in _CHEST_TEX or (block.startswith('waxed_') and block[6:] in _CHEST_TEX):
        return _chest(block, props)
    if block.endswith('shulker_box'):
        return _shulker(block, props)
    if block.endswith('_banner'):
        return _banner(block, props)
    if block == 'bell':
        return _bell(block, props)
    if block == 'end_portal':
        return _end_portal(block, props)
    if block.replace('_wall_', '_') in _SKULL_TEX:
        return _skull(block, props)
    return []

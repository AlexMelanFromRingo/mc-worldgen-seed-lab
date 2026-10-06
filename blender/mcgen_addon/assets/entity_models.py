"""Заглушки «особых» блоков, которые игра рисует не JSON-моделью, а моделью сущности: сундуки (обычный, ловушка, эндер, медные),
шулкеры, баннеры (цвет флага), головы/черепа, корпус колокола, плоскость портала Края. Геометрия и раскладка текстур — как в классах
ModelPart.Cube / ChestModel / ShulkerModel / BannerModel / SkullModel / BellModel игры 26.x (коробки с их `texOffs`), текстуры — из
каталога ресурсов пользователя (`textures/entity/...`). Узоры баннеров рисует render/banner_overlay.py слоями поверх полотна (banner_cloth_faces). Не воспроизводится: анимация (крышки, колокол, челюсть дракона при питании), ушки/шляпы голов, оверлей порталов. Чистый Python, без bpy.
"""
import math

from .models import DIR_VEC, Quad, _closest_dir

__all__ = ['entity_quads', 'is_entity_block', 'DYE_RGB', 'banner_color', 'banner_cloth_faces', 'pot_sherd_faces', 'sherd_pattern', 'POT_SIDE_NAMES']

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


def _cube_polygons(origin, size, texoffs, faces=ALL_FACES, mirror=False):
    """Полигоны коробки как в ModelPart.Cube: список (вершины[4] (x, y, z) в пикселях, uv[4] (u, v) в пикселях текстуры).
    mirror — CubeListBuilder.mirror(true): x и x2 меняются местами (геометрия отражается, раскладка текстуры остаётся), порядок вершин полигона обращается (Polygon.mirror)."""
    x0, y0, z0 = origin
    w, h, d = size
    x1, y1, z1 = x0 + w, y0 + h, z0 + d
    if mirror:
        x0, x1 = x1, x0
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
        if mirror:
            verts, uv = tuple(reversed(verts)), tuple(reversed(uv))
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


def banner_cloth_faces(block, props):
    """Лицевая и тыльная грани полотна баннера (flag, коробка 20×40×1 пикселей модели BannerFlagModel), на которые накладываются узоры:
    [(вершины[4] (x, y, z) в долях блока, uv[4] (u, v) в долях текстуры 64×64 с v СВЕРХУ, нормаль (x, y, z) наружу)].
    Узор — слой с текстурой entity/banner/<имя>.png той же раскладки, что banner_base, цветом оттенка красителя."""
    wall = '_wall_banner' in block
    ang = _FACING_YROT.get(props.get('facing', 'north'), 180) if wall else int(props.get('rotation', '0')) * 22.5
    mat = _mul(_trans(0.5, 0, 0.5), _mul(_roty(-ang), _scale(2.0 / 3.0, -2.0 / 3.0, -2.0 / 3.0)))
    pose = (0, -44.0 if not wall else -20.5, 0.0 if not wall else 10.5)
    out = []
    for face, nrm in (('north', (0.0, 0.0, -1.0)), ('south', (0.0, 0.0, 1.0))):
        for verts, uv in _cube_polygons((-10, 0, -2), (20, 40, 1), (0, 0), (face,)):
            pos = tuple(_apply(mat, ((v[0] + pose[0]) / 16.0, (v[1] + pose[1]) / 16.0, (v[2] + pose[2]) / 16.0)) for v in verts)
            n = (mat[0][0] * nrm[0] + mat[0][1] * nrm[1] + mat[0][2] * nrm[2], mat[1][0] * nrm[0] + mat[1][1] * nrm[1] + mat[1][2] * nrm[2],
                 mat[2][0] * nrm[0] + mat[2][1] * nrm[1] + mat[2][2] * nrm[2])
            ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1.0
            out.append((pos, tuple((u / 64.0, v / 64.0) for u, v in uv), (n[0] / ln, n[1] / ln, n[2] / ln)))
    return out


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


# DragonHeadModel.createHeadLayer() (байткод клиента 26.3): голова — PartPose.offset(0, -7.986666, 0).scaled(0.75), челюсть — дочерняя часть (0, 4, -8) с xRot = (sin(0) + 1) * 0.2.
# Коробки: (имя, начало, размер, texOffs, mirror); текстура 256×256 entity/enderdragon/dragon.
_DRAGON_HEAD = (
    ((-6, -1, -24), (12, 5, 16), (176, 44), False),      # upper_lip
    ((-8, -8, -10), (16, 16, 16), (112, 30), False),      # upper_head
    ((-5, -12, -4), (2, 4, 6), (0, 0), True),             # scale (левый, mirror)
    ((-5, -3, -22), (2, 2, 4), (112, 0), True),           # nostril (левая, mirror)
    ((3, -12, -4), (2, 4, 6), (0, 0), False),             # scale
    ((3, -3, -22), (2, 2, 4), (112, 0), False),           # nostril
)
_DRAGON_JAW = ((-6, 0, -16), (12, 4, 16), (176, 65), False)
_DRAGON_JAW_XROT = 0.2                                    # рад: (sin(animationPos · π · 0.2) + 1) · 0.2 при animationPos = 0 — рот чуть приоткрыт


def _skull_mat(block, props):
    """Раскладка SkullBlockRenderer: настенная голова — createWallTransformation, напольная — createGroundTransformation (scale(-1, -1, 1) в конце)."""
    if '_wall_' in block:
        f = props.get('facing', 'north')
        sx, sz = _STEP[f]
        opposite = {'north': 'south', 'south': 'north', 'east': 'west', 'west': 'east'}[f]
        return _mul(_trans(0.5 - sx * 0.25, 0.25, 0.5 - sz * 0.25), _mul(_roty(-_FACING_YROT[opposite]), _scale(-1, -1, 1)))
    return _mul(_trans(0.5, 0, 0.5), _mul(_roty(-int(props.get('rotation', '0')) * 22.5), _scale(-1, -1, 1)))


def _dragon_head(block, props):
    tex = 'minecraft:entity/enderdragon/dragon'
    mat0 = _skull_mat(block, props)
    m_head = _mul(_trans(0.0, -7.986666 / 16.0, 0.0), _scale(0.75, 0.75, 0.75))
    m_jaw = _mul(m_head, _mul(_trans(0.0, 4.0 / 16.0, -8.0 / 16.0), _rotx(math.degrees(_DRAGON_JAW_XROT))))
    q = []
    for origin, size, uv, mirror in _DRAGON_HEAD:
        q += _quads(_cube_polygons(origin, size, uv, mirror=mirror), (0, 0, 0), _mul(mat0, m_head), tex, 256, 256)
    origin, size, uv, mirror = _DRAGON_JAW
    q += _quads(_cube_polygons(origin, size, uv, mirror=mirror), (0, 0, 0), _mul(mat0, m_jaw), tex, 256, 256)
    return q


def _bell(block, props):
    tex = 'minecraft:entity/bell/bell_body'
    mat = _ident()
    q = _quads(_cube_polygons((-3, -6, -3), (6, 7, 6), (0, 0)), (8, 12, 8), mat, tex, 32, 32)
    q += _quads(_cube_polygons((4, 4, 4), (8, 2, 8), (0, 13)), (0, 0, 0), mat, tex, 32, 32)
    return q


# ----------------------------------------------------------------------------------------------------------------------
#                       части моделей с вложенными позами (ModelPart: смещение + повороты ZYX, дети)
# ----------------------------------------------------------------------------------------------------------------------
def _rot_zyx(rx, ry, rz):
    """Поворот части: Quaternionf.rotationZYX(zRot, yRot, xRot) (углы в радианах), как в ModelPart.translateAndRotate."""
    return _mul(_rotz(math.degrees(rz)), _mul(_roty(math.degrees(ry)), _rotx(math.degrees(rx))))


def _part_mat(parent, pose):
    """Матрица части: родитель · перенос на смещение (пиксели / 16) · поворот; pose = (x, y, z, xRot, yRot, zRot)."""
    return _mul(parent, _mul(_trans(pose[0] / 16.0, pose[1] / 16.0, pose[2] / 16.0), _rot_zyx(pose[3], pose[4], pose[5])))


def _grown(polys, origin, size, grow):
    """CubeDeformation: коробка раздвигается на grow во все стороны (раскладка текстуры остаётся по исходным размерам)."""
    if not grow:
        return polys
    lo = tuple(origin)
    hi = tuple(o + s for o, s in zip(origin, size))

    def mv(v):
        return tuple(c - grow if c == lo[i] else (c + grow if c == hi[i] else c) for i, c in enumerate(v))
    return [(tuple(mv(v) for v in verts), uv) for verts, uv in polys]


def _quads_parts(parts, parent, tex, texw, texh):
    out = []
    for pose, cubes, children in parts:
        m = _part_mat(parent, pose)
        for origin, size, uv, grow in cubes:
            out += _quads(_grown(_cube_polygons(origin, size, uv), origin, size, grow), (0, 0, 0), m, tex, texw, texh)
        out += _quads_parts(children, m, tex, texw, texh)
    return out


# ----------------------------------------------------------------------------------------------------------------------
#                                            декоративный горшок
# ----------------------------------------------------------------------------------------------------------------------
# DecoratedPotRenderer (байткод клиента 26.3): слой «base» 32×32 — горлышко (две коробки, расширение 0,2 и −0,1, поза (0, 37, 16) с поворотом X на π),
# крышка-плоскость y = 16 (texOffs −14, 13) и дно-плоскость y = 0; слой «sides» 16×16 — четыре боковины 14×16 (коробка только с гранью NORTH, texOffs 1, 0).
# Боковина без черепка — decorated_pot_side, с черепком — entity/decorated_pot/<имя>_pottery_pattern (слой поверх: render/banner_overlay.py).
_POT_BASE = 'minecraft:entity/decorated_pot/decorated_pot_base'
_POT_SIDE = 'minecraft:entity/decorated_pot/decorated_pot_side'
_POT_SIDES = (            # (боковина, поза части)
    ('back', (15, 16, 1, 0, 0, math.pi)),
    ('left', (1, 16, 1, 0, -math.pi / 2, math.pi)),
    ('right', (15, 16, 15, 0, math.pi / 2, math.pi)),
    ('front', (1, 16, 15, math.pi, 0, 0)),
)
POT_SIDE_NAMES = tuple(n for n, _p in _POT_SIDES)


def _pot_facing_mat(props):
    """DecoratedPotRenderer.createModelTransformation: поворот вокруг центра блока на (180 − toYRot(facing))."""
    yrot = _FACING_YROT.get(props.get('facing', 'north'), 180)
    return _mul(_trans(0.5, 0.5, 0.5), _mul(_roty(180 - yrot), _trans(-0.5, -0.5, -0.5)))


def _decorated_pot(block, props):
    m0 = _pot_facing_mat(props)
    q = []
    neck = _part_mat(m0, (0, 37, 16, math.pi, 0, 0))
    for origin, size, uv, grow in (((4, 17, 4), (8, 3, 8), (0, 0), 0.2), ((5, 20, 5), (6, 1, 6), (0, 5), -0.1)):
        q += _quads(_grown(_cube_polygons(origin, size, uv), origin, size, grow), (0, 0, 0), neck, _POT_BASE, 32, 32)
    q += _quads(_cube_polygons((0, 0, 0), (14, 0, 14), (-14, 13), ('up',)), (0, 0, 0), _part_mat(m0, (1, 16, 1, 0, 0, 0)), _POT_BASE, 32, 32)
    q += _quads(_cube_polygons((0, 0, 0), (14, 0, 14), (-14, 13), ('down',)), (0, 0, 0), _part_mat(m0, (1, 0, 1, 0, 0, 0)), _POT_BASE, 32, 32)
    for _name, pose in _POT_SIDES:
        q += _quads(_cube_polygons((0, 0, 0), (14, 16, 0), (1, 0), ('north',)), (0, 0, 0), _part_mat(m0, pose), _POT_SIDE, 16, 16)
    return q


def sherd_pattern(item_id):
    """Предмет-черепок -> имя текстуры узора (без расширения) или None: minecraft:scrape_pottery_sherd -> scrape_pottery_pattern; кирпич (и всё прочее) — пустая боковина."""
    name = item_id.split(':', 1)[-1]
    if not name.endswith('_pottery_sherd'):
        return None
    return name[:-len('_pottery_sherd')] + '_pottery_pattern'


def pot_sherd_faces(props, side):
    """Лицевая грань боковины горшка (side: back/left/right/front), на которую накладывается узор черепка:
    [(вершины[4] в долях блока, uv[4] в долях текстуры 16×16 с v СВЕРХУ, нормаль наружу)]."""
    pose = dict(_POT_SIDES)[side]
    m = _part_mat(_pot_facing_mat(props), pose)
    nrm = (m[0][2] * -1.0, m[1][2] * -1.0, m[2][2] * -1.0)                       # нормаль грани NORTH коробки (0, 0, −1)
    out = []
    for verts, uv in _cube_polygons((0, 0, 0), (14, 16, 0), (1, 0), ('north',)):
        pos = tuple(_apply(m, (v[0] / 16.0, v[1] / 16.0, v[2] / 16.0)) for v in verts)
        out.append((pos, tuple((u / 16.0, v / 16.0) for u, v in uv), nrm))
    return out


# ----------------------------------------------------------------------------------------------------------------------
#                                          статуя медного голема
# ----------------------------------------------------------------------------------------------------------------------
# CopperGolemModel.create{Body,RunningPose,SittingPose,StarPose}Layer (байткод клиента 26.3, разобран интерпретатором): дерево частей
# (поза, коробки (начало, размер, texOffs, расширение), дети). CopperGolemStatueModel.setupAnim: root.y = 0, root.zRot = π (модель «вверх ногами» -> y вверх);
# CopperGolemStatueBlockRenderer: перенос (0,5; 0; 0,5) и поворот Y на −toYRot(facing.opposite). Текстура 64×64 — по степени окисления; глаза не рисуются.
_GOLEM_POSES = {
    'standing': (
        ((0, -5, 0, 0, 0, 0), (((-4, -6, -3), (8, 6, 6), (0, 15), 0), ), (
            ((0, -6, 0, 0, 0, 0), (((-4, -5, -5), (8, 5, 10), (0, 0), 0.015), ((-1, -2, -6), (2, 3, 2), (56, 0), 0), ((-1, -9, -1), (2, 4, 2), (37, 8), -0.015), ((-2, -13, -2), (4, 4, 4), (37, 0), -0.015), ), ()),
            ((-4, -6, 0, 0, 0, 0), (((-3, -1, -2), (3, 10, 4), (36, 16), 0), ), ()),
            ((4, -6, 0, 0, 0, 0), (((0, -1, -2), (3, 10, 4), (50, 16), 0), ), ()),
        )),
        ((0, -5, 0, 0, 0, 0), (((-4, 0, -2), (4, 5, 4), (0, 27), 0), ), ()),
        ((0, -5, 0, 0, 0, 0), (((0, 0, -2), (4, 5, 4), (16, 27), 0), ), ()),
    ),
    'running': (
        ((-1.064, -5, 0, 0, 0, 0), (), (
            ((1.1, 0.1, 0.7, 0.1204, -0.0064, -0.0779), (((-4.02, -6.116, -3.5), (8, 6, 6), (0, 15), 0), ), ()),
            ((0.7, -5.6, -1.8, 0, 0, 0), (((-4, -5.1, -5), (8, 5, 10), (0, 0), 0), ((-1.02, -2.1, -6), (2, 3, 2), (56, 0), 0), ((-1.02, -9.1, -1), (2, 4, 2), (37, 8), -0.015), ((-2, -13.1, -2), (4, 4, 4), (37, 0), -0.015), ), ()),
            ((-4, -6, 0, 0, 0, 0), (), (
                ((0.7, -0.248, -1.62, 1.0036, 0, 0), (((-3.052, -1.11, -2.036), (3, 10, 4), (36, 16), 0), ), ()),
            )),
            ((4, -6, 0, 0, 0, 0), (), (
                ((0.732, 0, 0, -0.8715, -0.0535, -0.0449), (((0.032, -1.1, -2), (3, 10, 4), (50, 16), 0), ), ()),
            )),
        )),
        ((-3.064, -5, 0, 0, 0, 0), (), (
            ((1.048, 0, -0.9, -0.8727, 0, 0), (((-1.856, -0.1, -1.09), (4, 5, 4), (0, 27), 0), ), ()),
        )),
        ((0.936, -5, 0, 0, 0, 0), (), (
            ((1, 0, 0, 0.7854, 0, 0), (((-2.088, -0.1, -2), (4, 5, 4), (16, 27), 0), ), ()),
        )),
    ),
    'sitting': (
        ((0, -3, 2.325, 0, 0, 0), (((-3, -4, -4.525), (6, 1, 6), (3, 19), 0), ((-4, -3, -3.525), (8, 6, 6), (0, 15), 0), ), (
            ((0, -1, -4.325, 0, 0, -3.1416), (((-4, -3, -2.2), (8, 6, 3), (3, 18), 0), ), ()),
            ((0, -6, -0.2, 0, 0, 0), (((-1, -7, -3.3), (2, 4, 2), (37, 8), -0.015), ((-2, -11, -4.3), (4, 4, 4), (37, 0), -0.015), ((-4, -3, -7.325), (8, 5, 10), (0, 0), 0), ((-1, 0, -8.325), (2, 3, 2), (56, 0), 0), ), ()),
            ((-4, -5.6, -1.8, 0.4363, 0, 0), (), (
                ((0, 0.0893, 0.1198, -1.0472, 0, 0), (((-3.075, -0.9733, -1.9966), (3, 10, 4), (36, 16), 0), ), ()),
            )),
            ((4, -5.6, -1.7, 0.4363, 0, 0), (), (
                ((0, -0.0015, -0.0808, -1.0472, 0, 0), (((0.075, -1.0443, -1.8997), (3, 10, 4), (50, 16), 0), ), ()),
            )),
        )),
        ((-2.1, -2.1, -2.075, 0, 0, 0), (), (
            ((0.05, -1.9, 1.075, -1.5708, 0, 0), (((-2, 0.975, 0), (4, 5, 4), (0, 27), 0), ), ()),
        )),
        ((2, -2, -2.075, 0, 0, 0), (), (
            ((0.05, -2, 1.075, -1.5708, 0, 0), (((-2, 0.975, 0), (4, 5, 4), (16, 27), 0), ), ()),
        )),
    ),
    'star': (
        ((0, -5, 0, 0, 0, 0), (((-4, -6, -3), (8, 6, 6), (0, 15), 0), ), (
            ((0, -6, 0, 0, 0, 0), (((-4, -5, -5), (8, 5, 10), (0, 0), 0), ((-1, -2, -6), (2, 3, 2), (56, 0), 0), ((-1, -9, -1), (2, 4, 2), (37, 8), -0.015), ((-2, -13, -2), (4, 4, 4), (37, 0), -0.015), ), ()),
            ((-4, -6, 0, 0, 0, 0), (), (
                ((1, 1, 0, 0, 0, 1.9199), (((-1.5, -5, -2), (3, 10, 4), (36, 16), 0), ), ()),
            )),
            ((4, -6, 0, 0, 0, 0), (), (
                ((-1, 1, 0, 0, 0, -1.9199), (((-1.5, -5, -2), (3, 10, 4), (50, 16), 0), ), ()),
            )),
        )),
        ((-3, -5, 0, 0, 0, 0), (), (
            ((0.35, 2, 0.01, 0, 0, 0.2618), (((-2, -2.5, -2), (4, 5, 4), (0, 27), 0), ), ()),
        )),
        ((1, -5, 0, 0, 0, 0), (), (
            ((1.65, 2, 0, 0, 0, -0.2618), (((-2, -2.5, -2), (4, 5, 4), (16, 27), 0), ), ()),
        )),
    ),
}
_GOLEM_TEX = {'': 'copper_golem', 'exposed': 'copper_golem_exposed', 'weathered': 'copper_golem_weathered', 'oxidized': 'copper_golem_oxidized'}
_GOLEM_OPPOSITE = {'north': 'south', 'south': 'north', 'east': 'west', 'west': 'east'}


def _is_golem_statue(block):
    return block.endswith('copper_golem_statue')


def _golem_statue(block, props):
    base = block[6:] if block.startswith('waxed_') else block
    level = base[:-len('_copper_golem_statue')] if base != 'copper_golem_statue' else ''
    tex = 'minecraft:entity/copper_golem/' + _GOLEM_TEX.get(level, 'copper_golem')
    parts = _GOLEM_POSES.get(props.get('copper_golem_pose', 'standing'), _GOLEM_POSES['standing'])
    opp = _GOLEM_OPPOSITE.get(props.get('facing', 'north'), 'south')
    root = _mul(_trans(0.5, 0.0, 0.5), _mul(_roty(-_FACING_YROT[opp]), _rotz(180.0)))          # корень: поворот Z на π
    return _quads_parts(parts, root, tex, 64, 64)


def _end_portal(block, props):
    pos = ((0.0, 0.75, 0.0), (0.0, 0.75, 1.0), (1.0, 0.75, 1.0), (1.0, 0.75, 0.0))
    return [Quad(pos, ((0.0, 0.0), (0.0, 1.0), (1.0, 1.0), (1.0, 0.0)), 'minecraft:block/black_concrete', False, -1, -1, 1, 1, 0)]


def is_entity_block(block):
    """Короткое имя блока (без minecraft:) -> есть ли заглушка."""
    if block in _CHEST_TEX or (block.startswith('waxed_') and block[6:] in _CHEST_TEX):
        return True
    return block.endswith('shulker_box') or block.endswith('_banner') or block in ('bell', 'end_portal', 'decorated_pot') or block.replace('_wall_', '_') in _SKULL_TEX or \
        block in ('dragon_head', 'dragon_wall_head') or _is_golem_statue(block)


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
    if block in ('dragon_head', 'dragon_wall_head'):
        return _dragon_head(block, props)
    if block == 'decorated_pot':
        return _decorated_pot(block, props)
    if _is_golem_statue(block):
        return _golem_statue(block, props)
    if block.replace('_wall_', '_') in _SKULL_TEX:
        return _skull(block, props)
    return []

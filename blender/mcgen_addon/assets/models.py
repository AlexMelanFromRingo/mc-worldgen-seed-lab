"""Модели блоков (assets/minecraft/models/**.json): цепочки parent, переменные текстур `#имя`, elements, повороты элементов
(ось/угол/origin/rescale и формат x/y/z), грани с UV и поворотом, cullface, tintindex, shade_direction_override,
поворот модели из blockstate (x, y, z, uvlock). Запекание в квады (4 вершины) воспроизводит поведение FaceBakery/UnbakedCuboidGeometry
игры 26.x (порядок вершин, UV по умолчанию, uvlock через обратное преобразование грани). Чистый Python, без bpy.

Направления — как в `Direction.values()` игры: DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5.
Координаты квадов — в долях блока (0..1) в системе Minecraft (X восток, Y вверх, Z юг). UV — в долях спрайта (0..1), V вниз.
"""
import json
import math
import os

__all__ = ['DIRS', 'DIR_VEC', 'DIR_NAME', 'DIR_BY_NAME', 'OPPOSITE', 'Quad', 'ResolvedModel', 'ModelStore',
           'block_rotation', 'rotate_dir', 'MISSING_TEXTURE']

DIR_NAME = ('down', 'up', 'north', 'south', 'west', 'east')
DIR_BY_NAME = {n: i for i, n in enumerate(DIR_NAME)}
DIRS = tuple(range(6))
DIR_VEC = ((0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1), (-1, 0, 0), (1, 0, 0))
OPPOSITE = (1, 0, 3, 2, 5, 4)
MISSING_TEXTURE = 'minecraft:missingno'

# Вершины граней (FaceInfo): для каждой вершины три селектора по осям x, y, z: 0 -> from, 1 -> to
FACE_VERTS = (
    ((0, 0, 1), (0, 0, 0), (1, 0, 0), (1, 0, 1)),   # DOWN
    ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),   # UP
    ((1, 1, 0), (1, 0, 0), (0, 0, 0), (0, 1, 0)),   # NORTH
    ((0, 1, 1), (0, 0, 1), (1, 0, 1), (1, 1, 1)),   # SOUTH
    ((0, 1, 0), (0, 0, 0), (0, 0, 1), (0, 1, 1)),   # WEST
    ((1, 1, 1), (1, 0, 1), (1, 0, 0), (1, 1, 0)),   # EAST
)


class Quad:
    """Запечённая грань. pos: 4 вершины (x, y, z) в долях блока; uv: 4 пары (u, v) в долях спрайта (v вниз)."""
    __slots__ = ('pos', 'uv', 'tex', 'force_translucent', 'cull', 'tint', 'dir', 'shade', 'emit')

    def __init__(self, pos, uv, tex, force_translucent, cull, tint, direction, shade, emit):
        self.pos, self.uv, self.tex, self.force_translucent = pos, uv, tex, force_translucent
        self.cull, self.tint, self.dir, self.shade, self.emit = cull, tint, direction, shade, emit

    def __repr__(self):
        return 'Quad(dir=%s cull=%d tint=%d tex=%s)' % (DIR_NAME[self.dir], self.cull, self.tint, self.tex)


# ----------------------------------------------------------------------------------------------------------------------
#                                                линейная алгебра 3×3
# ----------------------------------------------------------------------------------------------------------------------

def _mm(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


def _mv(m, v):
    return (m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2])


def _rot(axis, deg):
    """Правая вращательная матрица вокруг +оси на угол deg (градусы)."""
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    if axis == 0:
        return ((1, 0, 0), (0, c, -s), (0, s, c))
    if axis == 1:
        return ((c, 0, s), (0, 1, 0), (-s, 0, c))
    return ((c, -s, 0), (s, c, 0), (0, 0, 1))


def _round_mat(m):
    return tuple(tuple(float(round(x)) for x in row) for row in m)


def block_rotation(x, y, z):
    """Поворот модели из blockstate: сначала X, затем Y, затем Z, каждый на -угол (как BLOCK_ROT_*_n игры). 3×3 с целыми элементами."""
    m = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    if x % 360:
        m = _mm(_rot(0, -x), m)
    if y % 360:
        m = _mm(_rot(1, -y), m)
    if z % 360:
        m = _mm(_rot(2, -z), m)
    return _round_mat(m)


def _nearest_dir(v):
    best, bd = 0, -1e30
    for d in DIRS:
        dv = DIR_VEC[d]
        p = v[0] * dv[0] + v[1] * dv[1] + v[2] * dv[2]
        if p > bd + 1e-12:
            best, bd = d, p
    return best


def rotate_dir(m, d):
    """Направление после линейного поворота m (Direction.rotate)."""
    return _nearest_dir(_mv(m, DIR_VEC[d]))


# локальный кадр грани (нормаль +Z) -> глобальный: BlockMath.VANILLA_UV_TRANSFORM_LOCAL_TO_GLOBAL
_L2G = {
    3: ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)),
    5: _round_mat(_rot(1, 90)), 4: _round_mat(_rot(1, -90)), 2: _round_mat(_rot(1, 180)),
    1: _round_mat(_rot(0, -90)), 0: _round_mat(_rot(0, 90)),
}


def _transpose(m):
    return tuple(tuple(m[j][i] for j in range(3)) for i in range(3))


_G2L = {d: _transpose(m) for d, m in _L2G.items()}   # для вращений обратная = транспонированная


def _uvlock_inverse(rot, side):
    """Обратная матрица преобразования UV грани `side` для блока, повёрнутого на rot (BlockMath.getFaceTransformation + invert)."""
    face_action = _mm(rot, _L2G[side])
    n = _mv(face_action, (0.0, 0.0, 1.0))
    new_side = _nearest_dir(n)
    res = _round_mat(_mm(_G2L[new_side], face_action))
    return _transpose(res)


# ----------------------------------------------------------------------------------------------------------------------
#                                                      модели
# ----------------------------------------------------------------------------------------------------------------------

def _norm_id(name):
    return name if ':' in name else 'minecraft:' + name


class _Rotation:
    """Поворот элемента: origin (в долях блока) и 3×3 матрица (с rescale)."""
    __slots__ = ('origin', 'mat')

    def __init__(self, origin, mat):
        self.origin, self.mat = origin, mat


def _parse_rotation(r):
    if r is None:
        return None
    o = r.get('origin', [8, 8, 8])
    origin = (o[0] / 16.0, o[1] / 16.0, o[2] / 16.0)
    if 'axis' in r or 'angle' in r:
        axis = {'x': 0, 'y': 1, 'z': 2}[str(r['axis']).lower()]
        angle = float(r['angle'])
        m = _rot(axis, angle) if angle != 0.0 else ((1.0, 0, 0), (0, 1.0, 0), (0, 0, 1.0))
    else:
        if not any(k in r for k in ('x', 'y', 'z')):
            raise ValueError('rotation без axis/angle и без x/y/z')
        ax, ay, az = float(r.get('x', 0)), float(r.get('y', 0)), float(r.get('z', 0))
        # rotationZYX(z, y, x) = Rz * Ry * Rx (сначала X)
        m = _mm(_rot(2, az), _mm(_rot(1, ay), _rot(0, ax)))
    ident = all(abs(m[i][j] - (1.0 if i == j else 0.0)) < 1e-9 for i in range(3) for j in range(3))
    if r.get('rescale', False) and not ident:
        sc = []
        for ax in range(3):
            col = (m[0][ax], m[1][ax], m[2][ax])        # M * e_axis
            mx = max(abs(col[0]), abs(col[1]), abs(col[2]))
            sc.append(1.0 / mx)
        # result.scale(s) = M * diag(s)
        m = tuple(tuple(m[i][j] * sc[j] for j in range(3)) for i in range(3))
    return _Rotation(origin, m)


class _Face:
    __slots__ = ('texture', 'uv', 'cull', 'rotation', 'tint')

    def __init__(self, d):
        self.texture = d['texture']
        self.uv = d.get('uv')
        c = d.get('cullface', '')
        self.cull = DIR_BY_NAME.get(c, -1) if isinstance(c, str) else -1
        rot = int(d.get('rotation', 0))
        if rot % 90:
            raise ValueError('rotation грани %d не кратен 90' % rot)
        self.rotation = (rot % 360) // 90
        self.tint = int(d.get('tintindex', -1))


class _Element:
    __slots__ = ('from_', 'to', 'faces', 'rotation', 'shade_dir', 'emit')

    def __init__(self, d):
        self.from_ = tuple(float(v) for v in d['from'])
        self.to = tuple(float(v) for v in d['to'])
        self.faces = {DIR_BY_NAME[k]: _Face(v) for k, v in d.get('faces', {}).items() if k in DIR_BY_NAME}
        self.rotation = _parse_rotation(d.get('rotation'))
        sd = d.get('shade_direction_override')
        self.shade_dir = DIR_BY_NAME.get(sd) if isinstance(sd, str) else None
        self.emit = int(d.get('light_emission', 0))


class ResolvedModel:
    """Модель после разворота цепочки parent."""
    __slots__ = ('name', 'elements', 'textures', 'ambient_occlusion', 'builtin', 'error')

    def __init__(self, name):
        self.name = name
        self.elements = []
        self.textures = {}         # слот -> ('ref', '#x') | ('id', 'minecraft:block/x', force_translucent)
        self.ambient_occlusion = True
        self.builtin = None
        self.error = None


def _parse_tex_value(v):
    force = False
    if isinstance(v, dict):
        force = bool(v.get('force_translucent', False))
        v = v.get('sprite', v.get('texture', ''))
    v = str(v)
    if v.startswith('#'):
        return ('ref', v[1:], force)
    return ('id', _norm_id(v), force)


class ModelStore:
    """Загрузчик/кэш моделей из каталога ресурсов (`.../assets/minecraft`)."""

    def __init__(self, assets_dir):
        self.root = assets_dir
        self._raw = {}
        self._resolved = {}
        self._baked = {}
        self.errors = {}

    def _path(self, name):
        n = name.split(':', 1)[-1]
        return os.path.join(self.root, 'models', n + '.json')

    def raw(self, name):
        name = _norm_id(name)
        if name in self._raw:
            return self._raw[name]
        try:
            with open(self._path(name), encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError) as e:
            self.errors[name] = str(e)
            d = None
        self._raw[name] = d
        return d

    def resolve(self, name):
        """Полный разбор модели (без родителя `builtin/*`: у них нет геометрии)."""
        name = _norm_id(name)
        r = self._resolved.get(name)
        if r is not None:
            return r
        r = ResolvedModel(name)
        self._resolved[name] = r
        chain = []
        cur = name
        seen = set()
        while cur is not None:
            if cur in seen:
                r.error = 'цикл parent: %s' % cur
                break
            seen.add(cur)
            short = cur.split(':', 1)[-1]
            if short.startswith('builtin/'):
                r.builtin = short
                break
            d = self.raw(cur)
            if d is None:
                r.error = 'нет модели %s' % cur
                break
            chain.append(d)
            p = d.get('parent')
            cur = _norm_id(p) if p else None
        got_elements = False
        got_ao = False
        for d in chain:                       # от потомка к предку: потомок важнее
            for slot, v in d.get('textures', {}).items():
                if slot not in r.textures:
                    r.textures[slot] = _parse_tex_value(v)
            if not got_elements and 'elements' in d:
                try:
                    r.elements = [_Element(e) for e in d['elements']]
                except (KeyError, ValueError, TypeError) as e:
                    r.error = 'элементы: %s' % (e,)
                got_elements = True
            if not got_ao and 'ambientocclusion' in d:
                r.ambient_occlusion = bool(d['ambientocclusion'])
                got_ao = True
        return r

    def texture_of(self, model, ref):
        """Разрешение текстуры грани. В игре значение грани — ВСЕГДА имя слота (`#side` или просто `side`), прямых id нет.
        -> (id спрайта, force_translucent) либо (MISSING_TEXTURE, False)."""
        force = False
        cur = ref[1:] if ref.startswith('#') else ref
        for _ in range(64):
            t = model.textures.get(cur)
            if t is None:
                return MISSING_TEXTURE, False
            force = force or t[2]
            if t[0] == 'ref':
                cur = t[1]
                continue
            return t[1], force
        return MISSING_TEXTURE, False

    def bake(self, name, x=0, y=0, z=0, uvlock=False):
        """-> список Quad (кэшируется по ключу)."""
        key = (_norm_id(name), x % 360, y % 360, z % 360, bool(uvlock))
        q = self._baked.get(key)
        if q is not None:
            return q
        q = bake_model(self, self.resolve(name), key[1], key[2], key[3], key[4])
        self._baked[key] = q
        return q


def _default_uv(frm, to, d):
    fx, fy, fz = frm
    tx, ty, tz = to
    if d == 0:
        return (fx, 16.0 - tz, tx, 16.0 - fz)
    if d == 1:
        return (fx, fz, tx, tz)
    if d == 2:
        return (16.0 - tx, 16.0 - ty, 16.0 - fx, 16.0 - fy)
    if d == 3:
        return (fx, 16.0 - ty, tx, 16.0 - fy)
    if d == 4:
        return (fz, 16.0 - ty, tz, 16.0 - fy)
    return (16.0 - tz, 16.0 - ty, 16.0 - fz, 16.0 - fy)


def _closest_dir(p0, p1, p2):
    """Направление ближайшей к нормали грани (calculateFacing); нормаль = (p1-p0)×(p2-p0). None, если вырожденная."""
    ax, ay, az = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
    bx, by, bz = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
    n = (ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx)
    ln = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
    if ln == 0.0:
        return None
    n = (n[0] / ln, n[1] / ln, n[2] / ln)
    best, bp = None, 0.0
    for d in DIRS:
        dv = DIR_VEC[d]
        p = n[0] * dv[0] + n[1] * dv[1] + n[2] * dv[2]
        if p >= 0.0 and p > bp:
            best, bp = d, p
    return best


def bake_model(store, model, x=0, y=0, z=0, uvlock=False):
    quads = []
    if model.error or model.builtin:
        return quads
    rot = block_rotation(x, y, z)
    ident = (x % 360 == 0 and y % 360 == 0 and z % 360 == 0)
    for el in model.elements:
        frm, to = el.from_, el.to
        dx = frm[0] != to[0]
        dy = frm[1] != to[1]
        dz = frm[2] != to[2]
        draw = [True, True, True]   # X, Y, Z — как в UnbakedCuboidGeometry
        if not dx:
            draw[1] = draw[2] = False
        if not dy:
            draw[0] = draw[2] = False
        if not dz:
            draw[0] = draw[1] = False
        if not (draw[0] or draw[1] or draw[2]):
            continue
        for d, face in el.faces.items():
            axis = 0 if d >= 4 else (1 if d < 2 else 2)
            if not draw[axis]:
                continue
            tex, force = store.texture_of(model, face.texture)
            uvs = face.uv if face.uv is not None else _default_uv(frm, to, d)
            minu, minv, maxu, maxv = (float(v) for v in uvs)
            inv = _uvlock_inverse(rot, d) if (uvlock and not ident) else None
            pos = []
            uv = []
            for i in range(4):
                sel = FACE_VERTS[d][i]
                v = ((to[0] if sel[0] else frm[0]) / 16.0, (to[1] if sel[1] else frm[1]) / 16.0, (to[2] if sel[2] else frm[2]) / 16.0)
                if el.rotation is not None:
                    o = el.rotation.origin
                    v = _mv(el.rotation.mat, (v[0] - o[0], v[1] - o[1], v[2] - o[2]))
                    v = (v[0] + o[0], v[1] + o[1], v[2] + o[2])
                if not ident:
                    v = _mv(rot, (v[0] - 0.5, v[1] - 0.5, v[2] - 0.5))
                    v = (v[0] + 0.5, v[1] + 0.5, v[2] + 0.5)
                # UV вершины с поворотом грани: индекс (i + shift) % 4
                j = (i + face.rotation) % 4
                u = (minu if (j == 0 or j == 1) else maxu) / 16.0
                w = (minv if (j == 0 or j == 3) else maxv) / 16.0
                if inv is not None:
                    t = _mv(inv, (u - 0.5, w - 0.5, 0.0))
                    u, w = t[0] + 0.5, t[1] + 0.5
                pos.append(v)
                uv.append((u, w))
            fd = _closest_dir(pos[0], pos[1], pos[2])
            if fd is None:
                fd = 1
            cull = face.cull
            if cull >= 0 and not ident:
                cull = rotate_dir(rot, cull)
            shade = el.shade_dir if el.shade_dir is not None else fd
            quads.append(Quad(tuple(pos), tuple(uv), tex, force, cull, face.tint, fd, shade, el.emit))
    return quads

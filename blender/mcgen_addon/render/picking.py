"""Выбор блока лучом: луч (экран или мир) → грань меша чанка → (блок, грань) через атрибут грани `mc_face` (обратное отображение
«грань → локальный блок», которое строит C-ядро). bpy-слой; сами вычисления координат — чистая арифметика.

Координаты: Minecraft (x восток, y вверх, z юг) ↔ Blender (X = x, Y = -z, Z = y), масштаб — `view_settings.scale` блоков на единицу.
"""
import math

import bpy
from mathutils import Matrix, Vector

from ..mesh.edit import DIR_VEC

__all__ = ['PickResult', 'pick_ray', 'pick_screen', 'mc_to_blender', 'blender_to_mc', 'view_dir_mc']

MAT_WATER = 3


def blender_to_mc(v, scale=1.0):
    """Точка Blender -> координаты Minecraft (float)."""
    return (v[0] / scale, v[2] / scale, -v[1] / scale)


def mc_to_blender(p, scale=1.0):
    return Vector((p[0] * scale, -p[2] * scale, p[1] * scale))


def view_dir_mc(d):
    """Направление Blender -> Minecraft (без масштаба)."""
    return (d[0], d[2], -d[1])


class PickResult:
    """Результат выбора. block — блок, в который попал луч (x, y, z мира), face — направление грани (0..5, куда смотрит нормаль грани),
    place — соседняя клетка в сторону грани (куда ставить), hit — точка попадания (Minecraft)."""
    __slots__ = ('chunk', 'local', 'block', 'face', 'place', 'hit', 'distance', 'state', 'obj', 'poly', 'material')

    def __repr__(self):
        return 'PickResult(block=%r face=%d place=%r dist=%.2f)' % (self.block, self.face, self.place, self.distance)


def _world_matrix(obj):
    """Матрица объекта: в фоновом режиме/до обновления depsgraph matrix_world может быть единичной — тогда берём сдвиг из location
    (объекты чанков не поворачиваются и не масштабируются)."""
    mw = obj.matrix_world
    if mw == Matrix.Identity(4) and tuple(obj.location) != (0.0, 0.0, 0.0):
        return Matrix.Translation(obj.location)
    return mw.copy()


def _ray_aabb(origin, inv_dir, bmin, bmax):
    """Расстояние входа луча в AABB или None."""
    t0, t1 = 0.0, float('inf')
    for i in range(3):
        a = (bmin[i] - origin[i]) * inv_dir[i]
        b = (bmax[i] - origin[i]) * inv_dir[i]
        if a > b:
            a, b = b, a
        t0 = max(t0, a)
        t1 = min(t1, b)
        if t0 > t1:
            return None
    return t0


def pick_ray(sb, origin, direction, max_dist=1000.0, skip_water=True):
    """Бросает луч (мировые координаты Blender) по группам-объектам сцены `sb` (SceneBuilder). -> PickResult | None."""
    d = Vector(direction).normalized()
    o = Vector(origin)
    inv = tuple((1.0 / c) if abs(c) > 1e-12 else 1e30 for c in d)
    cands = []
    for g, part in sb.all_objects():
        obj = part.obj
        if part.kind == 'lod' or part.n_quads == 0:
            continue
        mw = _world_matrix(obj)
        bb = [mw @ Vector(c) for c in obj.bound_box]
        bmin = tuple(min(v[i] for v in bb) for i in range(3))
        bmax = tuple(max(v[i] for v in bb) for i in range(3))
        t = _ray_aabb(o, inv, bmin, bmax)
        if t is not None and t <= max_dist:
            cands.append((t, part, mw))
    cands.sort(key=lambda c: c[0])
    best = None
    best_t = max_dist
    for t_enter, part, mw in cands:
        if t_enter > best_t:
            break
        obj = part.obj
        mw_inv = mw.inverted()
        lo = mw_inv @ o
        ld = (mw_inv.to_3x3() @ d).normalized()
        dist_off = 0.0
        for _ in range(64):
            hit, loc, nrm, idx = obj.ray_cast(lo, ld, distance=max(best_t - dist_off, 0.0) + 1e-6)
            if not hit:
                break
            mat = part.mesh.polygons[idx].material_index
            if skip_water and mat == MAT_WATER:
                step = (loc - lo).length + 1e-3
                dist_off += step
                lo = loc + ld * 1e-3
                continue
            wloc = mw @ loc
            dist = (wloc - o).length
            if dist < best_t:
                best_t = dist
                best = (part, idx, wloc, mat)
            break
    if best is None:
        return None
    part, idx, wloc, mat = best
    rf = sb.resolve_face(part.obj, idx)
    if rf is None:
        return None
    cx, cz, lx, ly, lz, face = rf
    s = float(sb.vs.scale)
    r = PickResult()
    hit = blender_to_mc(wloc, s)
    if part.kind == 'merged':
        # слитая грань покрывает много блоков: блок — тот, чья грань содержит точку попадания (чуть «внутрь» против нормали грани)
        v = DIR_VEC[face]
        wx, wy, wz = (int(math.floor(hit[0] - v[0] * 1e-3)), int(math.floor(hit[1] - v[1] * 1e-3)), int(math.floor(hit[2] - v[2] * 1e-3)))
        cx, cz = wx >> 4, wz >> 4
        lx, ly, lz = wx & 15, wy - sb.min_y, wz & 15
    else:
        wx, wy, wz = cx * 16 + lx, ly + sb.min_y, cz * 16 + lz
    r.chunk = (cx, cz)
    r.local = (lx, ly, lz)
    r.block = (wx, wy, wz)
    r.face = face
    v = DIR_VEC[face]
    r.place = (wx + v[0], wy + v[1], wz + v[2])
    r.hit = hit
    r.distance = best_t
    r.obj, r.poly, r.material = part.obj, idx, mat
    arr = sb.blocks.get((cx, cz))
    r.state = int(arr[(ly * 16 + lz) * 16 + lx]) if arr is not None and 0 <= ly < sb.height else -1
    return r


def pick_screen(sb, context, event_or_xy, skip_water=True):
    """Выбор по положению указателя в 3D-области (region/rv3d из контекста)."""
    from bpy_extras import view3d_utils
    region = context.region
    rv3d = context.region_data
    xy = (event_or_xy.mouse_region_x, event_or_xy.mouse_region_y) if hasattr(event_or_xy, 'mouse_region_x') else event_or_xy
    origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, xy)
    direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, xy)
    return pick_ray(sb, origin, direction, skip_water=skip_water)

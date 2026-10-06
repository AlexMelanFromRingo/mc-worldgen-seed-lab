"""Содержимое блок-сущностей построек, которого нет в состоянии блока: слои поверх воксельного меша (bpy).

* Баннер: игра рисует модель сущности — полотно базового цвета, а поверх него по слою на каждый узор (текстура entity/banner/<узор>.png той же
  раскладки 64×64, цвет красителя узора).
* Декоративный горшок: боковина без черепка рисуется воксельным мешем (decorated_pot_side), боковина с черепком — слоем entity/decorated_pot/<имя>_pottery_pattern.png.
Данные лежат не в блоке, а в блок-сущности (NBT шаблона постройки): libmcgen отдаёт их через `McRegion.block_entities()` (баннеры мельниц, аванпостов, особняков,
башен Края; горшки залов испытаний). Воксельный меш рисует основу (entity_models._banner / _decorated_pot);
здесь для каждой группы чанков строится ОДИН дополнительный объект: четырёхугольники слоёв, с материалом
на текстуру (текстура × цвет грани `Col`, как у остальных материалов мира) и смещением слоя наружу на доли миллиметра (иначе слои мерцают друг в друге).

Объекты помечены `mc_overlay` (не `mc_group`: это не части воксельных групп), удаляются при очистке сцены и пересборке. Чистая геометрия — в
assets/entity_models (banner_cloth_faces, pot_sherd_faces).
"""
import os
import re

import bpy
import numpy as np

from ..assets import entity_models
from . import materials as materials_mod

__all__ = ['OVERLAY_KEY', 'remove_all', 'rebuild', 'refresh', 'visible_entries']

OVERLAY_KEY = 'mc_overlay'
LAYER_EPS = 0.0006          # смещение слоя наружу в долях блока на номер слоя
_PROPS = re.compile(r'\[(.*)\]')


def remove_all():
    """Удаляет объекты узоров и их меши; возвращает число удалённых объектов."""
    n = 0
    for o in list(bpy.data.objects):
        if OVERLAY_KEY in o.keys():
            mesh = o.data if o.type == 'MESH' else None
            bpy.data.objects.remove(o, do_unlink=True)
            if mesh is not None and mesh.users == 0:
                bpy.data.meshes.remove(mesh)
            n += 1
    return n


def _state_props(state_name):
    m = _PROPS.search(state_name)
    if not m:
        return {}
    return dict(kv.split('=', 1) for kv in m.group(1).split(',') if '=' in kv)


def visible_entries(entries, region, names, view, live_blocks=None):
    """Записи баннеров, которые сейчас видны: блок на месте (не сломан правкой/постобработкой) и внутри обрезки области / диапазона высот вида.
    live_blocks — {(cx, cz): плоский массив блоков} построителя сцены (с учётом правок пользователя); без него — блоки региона. -> [(запись, свойства состояния)]."""
    crop = view.get('crop') if view else None
    lo = view.get('y_min') if view else None
    hi = view.get('y_max') if view else None
    min_y = region.info.min_y
    out = []
    for e in entries:
        x, y, z = e['x'], e['y'], e['z']
        if crop and not (crop[0] <= x < crop[2] and crop[1] <= z < crop[3]):
            continue
        if (lo is not None and y < lo) or (hi is not None and y > hi):
            continue
        cx, cz = x >> 4, z >> 4
        if not region.has_chunk(cx, cz):
            continue
        try:
            if live_blocks is not None and (cx, cz) in live_blocks:
                sid = int(live_blocks[(cx, cz)][((y - min_y) * 16 + (z & 15)) * 16 + (x & 15)])
            else:
                sid = int(region.blocks(cx, cz)[y - min_y, z & 15, x & 15])
            nm = names[sid]
        except (IndexError, ValueError, KeyError):
            continue
        if nm.split('[')[0] != e['block']:
            continue
        out.append((e, _state_props(nm)))
    return out


def _material(kind, pattern, assets_dir, shading, pixel_style):
    """Материал «текстура слоя × цвет грани» (кэш по виду и имени); kind — каталог текстур сущностей (banner, decorated_pot); None — если текстуры нет."""
    short = pattern.split(':', 1)[-1]
    name = 'MC_%s_%s' % (kind, short)
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat
    path = os.path.join(assets_dir or '', 'assets', 'minecraft', 'textures', 'entity', kind, short + '.png')
    if not os.path.isfile(path):
        path = os.path.join(assets_dir or '', 'textures', 'entity', kind, short + '.png')     # каталог ресурсов без префикса assets/minecraft
        if not os.path.isfile(path):
            return None
    img = bpy.data.images.get(name)
    if img is None:
        img = bpy.data.images.load(path, check_existing=False)
        img.name = name
        img.colorspace_settings.name = 'sRGB'
        img.alpha_mode = 'STRAIGHT'
    mat = bpy.data.materials.new(name)
    materials_mod._build(mat, img, 'cutout', shading, pixel_style)
    return mat


def _dye_srgb(color):
    c = entity_models.DYE_RGB.get(color.split(':', 1)[-1], 16383998)
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def _layers(e, props):
    """Слои записи: [(каталог текстур, имя текстуры, цвет грани (r, g, b), грани [(вершины, uv, нормаль)])] в порядке наложения."""
    block = e['block'].split(':', 1)[-1]
    out = []
    if block.endswith('_banner'):
        faces = entity_models.banner_cloth_faces(block, props)
        for color, pattern in e.get('patterns', []):
            out.append(('banner', pattern, _dye_srgb(color), faces))
    elif block == 'decorated_pot':
        for side in entity_models.POT_SIDE_NAMES:
            pat = entity_models.sherd_pattern(e.get('sherds', {}).get(side, 'minecraft:brick'))
            if pat:
                out.append(('decorated_pot', pat, (255, 255, 255), entity_models.pot_sherd_faces(props, side)))
    return out


def rebuild(sb, region, names, entries, assets_dir, view=None):
    """Пересоздаёт объекты узоров всей сцены. sb — SceneBuilder (масштаб, группировка, коллекция). Возвращает число баннеров с узорами."""
    remove_all()
    sb._overlay_state = dict(region=region, names=names, entries=entries, assets_dir=assets_dir, view=view)      # для refresh() после правок блоков
    live = getattr(sb, 'blocks', None)
    vis = visible_entries(entries, region, names, view or {}, live if live else None) if entries else []
    if not vis:
        return 0
    vs = sb.vs
    s = float(vs.scale)
    N = max(1, int(vs.chunks_per_object))
    shading = getattr(vs, 'shading', 'lit')
    pixel_style = bool(getattr(vs, 'pixel_style', True))
    min_y = region.info.min_y
    groups = {}
    for e, props in vis:
        groups.setdefault(((e['x'] >> 4) // N, (e['z'] >> 4) // N), []).append((e, props))
    col = sb._get_collection() if hasattr(sb, '_get_collection') else bpy.context.scene.collection
    total = 0
    for gk, items in groups.items():
        loc = (gk[0] * N * 16 * s, -gk[1] * N * 16 * s, min_y * s)
        mats, mat_index = [], {}
        verts, faces, uvs, cols, fmat = [], [], [], [], []
        for e, props in items:
            for k, (kind, pattern, rgb, faces_geo) in enumerate(_layers(e, props)):
                mat = _material(kind, pattern, assets_dir, shading, pixel_style)
                if mat is None:
                    continue
                if mat.name not in mat_index:
                    mat_index[mat.name] = len(mats)
                    mats.append(mat)
                for pos, uv, nrm in faces_geo:
                    base = len(verts)
                    off = LAYER_EPS * (k + 1)
                    for p in pos:
                        wx, wy, wz = e['x'] + p[0] + nrm[0] * off, e['y'] + p[1] + nrm[1] * off, e['z'] + p[2] + nrm[2] * off
                        verts.append((wx * s - loc[0], -wz * s - loc[1], wy * s - loc[2]))      # мир (x, y, z) -> Blender (x, -z, y), как у воксельных мешей
                    faces.append((base, base + 1, base + 2, base + 3))
                    uvs += [(u, 1.0 - v) for u, v in uv]
                    cols.append((rgb[0], rgb[1], rgb[2], 255))
                    fmat.append(mat_index[mat.name])
                total += 1 if k == 0 else 0
        if not faces:
            continue
        mesh = bpy.data.meshes.new('mc_banners_%d_%d' % gk)
        mesh.from_pydata(verts, [], faces)
        uvl = mesh.uv_layers.new(name='UVMap')
        uvl.data.foreach_set('uv', np.asarray(uvs, dtype=np.float32).reshape(-1))
        ca = mesh.attributes.new('Col', 'BYTE_COLOR', 'FACE')
        ca.data.foreach_set('color_srgb', (np.asarray(cols, dtype=np.float32) * np.float32(1.0 / 255.0)).reshape(-1))
        for m in mats:
            mesh.materials.append(m)
        mesh.polygons.foreach_set('material_index', np.asarray(fmat, dtype=np.int32))
        mesh.update()
        obj = bpy.data.objects.new('mc_banners_%d_%d' % gk, mesh)
        obj.location = loc
        obj[OVERLAY_KEY] = 'banner'
        col.objects.link(obj)
    return total


def refresh(sb):
    """Пересоздаёт узоры по сохранённым данным последней сборки (после правок блоков: сломанный баннер теряет узор)."""
    st = getattr(sb, '_overlay_state', None)
    if st is None:
        return 0
    return rebuild(sb, st['region'], st['names'], st['entries'], st['assets_dir'], st['view'])

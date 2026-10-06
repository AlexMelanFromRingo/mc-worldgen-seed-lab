"""Материалы и изображение атласа для объектов мира (bpy). Четыре материала по слоям прозрачности: opaque, cutout, translucent, water.
Цвет = текстура атласа × цвет вершины (атрибут `Col` = оттенок биома [× затенение]). Интерполяция Closest (пиксельный стиль) или Linear."""
import os

import bpy

from ..assets import pbr as pbr_mod
from ..assets import pngio

__all__ = ['MAT_NAMES', 'TILED_NAMES', 'LOD_NAME', 'ensure_atlas_image', 'ensure_pbr_images', 'ensure_materials', 'ensure_tiled_materials', 'ensure_lod_material',
           'remove_materials']

MAT_NAMES = ('MC_opaque', 'MC_cutout', 'MC_translucent', 'MC_water')
TILED_NAMES = ('MC_opaque_tiled', 'MC_cutout_tiled', 'MC_translucent_tiled', 'MC_water_tiled')
LOD_NAME = 'MC_lod'
ATLAS_PREFIX = 'MC_atlas'
PBR_PREFIX = 'MC_pbr'
EMISSION_STRENGTH = 4.0           # множитель свечения карты orm.B (Principled «Emission Strength»)


def ensure_atlas_image(table, cache_dir, pixel_style=True):
    """Сохраняет атлас PNG в cache_dir (если нет) и загружает в Blender как FILE-изображение. Возвращает bpy.types.Image."""
    tag = (table.source_hash or 'x')[:12]
    name = '%s_%s' % (ATLAS_PREFIX, tag)
    img = bpy.data.images.get(name)
    if img is not None and img.size[0] == table.atlas_image.shape[1] and img.size[1] == table.atlas_image.shape[0]:
        return img
    os.makedirs(cache_dir or '.', exist_ok=True)
    path = os.path.abspath(os.path.join(cache_dir or '.', name + '.png'))
    if not os.path.isfile(path):
        pngio.write_png(path, table.atlas_image, filter_mode='up')
    if img is not None:
        bpy.data.images.remove(img)
    img = bpy.data.images.load(path, check_existing=False)
    img.name = name
    img.colorspace_settings.name = 'sRGB'
    img.alpha_mode = 'STRAIGHT'
    img.use_fake_user = False
    return img


def ensure_pbr_images(table, cache_dir):
    """Карты PBR атласа (assets/pbr.py): normal (касательное пространство) и orm (R — шероховатость, G — металличность, B — свечение) как Non-Color изображения.
    PNG кэшируются рядом с атласом (ключ — хэш ресурсов и версия правил). Возвращает (normal, orm) — bpy.types.Image."""
    tag = '%s_v%d' % ((table.source_hash or 'x')[:12], pbr_mod.PBR_VERSION)
    out = []
    maps = None
    for kind in ('n', 'o'):
        name = '%s%s_%s' % (PBR_PREFIX, kind, tag)
        img = bpy.data.images.get(name)
        if img is not None and img.size[0] == table.atlas_image.shape[1] and img.size[1] == table.atlas_image.shape[0]:
            out.append(img)
            continue
        os.makedirs(cache_dir or '.', exist_ok=True)
        path = os.path.abspath(os.path.join(cache_dir or '.', name + '.png'))
        if not os.path.isfile(path):
            if maps is None:
                maps = pbr_mod.build_pbr(table.atlas_image, table.atlas_rect, table.atlas_names, pad=int(table.atlas_info.get('pad', 2)))
            pngio.write_png(path, maps[0] if kind == 'n' else maps[1], filter_mode='up')
        if img is not None:
            bpy.data.images.remove(img)
        img = bpy.data.images.load(path, check_existing=False)
        img.name = name
        img.colorspace_settings.name = 'Non-Color'
        img.alpha_mode = 'CHANNEL_PACKED'
        img.use_fake_user = False
        out.append(img)
    return out[0], out[1]


def _set(node, names, value):
    for n in names:
        s = node.inputs.get(n)
        if s is not None:
            s.default_value = value
            return True
    return False


def _pbr_inputs(nodes, links, bsdf, pbr, pixel_style, vec_out, color_out):
    """Подключает карты PBR к Principled: нормаль (Normal Map по UV-слою `UVMap` — касательный базис следует развёртке грани, поэтому поворот и uvlock не ломают
    рельеф), шероховатость и металличность (orm.R, orm.G), свечение (orm.B × цвет текстуры × EMISSION_STRENGTH). Выборка — тем же вектором, что и у альбедо."""
    img_n, img_o = pbr
    interp = 'Closest' if pixel_style else 'Linear'
    tn = nodes.new('ShaderNodeTexImage')
    tn.image = img_n
    tn.interpolation = interp
    tn.extension = 'EXTEND'
    tn.location = (-700, -450)
    to = nodes.new('ShaderNodeTexImage')
    to.image = img_o
    to.interpolation = interp
    to.extension = 'EXTEND'
    to.location = (-700, -750)
    if vec_out is not None:
        links.new(vec_out, tn.inputs['Vector'])
        links.new(vec_out, to.inputs['Vector'])
    nm = nodes.new('ShaderNodeNormalMap')
    nm.space = 'TANGENT'
    nm.uv_map = 'UVMap'
    nm.location = (-450, -450)
    links.new(tn.outputs['Color'], nm.inputs['Color'])
    links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])
    sep = nodes.new('ShaderNodeSeparateColor')
    sep.mode = 'RGB'
    sep.location = (-450, -750)
    links.new(to.outputs['Color'], sep.inputs['Color'])
    links.new(sep.outputs['Red'], bsdf.inputs['Roughness'])
    links.new(sep.outputs['Green'], bsdf.inputs['Metallic'])
    _set(bsdf, ('Specular IOR Level', 'Specular'), 0.5)
    if bsdf.inputs.get('Emission Color') is not None:
        mul = nodes.new('ShaderNodeMath')
        mul.operation = 'MULTIPLY'
        mul.inputs[1].default_value = EMISSION_STRENGTH
        mul.location = (-300, -750)
        links.new(sep.outputs['Blue'], mul.inputs[0])
        links.new(color_out, bsdf.inputs['Emission Color'])
        links.new(mul.outputs[0], bsdf.inputs['Emission Strength'])


def _build(mat, image, kind, shading, pixel_style, tiled=False, pbr=None):
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    nodes, links = nt.nodes, nt.links
    out = nodes.new('ShaderNodeOutputMaterial')
    tex = nodes.new('ShaderNodeTexImage')
    tex.image = image
    tex.interpolation = 'Closest' if pixel_style else 'Linear'
    tex.extension = 'EXTEND'
    tex.location = (-700, 100)
    vc = nodes.new('ShaderNodeAttribute')       # цвет граней (атрибут Col в области FACE — быстрее заполняется, чем цвет углов)
    vc.attribute_type = 'GEOMETRY'
    vc.attribute_name = 'Col'
    vc.location = (-700, -150)
    mix = nodes.new('ShaderNodeMix')
    mix.data_type = 'RGBA'
    mix.blend_type = 'MULTIPLY'
    mix.inputs[0].default_value = 1.0
    mix.location = (-450, 0)
    if tiled:
        # слитые грани: UV = локальные координаты тайла (0..w, 0..h), атрибут грани Rect = (u0, v0, du, dv) спрайта в атласе;
        # координата в атласе = Rect.xy + fract(local) * Rect.zw
        uvn = nodes.new('ShaderNodeUVMap')
        uvn.uv_map = 'UVMap'
        uvn.location = (-1250, 300)
        rect = nodes.new('ShaderNodeAttribute')
        rect.attribute_type = 'GEOMETRY'
        rect.attribute_name = 'Rect'
        rect.location = (-1250, 50)
        sep = nodes.new('ShaderNodeSeparateXYZ')
        sep.location = (-1050, 300)
        links.new(uvn.outputs['UV'], sep.inputs[0])
        frs = []
        for i, ax in enumerate(('X', 'Y')):
            fr = nodes.new('ShaderNodeMath')
            fr.operation = 'FRACT'
            fr.location = (-880, 320 - 120 * i)
            links.new(sep.outputs[ax], fr.inputs[0])
            frs.append(fr)
        # rect.color = (u0, v0, du), rect.alpha = dv
        rsep = nodes.new('ShaderNodeSeparateColor')
        rsep.location = (-1050, 50)
        links.new(rect.outputs['Color'], rsep.inputs[0])
        comps = []
        for i, (fr, off_out, size_out) in enumerate(((frs[0], rsep.outputs[0], rsep.outputs[2]), (frs[1], rsep.outputs[1], None))):
            mul = nodes.new('ShaderNodeMath')
            mul.operation = 'MULTIPLY'
            mul.location = (-700, 320 - 120 * i)
            links.new(fr.outputs[0], mul.inputs[0])
            if size_out is not None:
                links.new(size_out, mul.inputs[1])
            else:
                links.new(rect.outputs['Alpha'], mul.inputs[1])
            add = nodes.new('ShaderNodeMath')
            add.operation = 'ADD'
            add.location = (-540, 320 - 120 * i)
            links.new(mul.outputs[0], add.inputs[0])
            links.new(off_out, add.inputs[1])
            comps.append(add)
        comb = nodes.new('ShaderNodeCombineXYZ')
        comb.location = (-420, 250)
        links.new(comps[0].outputs[0], comb.inputs['X'])
        links.new(comps[1].outputs[0], comb.inputs['Y'])
        links.new(comb.outputs[0], tex.inputs['Vector'])
        vec_out = comb.outputs[0]
    else:
        vec_out = None
    links.new(tex.outputs['Color'], mix.inputs[6])
    links.new(vc.outputs['Color'], mix.inputs[7])
    color_out = mix.outputs[2]
    # альфа
    alpha_out = None
    if kind == 'cutout':
        gt = nodes.new('ShaderNodeMath')
        gt.operation = 'GREATER_THAN'
        gt.inputs[1].default_value = 0.5
        gt.location = (-450, 250)
        links.new(tex.outputs['Alpha'], gt.inputs[0])
        alpha_out = gt.outputs[0]
    elif kind in ('translucent', 'water'):
        mul = nodes.new('ShaderNodeMath')
        mul.operation = 'MULTIPLY'
        mul.location = (-450, 250)
        links.new(tex.outputs['Alpha'], mul.inputs[0])
        links.new(vc.outputs['Alpha'], mul.inputs[1])
        alpha_out = mul.outputs[0]
    if shading == 'emission':
        em = nodes.new('ShaderNodeEmission')
        em.location = (-200, 0)
        links.new(color_out, em.inputs['Color'])
        surf = em.outputs[0]
        if alpha_out is not None:
            tr = nodes.new('ShaderNodeBsdfTransparent')
            mx = nodes.new('ShaderNodeMixShader')
            mx.location = (0, 0)
            links.new(alpha_out, mx.inputs[0])
            links.new(tr.outputs[0], mx.inputs[1])
            links.new(surf, mx.inputs[2])
            surf = mx.outputs[0]
        links.new(surf, out.inputs['Surface'])
    else:
        bsdf = nodes.new('ShaderNodeBsdfPrincipled')
        bsdf.location = (-200, 0)
        if pbr is not None and kind != 'water':
            _pbr_inputs(nodes, links, bsdf, pbr, pixel_style, vec_out, color_out)
        else:
            _set(bsdf, ('Roughness',), 1.0)
            _set(bsdf, ('Specular IOR Level', 'Specular'), 0.0)
        links.new(color_out, bsdf.inputs['Base Color'])
        if alpha_out is not None:
            links.new(alpha_out, bsdf.inputs['Alpha'])
        links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    out.location = (200, 0)
    # режим смешивания: Blender 4.2+ (EEVEE Next) — surface_render_method; старые версии — blend_method
    if hasattr(mat, 'surface_render_method'):
        mat.surface_render_method = 'BLENDED' if kind == 'water' else 'DITHERED'
    else:
        mat.blend_method = 'BLEND' if kind == 'water' else ('OPAQUE' if kind == 'opaque' else 'HASHED')
        try:
            mat.shadow_method = 'OPAQUE' if kind == 'opaque' else 'HASHED'
        except Exception:
            pass
    mat.use_backface_culling = False
    mat.diffuse_color = (0.5, 0.5, 0.5, 1.0)


def ensure_materials(table, cache_dir, shading='lit', pixel_style=True, water_style='TRANSLUCENT', pbr=False):
    """Создаёт (или обновляет) четыре материала; возвращает список по индексам MAT_SOLID..MAT_WATER."""
    image = ensure_atlas_image(table, cache_dir, pixel_style)
    pbr_imgs = ensure_pbr_images(table, cache_dir) if (pbr and shading == 'lit') else None
    mats = []
    wk = 'opaque' if water_style == 'OPAQUE' else 'water'
    for name, kind in zip(MAT_NAMES, ('opaque', 'cutout', 'translucent', wk)):
        mat = bpy.data.materials.get(name)
        stamp = '%s|%s|%s|%s|%s' % (image.name, shading, pixel_style, wk, pbr_imgs[0].name if pbr_imgs else '-')
        if mat is None:
            mat = bpy.data.materials.new(name)
            mat['mc_stamp'] = ''
        if mat.get('mc_stamp') != stamp:
            _build(mat, image, kind, shading, pixel_style, pbr=pbr_imgs)
            mat['mc_stamp'] = stamp
        mats.append(mat)
    return mats


def ensure_tiled_materials(table, cache_dir, shading='lit', pixel_style=True, water_style='TRANSLUCENT', pbr=False):
    """Материалы для слитых граней (тайл повторяется в шейдере)."""
    image = ensure_atlas_image(table, cache_dir, pixel_style)
    pbr_imgs = ensure_pbr_images(table, cache_dir) if (pbr and shading == 'lit') else None
    mats = []
    wk = 'opaque' if water_style == 'OPAQUE' else 'water'
    for name, kind in zip(TILED_NAMES, ('opaque', 'cutout', 'translucent', wk)):
        mat = bpy.data.materials.get(name)
        stamp = '%s|%s|%s|%s|%s|t' % (image.name, shading, pixel_style, wk, pbr_imgs[0].name if pbr_imgs else '-')
        if mat is None:
            mat = bpy.data.materials.new(name)
            mat['mc_stamp'] = ''
        if mat.get('mc_stamp') != stamp:
            _build(mat, image, kind, shading, pixel_style, tiled=True, pbr=pbr_imgs)
            mat['mc_stamp'] = stamp
        mats.append(mat)
    return mats


def ensure_lod_material(shading='lit'):
    """Материал дальнего LOD: только цвет вершин (атрибут Col)."""
    mat = bpy.data.materials.get(LOD_NAME)
    stamp = 'lod|%s' % shading
    if mat is None:
        mat = bpy.data.materials.new(LOD_NAME)
        mat['mc_stamp'] = ''
    if mat.get('mc_stamp') != stamp:
        mat.use_nodes = True
        nt = mat.node_tree
        nt.nodes.clear()
        out = nt.nodes.new('ShaderNodeOutputMaterial')
        vc = nt.nodes.new('ShaderNodeAttribute')
        vc.attribute_type = 'GEOMETRY'
        vc.attribute_name = 'Col'
        if shading == 'emission':
            em = nt.nodes.new('ShaderNodeEmission')
            nt.links.new(vc.outputs['Color'], em.inputs['Color'])
            nt.links.new(em.outputs[0], out.inputs['Surface'])
        else:
            bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
            _set(bsdf, ('Roughness',), 1.0)
            _set(bsdf, ('Specular IOR Level', 'Specular'), 0.0)
            nt.links.new(vc.outputs['Color'], bsdf.inputs['Base Color'])
            nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
        mat['mc_stamp'] = stamp
    return mat


def remove_materials():
    for n in MAT_NAMES + TILED_NAMES + (LOD_NAME,):
        m = bpy.data.materials.get(n)
        if m is not None:
            bpy.data.materials.remove(m)
    for img in list(bpy.data.images):
        if img.name.startswith(ATLAS_PREFIX) or img.name.startswith(PBR_PREFIX):
            bpy.data.images.remove(img)

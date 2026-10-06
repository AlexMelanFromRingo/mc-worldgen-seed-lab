"""Проверка ориентации карты нормалей PBR в Blender (headless, Cycles CPU): плоскость с «выпуклостью» из assets/pbr.py при свете слева — левый склон
выпуклости светлее правого, верхний (при свете сверху) светлее нижнего. Запуск: blender -b --factory-startup --python blender/tests/test_pbr_blender.py"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _boot  # noqa: E402
import bpy  # noqa: E402
import mathutils  # noqa: E402
import numpy as np  # noqa: E402

from mcgen_addon.assets import pbr  # noqa: E402
from mcgen_addon.render import materials as materials_mod  # noqa: E402

FAILS = []


def check(name, cond, info=''):
    print(('PASS ' if cond else 'FAIL ') + name + (' | ' + str(info) if info and not cond else ''))
    if not cond:
        FAILS.append(name)


def bump_normal_map(size=16, strength=1.5):
    """RGBA спрайт с яркой выпуклостью 8×8 в центре → карта нормалей assets/pbr.py (строка 0 — верх)."""
    rgba = np.zeros((size, size, 4), np.uint8)
    rgba[:, :, :3] = 60
    rgba[:, :, 3] = 255
    q = size // 4
    rgba[q:size - q, q:size - q, :3] = 230
    p = dict(pbr.classify('stone'))
    p['nrm'] = strength
    normal, orm = pbr._sprite_maps(rgba, p)
    return normal


def render(light_dir):
    """Рисует плоскость с картой нормалей при свете, идущем вдоль light_dir (вектор распространения света, Blender-оси), возвращает яркость (N, N)."""
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    scn = bpy.context.scene
    scn.render.engine = 'CYCLES'
    scn.cycles.device = 'CPU'
    scn.cycles.samples = 24
    scn.cycles.use_denoising = False
    scn.render.resolution_x = scn.render.resolution_y = 128
    scn.render.resolution_percentage = 100
    scn.view_settings.view_transform = 'Standard'
    w = bpy.data.worlds.new('w')
    scn.world = w
    w.use_nodes = True
    w.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.0
    nm = bump_normal_map()
    n = nm.shape[0]
    img = bpy.data.images.new('bump_n', n, n, alpha=True)
    img.colorspace_settings.name = 'Non-Color'                                # до записи пикселей: смена пространства перезагружает буфер
    px = np.ones((n, n, 4), np.float32)
    px[:, :, :3] = np.flipud(nm).astype(np.float32) / 255.0                 # Blender хранит строки снизу вверх
    img.pixels.foreach_set(px.reshape(-1))
    me = bpy.data.meshes.new('plane')
    me.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
    me.uv_layers.new(name='UVMap')
    uv = me.uv_layers['UVMap'].data
    for i, (u, v) in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv[i].uv = (u, v)
    ob = bpy.data.objects.new('plane', me)
    scn.collection.objects.link(ob)
    mat = bpy.data.materials.new('m')
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    bsdf.inputs['Base Color'].default_value = (0.8, 0.8, 0.8, 1.0)
    bsdf.inputs['Roughness'].default_value = 1.0
    tn = nt.nodes.new('ShaderNodeTexImage')
    tn.image = img
    tn.interpolation = 'Closest'
    nmn = nt.nodes.new('ShaderNodeNormalMap')
    nmn.space = 'TANGENT'
    nmn.uv_map = 'UVMap'
    nt.links.new(tn.outputs['Color'], nmn.inputs['Color'])
    nt.links.new(nmn.outputs['Normal'], bsdf.inputs['Normal'])
    nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    me.materials.append(mat)
    sun = bpy.data.lights.new('sun', 'SUN')
    sun.energy = 5.0
    so = bpy.data.objects.new('sun', sun)
    scn.collection.objects.link(so)
    so.rotation_euler = (-mathutils.Vector(light_dir)).normalized().to_track_quat('Z', 'Y').to_euler()      # локальная +Z смотрит на источник, свет идёт по -Z
    cam = bpy.data.cameras.new('cam')
    cam.type = 'ORTHO'
    cam.ortho_scale = 2.0
    co = bpy.data.objects.new('cam', cam)
    scn.collection.objects.link(co)
    co.location = (0, 0, 5)
    scn.camera = co                                                              # смотрит вдоль -Z, +X вправо, +Y вверх
    path = os.path.join(_boot.SCRATCH, 'pbr_bump_%d_%d_%d.png' % tuple(int(round(v)) for v in light_dir))
    scn.render.filepath = path
    scn.render.image_settings.file_format = 'PNG'
    bpy.ops.render.render(write_still=True)
    r = bpy.data.images.load(path, check_existing=False)
    a = np.array(r.pixels[:], np.float32).reshape(r.size[1], r.size[0], 4)[:, :, :3].mean(axis=2)
    bpy.data.images.remove(r)
    return np.flipud(a)                                                          # строка 0 — верх кадра


def main():
    n = 128
    cell = n // 16
    lo, hi = 4 * cell, 12 * cell                                                 # границы выпуклости в пикселях кадра
    mid = n // 2
    # свет идёт слева направо и вниз: (+1, 0, -1)
    a = render((1.0, 0.0, -1.0))
    left = a[mid - 8:mid + 8, lo:lo + cell].mean()
    right = a[mid - 8:mid + 8, hi - cell:hi].mean()
    check('свет слева: левый склон выпуклости светлее правого (%.3f > %.3f)' % (left, right), left > right * 1.15, (left, right))
    # свет идёт сверху вниз по кадру (−Y) и вниз: (0, −1, −1) — верхний склон (смотрит вверх, +Y) освещён
    b = render((0.0, -1.0, -1.0))
    top = b[lo:lo + cell, mid - 8:mid + 8].mean()
    bottom = b[hi - cell:hi, mid - 8:mid + 8].mean()
    check('свет сверху: верхний склон светлее нижнего (%.3f > %.3f)' % (top, bottom), top > bottom * 1.15, (top, bottom))
    print('FAILED: %d' % len(FAILS) if FAILS else 'ALL OK')
    sys.exit(1 if FAILS else 0)


main()

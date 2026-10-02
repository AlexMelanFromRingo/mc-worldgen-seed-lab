"""Общие функции headless-рендера для render_mca.py / render_showcase.py: движок, небо, солнце, камера."""
import math
import os
import sys

import bpy
import mathutils

HERE = os.path.dirname(os.path.abspath(__file__))


def clean_default_scene():
    """Убирает объекты стартовой сцены Blender (куб, свет, камера), попадающие в кадр."""
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)


def setup_render(a):
    scn = bpy.context.scene
    names = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items}
    if a.engine == 'eevee':
        scn.render.engine = 'BLENDER_EEVEE_NEXT' if 'BLENDER_EEVEE_NEXT' in names else 'BLENDER_EEVEE'
        try:
            scn.eevee.taa_render_samples = a.samples
        except Exception:
            pass
    elif a.engine == 'workbench':
        scn.render.engine = 'BLENDER_WORKBENCH'
        sh = scn.display.shading
        sh.light = 'FLAT'
        sh.color_type = 'TEXTURE'
    else:
        scn.render.engine = 'CYCLES'
        scn.cycles.device = 'CPU'
        scn.cycles.samples = a.samples
        scn.cycles.use_denoising = False
    w, h = (int(v) for v in a.res.split('x'))
    scn.render.resolution_x, scn.render.resolution_y = w, h
    scn.render.resolution_percentage = 100
    scn.render.image_settings.file_format = 'PNG'
    scn.view_settings.view_transform = 'Standard'
    scn.view_settings.look = 'None'
    # небо
    world = bpy.data.worlds.new('MCWorld') if not bpy.data.worlds else bpy.data.worlds[0]
    scn.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get('Background')
    if bg:
        bg.inputs['Color'].default_value = (0.47, 0.66, 1.0, 1.0)
        bg.inputs['Strength'].default_value = 1.4
    # солнце
    sun = bpy.data.lights.new('sun', 'SUN')
    sun.energy = a.sun_strength
    sun.angle = math.radians(1.5)
    so = bpy.data.objects.new('sun', sun)
    scn.collection.objects.link(so)
    az, el = math.radians(a.sun_az), math.radians(a.sun_el)
    # направление света: от точки на небе к земле; азимут от севера (=+Y) по часовой
    d = mathutils.Vector((math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)))
    so.rotation_euler = d.to_track_quat('Z', 'Y').to_euler()
    # заполняющий свет с противоположной стороны (без теней): у игры тень не бывает чёрной
    fill = bpy.data.lights.new('fill', 'SUN')
    fill.energy = a.sun_strength * 0.35
    fill.use_shadow = False
    fo = bpy.data.objects.new('fill', fill)
    scn.collection.objects.link(fo)
    d2 = mathutils.Vector((-math.sin(az) * math.cos(el) * 0.7, -math.cos(az) * math.cos(el) * 0.7, 0.55)).normalized()
    fo.rotation_euler = d2.to_track_quat('Z', 'Y').to_euler()


def setup_camera(a, bbox):
    scn = bpy.context.scene
    (x0, y0, z0), (x1, y1, z1) = bbox
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    cz = a.target_y if a.target_y is not None else (z0 + z1) / 2
    target = mathutils.Vector((cx, cy, cz))
    span = max(x1 - x0, y1 - y0)
    cam = bpy.data.cameras.new('cam')
    co = bpy.data.objects.new('cam', cam)
    scn.collection.objects.link(co)
    scn.camera = co
    yaw, pitch = math.radians(a.yaw), math.radians(a.pitch)
    if a.cam == 'top':
        pitch = math.radians(89.5)
    dist = a.dist or span * 1.5
    dirv = mathutils.Vector((math.sin(yaw) * math.cos(pitch), math.cos(yaw) * math.cos(pitch), math.sin(pitch)))
    # камера «смотрит с юга-запада» при yaw=35: позиция против направления взгляда
    pos = target + mathutils.Vector((-dirv.x, -dirv.y, dirv.z)) * dist if False else target + mathutils.Vector((-math.sin(yaw) * math.cos(pitch), -math.cos(yaw) * math.cos(pitch), math.sin(pitch))) * dist
    co.location = pos
    look = (target - pos).normalized()
    co.rotation_euler = look.to_track_quat('-Z', 'Y').to_euler()
    if a.cam in ('iso', 'top', 'ortho'):
        cam.type = 'ORTHO'
        cam.ortho_scale = span * 1.15 / a.zoom
    else:
        cam.type = 'PERSP'
        cam.lens = 35 * a.zoom
    cam.clip_start = 0.5
    cam.clip_end = 5000



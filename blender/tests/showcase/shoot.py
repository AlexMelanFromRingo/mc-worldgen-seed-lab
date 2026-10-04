"""Показательный кадр ВНУТРИ Blender: настоящий аддон (оператор Generate, все стадии) -> небо/солнце/камера -> рендер EEVEE.

    blender -b --factory-startup --python blender/tests/showcase/shoot.py -- --scene forest [--spec scenes.json] [--out файл] [--preview]

Запускается через run_showcase.py (он готовит чистый профиль Blender с аддоном и переменные окружения). Спецификация сцены — запись
scenes.json: версия, измерение, сид, область, диапазон высот, вид, окружение (overworld / nether / end), камера в МИРОВЫХ координатах
Minecraft (target [x, y, z], azimuth — куда смотрит камера по компасу: 0 — на север (−z), 90 — на восток (+x); elevation — угол вниз;
dist), солнце (az — откуда светит, el — высота). Печатает строку «RESULT {json}».
"""
import argparse
import importlib
import json
import math
import os
import sys
import time

import addon_utils
import bpy
import mathutils

HERE = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
EXT = os.environ.get('MCGEN_EXT_MODULE', 'bl_ext.user_default.mcgen')

ENVS = {
    # ambient — цвет мягкого света неба; zenith/horizon — вид неба для камеры; strength — сила ambient; sun/sun_color — солнце; fill — заполняющий свет
    'overworld': dict(ambient=(0.55, 0.70, 1.0), zenith=(0.12, 0.33, 0.85), horizon=(0.62, 0.78, 1.0), strength=0.85, sun=3.0,
                      sun_color=(1.0, 0.95, 0.85), fill=0.7),
    'snow': dict(ambient=(0.60, 0.74, 1.0), zenith=(0.14, 0.36, 0.88), horizon=(0.66, 0.80, 1.0), strength=0.9, sun=3.0,
                 sun_color=(1.0, 0.96, 0.90), fill=0.7),
    'nether': dict(ambient=(0.55, 0.16, 0.08), zenith=(0.16, 0.03, 0.02), horizon=(0.30, 0.06, 0.03), strength=2.2, sun=3.2,
                   sun_color=(1.0, 0.6, 0.35), fill=2.6),
    'end': dict(ambient=(0.30, 0.22, 0.45), zenith=(0.02, 0.01, 0.05), horizon=(0.05, 0.03, 0.10), strength=1.1, sun=1.8,
                sun_color=(0.85, 0.78, 1.0), fill=1.2),
}


def parse():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--spec', default=os.path.join(HERE, 'scenes.json'))
    ap.add_argument('--scene', required=True)
    ap.add_argument('--out', default=None, help='файл кадра (по умолчанию docs/blender/img/render-<scene>.jpg или outdir)')
    ap.add_argument('--outdir', default=os.path.join(REPO, 'docs', 'blender', 'img'))
    ap.add_argument('--preview', action='store_true', help='быстрый черновик 960×540, 8 выборок')
    ap.add_argument('--override', default='{}', help='JSON-перекрытие полей записи сцены (камера и т. п.)')
    ap.add_argument('--save-blend', default=None)
    ap.add_argument('--engine', default='eevee', choices=['eevee', 'workbench'])
    ap.add_argument('--no-render', action='store_true')
    ap.add_argument('--pick', default=None, help='пиксели кадра x,y;x,y — вывести блок, в который попадает луч камеры (отладка артефактов)')
    return ap.parse_args(argv)


def deep_update(d, u):
    for k, v in u.items():
        if isinstance(v, dict) and isinstance(d.get(k), dict):
            deep_update(d[k], v)
        else:
            d[k] = v
    return d


def mc_to_scene(p, cx0=0, cz0=0):
    """Мировая точка Minecraft (x, y, z) -> координаты Blender. Построитель сцены W4 кладёт группы чанков в АБСОЛЮТНЫЕ координаты
    (Blender = (x, −z, y)); запасной предпросмотр — относительно угла области (в съёмке не используется)."""
    return mathutils.Vector((p[0], -p[2], p[1]))


def setup_world(env):
    """Небо: для лучей камеры — насыщенный градиент (зенит -> светлый горизонт), для освещения сцены — ровный цвет env['sky'] (мягкий свет)."""
    scn = bpy.context.scene
    world = bpy.data.worlds.new('MCShowcase')
    scn.world = world
    world.use_nodes = True
    nt = world.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new('ShaderNodeOutputWorld')
    light = nt.nodes.new('ShaderNodeBackground')
    light.inputs['Color'].default_value = (*env['ambient'], 1)
    light.inputs['Strength'].default_value = env['strength']
    view = nt.nodes.new('ShaderNodeBackground')
    view.inputs['Strength'].default_value = 1.0
    texco = nt.nodes.new('ShaderNodeTexCoord')
    sep = nt.nodes.new('ShaderNodeSeparateXYZ')
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    nt.links.new(texco.outputs['Generated'], sep.inputs['Vector'])
    nt.links.new(sep.outputs['Z'], ramp.inputs['Fac'])
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (*env['horizon'], 1)
    ramp.color_ramp.elements[1].position = 0.55
    ramp.color_ramp.elements[1].color = (*env['zenith'], 1)
    nt.links.new(ramp.outputs['Color'], view.inputs['Color'])
    lp = nt.nodes.new('ShaderNodeLightPath')
    mix = nt.nodes.new('ShaderNodeMixShader')
    nt.links.new(lp.outputs['Is Camera Ray'], mix.inputs['Fac'])
    nt.links.new(light.outputs['Background'], mix.inputs[1])
    nt.links.new(view.outputs['Background'], mix.inputs[2])
    nt.links.new(mix.outputs['Shader'], out.inputs['Surface'])


def setup_lights(env, sun_az, sun_el, strength=None):
    scn = bpy.context.scene
    sun = bpy.data.lights.new('sun', 'SUN')
    sun.energy = strength if strength is not None else env['sun']
    sun.color = env['sun_color']
    sun.angle = math.radians(2.0)
    so = bpy.data.objects.new('sun', sun)
    scn.collection.objects.link(so)
    az, el = math.radians(sun_az), math.radians(sun_el)
    d = mathutils.Vector((math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)))
    so.rotation_euler = d.to_track_quat('Z', 'Y').to_euler()
    fill = bpy.data.lights.new('fill', 'SUN')
    fill.energy = env['fill']
    fill.use_shadow = False
    fo = bpy.data.objects.new('fill', fill)
    scn.collection.objects.link(fo)
    d2 = mathutils.Vector((-math.sin(az) * 0.6, -math.cos(az) * 0.6, 0.8)).normalized()
    fo.rotation_euler = d2.to_track_quat('Z', 'Y').to_euler()


def ground_fn():
    """ground(x, z) -> y самого верхнего непрозрачного блока без листвы в колонке мира (карта высот MOTION_BLOCKING_NO_LEAVES)."""
    jobs = importlib.import_module(EXT + '.core.jobs')
    sess = jobs.session(bpy.context.scene.name)
    reg = sess.region

    def ground(x, z, kind=3):
        cx, cz = int(x) // 16, int(z) // 16
        return int(reg.heightmap(cx, cz, kind)[int(z) - cz * 16, int(x) - cx * 16]) - 1
    return ground


def resolve(p, ground):
    """[x, y, z]; y — число или строка 'g+N' / 'g-N' (над уровнем земли в колонке x, z)."""
    x, y, z = p
    if isinstance(y, str):
        y = ground(x, z) + float(y[1:] if y[0] == 'g' else y)
    return [x, y, z]


def setup_camera(cam_spec, cx0, cz0, auto_target, auto_span, ground):
    scn = bpy.context.scene
    target = mc_to_scene(resolve(cam_spec['target'], ground) if cam_spec.get('target') else auto_target, cx0, cz0)
    az = math.radians(cam_spec.get('azimuth', 20.0))
    el = math.radians(cam_spec.get('elevation', 32.0))
    dist = cam_spec.get('dist', auto_span * 1.3)
    look = mathutils.Vector((math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), -math.sin(el)))
    pos = mc_to_scene(resolve(cam_spec['pos'], ground), cx0, cz0) if cam_spec.get('pos') else target - look * dist
    cam = bpy.data.cameras.new('cam')
    co = bpy.data.objects.new('cam', cam)
    scn.collection.objects.link(co)
    scn.camera = co
    co.location = pos
    co.rotation_euler = (target - pos).normalized().to_track_quat('-Z', 'Y').to_euler()
    if cam_spec.get('ortho'):
        cam.type = 'ORTHO'
        cam.ortho_scale = cam_spec['ortho']
    else:
        cam.type = 'PERSP'
        cam.lens = cam_spec.get('lens', 35.0)
    cam.sensor_width = 36.0
    cam.clip_start = 0.5
    cam.clip_end = 6000
    return co


def setup_render(res, samples, engine):
    scn = bpy.context.scene
    names = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items}
    if engine == 'workbench':
        scn.render.engine = 'BLENDER_WORKBENCH'
        scn.display.shading.light = 'FLAT'
        scn.display.shading.color_type = 'TEXTURE'
    else:
        scn.render.engine = 'BLENDER_EEVEE_NEXT' if 'BLENDER_EEVEE_NEXT' in names else 'BLENDER_EEVEE'
        try:
            scn.eevee.taa_render_samples = samples
        except Exception:     # noqa: BLE001
            pass
    scn.render.resolution_x, scn.render.resolution_y = res
    scn.render.resolution_percentage = 100
    scn.view_settings.view_transform = 'Standard'
    scn.view_settings.look = 'None'


def surface_stats():
    """(x0, x1, y0, y1, ztop_median) по объектам чанков сцены — для автокамеры."""
    xs, ys, tops = [], [], []
    for o in bpy.data.objects:
        if o.name.startswith('mc_') and o.type == 'MESH':
            bb = [o.matrix_world @ mathutils.Vector(c) for c in o.bound_box]
            xs += [v.x for v in bb]
            ys += [v.y for v in bb]
            tops.append(max(v.z for v in bb))
    if not xs:
        return (0, 128, -128, 0, 80)
    tops.sort()
    return (min(xs), max(xs), min(ys), max(ys), tops[len(tops) // 2])


def pick_pixels(txt, res, cam_obj):
    """Луч камеры через пиксель -> первый непустой блок региона (обход вокселей). Только для отладки кадров."""
    jobs = importlib.import_module(EXT + '.core.jobs')
    sess = jobs.session(bpy.context.scene.name)
    reg = sess.region
    names = sess.gen.block_names()
    air = {i for i, n in enumerate(names) if n in ('minecraft:air', 'minecraft:cave_air', 'minecraft:void_air')}
    bpy.context.view_layer.update()
    cam = cam_obj.data
    w, h = res
    M = cam_obj.matrix_world
    o = M.translation
    for part in txt.split(';'):
        px, py = (float(v) for v in part.split(','))
        fx = (px / w - 0.5) * cam.sensor_width / cam.lens
        fy = (0.5 - py / h) * cam.sensor_width / cam.lens * (h / w)
        d = (M.to_3x3() @ mathutils.Vector((fx, fy, -1.0))).normalized()
        # Blender (x, y, z) -> Minecraft (x, y=z, z=-y)
        pos = [o.x, o.z, -o.y]
        dr = [d.x, d.z, -d.y]
        t = 0.0
        found = None
        while t < 3000:
            q = [pos[i] + dr[i] * t for i in range(3)]
            x, y, z = (int(math.floor(v)) for v in q)
            cx, cz = x >> 4, z >> 4
            if reg.has_chunk(cx, cz):
                b = reg.blocks(cx, cz).reshape(-1, 16, 16)
                yi = y + 64 if 'overworld' in spec_dim[0] else y
                if 0 <= yi < b.shape[0] and spec_dim[1] <= y <= spec_dim[2]:
                    sid = int(b[yi, z & 15, x & 15])
                    if sid not in air:
                        found = (x, y, z, names[sid])
                        break
            t += 0.25
        print('PICK', part, found, flush=True)


spec_dim = ['', -10 ** 6, 10 ** 6]      # измерение, y_min, y_max среза (блоки вне среза лучом не учитываются)


def main():
    a = parse()
    spec = json.load(open(a.spec))[a.scene]
    deep_update(spec, json.loads(a.override))
    spec_dim[:] = [spec.get('dim', 'minecraft:overworld'), spec.get('y_min', -10 ** 6), spec.get('y_max', 10 ** 6)]
    t_all = time.time()
    addon_utils.enable(EXT, default_set=True, persistent=False, handle_error=lambda e: (_ for _ in ()).throw(e))
    props = importlib.import_module(EXT + '.ui.props')
    pack = os.path.join(REPO, 'run', 'pack-' + spec.get('version', '26.3'))
    assets = os.path.join(REPO, 'run', 'assets-' + spec.get('version', '26.3'))
    prefs = props.get_prefs()
    prefs.pack_override = os.environ.get('MCGEN_PACK', pack)
    prefs.assets_override = os.environ.get('MCGEN_ASSETS', assets)
    prefs.sink = 'AUTO'
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    s = bpy.context.scene.mcgen
    s.version = spec.get('version', '26.3')
    s.dimension = spec.get('dim', 'minecraft:overworld')
    s.preset = spec.get('preset', 'normal')
    s.seed_mode = 'UNIFIED'
    s.seed = str(spec['seed'])
    s.unit = 'CHUNKS'
    s.origin_x, s.origin_z = spec['cx0'], spec['cz0']
    s.size_x, s.size_z = spec['nx'], spec['nz']
    s.use_terrain = s.use_surface = s.use_caves = s.use_features = s.use_structures = True
    if 'y_min' in spec:
        s.y_min = spec['y_min']
    if 'y_max' in spec:
        s.y_max = spec['y_max']
    v = spec.get('view', {})
    s.tint_biomes = v.get('tint', True)
    s.water_style = v.get('water', 'TRANSLUCENT')
    s.chunks_per_object = str(v.get('per_object', 1))
    s.greedy_merge = v.get('merge', False)
    s.pixel_style = v.get('pixel', 'PIXEL')
    t0 = time.time()
    bpy.ops.mcgen.generate('EXEC_DEFAULT')
    t_gen = time.time() - t0
    st = s.stats
    if not st.has_data:
        print('RESULT', json.dumps({'error': st.error or 'нет данных'}))
        sys.exit(1)
    if spec.get('markers'):
        bpy.ops.mcgen.structure_markers('EXEC_DEFAULT')
    x0, x1, y0, y1, ztop = surface_stats()
    g = ground_fn()
    import numpy as np
    xs_ = list(range(spec['cx0'] * 16, (spec['cx0'] + spec['nx']) * 16, 4))
    zs_ = list(range(spec['cz0'] * 16, (spec['cz0'] + spec['nz']) * 16, 4))
    gh = np.array([[g(x, z) for x in xs_] for z in zs_])
    pk = np.unravel_index(int(np.argmax(gh)), gh.shape)
    cx0, cz0 = s.origin_chunks()
    env = ENVS[spec.get('env', 'overworld')]
    res = (960, 540) if a.preview else tuple(spec.get('res', (1920, 1080)))
    setup_world(env)
    sun = spec.get('sun', {})
    setup_lights(env, sun.get('az', 140.0), sun.get('el', 45.0), sun.get('strength'))
    auto_target = ((x0 + x1) / 2, ztop - 6, -((y0 + y1) / 2))
    setup_camera(spec.get('cam', {}), cx0, cz0, auto_target, max(x1 - x0, y1 - y0), ground_fn())
    setup_render(res, 8 if a.preview else spec.get('samples', 32), a.engine)
    ext = '.jpg'
    out = a.out or os.path.join(a.outdir, ('preview-' if a.preview else 'render-') + a.scene + ext)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    scn = bpy.context.scene
    scn.render.image_settings.file_format = 'JPEG' if out.endswith('.jpg') else 'PNG'
    if scn.render.image_settings.file_format == 'JPEG':
        scn.render.image_settings.quality = 90
    scn.render.filepath = out
    if a.pick:
        pick_pixels(a.pick, res, scn.camera)
    t_render = None
    if not a.no_render:
        t0 = time.time()
        bpy.ops.render.render(write_still=True)
        t_render = time.time() - t0
    if a.save_blend:
        bpy.ops.wm.save_as_mainfile(filepath=a.save_blend)
    info = dict(scene=a.scene, out=out, blender=bpy.app.version_string, generate_s=round(t_gen, 1), render_s=round(t_render, 1) if t_render else None,
                chunks=st.chunks, objects=st.objects, faces=st.faces, structures=st.structures_total, auto_target=[round(v, 1) for v in auto_target],
                area_blender=[round(x0), round(x1), round(y0), round(y1)], ztop=round(ztop, 1), ground=[int(gh.min()), int(np.median(gh)), int(gh.max())], peak=[xs_[pk[1]], int(gh.max()), zs_[pk[0]]], total_s=round(time.time() - t_all, 1),
                stages={e.name: round(e.seconds, 2) for e in st.stage_times})
    print('RESULT', json.dumps(info), flush=True)


main()

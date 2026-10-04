"""Сквозной сценарий пользователя ВНУТРИ Blender (запускает e2e_extension.py): установленное расширение, чистый профиль, настоящие jar.

    Prepare Resources (jar -> кэш) -> Generate 32×32 (все стадии, область не в нуле) -> Structure Markers -> строительство/разрушение (луч по мешу,
    «Поставить» / «Сломать» / undo) -> сохранить .blend -> открыть -> Load Voxels (правки вернулись) -> Clear.

Печатает строку «E2E {json}» с замерами и результатом проверок; код выхода 0 — все проверки прошли.
Окружение: MCGEN_E2E_SERVER_JAR, MCGEN_E2E_CLIENT_JAR, MCGEN_E2E_N (сторона области в чанках, по умолчанию 32), MCGEN_E2E_BLEND (куда сохранить .blend).
"""
import importlib
import json
import os
import sys
import time

import addon_utils
import bpy
import mathutils

EXT = os.environ.get('MCGEN_EXT_MODULE', 'bl_ext.user_default.mcgen')
N = int(os.environ.get('MCGEN_E2E_N', '32'))
out = {'blender': bpy.app.version_string, 'n': N, 'checks': {}}
fails = []


def mem(label):
    """Текущий и пиковый (за всё время процесса) RSS на этапе сценария, МиБ."""
    rss, peak = sysinfo.process_memory()
    out.setdefault('mem_mb', {})[label] = [round(rss / 2**20), round(peak / 2**20)]


def check(name, cond, info=None):
    out['checks'][name] = bool(cond) if info is None else {'ok': bool(cond), 'info': info}
    if not cond:
        fails.append(name)
        print('E2E-FAIL', name, info, flush=True)


addon_utils.enable(EXT, default_set=True, persistent=False, handle_error=lambda e: (_ for _ in ()).throw(e))
props = importlib.import_module(EXT + '.ui.props')
pack = importlib.import_module(EXT + '.core.pack')
jobs = importlib.import_module(EXT + '.core.jobs')
backend = importlib.import_module(EXT + '.core.backend')
sysinfo = importlib.import_module(EXT + '.core.sysinfo')
edit_ops = importlib.import_module(EXT + '.render.edit_ops')
picking = importlib.import_module(EXT + '.render.picking')
check('addon enabled', EXT in sys.modules)
out['backend'] = backend.describe_parts()[0] + ' ' + str(backend.describe_parts()[1].get('v', ''))
check('real library (not demo)', backend.name() == 'lib', out['backend'])

# 1. Prepare Resources из jar пользователя (чистый кэш)
prefs = props.get_prefs()
prefs.server_jar = os.environ['MCGEN_E2E_SERVER_JAR']
prefs.client_jar = os.environ['MCGEN_E2E_CLIENT_JAR']
t0 = time.perf_counter()
r = bpy.ops.mcgen.prepare_resources('EXEC_DEFAULT')
out['prepare_s'] = round(time.perf_counter() - t0, 1)
res = pack.resolve('26.3')
check('prepare resources', r == {'FINISHED'} and res['pack_ok'] and res['assets_ok'], res)
check('block flags (feature rules)', res['flags_ok'])
prefs.server_jar = prefs.client_jar = ''           # дальше — только кэш
mem('after prepare')

# 2. Generate N×N, область не в нуле, все стадии
s = bpy.context.scene.mcgen
s.version = '26.3'
s.dimension = 'minecraft:overworld'
s.seed_mode = 'UNIFIED'
s.seed = '12345'
s.unit = 'CHUNKS'
s.origin_x, s.origin_z = -N // 2, -N // 2
s.size_x = s.size_z = N
s.use_terrain = s.use_surface = s.use_caves = s.use_features = s.use_structures = True
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
t0 = time.perf_counter()
r = bpy.ops.mcgen.generate('EXEC_DEFAULT')
out['generate_total_s'] = round(time.perf_counter() - t0, 1)
st = s.stats
out['generate'] = dict(t_generate=round(st.t_generate, 2), t_build=round(st.t_build, 2), chunks=st.chunks, objects=st.objects, faces=st.faces,
                       voxel_mb=round(st.memory_mb), peak_rss_mb=round(st.peak_rss_mb), sink=st.sink, structures=st.structures_total,
                       stages={e.name: round(e.seconds, 2) for e in st.stage_times})
check('generate', r == {'FINISHED'} and st.has_data and st.objects == N * N and st.sink == 'render.scene', out['generate'])
mc = lambda: sorted(o.name for o in bpy.data.objects if o.name.startswith('mc_') and o.type == 'MESH')   # noqa: E731
names0 = mc()
mem('after generate')
check('one object per chunk', len(names0) == N * N, len(names0))

# 3. Structure Markers: центр bounding box постройки совпадает с положением мешей
r = bpy.ops.mcgen.structure_markers('EXEC_DEFAULT')
check('structure markers', r == {'FINISHED'} and len([o for o in bpy.data.objects if o.get('mcgen_structure')]) == st.structures_total)
e = st.structure_starts[0]
x0, y0, z0, x1, y1, z1 = e.bb
m = next(o for o in bpy.data.objects if o.get('mcgen_structure') == e.name and abs(o.location.x - (x0 + x1 + 1) / 2.0) < 1e-3)
chunk_obj = bpy.data.objects.get('mc_%d_%d' % (e.chunk_x, e.chunk_z))
check('markers share the coordinate system of the blocks',
      chunk_obj is not None and abs(chunk_obj.location.x - e.chunk_x * 16) < 1e-6 and abs(chunk_obj.location.y + e.chunk_z * 16) < 1e-6 and
      abs(m.location.y + (z0 + z1 + 1) / 2.0) < 1e-3, [tuple(m.location), tuple(chunk_obj.location) if chunk_obj else None])
out['structure_types'] = {x.name.split(':')[-1]: x.count for x in st.structure_types}

# 4. Строительство и разрушение (те же функции, что у модальных инструментов)
sb = edit_ops.STATE.sb
check('edit tools attached', sb is not None and all(hasattr(bpy.ops.mcgen, n) for n in ('edit_place', 'edit_break', 'edit_pick', 'edit_undo', 'edit_redo')))
wx, wz = 8, 8
origin = mathutils.Vector((wx + 0.5, -(wz + 0.5), 400.0))
down = mathutils.Vector((0, 0, -1))
pk = picking.pick_ray(sb, origin, down)
check('ray hits the surface', pk is not None and (pk.block[0], pk.block[2]) == (wx, wz), pk)
times = []
t0 = time.perf_counter()
r1 = edit_ops.tool_place(pk, (0.0, -1.0, 0.0), 'minecraft:glass')
times.append(('place', (time.perf_counter() - t0) * 1000, r1['mesh_ms']))
pk2 = picking.pick_ray(sb, origin, down)
check('placed block is hit by the ray', pk2 is not None and pk2.block == pk.place)
t0 = time.perf_counter()
r2 = edit_ops.tool_break(pk2)
times.append(('break', (time.perf_counter() - t0) * 1000, r2['mesh_ms']))
check('block broken', picking.pick_ray(sb, origin, down).block == pk.block)
check('undo restores the block', bool(edit_ops.tool_undo()) and picking.pick_ray(sb, origin, down).block == pk.place)
out['edit_ms'] = {k: (round(t, 1), round(mm, 1)) for k, t, mm in times}       # (всего, из них меш)

# 5. Сохранение .blend: меши и правки остаются, воксели — нет; Load Voxels
mem('after edits')
path = os.environ.get('MCGEN_E2E_BLEND', os.path.join(os.environ.get('TMPDIR', '.'), 'mcgen-e2e.blend'))
t0 = time.perf_counter()
bpy.ops.wm.save_as_mainfile(filepath=path)
out['save_s'] = round(time.perf_counter() - t0, 2)
out['blend_mb'] = round(os.path.getsize(path) / 2**20, 1)
t0 = time.perf_counter()
bpy.ops.wm.open_mainfile(filepath=path)
out['open_s'] = round(time.perf_counter() - t0, 2)
mem('after open .blend')
s = bpy.context.scene.mcgen
check('after open: meshes and settings kept, voxels not loaded',
      mc() == names0 and s.size_x == N and s.seed == '12345' and edit_ops.STATE.sb is None and jobs.session(bpy.context.scene.name).region is None)
check('edits saved in the file', edit_ops.TEXT_NAME in bpy.data.texts)
t0 = time.perf_counter()
r = bpy.ops.mcgen.load_voxels('EXEC_DEFAULT')
out['load_voxels_s'] = round(time.perf_counter() - t0, 1)
mem('after load voxels')
sb = edit_ops.STATE.sb
check('load voxels', r == {'FINISHED'} and sb is not None and mc() == names0 and (edit_ops.STATE.restored or {}).get('blocks', 0) >= 1, edit_ops.STATE.restored)
check('edit is back after reload', picking.pick_ray(sb, origin, down).block == pk.place)

# 6. Update Layers после правки настройки (смена уровня моря) + Clear
t0 = time.perf_counter()
s.use_tweaks = True
s.tweaks.sea_level_offset = 2
r = bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
out['update_layers_s'] = round(time.perf_counter() - t0, 1)
mem('after update layers')
check('update layers', r == {'FINISHED'} and mc() == names0)
r = bpy.ops.mcgen.clear('EXEC_DEFAULT')
check('clear', r == {'FINISHED'} and not mc() and edit_ops.STATE.sb is None)
mem('after clear')

rss, peak = sysinfo.process_memory()
out['rss_mb_end'], out['peak_rss_mb'] = round(rss / 2**20), round(peak / 2**20)
out['failed'] = fails
print('E2E', json.dumps(out, default=str), flush=True)
sys.exit(1 if fails else 0)

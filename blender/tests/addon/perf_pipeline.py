"""Замер полного конвейера из аддона (Blender headless): Generate всех стадий, Update Layers после смены слоя, отмена.
Запуск: run_tests.py --perf  (или blender -b --python perf_pipeline.py с окружением как в run_tests.py). Печатает строку «RESULT {json}»."""
import json
import os
import sys
import time

import addon_utils
import bpy

EXT = os.environ.get('MCGEN_EXT_MODULE', 'bl_ext.user_default.mcgen')
addon_utils.enable(EXT, default_set=True, persistent=False)
import importlib

props = importlib.import_module(EXT + '.ui.props')
jobs = importlib.import_module(EXT + '.core.jobs')
ops = importlib.import_module(EXT + '.ui.ops')
backend = importlib.import_module(EXT + '.core.backend')
props.get_prefs().sink = os.environ.get('SINK', 'AUTO')
s = bpy.context.scene.mcgen
N = int(os.environ.get('N', '32'))
s.size_x = s.size_z = N
s.seed = '12345'
s.use_terrain = s.use_surface = s.use_caves = s.use_features = s.use_structures = True
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
out = {'blender': bpy.app.version_string, 'n': N, 'backend': backend.describe().split(' (')[0], 'sink_pref': props.get_prefs().sink}
t0 = time.perf_counter()
bpy.ops.mcgen.generate('EXEC_DEFAULT')
out['generate_op_s'] = round(time.perf_counter() - t0, 2)
st = s.stats
out['full'] = dict(chunks=st.chunks, sink=st.sink, t_generate=round(st.t_generate, 2), t_build=round(st.t_build, 2), objects=st.objects, faces=st.faces,
                   voxel_mb=round(st.memory_mb), peak_rss_mb=round(st.peak_rss_mb), structures=st.structures_total,
                   stages={e.name: round(e.seconds, 2) for e in st.stage_times},
                   types=dict(list({e.name.split(':')[-1]: e.count for e in st.structure_types}.items())[:6]))
# Update Layers: выключаем Structures (воксели меняются только там, где были постройки)
s.use_structures = False
t0 = time.perf_counter()
bpy.ops.mcgen.update_layers('EXEC_DEFAULT')
out['update_layers_structures_off'] = dict(op_s=round(time.perf_counter() - t0, 2), t_generate=round(s.stats.t_generate, 2), t_build=round(s.stats.t_build, 2),
                                           rebuilt_objects=s.stats.rebuilt, objects=s.stats.objects, reasons=jobs.session(bpy.context.scene.name).stats['reasons'])
# отмена посреди генерации
params = ops.collect_params(bpy.context.scene, None)
sess = jobs.session(bpy.context.scene.name)
s.use_structures = True
params = ops.collect_params(bpy.context.scene, None)
job = jobs.GenerateJob(bpy.context.scene, params, mode='update', skey=bpy.context.scene.name, sink_pref='FALLBACK').start()
t0 = time.perf_counter()
cancel_at = None
while not job.finished:
    job.poll(0.01)
    if cancel_at is None and job.phase == 'generate' and job.fraction > 0.3:
        cancel_at = time.perf_counter()
        job.cancel()
    time.sleep(0.005)
out['cancel'] = dict(state=job.state, latency_s=round(time.perf_counter() - cancel_at, 2) if cancel_at else None)
print('RESULT', json.dumps(out, ensure_ascii=False), flush=True)

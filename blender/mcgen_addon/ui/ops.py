"""Операторы аддона. Тяжёлая работа — в рабочих потоках (core/tasks.py, core/jobs.py); оператор — modal-«насос»: по таймеру опрашивает
задачу, показывает прогресс в статус-баре, отменяет по Esc и собирает сцену порциями, чтобы интерфейс не зависал.
Вызов через EXEC_DEFAULT (скрипты, headless-тесты) выполняет то же самое синхронно."""
import os
import time

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy.types import Operator

from ..core import backend, biomes, catalog, fallback_preview, jobs, pack, paths, seeds, sysinfo
from ..core import params as P
from ..core.tasks import Task
from . import i18n, props
from .i18n import rpt

_active = []             # работающие операторы-насосы (чтобы unregister мог их остановить)
resource_state = {'message': '', 'error': '', 'java': None, 'ok': False}
_jar_cache = {}


def inspect_cached(path):
    """pack.inspect_jar с кэшем по (путь, mtime): интерфейс спрашивает о jar при каждой перерисовке."""
    if not path:
        return None
    p = bpy.path.abspath(path)
    try:
        key = (p, os.path.getmtime(p))
    except OSError:
        return None
    if key not in _jar_cache:
        _jar_cache[key] = pack.inspect_jar(p)
    return _jar_cache[key]


# ---- сборка параметров из свойств сцены ---------------------------------------------------------------------------------------

def ensure_seeds(s):
    """Пустые поля seed заменяются случайным числом (и показываются пользователю)."""
    for f in ('seed', 'seed_climate', 'seed_terrain', 'seed_structures', 'seed_features'):
        if getattr(s, f) == '':
            setattr(s, f, str(seeds.random_seed()))


def collect_params(scene, prefs=None):
    s = scene.mcgen
    ensure_seeds(s)
    if s.seed_mode == 'UNIFIED':
        sd = seeds.domains(s.seed, 'UNIFIED', None)
    else:
        sd = seeds.domains(None, 'SPLIT', (s.seed_climate, s.seed_terrain, s.seed_structures, s.seed_features))
    cx0, cz0 = s.origin_chunks()
    view = {'tint_biomes': s.tint_biomes, 'water_style': s.water_style, 'chunks_per_object': int(s.chunks_per_object), 'greedy_merge': s.greedy_merge,
            'lod_mode': s.lod_mode, 'lod_near': s.lod_near, 'pixel_style': s.pixel_style, 'y_min': s.y_min, 'y_max': s.y_max}
    return P.GenParams(
        version=s.version, dimension=s.dimension, preset=s.preset, seeds=tuple(sd),
        tweaks=s.tweaks_changed() if s.use_tweaks else (), cx0=cx0, cz0=cz0, nx=s.size_x, nz=s.size_z,
        stages=P.stage_mask(s.use_terrain, s.use_surface, s.use_caves, s.use_features, s.use_structures),
        threads=(prefs.threads if prefs else 0), view=tuple(sorted(view.items())))


def resource_overrides(prefs):
    if prefs is None:
        return '', ''
    return (bpy.path.abspath(prefs.pack_override) if prefs.pack_override else '', bpy.path.abspath(prefs.assets_override) if prefs.assets_override else '')


def tag_redraw(context):
    wm = bpy.context.window_manager
    for win in wm.windows:
        for area in win.screen.areas:
            area.tag_redraw()


# ---- базовый класс «насос» ------------------------------------------------------------------------------------------------------

class _Pump:
    """Адаптер Task/GenerateJob к единому интерфейсу насоса."""

    def __init__(self, obj):
        self.obj = obj

    @property
    def finished(self):
        return self.obj.finished

    @property
    def fraction(self):
        return self.obj.fraction

    @property
    def message(self):
        return self.obj.message

    def poll(self):
        if hasattr(self.obj, 'poll'):
            self.obj.poll()

    def cancel(self):
        self.obj.cancel()


class McGenPumpOperator(Operator):
    """Базовый класс: подкласс задаёт start(context) -> объект с finished/fraction/message/cancel() [и poll()] и finish(context, obj)."""
    bl_options = {'REGISTER'}
    _timer = None
    _pump = None

    def start(self, context):
        raise NotImplementedError

    def finish(self, context, obj):
        raise NotImplementedError

    status_title = 'MC World'

    # интерактивный запуск: modal
    def invoke(self, context, event):
        if busy():
            self.report({'WARNING'}, rpt('Another MC World job is running'))
            return {'CANCELLED'}
        obj = self.start(context)
        if obj is None:
            return {'CANCELLED'}
        self._pump = _Pump(obj)
        self._scene = context.scene.name
        wm = context.window_manager
        self._timer = wm.event_timer_add(0.05, window=context.window)
        wm.modal_handler_add(self)
        wm.progress_begin(0, 1000)
        _active.append(self)
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        if event.type == 'ESC' and event.value == 'PRESS':
            self._pump.cancel()
            self.report({'INFO'}, rpt('Cancelling …'))
            return {'RUNNING_MODAL'}
        if event.type != 'TIMER':
            return {'PASS_THROUGH'}
        p = self._pump
        p.poll()
        context.window_manager.progress_update(int(p.fraction * 1000))
        try:
            context.workspace.status_text_set(rpt('{title}: {msg}  [{pct}%]   Esc = cancel', title=rpt(self.status_title), msg=i18n.progress_text(p.message), pct=int(p.fraction * 100)))
        except AttributeError:
            pass
        if not p.finished:
            tag_redraw(context)
            return {'RUNNING_MODAL'}
        return self._end(context)

    def _end(self, context):
        wm = context.window_manager
        if self._timer is not None:
            wm.event_timer_remove(self._timer)
            self._timer = None
        wm.progress_end()
        try:
            context.workspace.status_text_set(None)
        except AttributeError:
            pass
        if self in _active:
            _active.remove(self)
        res = self.finish(context, self._pump.obj)
        tag_redraw(context)
        return res or {'FINISHED'}

    def cancel(self, context):
        if self._pump:
            self._pump.cancel()
        self._end(context)

    # синхронный запуск (скрипты, headless)
    def execute(self, context):
        if busy():
            self.report({'WARNING'}, rpt('Another MC World job is running'))
            return {'CANCELLED'}
        obj = self.start(context)
        if obj is None:
            return {'CANCELLED'}
        if hasattr(obj, 'run_blocking'):
            obj.run_blocking()
        res = self.finish(context, obj)
        return res or {'FINISHED'}


def busy():
    return any(not op._pump.finished for op in _active if getattr(op, '_pump', None))


def stop_all():
    """Останавливает все работающие задачи (unregister)."""
    for op in list(_active):
        try:
            op._pump.cancel()
        except Exception:
            pass
    _active.clear()
    for s in jobs._sessions.values():
        if s.job and not s.job.finished:
            s.job.cancel()


# ---- Generate / Update ----------------------------------------------------------------------------------------------------------

class _GenerateBase(McGenPumpOperator):
    """Общая часть Generate / Update Layers (не регистрируется: у Blender нельзя наследовать от зарегистрированного оператора)."""
    mode = 'generate'
    status_title = 'MC World generate'

    @classmethod
    def poll(cls, context):
        return context.scene is not None and hasattr(context.scene, 'mcgen')

    def start(self, context):
        scene = context.scene
        prefs = props.get_prefs(context)
        params = collect_params(scene, prefs)
        ok, need, limit = sysinfo.check_fits(params.dimension, params.nx, params.nz)
        if not ok:
            self.report({'ERROR'}, rpt('The voxel data of {n} chunks needs about {need:.1f} GB, more than 60% of the RAM ({limit:.1f} GB): choose a smaller area',
                                       n=params.nx * params.nz, need=need / 2**30, limit=limit / 2**30))
            return None
        po, ao = resource_overrides(prefs)
        job = jobs.GenerateJob(scene, params, mode=self.mode, skey=scene.name, pack_override=po, assets_override=ao,
                               sink_pref=(prefs.sink if prefs else 'AUTO'), collection_name=scene.mcgen.collection_name)
        job.start()
        jobs.session(scene.name).job = job
        if job.state == 'error':
            txt = i18n.exc_text(job.error_exc) if job.error_exc is not None else rpt(job.error_text)
            self.report({'ERROR'}, txt)
            scene.mcgen.stats.error = txt
            return None
        return job

    def finish(self, context, job):
        s = context.scene.mcgen
        if job.state == 'error':
            txt = i18n.exc_text(job.error_exc) if job.error_exc is not None else rpt(job.error_text)
            s.stats.error = txt
            self.report({'ERROR'}, txt)
            return {'CANCELLED'}
        if job.state == 'cancelled':
            self.report({'WARNING'}, rpt('Cancelled'))
            return {'CANCELLED'}
        if job.state == 'noop':
            self.report({'INFO'}, rpt(job.message))
            return {'FINISHED'}
        s.stats.fill(job.stats)
        gen = job.sess.gen
        if gen is not None:
            catalog.update_from_gen(job.params.version, gen)
        st = job.stats
        self.report({'INFO'}, rpt('{n} chunks: generated in {tg:.2f} s, scene built in {tb:.2f} s ({backend})', n=st['chunks'], tg=st['t_generate'], tb=st['t_build'], backend=st['backend']))
        return {'FINISHED'}


class MCGEN_OT_generate(_GenerateBase):
    bl_idname = 'mcgen.generate'
    bl_label = 'Generate'
    bl_description = 'Generate the world area with the current settings and build it in the scene (Esc cancels)'
    status_title = 'MC World generate'
    mode = 'generate'


class MCGEN_OT_update_layers(_GenerateBase):
    bl_idname = 'mcgen.update_layers'
    bl_label = 'Update Layers'
    bl_description = 'Recompute only what the changed settings affect and rebuild only the changed chunks'
    status_title = 'MC World update'
    mode = 'update'


class MCGEN_OT_cancel(Operator):
    bl_idname = 'mcgen.cancel'
    bl_label = 'Cancel'
    bl_description = 'Cancel the running MC World job'

    def execute(self, context):
        stop_all()
        return {'FINISHED'}


class MCGEN_OT_clear(Operator):
    bl_idname = 'mcgen.clear'
    bl_label = 'Clear'
    bl_description = 'Remove the generated objects from the scene and free the voxel data'

    def execute(self, context):
        s = context.scene.mcgen
        sess = jobs.session(context.scene.name)
        n = fallback_preview.clear_preview(context.scene, s.collection_name)
        if sess.sink is not None and not isinstance(sess.sink, fallback_preview.PreviewSink):
            try:
                sess.sink.clear(context.scene, s.collection_name)           # построитель W4 удаляет свои объекты/меши/материалы
            except Exception as e:      # noqa: BLE001
                self.report({'WARNING'}, i18n.exc_text(e))
        sess.sink = None
        sess.release()
        s.stats.has_data = False
        self.report({'INFO'}, rpt('Removed {n} objects', n=n))
        tag_redraw(context)
        return {'FINISHED'}


# ---- Biome Map -------------------------------------------------------------------------------------------------------------------

class MCGEN_OT_biome_map(McGenPumpOperator):
    bl_idname = 'mcgen.biome_map'
    bl_label = 'Biome Map'
    bl_description = 'Fast biome preview of the area (no terrain): an image, and optionally a plane in the scene'
    status_title = 'MC World biome map'

    def start(self, context):
        scene = context.scene
        prefs = props.get_prefs(context)
        params = collect_params(scene, prefs)
        po, ao = resource_overrides(prefs)
        res, problem = jobs.check_resources(params.version, po, ao)
        if problem:
            self.report({'ERROR'}, rpt(problem[0], **problem[1]))
            return None
        s = scene.mcgen
        self._step = jobs.auto_step(params, int(s.bm_step))
        self._params, self._res = params, res
        return Task('biome_map', jobs.biome_map_worker, params, res['pack'] if res['pack_ok'] else None, self._step, s.bm_y).start()

    def finish(self, context, task):
        s = context.scene.mcgen
        if task.state == 'error':
            self.report({'ERROR'}, i18n.exc_text(task.error))
            return {'CANCELLED'}
        if task.state == 'cancelled':
            self.report({'WARNING'}, rpt('Cancelled'))
            return {'CANCELLED'}
        grid, names, x0, z0, step, secs = task.result
        res = self._res
        pal = biomes.palette(names, s.bm_palette, res['pack'], res['assets'])
        rgba = biomes.colorize(grid, pal)
        img = make_biome_image(rgba, f'MCGen Biomes {self._params.dimension.split(":")[-1]}')
        if s.bm_plane:
            make_biome_plane(context, img, grid.shape[1] * step, grid.shape[0] * step, x0, z0, s.collection_name)
        self.report({'INFO'}, rpt('Biome map {w}x{h} px ({step} blocks/px) in {t:.2f} s [{backend}]', w=grid.shape[1], h=grid.shape[0], step=step, t=secs, backend=backend.name()))
        return {'FINISHED'}


def make_biome_image(rgba, name):
    """Image из RGBA (h, w, 4) float32, строки сверху вниз (север сверху): Blender хранит снизу вверх -> переворачиваем."""
    h, w = rgba.shape[:2]
    old = bpy.data.images.get(name)
    if old is not None:
        bpy.data.images.remove(old)
    img = bpy.data.images.new(name, w, h, alpha=True)
    img.colorspace_settings.name = 'sRGB'          # ДО записи пикселей: смена пространства после foreach_set обнуляет буфер
    img.pixels.foreach_set(rgba[::-1].ravel())
    img.update()
    img.pack()                                     # сохраняется внутри .blend
    img.use_fake_user = True                       # не пропадает при сохранении, даже если плоскость не создана
    return img


def make_biome_plane(context, img, size_x, size_z, x0, z0, collection_name):
    """Плоскость под картой биомов (север сверху: Blender Y = север). Материал без освещения (Emission), интерполяция Closest."""
    name = 'MC Biome Map'
    for o in [o for o in bpy.data.objects if o.get('mcgen_biome_plane')]:
        m = o.data
        bpy.data.objects.remove(o, do_unlink=True)
        if m.users == 0:
            bpy.data.meshes.remove(m)
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([(0, -size_z, 0), (size_x, -size_z, 0), (size_x, 0, 0), (0, 0, 0)], [], [(0, 1, 2, 3)])
    uv = mesh.uv_layers.new(name='UVMap')
    for i, c in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[i].uv = c
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    tex = nt.nodes.new('ShaderNodeTexImage')
    tex.image = img
    tex.interpolation = 'Closest'
    emit = nt.nodes.new('ShaderNodeEmission')
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(tex.outputs['Color'], emit.inputs['Color'])
    nt.links.new(emit.outputs['Emission'], out.inputs['Surface'])
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    obj['mcgen_biome_plane'] = True
    obj.location = (0, 0, 0)
    coll = bpy.data.collections.get(collection_name)
    if coll is None:
        coll = bpy.data.collections.new(collection_name)
        context.scene.collection.children.link(coll)
    coll.objects.link(obj)
    return obj


# ---- ресурсы ---------------------------------------------------------------------------------------------------------------------------

def _download_allowed(op):
    prefs = props.get_prefs()
    if prefs is None or not prefs.accept_eula:
        op.report({'ERROR'}, rpt('Tick "I accept the Minecraft EULA" first: downloading the jars means accepting it (https://aka.ms/MinecraftEULA)'))
        return False
    if not getattr(bpy.app, 'online_access', True):
        op.report({'ERROR'}, rpt('Online access is off: enable Preferences > System > Network > Allow Online Access (or start Blender with --online-mode)'))
        return False
    return True


class MCGEN_OT_prepare_resources(McGenPumpOperator):
    bl_idname = 'mcgen.prepare_resources'
    bl_label = 'Prepare Resources'
    bl_description = 'Unpack the datapack and client assets from your jars into the cache and obtain reports/blocks.json (needs Java 25+ or a reports folder)'
    status_title = 'MC World resources'

    def start(self, context):
        prefs = props.get_prefs(context)
        if prefs is None:
            self.report({'ERROR'}, rpt('Add-on preferences are not available'))
            return None
        sj, cj = (bpy.path.abspath(prefs.server_jar) if prefs.server_jar else ''), (bpy.path.abspath(prefs.client_jar) if prefs.client_jar else '')
        rf = bpy.path.abspath(prefs.reports_folder) if prefs.reports_folder else ''
        jv = bpy.path.abspath(prefs.java_path) if prefs.java_path else ''
        resource_state.update(message='', error='')
        return Task('prepare', lambda task: pack.prepare(sj, cj, rf, jv, task)).start()

    def finish(self, context, task):
        if task.state == 'error':
            txt = i18n.exc_text(task.error)
            resource_state.update(error=txt, ok=False)
            self.report({'ERROR'}, txt.split('\n')[0])
            return {'CANCELLED'}
        if task.state == 'cancelled':
            resource_state.update(message=rpt('Cancelled'))
            return {'CANCELLED'}
        r = task.result
        resource_state.update(message=rpt('Resources are ready for {v}', v=r.version), error='', ok=True)
        s = context.scene.mcgen
        catalog.clear()
        catalog.refresh(r.version, r.pack_dir)
        if r.version in pack.available_versions():
            try:
                s.version = r.version
            except TypeError:
                pass
        self.report({'INFO'}, rpt('Resources ready for {v} in {t:.1f} s', v=r.version, t=task.elapsed))
        return {'FINISHED'}


class MCGEN_OT_detect_jars(Operator):
    bl_idname = 'mcgen.detect_jars'
    bl_label = 'Auto-detect'
    bl_description = 'Look for Minecraft jars in .minecraft/versions, Downloads and the cache and fill the paths'

    def execute(self, context):
        prefs = props.get_prefs(context)
        found = pack.scan_jars()
        servers = {j.version: j for j in found if j.kind == 'server'}
        clients = {j.version: j for j in found if j.kind == 'client'}
        both = sorted(set(servers) & set(clients), key=pack.version_key)
        want = context.scene.mcgen.version
        pick = want if want in both else (both[-1] if both else None)
        if pick:
            prefs.server_jar, prefs.client_jar = servers[pick].path, clients[pick].path
        else:
            if clients:
                prefs.client_jar = clients[sorted(clients, key=pack.version_key)[-1]].path
            if servers:
                prefs.server_jar = servers[sorted(servers, key=pack.version_key)[-1]].path
        resource_state['java'] = None
        msg = rpt('Found {s} server and {c} client jars', s=len(servers), c=len(clients)) + (rpt('; selected {v}', v=pick) if pick else '')
        self.report({'INFO'} if found else {'WARNING'}, msg if found else rpt('No Minecraft jars found: use Download or pick the files manually'))
        return {'FINISHED'}


class MCGEN_OT_check_java(Operator):
    bl_idname = 'mcgen.check_java'
    bl_label = 'Check Java'
    bl_description = 'Look for a suitable Java (25+) for the game data generator'

    def execute(self, context):
        prefs = props.get_prefs(context)
        jv = bpy.path.abspath(prefs.java_path) if prefs and prefs.java_path else None
        found = pack.find_java(25, jv)
        resource_state['java'] = rpt('Java {v}: {path}', v=found[1], path=found[0]) if found else rpt('No Java 25+ found')
        self.report({'INFO'} if found else {'WARNING'}, resource_state['java'])
        return {'FINISHED'}


class MCGEN_OT_refresh_versions(McGenPumpOperator):
    bl_idname = 'mcgen.refresh_versions'
    bl_label = 'Refresh list'
    bl_description = 'Fetch the list of 26.x versions from Mojang (piston-meta.mojang.com)'
    status_title = 'MC World versions'

    def start(self, context):
        if not getattr(bpy.app, 'online_access', True):
            self.report({'ERROR'}, rpt('Online access is off: enable Preferences > System > Network > Allow Online Access'))
            return None
        return Task('manifest', lambda task: pack.manifest_versions(pack.fetch_manifest())).start()

    def finish(self, context, task):
        if task.state != 'done':
            self.report({'ERROR'}, i18n.exc_text(task.error) if task.error is not None else task.state)
            return {'CANCELLED'}
        props.set_manifest_versions(task.result)
        self.report({'INFO'}, rpt('{n} versions', n=len(task.result)))
        return {'FINISHED'}


class MCGEN_OT_download_jars(McGenPumpOperator):
    bl_idname = 'mcgen.download_jars'
    bl_label = 'Download'
    bl_description = 'Download the official server and client jars of the selected version from Mojang (requires accepting the Minecraft EULA)'
    status_title = 'MC World download'

    def start(self, context):
        if not _download_allowed(self):
            return None
        prefs = props.get_prefs(context)
        v = prefs.download_version
        self._version = v
        return Task('download', lambda task: pack.download_jars(v, True, task=task)).start()

    def finish(self, context, task):
        if task.state != 'done':
            self.report({'ERROR'}, (i18n.exc_text(task.error) if task.error is not None else task.state).split('\n')[0])
            return {'CANCELLED'}
        prefs = props.get_prefs(context)
        prefs.server_jar, prefs.client_jar = task.result['server'], task.result['client']
        self.report({'INFO'}, rpt('Downloaded {v}; press Prepare Resources', v=self._version))
        return {'FINISHED'}


class MCGEN_OT_open_cache(Operator):
    bl_idname = 'mcgen.open_cache'
    bl_label = 'Open Cache Folder'
    bl_description = 'Show the add-on cache folder in the file browser'

    def execute(self, context):
        bpy.ops.wm.path_open(filepath=paths.cache_dir())
        return {'FINISHED'}


class MCGEN_OT_clear_cache(Operator):
    bl_idname = 'mcgen.clear_cache'
    bl_label = 'Clear Cache'
    bl_description = 'Delete the prepared resources (they can be prepared again from the jars)'

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        pack.clear_cache('all')
        catalog.clear()
        backend.release_all()
        resource_state.update(message=rpt('Cache cleared'), error='', ok=False)
        self.report({'INFO'}, rpt('Cache cleared'))
        return {'FINISHED'}


# ---- мелкие операторы ---------------------------------------------------------------------------------------------------------------

class MCGEN_OT_random_seed(Operator):
    bl_idname = 'mcgen.random_seed'
    bl_label = 'Random'
    bl_description = 'Pick a random seed'
    target: StringProperty(default='seed', options={'HIDDEN', 'SKIP_SAVE'})

    def execute(self, context):
        s = context.scene.mcgen
        if self.target in ('seed', 'seed_climate', 'seed_terrain', 'seed_structures', 'seed_features'):
            setattr(s, self.target, str(seeds.random_seed()))
            return {'FINISHED'}
        return {'CANCELLED'}


class MCGEN_OT_reset_tweaks(Operator):
    bl_idname = 'mcgen.reset_tweaks'
    bl_label = 'Reset to Vanilla'
    bl_description = 'Set every world tweak back to its vanilla value'

    def execute(self, context):
        tw = context.scene.mcgen.tweaks
        for t in P.tweaks_doc()['tweaks']:
            tw.property_unset(t['id'])
        return {'FINISHED'}


classes = (MCGEN_OT_generate, MCGEN_OT_update_layers, MCGEN_OT_cancel, MCGEN_OT_clear, MCGEN_OT_biome_map, MCGEN_OT_prepare_resources,
           MCGEN_OT_detect_jars, MCGEN_OT_check_java, MCGEN_OT_refresh_versions, MCGEN_OT_download_jars, MCGEN_OT_open_cache,
           MCGEN_OT_clear_cache, MCGEN_OT_random_seed, MCGEN_OT_reset_tweaks)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    stop_all()
    for c in reversed(classes):
        try:
            bpy.utils.unregister_class(c)
        except RuntimeError:
            pass

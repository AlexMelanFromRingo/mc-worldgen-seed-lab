"""Панели интерфейса: «MC World» во вкладке Scene редактора свойств и копия в боковой панели N (вкладка «MC World»).

Вкладки редактора свойств Python добавлять не позволяет, поэтому «вкладка» — верхняя панель Scene > MC World с подпанелями
(как у Render properties). Подпанели: Version & World · Seeds · Area · Layers · World Tweaks (+ группы из tweaks.json) · View ·
Biome Preview · Resources · Stats. Функции draw_* принимают layout и не зависят от места размещения — поэтому один код рисует обе копии.
"""
import bpy
from bpy.app.translations import pgettext_iface as iface_
from bpy.types import Panel
from bl_ui.utils import PresetPanel

from ..core import backend, catalog, gpu, gpu_build, jobs, pack, paths, seeds, sysinfo
from ..core import params as P
from . import i18n, ops, props

_dim_short = {'minecraft:overworld': 'overworld', 'minecraft:the_nether': 'nether', 'minecraft:the_end': 'end'}


def _s(context):
    return context.scene.mcgen


def _fmt_bytes(n):
    for unit in ('B', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return f'{n:.0f} {unit}' if unit == 'B' else f'{n:.1f} {unit}'
        n /= 1024.0


# ---- главная панель: кнопки и прогресс ---------------------------------------------------------------------------------------------

def draw_main(layout, context):
    s = _s(context)
    layout.use_property_split = False
    sess = jobs.session(context.scene.name)
    running = ops.busy()
    col = layout.column(align=True)
    row = col.row(align=True)
    row.scale_y = 1.5
    row.enabled = not running
    row.operator('mcgen.generate', icon='PLAY')
    row.operator('mcgen.update_layers', icon='FILE_REFRESH')
    row2 = col.row(align=True)
    row2.enabled = not running
    row2.operator('mcgen.biome_map', icon='IMAGE_DATA')
    row2.operator('mcgen.clear', icon='TRASH')
    if running:
        pump = next((op._pump for op in ops._active if getattr(op, '_pump', None) and not op._pump.finished), None)
        if pump is not None:
            row = layout.row(align=True)
            f = max(0.0, min(1.0, pump.fraction))
            row.progress(factor=f, text=f'{i18n.progress_text(pump.message) if pump.message else iface_("Working …")}  {int(f * 100)}%', type='BAR')
            row.operator('mcgen.cancel', text='', icon='CANCEL')
    st = s.stats
    if st.error:
        box = layout.box()
        box.alert = True
        for line in st.error.split('\n')[:4]:
            box.label(text=line[:90], icon='ERROR')
    elif sess.region is None and not running and s.stats.has_data:
        box = layout.box()
        box.label(text=iface_('The voxel data is not loaded (it is not stored in the .blend file)'), icon='INFO')
        box.operator('mcgen.load_voxels', icon='FILE_REFRESH')
    elif sess.region is None and not running:
        layout.label(text=iface_('Set the area and press Generate'), icon='INFO')
    if backend.name() == 'mock':
        box = layout.box()
        box.label(text=iface_('Demo generator: not real Minecraft terrain'), icon='EXPERIMENTAL')


# ---- подпанели ---------------------------------------------------------------------------------------------------------------------

def draw_world(layout, context):
    s = _s(context)
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.prop(s, 'version')
    col.prop(s, 'dimension')
    col.prop(s, 'preset')
    res = pack.resolve(s.version, *ops.resource_overrides(props.get_prefs(context)))
    if not res['pack_ok']:
        layout.label(text=iface_('Resources not prepared: see the Resources panel'), icon='ERROR')


def draw_seeds(layout, context):
    s = _s(context)
    layout.use_property_split = True
    layout.use_property_decorate = False
    layout.prop(s, 'seed_mode', expand=True)

    def field(prop, label=None):
        row = layout.row(align=True)
        row.prop(s, prop, text=label) if label else row.prop(s, prop)
        op = row.operator('mcgen.random_seed', text='', icon='FILE_REFRESH')
        op.target = prop
        v, kind = seeds.describe(getattr(s, prop))
        if kind == 'string':
            layout.label(text=iface_('Text seed = {n}').format(n=v), icon='INFO')

    if s.seed_mode == 'UNIFIED':
        field('seed')
    else:
        for p in ('seed_climate', 'seed_terrain', 'seed_structures', 'seed_features'):
            field(p)


def draw_area(layout, context):
    s = _s(context)
    layout.use_property_split = True
    layout.use_property_decorate = False
    layout.prop(s, 'unit', expand=True)
    col = layout.column(align=True)
    col.prop(s, 'origin_x', text=iface_('Origin X'))
    col.prop(s, 'origin_z', text=iface_('Origin Z'))
    col = layout.column(align=True)
    col.prop(s, 'size_x')
    col.prop(s, 'size_z')
    col = layout.column(align=True)
    col.prop(s, 'y_min')
    col.prop(s, 'y_max')
    cx0, cz0 = s.origin_chunks()
    n = s.size_x * s.size_z
    box = layout.box()
    box.label(text=iface_('{w} × {d} blocks, {n} chunks').format(w=s.size_x * 16, d=s.size_z * 16, n=n), icon='MESH_GRID')
    box.label(text=iface_('Chunks {a},{b} to {c},{d}').format(a=cx0, b=cz0, c=cx0 + s.size_x - 1, d=cz0 + s.size_z - 1))
    need = sysinfo.estimate_bytes(s.dimension, s.size_x, s.size_z)
    box.label(text=iface_('Voxel data: about {m}').format(m=_fmt_bytes(need)), icon='INFO')
    if n > 1024:
        box.label(text=iface_('Large area: generation and the scene will take long'), icon='ERROR')


def draw_layers(layout, context):
    s = _s(context)
    layout.use_property_split = False
    col = layout.column(align=True)
    col.prop(s, 'use_terrain')
    sub = col.column(align=True)
    sub.active = s.use_terrain
    sub.prop(s, 'use_surface')
    sub.prop(s, 'use_caves')
    sub.prop(s, 'use_features')
    if s.use_features:
        sub.prop(s, 'schedule_file')
    sub.prop(s, 'use_structures')
    if not s.use_terrain:
        layout.label(text=iface_('Without Terrain only biomes are computed'), icon='INFO')


def draw_tweaks_body(layout, context):
    s = _s(context)
    layout.active = s.use_tweaks
    if not s.use_tweaks:
        layout.label(text=iface_('Off: the world is generated exactly like the game'), icon='INFO')
    layout.operator('mcgen.reset_tweaks', icon='RECOVER_LAST')


def make_tweak_group_draw(group_id):
    def draw(self, context):
        s = _s(context)
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.active = s.use_tweaks
        dim = _dim_short.get(s.dimension, 'overworld')
        for t in P.tweaks_doc()['tweaks']:
            if t['group'] != group_id:
                continue
            row = layout.row()
            row.enabled = dim in t['dimensions']
            row.prop(s.tweaks, t['id'])
    return draw


def draw_view(layout, context):
    s = _s(context)
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.prop(s, 'tint_biomes')
    col.prop(s, 'water_style')
    col.prop(s, 'pixel_style', expand=True)
    col = layout.column()
    col.prop(s, 'chunks_per_object')
    col.prop(s, 'greedy_merge')
    col = layout.column()
    col.prop(s, 'lod_mode')
    sub = col.column()
    sub.active = s.lod_mode != 'OFF'
    sub.prop(s, 'lod_near')
    col = layout.column()
    col.prop(s, 'compute')
    layout.prop(s, 'auto_update')
    layout.prop(s, 'collection_name')


def draw_biome_preview(layout, context):
    s = _s(context)
    layout.use_property_split = True
    layout.use_property_decorate = False
    col = layout.column()
    col.prop(s, 'bm_step')
    col.prop(s, 'bm_y')
    col.prop(s, 'bm_palette')
    col.prop(s, 'bm_plane')
    layout.operator('mcgen.biome_map', icon='IMAGE_DATA')


def draw_resources(layout, context, prefs=None, in_prefs=False):
    prefs = prefs or props.get_prefs(context)
    layout.use_property_split = True
    layout.use_property_decorate = False
    if prefs is None:
        layout.label(text=iface_('Enable the add-on to edit its preferences'), icon='ERROR')
        return
    s = context.scene.mcgen if (context.scene and hasattr(context.scene, 'mcgen')) else None
    box = layout.box()
    box.label(text=iface_('Minecraft jars (your own copy, not shipped)'), icon='FILE_FOLDER')
    box.prop(prefs, 'server_jar')
    box.prop(prefs, 'client_jar')
    row = box.row(align=True)
    row.operator('mcgen.detect_jars', icon='VIEWZOOM')
    for key, label in (('server_jar', 'Server'), ('client_jar', 'Client')):
        info = ops.inspect_cached(getattr(prefs, key))
        if getattr(prefs, key):
            box.label(text=(iface_('{kind}: version {v}').format(kind=iface_(label), v=info.version) if info and info.kind == label.lower()
                            else iface_('{kind}: not a valid jar').format(kind=iface_(label))),
                      icon='CHECKMARK' if info and info.kind == label.lower() else 'ERROR')
    dl = layout.box()
    dl.label(text=iface_('Download official jars from Mojang'), icon='URL')
    row = dl.row(align=True)
    row.prop(prefs, 'download_version', text='')
    row.operator('mcgen.refresh_versions', text='', icon='FILE_REFRESH')
    dl.prop(prefs, 'accept_eula')
    sub = dl.row()
    sub.enabled = prefs.accept_eula
    sub.operator('mcgen.download_jars', icon='IMPORT')
    if not getattr(bpy.app, 'online_access', True):
        dl.label(text=iface_('Online access is off (Preferences > System > Network)'), icon='ERROR')
    box = layout.box()
    box.label(text=iface_('Game data'), icon='SETTINGS')
    box.prop(prefs, 'java_path')
    box.prop(prefs, 'reports_folder')
    row = box.row(align=True)
    row.operator('mcgen.prepare_resources', icon='PLAY')
    row.operator('mcgen.check_java', icon='QUESTION')
    st = ops.resource_state
    if st['java']:
        box.label(text=st['java'], icon='INFO')
    if st['error']:
        eb = box.box()
        eb.alert = True
        for line in st['error'].split('\n')[:7]:
            eb.label(text=line[:100])
    elif st['message']:
        box.label(text=st['message'][:100], icon='CHECKMARK')
    if s is not None:
        res = pack.resolve(s.version, *ops.resource_overrides(prefs))
        for flag, label in ((res['pack_ok'], 'Datapack and reports'), (res['flags_ok'], 'Block flags (feature rules)'), (res['assets_ok'], 'Textures and models')):
            box.label(text=f'{iface_(label)}: ' + (iface_('ready') if flag else iface_('missing')), icon='CHECKMARK' if flag else 'X')
    col = layout.column(heading=iface_('Advanced'))
    col.prop(prefs, 'backend')
    col.prop(prefs, 'threads')
    col.prop(prefs, 'sink')
    col.prop(prefs, 'cache_dir')
    col.prop(prefs, 'pack_override')
    col.prop(prefs, 'assets_override')
    row = layout.row(align=True)
    row.operator('mcgen.open_cache', icon='FILEBROWSER')
    row.operator('mcgen.clear_cache', icon='TRASH')
    layout.label(text=iface_('Cache: {size} in {path}').format(size=(_fmt_bytes(sz) if (sz := pack.cache_size_cached()) is not None else '…'), path=paths.cache_dir(False)))
    tpl, kw = backend.describe_parts()
    layout.label(text=iface_('Generator: {d}').format(d=iface_(tpl).format(**kw))[:110], icon='SYSTEM')


def draw_gpu(layout, context):
    """Блок «Compute»: режим, устройство, самопроверка, причины отказа, замеры."""
    st = gpu.status()
    box = layout.box()
    row = box.row()
    row.label(text=iface_('Compute: {m}').format(m=st.get('mode') or iface_('CPU')), icon='MEMORY' if st.get('ready') else 'SYSTEM')
    row.operator('mcgen.gpu_selftest', text='', icon='CHECKMARK')
    row.operator('mcgen.gpu_benchmark', text='', icon='TIME')
    if st.get('ready'):
        box.label(text=st.get('device', '')[:90], icon='OUTLINER_DATA_LIGHTPROBE')
        res = gpu.last_selftest()
        if res:
            ok, txt = list(res.values())[-1]
            box.label(text=(iface_('Self-test passed (GPU = CPU)') if ok else iface_('Self-test FAILED: the CPU is used')), icon='CHECKMARK' if ok else 'ERROR')
        if st.get('last_fallback') not in (None, '-', ''):
            box.label(text=iface_('Fallback to CPU: {r}').format(r=st['last_fallback'])[:110], icon='INFO')
    else:
        reason = st.get('reason') or st.get('state', '')
        box.label(text=iface_('GPU is not used: {r}').format(r=reason)[:110], icon='INFO')
        if gpu_build.sources_dir() and gpu.supported():
            box.operator('mcgen.build_gpu', icon='TOOL_SETTINGS')
    b = gpu.last_benchmark()
    if b:
        col = box.column(align=True)
        if b.get('biome_gpu'):
            col.label(text=iface_('Biome map {n}x{n}: CPU {c:.2f} s, GPU {g:.3f} s ({x:.0f}x)').format(n=b['biome_n'], c=b['biome_cpu'], g=b['biome_gpu'], x=b.get('biome_speedup', 0)))
        if b.get('terrain_gpu'):
            col.label(text=iface_('Terrain {n}x{n} chunks: CPU {c:.2f} s, GPU {g:.2f} s ({x:.1f}x)').format(n=b['terrain_n'], c=b['terrain_cpu'], g=b['terrain_gpu'], x=b.get('terrain_speedup', 0)))
        if b.get('biome_identical') is False or b.get('terrain_identical') is False:
            col.alert = True
            col.label(text=iface_('GPU and CPU results differ!'), icon='ERROR')


def draw_stats(layout, context):
    s = _s(context)
    st = s.stats
    layout.use_property_split = False
    draw_gpu(layout, context)
    if not st.has_data:
        layout.label(text=iface_('No statistics yet'), icon='INFO')
        return
    col = layout.column(align=True)
    col.label(text=iface_('Generator: {n}').format(n=st.backend))
    col.label(text=iface_('Generation: {t:.2f} s').format(t=st.t_generate))
    for e in st.stage_times:
        col.label(text='    ' + iface_('{name}: {t:.2f} s').format(name=iface_(e.name), t=e.seconds))
    col.label(text=iface_('Scene build: {t:.2f} s ({n})').format(t=st.t_build, n=st.sink))
    col.label(text=iface_('Total: {t:.2f} s').format(t=st.t_total))
    col = layout.column(align=True)
    col.label(text=iface_('Chunks: {n}').format(n=st.chunks))
    col.label(text=iface_('Objects: {n}').format(n=st.objects))
    if st.mode == 'update':
        col.label(text=iface_('Rebuilt objects: {n}').format(n=st.rebuilt))
    col.label(text=iface_('Faces: {n:,}').format(n=st.faces))
    col.label(text=iface_('Voxel memory: {m:.0f} MB').format(m=st.memory_mb))
    if st.peak_rss_mb:
        col.label(text=iface_('Peak process memory: {m:.0f} MB').format(m=st.peak_rss_mb))
    if st.structures_total or len(st.structure_types):
        box = layout.box()
        box.label(text=iface_('Structures: {n}').format(n=st.structures_total), icon='HOME')
        for e in list(st.structure_types)[:12]:
            box.label(text=f'{e.name.split(":", 1)[-1]}: {e.count}')
        if len(st.structure_types) > 12:
            box.label(text=iface_('… and {n} more types').format(n=len(st.structure_types) - 12))
        first = list(st.structure_starts)[:3]
        for e in first:
            b = e.bb
            box.label(text=f'{e.name.split(":", 1)[-1]}  x {b[0]}..{b[3]}  y {b[1]}..{b[4]}  z {b[2]}..{b[5]}')
        box.operator('mcgen.structure_markers', icon='EMPTY_AXIS')


def draw_edit(layout, context):
    """Строительство и разрушение: кнопки модальных инструментов (работают в 3D-виде, поэтому подпанель есть только в боковой панели N)."""
    from ..render import edit_ops
    s = _s(context)
    layout.use_property_split = False
    ready = edit_ops.STATE.sb is not None
    col = layout.column(align=True)
    col.enabled = ready
    col.prop(s, 'edit_block', text='')
    col.operator('mcgen.edit_place', icon='ADD').block = s.edit_block
    col.operator('mcgen.edit_break', icon='REMOVE')
    col.operator('mcgen.edit_pick', icon='EYEDROPPER')
    row = col.row(align=True)
    row.operator('mcgen.edit_undo', icon='LOOP_BACK')
    row.operator('mcgen.edit_redo', icon='LOOP_FORWARDS')
    if not ready:
        layout.label(text=iface_('Generate the world first'), icon='INFO')
        return
    box = layout.box()
    col = box.column(align=True)
    col.label(text=iface_('LMB: apply'))
    col.label(text=iface_('RMB / Esc: leave the tool'))
    col.label(text=iface_('Ctrl+Z: undo'))
    col.label(text=iface_('Ctrl+Shift+Z: redo'))
    last = edit_ops.STATE.last
    if last and 'edit_ms' in last:
        col.label(text=iface_('{op}: {n} blocks, mesh {ms:.1f} ms').format(op=last['op'], n=last['blocks'], ms=last['mesh_ms']))
    rs = edit_ops.STATE.restored
    if rs:
        col.label(text=iface_('Restored edits: {n} blocks').format(n=rs['blocks']))
    if edit_ops.TEXT_NAME in bpy.data.texts:
        layout.operator('mcgen.edit_reset', icon='TRASH')


# ---- фабрика панелей ------------------------------------------------------------------------------------------------------------------

class MCGEN_PT_presets(PresetPanel, Panel):
    bl_label = 'MC World Presets'
    preset_subdir = 'mcgen'
    preset_operator = 'script.execute_preset'
    preset_add_operator = 'mcgen.preset_add'


class MCGEN_PT_presets_view3d(PresetPanel, Panel):
    bl_label = 'MC World Presets'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'           # всплывающее окно пресетов, а не вкладка боковой панели
    preset_subdir = 'mcgen'
    preset_operator = 'script.execute_preset'
    preset_add_operator = 'mcgen.preset_add'


# (ключ, подпись, функция, закрыта по умолчанию, функция заголовка)
SUBPANELS = [
    ('world', 'Version & World', draw_world, False, None),
    ('seeds', 'Seeds', draw_seeds, False, None),
    ('area', 'Area', draw_area, False, None),
    ('layers', 'Layers', draw_layers, False, None),
    ('tweaks', 'World Tweaks', draw_tweaks_body, True, 'use_tweaks'),
    ('view', 'View', draw_view, True, None),
    ('edit', 'Edit Blocks', draw_edit, True, None),
    ('biomes', 'Biome Preview', draw_biome_preview, True, None),
    ('resources', 'Resources', draw_resources, True, None),
    ('stats', 'Stats', draw_stats, True, None),
]


VIEW3D_ONLY = {'edit'}        # подпанели, нужные только в 3D-виде (инструменты редактирования — модальные операторы 3D-вида)


def _poll(cls, context):
    return context.scene is not None and hasattr(context.scene, 'mcgen')


def _wrap_draw(fn):
    def draw(self, context):
        fn(self.layout, context)
    return draw


def _make_header(prop_name):
    def draw_header(self, context):
        self.layout.prop(_s(context), prop_name, text='')
    return draw_header


def make_panels(prefix, space, region, extra, preset_panel):
    """Создаёт набор классов панелей для одного места размещения. prefix: «MCGEN_PT» или «MCGEN_PT_n»; extra: bl_context / bl_category."""
    out = []
    main_id = f'{prefix}_main'

    def main_header_preset(self, context):
        preset_panel.draw_panel_header(self.layout)

    main = type(f'{prefix}_main', (Panel,), {
        'bl_idname': main_id, 'bl_label': 'MC World', 'bl_space_type': space, 'bl_region_type': region, 'bl_order': 10,
        'draw': _wrap_draw(draw_main), 'draw_header_preset': main_header_preset, 'poll': classmethod(_poll), **extra})
    out.append(main)
    for i, (key, label, fn, closed, hdr) in enumerate(SUBPANELS):
        if key in VIEW3D_ONLY and space != 'VIEW_3D':
            continue
        attrs = {'bl_idname': f'{prefix}_{key}', 'bl_label': label, 'bl_space_type': space, 'bl_region_type': region, 'bl_parent_id': main_id,
                 'bl_order': i, 'poll': classmethod(_poll), 'draw': _wrap_draw(fn), **extra}
        if closed:
            attrs['bl_options'] = {'DEFAULT_CLOSED'}
        if hdr:
            attrs['draw_header'] = _make_header(hdr)
        out.append(type(f'{prefix}_{key}', (Panel,), attrs))
        if key == 'tweaks':
            for gi, (gid, glabel, items) in enumerate(props.tweak_groups()):
                gattrs = {'bl_idname': f'{prefix}_tweaks_{gid}', 'bl_label': glabel, 'bl_space_type': space, 'bl_region_type': region,
                          'bl_parent_id': f'{prefix}_tweaks', 'bl_order': gi, 'poll': classmethod(_poll), 'draw': make_tweak_group_draw(gid), **extra}
                out.append(type(f'{prefix}_tweaks_{gid}', (Panel,), gattrs))
    return out


def all_panels():
    # (Вкладка Scene редактора свойств, боковая панель 3D-вида)
    classes = [MCGEN_PT_presets, MCGEN_PT_presets_view3d]
    classes += make_panels('MCGEN_PT', 'PROPERTIES', 'WINDOW', {'bl_context': 'scene'}, MCGEN_PT_presets)
    classes += make_panels('MCGEN_PT_n', 'VIEW_3D', 'UI', {'bl_category': 'MC World'}, MCGEN_PT_presets_view3d)
    return classes


_registered = []


def register():
    global _registered
    _registered = all_panels()
    for c in _registered:
        bpy.utils.register_class(c)


def unregister():
    global _registered
    for c in reversed(_registered):
        try:
            bpy.utils.unregister_class(c)
        except RuntimeError:
            pass
    _registered = []

"""Свойства аддона: настройки сцены (Scene.mcgen), динамические «тонкие настройки» из tweaks.json, статистика, настройки аддона."""
import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, IntProperty, IntVectorProperty, PointerProperty,
                       StringProperty)
from bpy.types import AddonPreferences, PropertyGroup

from ..core import backend, catalog, pack, paths, seeds
from ..core import params as P

ROOT = __package__.rsplit('.', 1)[0]          # имя пакета: «bl_ext.<репозиторий>.mcgen» или «mcgen_addon»

_enum_keep = {}                                # строки динамических списков должны жить, пока жив список (особенность bpy)


def _keep(key, items):
    _enum_keep[key] = items
    return items


# ---- динамические списки --------------------------------------------------------------------------------------------------

def _version_items(self, context):
    ready = {e['version'] for e in pack.list_packs() if e['has_reports']}
    items = []
    for v in catalog.versions():
        desc = 'Resources prepared' if v in ready else 'Resources not prepared yet (open the Resources panel)'
        items.append((v, v, desc))
    return _keep('version', items)


def _dimension_items(self, context):
    items = [(d, catalog.pretty_dim(d), d) for d in catalog.dimensions(self.version)]
    return _keep('dimension', items)


def _preset_items(self, context):
    items = [(p, catalog.pretty_preset(p), p) for p in catalog.presets(self.version, self.dimension)]
    return _keep('preset', items)


def _download_version_items(self, context):
    vs = _manifest_versions or list(pack.SUPPORTED_VERSIONS)
    return _keep('dlver', [(v, v, '') for v in vs])


_manifest_versions = []


def set_manifest_versions(vs):
    global _manifest_versions
    _manifest_versions = list(vs)


# ---- обратные вызовы ------------------------------------------------------------------------------------------------------

def _fix_enums(self, context):
    dims = [d for d in catalog.dimensions(self.version)]
    if self.dimension not in dims:
        self.dimension = dims[0]
    pres = catalog.presets(self.version, self.dimension)
    if self.preset not in pres:
        self.preset = pres[0]


def _on_world_changed(self, context):
    if not catalog.has(self.version):                       # списки измерений/пресетов берём из библиотеки (один раз на версию)
        res = pack.resolve(self.version, *_overrides())
        if res['pack_ok'] or backend.name() == 'mock':
            catalog.refresh(self.version, res['pack'] if res['pack_ok'] else None)
    _fix_enums(self, context)
    _auto_update(self, context)


def _overrides():
    pr = get_prefs()
    if pr is None:
        return '', ''
    return (bpy.path.abspath(pr.pack_override) if pr.pack_override else '', bpy.path.abspath(pr.assets_override) if pr.assets_override else '')


def _on_seed_mode(self, context):
    if self.seed_mode == 'SPLIT':        # при переходе в раздельный режим все поля получают текущий единый seed
        self.seed_climate = self.seed_terrain = self.seed_structures = self.seed_features = self.seed
    else:
        self.seed = self.seed_climate
    _auto_update(self, context)


def _on_unit(self, context):
    """Смена единиц переводит ВСЮ область (начало и размер) блоки <-> чанки, чтобы она осталась на месте."""
    prev = self._prev_unit()
    if self.unit == prev:
        return
    x0, z0, x1, z1 = self._box_for(prev)                     # область в блоках по прежним единицам
    self['_unit_state'] = self.unit
    if self.unit == 'CHUNKS':                                 # в чанках: ближайшая сетка чанков, накрывающая область
        cx0, cz0 = x0 // 16, z0 // 16
        nx, nz = (x1 - 1) // 16 - cx0 + 1, (z1 - 1) // 16 - cz0 + 1
        self.origin_x, self.origin_z = cx0, cz0
        self.size_x, self.size_z = max(1, min(512, nx)), max(1, min(512, nz))      # размер в чанках сбрасывает точный размер в блоках
    else:
        self.origin_x, self.origin_z = x0, z0


def _on_size_chunks(self, context):
    """Размер задан в чанках (режим Chunks, скрипты, пресеты): точный размер в блоках сбрасывается — область снова кратна чанкам."""
    for k in ('_wb', '_wd'):
        if k in self:
            del self[k]
    _auto_update(self, context)


def _get_width(self):
    v = self.get('_wb')
    return int(v) if v else self.size_x * 16


def _set_width(self, v):
    if '_wd' not in self:                                     # вторая сторона остаётся как была
        self['_wd'] = self.size_z * 16
    self['_wb'] = max(1, int(v))


def _get_depth(self):
    v = self.get('_wd')
    return int(v) if v else self.size_z * 16


def _set_depth(self, v):
    if '_wb' not in self:
        self['_wb'] = self.size_x * 16
    self['_wd'] = max(1, int(v))


def _prev_unit_impl(self):
    return self.get('_unit_state', 'BLOCKS')


_auto_pending = {'t': False}


def _compute_items(self, context):
    from ..core import gpu
    lab = gpu.device_label()
    return _keep('compute', [
        ('AUTO', 'Auto', 'Use the GPU where it pays off and has passed the GPU = CPU self-check: biome maps, and terrain for areas of 256+ chunks (16 x 16); smaller areas are faster on the CPU'),
        ('CPU', 'CPU', 'Always compute on the CPU'),
        ('GPU', f'GPU ({lab})' if lab else 'GPU (not available)', 'Compute everything the GPU can (biome maps and terrain); the result is bit-identical to the CPU'),
    ])


def _on_compute(self, context):
    from ..core import gpu
    gpu.set_mode(self.compute)


def _auto_update(self, context):
    """Включённое «Auto update»: после паузы в 0.6 с запускает Update Layers (дебаунс таймером)."""
    scene = getattr(self, 'id_data', None)
    if scene is None or not getattr(scene.mcgen, 'auto_update', False) or bpy.app.background:
        return
    if _auto_pending['t']:
        return
    _auto_pending['t'] = True

    def fire():
        _auto_pending['t'] = False
        try:
            from ..core import jobs
            s = jobs.session(scene.name)
            if s.region is None or (s.job and not s.job.finished):
                return None
            bpy.ops.mcgen.update_layers('INVOKE_DEFAULT')
        except Exception:       # noqa: BLE001 - аддон могли выключить за время паузы; сцена удалена и т. п.
            pass
        return None
    bpy.app.timers.register(fire, first_interval=0.6)


# ---- «тонкие настройки» (строятся из libmcgen/tweaks.json) --------------------------------------------------------------------

def build_tweaks_group():
    """Создаёт в рантайме класс PropertyGroup с одним свойством на каждую настройку из tweaks.json."""
    ann = {}
    for t in P.tweaks_doc()['tweaks']:
        common = dict(name=t['label'], description=t['description'])
        if t['type'] == 'float':
            ann[t['id']] = FloatProperty(default=float(t['default']), min=float(t['min']), max=float(t['max']), soft_min=float(t['soft_min']),
                                         soft_max=float(t['soft_max']), precision=2, step=5, update=_auto_update, **common)
        elif t['type'] == 'int':
            ann[t['id']] = IntProperty(default=int(t['default']), min=int(t['min']), max=int(t['max']), soft_min=int(t['soft_min']),
                                       soft_max=int(t['soft_max']), update=_auto_update, **common)
        else:
            ann[t['id']] = BoolProperty(default=bool(t['default']), update=_auto_update, **common)
    return type('McGenTweaks', (PropertyGroup,), {'__annotations__': ann})


McGenTweaks = build_tweaks_group()


def tweak_groups():
    """[(id группы, подпись, [tweak-словари])] в порядке файла."""
    doc = P.tweaks_doc()
    out = []
    for g in doc['groups']:
        items = [t for t in doc['tweaks'] if t['group'] == g['id']]
        if items:
            out.append((g['id'], g['label'], items))
    return out


# ---- статистика --------------------------------------------------------------------------------------------------------------

class McGenStageTime(PropertyGroup):
    name: StringProperty(name='Stage')
    seconds: FloatProperty(name='Seconds', precision=3)


class McGenStructureType(PropertyGroup):
    name: StringProperty(name='Structure')
    count: IntProperty(name='Count')


class McGenStructureStart(PropertyGroup):
    name: StringProperty(name='Structure')
    chunk_x: IntProperty()
    chunk_z: IntProperty()
    bb: IntVectorProperty(size=6)               # x0, y0, z0, x1, y1, z1 в блоках мира
    pieces: IntProperty()


MAX_LISTED_STARTS = 200


class McGenStats(PropertyGroup):
    has_data: BoolProperty(default=False)
    backend: StringProperty(name='Backend')
    sink: StringProperty(name='Scene builder')
    mode: StringProperty(name='Last run')
    error: StringProperty(name='Error')
    t_generate: FloatProperty(name='Generation', precision=3, subtype='TIME_ABSOLUTE')
    t_build: FloatProperty(name='Scene build', precision=3, subtype='TIME_ABSOLUTE')
    t_total: FloatProperty(name='Total', precision=3, subtype='TIME_ABSOLUTE')
    chunks: IntProperty(name='Chunks')
    objects: IntProperty(name='Objects')
    rebuilt: IntProperty(name='Rebuilt objects')
    vertices: IntProperty(name='Vertices')
    faces: IntProperty(name='Faces')
    memory_mb: FloatProperty(name='Voxel memory (MB)', precision=1)
    peak_rss_mb: FloatProperty(name='Peak process memory (MB)', precision=0)
    structures_total: IntProperty(name='Structures')
    structure_types: CollectionProperty(type=McGenStructureType)
    structure_starts: CollectionProperty(type=McGenStructureStart)
    stage_times: CollectionProperty(type=McGenStageTime)

    def fill(self, st):
        self.has_data = True
        self.backend = st.get('backend', '')
        self.sink = st.get('sink', '')
        self.mode = st.get('mode', '')
        self.t_generate, self.t_build, self.t_total = st.get('t_generate', 0.0), st.get('t_build', 0.0), st.get('t_total', 0.0)
        self.chunks, self.objects = st.get('chunks', 0), st.get('objects', 0)
        self.rebuilt = st.get('rebuilt', st.get('objects', 0))
        self.vertices, self.faces = st.get('vertices', 0), st.get('faces', 0)
        self.memory_mb = st.get('memory', 0) / 1048576.0
        self.error = ''
        self.peak_rss_mb = st.get('peak_rss', 0) / 1048576.0
        self.stage_times.clear()
        for k, v in st.get('stage_times', {}).items():
            e = self.stage_times.add()
            e.name, e.seconds = k, v
        self.structures_total = st.get('structures_total', 0)
        self.structure_types.clear()
        for k, n in st.get('structure_types', {}).items():
            e = self.structure_types.add()
            e.name, e.count = k, n
        self.structure_starts.clear()
        for (sid, cx, cz, bb, pieces) in st.get('structure_starts', [])[:MAX_LISTED_STARTS]:
            e = self.structure_starts.add()
            e.name, e.chunk_x, e.chunk_z, e.pieces = sid, cx, cz, pieces
            e.bb = bb


# ---- настройки сцены -----------------------------------------------------------------------------------------------------------

_PX = dict(step=1)


class McGenSettings(PropertyGroup):
    # Version & World
    version: EnumProperty(name='Version', description='Minecraft version of the world generator', items=_version_items, update=_on_world_changed)
    dimension: EnumProperty(name='Dimension', description='Dimension to generate', items=_dimension_items, update=_on_world_changed)
    preset: EnumProperty(name='Preset', description='World type preset of the datapack', items=_preset_items, update=_on_world_changed)

    # Seeds
    seed_mode: EnumProperty(name='Seed mode', description='One seed for everything (exactly like the game) or separate seeds per generation domain',
                            items=[('UNIFIED', 'Unified', 'One seed for all domains (vanilla behaviour)'),
                                   ('SPLIT', 'Separate', 'Separate seeds: climate, terrain, structures, features')],
                            default='UNIFIED', update=_on_seed_mode)
    seed: StringProperty(name='World seed', description='A number or any text, as in the game (text is hashed like Java String.hashCode). Empty = random',
                         default='12345', update=_auto_update)
    seed_climate: StringProperty(name='Climate', description='Climate noises: temperature, humidity, continentalness, erosion, weirdness. Sets biomes and the shape of the land',
                                 default='12345', update=_auto_update)
    seed_terrain: StringProperty(name='Terrain', description='All other named noises: caves, aquifers, ore veins, surface, badlands bands …', default='12345', update=_auto_update)
    seed_structures: StringProperty(name='Structures', description='Placement and generation of structures (villages, strongholds …)', default='12345', update=_auto_update)
    seed_features: StringProperty(name='Features', description='Decoration: trees, plants, ore and other features of every chunk', default='12345', update=_auto_update)

    # Area
    unit: EnumProperty(name='Units', description='Units of the area origin and size', items=[('BLOCKS', 'Blocks', 'Origin and size in blocks: the area is cut exactly at the block bounds'),
                                                                                             ('CHUNKS', 'Chunks', 'Origin and size in chunks (16 blocks each)')],
                       default='BLOCKS', update=_on_unit)
    origin_x: IntProperty(name='X', description='Area origin, west edge', default=0, min=-30_000_000, max=30_000_000, soft_min=-2048, soft_max=2048, update=_auto_update)
    origin_z: IntProperty(name='Z', description='Area origin, north edge', default=0, min=-30_000_000, max=30_000_000, soft_min=-2048, soft_max=2048, update=_auto_update)
    size_x: IntProperty(name='Size X', description='Area width in chunks (16 blocks each)', default=8, min=1, max=512, soft_min=1, soft_max=64, update=_on_size_chunks)
    size_z: IntProperty(name='Size Z', description='Area depth in chunks (16 blocks each)', default=8, min=1, max=512, soft_min=1, soft_max=64, update=_on_size_chunks)
    width: IntProperty(name='Size X', description='Area width in blocks (any number: the world is generated by whole chunks and shown cut exactly at this width)',
                       min=1, max=8176, soft_min=1, soft_max=1024, get=_get_width, set=_set_width, update=_auto_update)
    depth: IntProperty(name='Size Z', description='Area depth in blocks (any number: the world is generated by whole chunks and shown cut exactly at this depth)',
                       min=1, max=8176, soft_min=1, soft_max=1024, get=_get_depth, set=_set_depth, update=_auto_update)
    y_min: IntProperty(name='Min height', description='Lowest block row shown (the dimension limits apply)', default=-64, min=-2048, max=2048, soft_min=-64, soft_max=320, update=_auto_update)
    y_max: IntProperty(name='Max height', description='Highest block row shown (the dimension limits apply)', default=319, min=-2048, max=2048, soft_min=-64, soft_max=320, update=_auto_update)

    # Layers
    use_terrain: BoolProperty(name='Terrain', description='Stone, water, lava, aquifers and large ore veins (noise fill)', default=True, update=_auto_update)
    use_surface: BoolProperty(name='Surface', description='Surface rules: grass, sand, bedrock, deepslate, badlands bands, ice', default=True, update=_auto_update)
    use_caves: BoolProperty(name='Caves', description='Caves and canyons (carvers)', default=True, update=_auto_update)
    use_features: BoolProperty(name='Features', description='Trees, plants, ores and other decoration', default=False, update=_auto_update)
    use_structures: BoolProperty(name='Structures', description='Villages, temples, strongholds and other structures', default=False, update=_auto_update)
    schedule_file: StringProperty(name='Server schedule', description='Optional .mcsched file recorded from a real server run (tools/gt/record_schedule.py): '
                                  'decoration and fluid order are taken from it, so the world repeats that run of the game', subtype='FILE_PATH', default='', update=_auto_update)

    # World Tweaks
    use_tweaks: BoolProperty(name='World tweaks', description='Apply the settings below (off = exactly like the game)', default=False, update=_auto_update)
    tweaks: PointerProperty(type=McGenTweaks)

    # View
    tint_biomes: BoolProperty(name='Biome tints', description='Tint grass, foliage and water with the biome colors', default=True, update=_auto_update)
    water_style: EnumProperty(name='Water', description='How water is shown', items=[('TRANSLUCENT', 'Translucent', 'Semi-transparent water'),
                                                                                       ('OPAQUE', 'Opaque', 'Solid water surface'),
                                                                                       ('HIDDEN', 'Hidden', 'No water (seabed visible)')],
                              default='TRANSLUCENT', update=_auto_update)
    chunks_per_object: EnumProperty(name='Chunks per object', description='Chunks merged into one Blender object (more = faster viewport, slower edits)',
                                    items=[('1', '1 × 1', 'One object per chunk'), ('2', '2 × 2', '4 chunks per object'),
                                           ('4', '4 × 4', '16 chunks per object'), ('8', '8 × 8', '64 chunks per object')], default='1', update=_auto_update)
    greedy_merge: BoolProperty(name='Greedy merge', description='Merge flat faces into larger quads (tiled texture): far fewer polygons for huge scenes', default=False, update=_auto_update)
    lod_mode: EnumProperty(name='Distant LOD', description='Level of detail far from the display range',
                           items=[('OFF', 'Off', 'Full detail everywhere'), ('HEIGHTMAP', 'Height map', 'Distant chunks as simplified height-map columns')],
                           default='OFF', update=_auto_update)
    lod_near: IntProperty(name='Display range', description='Chunks around the area center shown in full detail (the rest uses the LOD)', default=16, min=1, max=512, soft_min=2, soft_max=64, update=_auto_update)
    pixel_style: EnumProperty(name='Texture style', description='Texture filtering of block textures', items=[('PIXEL', 'Pixel', 'Sharp pixels (closest filtering)'),
                                                                                                          ('SMOOTH', 'Smooth', 'Smooth (linear filtering)')],
                              default='PIXEL', update=_auto_update)
    compute: EnumProperty(name='Compute', description='Where the generator computes: Auto, the CPU, or the NVIDIA GPU (optional CUDA library; the result is identical)',
                          items=_compute_items, update=_on_compute)
    auto_update: BoolProperty(name='Auto update', description='Re-run Update Layers shortly after a setting changes', default=False)
    collection_name: StringProperty(name='Collection', description='Collection that receives the generated objects', default='MC World')

    # Biome Preview
    bm_step: EnumProperty(name='Resolution', description='Blocks per pixel of the biome map', items=[('1', '1 block', ''), ('2', '2 blocks', ''), ('4', '4 blocks', ''),
                                                                                                       ('8', '8 blocks', ''), ('16', '16 blocks', ''), ('32', '32 blocks', '')],
                          default='4')
    bm_y: IntProperty(name='Sample height', description='Y level at which biomes are sampled (matters underground)', default=64, min=-2048, max=2048, soft_min=-64, soft_max=320)
    bm_palette: EnumProperty(name='Palette', description='Biome colors', items=[('MAP', 'Map colors', 'Familiar map colors per biome'),
                                                                                  ('HASH', 'Hash', 'Deterministic color from the biome name'),
                                                                                  ('JSON', 'Biome JSON', 'Colors from the datapack biome files (water / grass)')], default='MAP')
    bm_plane: BoolProperty(name='Add plane', description='Also add a textured plane with the biome map to the scene', default=True)

    # Edit Blocks (строительство и разрушение)
    edit_block: StringProperty(name='Block', description='Block to place: name such as stone, oak_stairs or minecraft:glass (the eyedropper tool fills it with the block under the cursor)',
                               default='minecraft:stone')

    stats: PointerProperty(type=McGenStats)

    # производные величины
    def _prev_unit(self):
        return _prev_unit_impl(self)

    def origin_chunks(self):
        if self.unit == 'CHUNKS':
            return self.origin_x, self.origin_z
        return self.origin_x // 16, self.origin_z // 16

    def _box_for(self, unit):
        """Область (x0, z0, x1, z1) в блоках, x1/z1 исключительно, при единицах unit."""
        if unit == 'CHUNKS':
            x0, z0 = self.origin_x * 16, self.origin_z * 16
            return x0, z0, x0 + self.size_x * 16, z0 + self.size_z * 16
        x0, z0 = self.origin_x, self.origin_z
        return x0, z0, x0 + self.width, z0 + self.depth

    def block_box(self):
        return self._box_for(self.unit)

    def area_chunks(self):
        """(cx0, cz0, nx, nz): чанки, накрывающие область (их генерирует libmcgen)."""
        x0, z0, x1, z1 = self.block_box()
        cx0, cz0 = x0 // 16, z0 // 16
        return cx0, cz0, min(512, (x1 - 1) // 16 - cx0 + 1), min(512, (z1 - 1) // 16 - cz0 + 1)

    def crop_box(self):
        """Точная область в блоках, если она не кратна чанкам (сцена обрезается по ней), иначе None."""
        cx0, cz0, nx, nz = self.area_chunks()
        x0, z0, x1, z1 = self.block_box()
        x1, z1 = min(x1, (cx0 + nx) * 16), min(z1, (cz0 + nz) * 16)
        if (x0, z0, x1, z1) == (cx0 * 16, cz0 * 16, (cx0 + nx) * 16, (cz0 + nz) * 16):
            return None
        return (x0, z0, x1, z1)

    def tweaks_changed(self):
        """[(id, значение)] настроек, отличных от ванили."""
        out = []
        for t in P.tweaks_doc()['tweaks']:
            v = getattr(self.tweaks, t['id'])
            v = float(v) if t['type'] == 'float' else int(v)
            if v != t['default']:
                out.append((t['id'], v))
        return tuple(out)


SETTINGS_PRESET_FIELDS = ['version', 'dimension', 'preset', 'seed_mode', 'seed', 'seed_climate', 'seed_terrain', 'seed_structures', 'seed_features',
                          'unit', 'origin_x', 'origin_z', 'size_x', 'size_z', 'width', 'depth', 'y_min', 'y_max', 'use_terrain', 'use_surface', 'use_caves', 'use_features',
                          'use_structures', 'use_tweaks', 'tint_biomes', 'water_style', 'chunks_per_object', 'greedy_merge', 'lod_mode', 'lod_near', 'pixel_style',
                          'bm_step', 'bm_y', 'bm_palette']


def preset_values():
    """Имена свойств для пресетов («s.seed», «s.tweaks.cave_density», …)."""
    v = ['s.' + f for f in SETTINGS_PRESET_FIELDS]
    v += ['s.tweaks.' + t['id'] for t in P.tweaks_doc()['tweaks']]
    return v


# ---- настройки аддона (Preferences) ------------------------------------------------------------------------------------------------

def _on_cache_dir(self, context):
    paths.set_cache_override(bpy.path.abspath(self.cache_dir) if self.cache_dir else None)


def _on_backend(self, context):
    backend.set_mode(self.backend)
    catalog.clear()


class McGenPreferences(AddonPreferences):
    bl_idname = ROOT

    server_jar: StringProperty(name='Server jar', description='Official Minecraft server jar (server.jar from minecraft.net or the Download button)', subtype='FILE_PATH')
    client_jar: StringProperty(name='Client jar', description='Minecraft client jar, e.g. .minecraft/versions/<version>/<version>.jar (textures and models are read from it)', subtype='FILE_PATH')
    java_path: StringProperty(name='Java', description='java executable of Java 25 or newer (empty = find automatically: JAVA_HOME, PATH, the launcher runtime)', subtype='FILE_PATH')
    reports_folder: StringProperty(name='Reports folder', description='Fallback: a folder with reports/blocks.json made by the game data generator (use it when no Java is available)', subtype='DIR_PATH')
    pack_override: StringProperty(name='Existing pack folder', description='Advanced: use an already prepared libmcgen pack folder (data/ + reports/) instead of the cache', subtype='DIR_PATH')
    assets_override: StringProperty(name='Existing assets folder', description='Advanced: use an already extracted client assets folder (assets/minecraft/…) instead of the cache', subtype='DIR_PATH')
    cache_dir: StringProperty(name='Cache folder', description='Where prepared resources are stored (empty = the extension user folder)', subtype='DIR_PATH', update=_on_cache_dir)
    accept_eula: BoolProperty(name='I accept the Minecraft EULA', description='Required to download the official jars from Mojang (https://aka.ms/MinecraftEULA)', default=False)
    download_version: EnumProperty(name='Version', description='Version to download', items=_download_version_items)
    backend: EnumProperty(name='Generator', description='Which generator to use', items=[('auto', 'Automatic', 'Use libmcgen if it is found for this platform, otherwise the demo generator'),
                                                                                         ('lib', 'libmcgen', 'The real library only (error if missing)'),
                                                                                         ('mock', 'Demo', 'Demo generator (noise terrain, not Minecraft) for trying out the interface')],
                          default='auto', update=_on_backend)
    threads: IntProperty(name='Threads', description='Worker threads of the generator (0 = all cores)', default=0, min=0, max=256, soft_max=32)
    sink: EnumProperty(name='Scene builder', description='Which scene builder to use', items=[('AUTO', 'Automatic', 'Block meshes with textures when the resources are ready, otherwise the height-map preview'),
                                                                                              ('FALLBACK', 'Preview', 'Always the simple height-map preview')], default='AUTO')

    def draw(self, context):
        from . import panels
        panels.draw_resources(self.layout, context, self, in_prefs=True)


def get_prefs(context=None):
    ctx = context or bpy.context
    try:
        return ctx.preferences.addons[ROOT].preferences
    except (KeyError, AttributeError):
        return None


def settings(context=None):
    ctx = context or bpy.context
    return ctx.scene.mcgen


classes = (McGenStageTime, McGenStructureType, McGenStructureStart, McGenStats, McGenTweaks, McGenSettings, McGenPreferences)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.mcgen = PointerProperty(type=McGenSettings)
    pr = get_prefs()
    if pr is not None:
        paths.set_cache_override(bpy.path.abspath(pr.cache_dir) if pr.cache_dir else None)
        backend.set_mode(pr.backend)


def unregister():
    try:
        del bpy.types.Scene.mcgen
    except AttributeError:
        pass
    for c in reversed(classes):
        try:
            bpy.utils.unregister_class(c)
        except RuntimeError:
            pass

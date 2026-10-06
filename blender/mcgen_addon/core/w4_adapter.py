"""Адаптер приёмника сцены к render.scene.SceneBuilder (поток W4, контракт — docs/blender/assets-mesh.md §1.2).

Контракт W4:  sb = SceneBuilder(view_settings); sb.build(blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names, progress);
              sb.update_chunk(cx, cz) / update_chunks(keys); sb.clear().
build() — блокирующий. Чтобы интерфейс не зависал, адаптер выбирает ПОРЦИОННУЮ форму по возможностям SceneBuilder (по убыванию предпочтения):

  1. sb.build_iter(blocks, biomes, info, block_names, biome_names)  — генератор, yield после части работы (желаемый API, см. docs/blender/addon.md);
  2. sb.begin(...) + sb.step(budget_s) -> bool;
  3. «через update_chunks»: clear() + _ensure_resources() + set_chunks() и пачки update_chunks(ключи группы) — работает с текущим кодом W4
     (update_chunks создаёт недостающие группы), но использует «приватные» _ensure_resources/set_chunks;
  4. один вызов build() (UI на это время замирает; в статус-баре остаётся прогресс из колбэка).

Для Update Layers (ctx.changed) при наличии прежнего построителя в сессии пересобираются только изменившиеся группы (форма 3).
"""
import dataclasses
import importlib
import time

from . import paths
from .scene_iface import SceneSink


def _module():
    pkg = __package__.rsplit('.', 1)[0]
    return importlib.import_module(pkg + '.render.scene')


def make_view_settings(mod, ctx, threads=0):
    """ViewSettings из настроек аддона; неизвестные поля версии W4 пропускаются (ViewSettings принимает dict)."""
    v = ctx.view
    want = {
        'assets_dir': ctx.assets_dir, 'pack_dir': ctx.pack_dir, 'cache_dir': paths.cache_dir(), 'version': ctx.params.version,
        'chunks_per_object': int(v.get('chunks_per_object', 1)), 'pixel_style': v.get('pixel_style', 'PIXEL') == 'PIXEL',
        'collection': ctx.collection_name, 'merge_flat': bool(v.get('greedy_merge', False)),
        'lod': v.get('lod_mode', 'OFF') != 'OFF', 'lod_distance': int(v.get('lod_near', 16)), 'threads': int(threads or ctx.params.threads or 0),
        # поля, которых у W4 пока нет (будут проигнорированы), оставлены для будущих версий ViewSettings
        'tint_biomes': bool(v.get('tint_biomes', True)), 'water_style': v.get('water_style', 'TRANSLUCENT'),
        'y_min': v.get('y_min'), 'y_max': v.get('y_max'),
    }
    cls = getattr(mod, 'ViewSettings', None)
    if cls is None:
        return want
    if dataclasses.is_dataclass(cls):
        names = {f.name for f in dataclasses.fields(cls)}
        return cls(**{k: x for k, x in want.items() if k in names})
    try:
        return cls(want)                 # ViewSettings(dict): берёт известные ключи
    except TypeError:
        obj = cls()
        for k, x in want.items():
            if hasattr(obj, k):
                setattr(obj, k, x)
        return obj


def remove_stale_objects():
    """Удаляет объекты построителя сцены (признак — свойство `mc_group`), оставшиеся от прежнего построителя: например, из открытого .blend, где сохранены меши,
    но самого построителя (и вокселей) нет. Иначе новая сборка создала бы дубликаты `mc_x_z.001`. Возвращает число удалённых объектов."""
    import bpy
    stale = [o for o in bpy.data.objects if 'mc_group' in o.keys() or 'mc_overlay' in o.keys()]
    for o in stale:
        mesh = o.data if o.type == 'MESH' else None
        bpy.data.objects.remove(o, do_unlink=True)
        if mesh is not None and mesh.users == 0:
            bpy.data.meshes.remove(mesh)
    return len(stale)


class W4Sink(SceneSink):
    name = 'render.scene'

    def __init__(self):
        self._mod = None
        self._sb = None
        self._gen = None
        self._frac = 0.0
        self._blocking_args = None
        self._batches = None
        self._bi = 0
        self._stats = {}
        self._done = False
        self._mode = ''

    def begin(self, ctx):
        self._mod = _module()
        self._ctx = ctx
        reg = ctx.region
        prev = getattr(ctx, 'prev_sink', None)
        self._sb = prev._sb if (prev is not None and isinstance(prev, W4Sink) and prev._sb is not None and ctx.changed is not None) else None
        ctx.prev_sink = None           # цепочка «приёмник -> контекст -> прежний приёмник …» держала бы в памяти ВСЕ прошлые сборки (утечка ≈ размер сцены на каждую пересборку)
        reuse = self._sb is not None
        if self._sb is None:
            remove_stale_objects()
            self._sb = self._mod.SceneBuilder(make_view_settings(self._mod, ctx))
        sb = self._sb
        blocks = {c: self._cropped(reg, c, ctx.view) for c in reg.chunks()}
        biomes = {c: reg.biomes(*c).reshape(-1) for c in reg.chunks()}
        args = (blocks, biomes, reg.info, ctx.gen.block_names(), ctx.gen.biome_names())
        self._n = len(blocks)
        if hasattr(sb, 'build_iter'):
            self._mode = 'build_iter'
            self._gen = iter(sb.build_iter(*args))
        elif hasattr(sb, 'begin') and hasattr(sb, 'step'):
            self._mode = 'begin_step'
            sb.begin(*args)
        elif hasattr(sb, 'update_chunks') and hasattr(sb, '_ensure_resources') and hasattr(sb, 'set_chunks'):
            self._mode = 'update_chunks'
            if not reuse:
                sb.clear()
            sb._ensure_resources(args[3], args[4])
            sb.min_y, sb.height = reg.info.min_y, reg.info.height
            sb.set_chunks(blocks, biomes)
            N = max(1, int(ctx.view.get('chunks_per_object', 1)))
            groups = {}
            for c in reg.chunks():
                if ctx.changed is not None and reuse and c not in ctx.changed:
                    continue
                groups.setdefault((c[0] // N, c[1] // N), []).append(c)
            self._batches = [groups[k] for k in sorted(groups)]
            self._bi = 0
        else:
            self._mode = 'blocking'
            self._blocking_args = args
        self._t0 = time.perf_counter()

    @staticmethod
    def _cropped(reg, c, view):
        """Массив блоков чанка; блоки вне диапазона высот и вне точной области в блоках (view['crop'] = (x0, z0, x1, z1), x1/z1 исключительно) заменяются воздухом
        (копия — данные региона не меняются; копируются только краевые чанки)."""
        a = reg.blocks(*c)
        lo = view.get('y_min')
        hi = view.get('y_max')
        crop = view.get('crop')
        min_y, h = reg.info.min_y, reg.info.height
        cut_y = not ((lo is None or lo <= min_y) and (hi is None or hi >= min_y + h - 1))
        lx0 = lx1 = lz0 = lz1 = None
        if crop:
            lx0, lx1 = max(0, crop[0] - c[0] * 16), min(16, crop[2] - c[0] * 16)
            lz0, lz1 = max(0, crop[1] - c[1] * 16), min(16, crop[3] - c[1] * 16)
        cut_xz = crop is not None and (lx0 > 0 or lx1 < 16 or lz0 > 0 or lz1 < 16)
        if not cut_y and not cut_xz:
            return a.reshape(-1)
        a = a.copy()
        if lo is not None and lo > min_y:
            a[:max(0, min(h, lo - min_y))] = 0
        if hi is not None and hi < min_y + h - 1:
            a[max(0, hi - min_y + 1):] = 0
        if cut_xz:                                               # оси блоков чанка: [y][z][x]
            a[:, :max(0, lz0), :] = 0
            a[:, max(0, lz1):, :] = 0
            a[:, :, :max(0, lx0)] = 0
            a[:, :, max(0, lx1):] = 0
        return a.reshape(-1)

    def step(self, budget_s):
        t_end = time.perf_counter() + budget_s
        sb = self._sb
        if self._mode == 'blocking':
            args, self._blocking_args = self._blocking_args, None
            if args is not None:
                sb.build(*args, progress=lambda f, m='': setattr(self, '_frac', f))
            self._done = True
        elif self._mode == 'build_iter':
            try:
                while time.perf_counter() < t_end:
                    r = next(self._gen)
                    if isinstance(r, (int, float)):
                        self._frac = float(r)
            except StopIteration:
                self._done = True
        elif self._mode == 'begin_step':
            self._done = bool(sb.step(budget_s))
        else:                                                    # update_chunks
            while self._bi < len(self._batches) and time.perf_counter() < t_end:
                sb.update_chunks(self._batches[self._bi])
                self._bi += 1
            self._frac = self._bi / max(1, len(self._batches))
            self._done = self._bi >= len(self._batches)
        if self._done:
            self._frac = 1.0
            self.refresh_overlays()
            self._collect_stats()
            self._attach_edit()
        return self._done

    def refresh_overlays(self):
        """Узоры баннеров построек (render/banner_overlay.py) пересоздаются заново: блок-сущности берутся из региона, сломанные правкой баннеры отбрасываются."""
        try:
            ctx = self._ctx
            reg = ctx.region
            entries = reg.block_entities() if hasattr(reg, 'block_entities') else []
            from importlib import import_module
            mod = import_module(__package__.rsplit('.', 1)[0] + '.render.banner_overlay')
            mod.rebuild(self._sb, reg, ctx.gen.block_names(), entries, ctx.assets_dir, ctx.view)
        except Exception:      # noqa: BLE001 - узоры необязательны: сцена без них остаётся рабочей
            import traceback
            self.overlay_error = traceback.format_exc()

    def _attach_edit(self):
        """Привязывает инструменты строительства/разрушения (mcgen.edit_*) к собранной сцене."""
        try:
            p = self._ctx.params
            key = [p.version, p.dimension, p.preset] + [int(x) for x in p.seeds]       # правки накладываются только на тот же мир
            importlib.import_module(__package__.rsplit('.', 1)[0] + '.render.edit_ops').attach(self._sb, key)
        except Exception:      # noqa: BLE001 — редактирование необязательно для показа мира (нет bpy-операторов в тестах ядра и т. п.)
            pass

    def _collect_stats(self):
        sb = self._sb
        groups = getattr(sb, 'groups', None) or {}
        quads = sum(getattr(g, 'n_quads', 0) for g in groups.values())
        st = getattr(sb, 'stats', None)
        st = st() if callable(st) else (st or {})
        build = st.get('build', {}) if isinstance(st, dict) else {}
        total = len(groups) or st.get('objects', 0)
        rebuilt = len(self._batches or []) if self._mode == 'update_chunks' else total       # одна пачка = одна группа (объект)
        self._stats = {'objects': total, 'faces': quads or build.get('quads', 0) or st.get('faces', 0), 'vertices': (quads or build.get('quads', 0)) * 4,
                       'rebuilt': rebuilt}

    @property
    def progress(self):
        return self._frac

    def stats(self):
        return dict(self._stats)

    def clear(self, scene, collection_name):
        try:
            importlib.import_module(__package__.rsplit('.', 1)[0] + '.render.edit_ops').detach()
        except Exception:      # noqa: BLE001
            pass
        if self._sb is not None:
            self._sb.clear(full=True) if 'full' in self._sb.clear.__code__.co_varnames else self._sb.clear()

    def abort(self):
        self._gen = None
        self._batches = []
        self._done = True

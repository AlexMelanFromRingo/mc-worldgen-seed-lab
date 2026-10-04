"""Конвейер «Generate / Update Layers / Biome Map»: рабочий поток (libmcgen через ctypes) + порционная сборка сцены в главном потоке.

Модальный оператор (ui/ops.py) только опрашивает GenerateJob.poll() по таймеру; тесты и скрипты вызывают run_blocking().
Фазы: 'generate' (поток: McGen -> McWorld -> generate_region с прогрессом и отменой) -> 'build' (главный поток: SceneSink.step(бюджет)
порциями, чтобы интерфейс не зависал) -> 'done' | 'error' | 'cancelled'.
"""
import time

import numpy as np

from . import backend, pack, params as P, sysinfo
from .scene_iface import BuildContext, pick_sink
from .tasks import Task

_sessions = {}


class Session:
    """Состояние последней сборки сцены (в памяти; воксели в .blend не сохраняются — воспроизводятся по настройкам)."""

    def __init__(self):
        self.params = None
        self.gen = None
        self.world = None
        self.region = None
        self.sink = None
        self.stats = {}
        self.job = None
        self.backend_name = ''

    def release(self):
        if self.region is not None:
            try:
                self.region.close()
            except Exception:
                pass
        self.region = self.world = None
        self.params = None


def session(key='default'):
    return _sessions.setdefault(key, Session())


def drop_sessions():
    for s in _sessions.values():
        s.release()
    _sessions.clear()


class JobError(RuntimeError):
    pass


def check_resources(version, pack_override='', assets_override=''):
    """-> (resolved, problem|None). Для настоящей библиотеки нужен готовый pack-каталог; для макета — нет."""
    res = pack.resolve(version, pack_override, assets_override)
    mode = backend.requested()
    if mode == 'lib' or (mode == 'auto' and backend.name() == 'lib'):
        if not res['pack_ok']:
            return res, ('Resources for {version} are not prepared (datapack + reports/blocks.json). Open the Resources panel and press Prepare Resources.', {'version': version})
    return res, None


def _generate_worker(task, params, pack_dir, reuse):
    """Рабочий поток: открыть McGen, создать/переиспользовать McWorld, сгенерировать регион. Возвращает dict с объектами и временами."""
    t = {}
    t0 = time.perf_counter()
    gen = backend.open_gen(pack_dir, params.version)
    t['open'] = time.perf_counter() - t0
    task.check()                                 # контрольные точки отмены: библиотека проверяет её только между шагами обратного вызова прогресса
    world = reuse
    if world is None:
        t0 = time.perf_counter()
        task.report(0.02, 'Creating world')
        world = gen.world(params.dimension, params.preset, params.seeds, params.tweaks_dict(), params.schedule)
        t['world'] = time.perf_counter() - t0
        task.check()
    stage_times = {}
    last = [time.perf_counter(), None]

    def progress(frac, what):
        # what: «terrain 12/256», «fluids 3/18», «features», «heightmaps», «done» — стадия = первое слово
        now = time.perf_counter()
        stage = (what or '').split(' ', 1)[0] or 'generating'
        if last[1] is not None:
            stage_times[last[1]] = stage_times.get(last[1], 0.0) + (now - last[0])
        last[0], last[1] = now, stage
        task.report(0.05 + 0.95 * frac, what or 'Generating')
        return task.should_cancel()

    t0 = time.perf_counter()
    region = world.generate_region(params.cx0, params.cz0, params.nx, params.nz, params.stages, params.threads, progress)
    now = time.perf_counter()
    if last[1] is not None:
        stage_times[last[1]] = stage_times.get(last[1], 0.0) + (now - last[0])
    t['generate'] = now - t0
    task.check()
    stage_times.pop('done', None)
    structs = None
    if params.stages & 32:                                   # MC_STAGE_STRUCTURES: список стартов (типы, координаты) для панели Stats
        t1 = time.perf_counter()
        try:
            structs = world.structure_starts(params.cx0, params.cz0, params.nx, params.nz)
        except Exception:        # noqa: BLE001 - библиотека без mcgen_structure_starts: список просто пуст
            structs = []
        stage_times['structure starts'] = time.perf_counter() - t1
    rss, peak = sysinfo.process_memory()
    return {'gen': gen, 'world': world, 'region': region, 'times': t, 'stage_times': stage_times, 'structures': structs, 'rss': rss, 'peak_rss': peak}


def changed_chunks(old_region, new_region, dilate=True):
    """Множество (cx, cz) чанков нового региона, которых нет в старом или чьи блоки/биомы изменились.

    dilate=True добавляет 8 соседей каждого изменившегося чанка (в пределах региона): отсечение граней, стенки и смешивание оттенков
    биомов на границе чанка зависят от соседа, поэтому меш соседа тоже нужно пересобрать. None — старый регион несовместим (строим всё).
    """
    if old_region is None or old_region.info.height != new_region.info.height or old_region.info.min_y != new_region.info.min_y:
        return None
    changed = set()
    for cx, cz in new_region.chunks():
        if not old_region.has_chunk(cx, cz):
            changed.add((cx, cz))
            continue
        if not np.array_equal(old_region.blocks(cx, cz), new_region.blocks(cx, cz)) or not np.array_equal(old_region.biomes(cx, cz), new_region.biomes(cx, cz)):
            changed.add((cx, cz))
    if dilate and changed:
        grown = set(changed)
        for cx, cz in changed:
            for dz in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if new_region.has_chunk(cx + dx, cz + dz):
                        grown.add((cx + dx, cz + dz))
        return grown
    return changed


class GenerateJob:
    """Задача «сгенерировать и показать». mode: 'generate' (всё заново) | 'update' (пересчитать только затронутое)."""

    def __init__(self, scene, params, mode='generate', skey='default', pack_override='', assets_override='', sink_pref='AUTO',
                 collection_name='MC World'):
        self.scene = scene
        self.params = params
        self.mode = mode
        self.skey = skey
        self.sess = session(skey)
        self.pack_override, self.assets_override = pack_override, assets_override
        self.sink_pref = sink_pref
        self.collection_name = collection_name
        self.phase = 'new'
        self.state = 'new'           # new | running | done | error | cancelled | noop
        self.error_text = ''
        self.error_exc = None
        self.message = ''
        self.task = None
        self.sink = None
        self.t_gen = self.t_build = 0.0
        self.stats = {}
        self.plan = {}
        self._t0 = 0.0
        self._gen_result = None
        self._ctx = None
        self.cancel_requested = False

    # --- запуск ---
    def start(self):
        self._t0 = time.perf_counter()
        res, problem = check_resources(self.params.version, self.pack_override, self.assets_override)
        if problem:
            from .scene_iface import w4_available  # noqa: F401  (импорт для единообразия; сообщение — шаблон с параметрами)
            tpl, kw = problem
            return self._fail(tpl.format(**kw))
        self.res = res
        self.plan = P.diff(self.sess.params, self.params) if self.mode == 'update' else {'world': True, 'area': True, 'stages': True, 'view': True,
                                                                                       'from_stage': 'biomes', 'reasons': ['generate']}
        s = self.sess
        need_data = (self.mode == 'generate' or s.region is None or self.plan['world'] or self.plan['area'] or self.plan['stages'])
        if self.mode == 'update' and s.region is not None and not need_data and not self.plan['view']:
            self.state, self.phase, self.message = 'noop', 'done', 'Nothing changed'
            return self
        self.state = 'running'
        if need_data:
            reuse = s.world if (self.mode == 'update' and s.world is not None and s.params is not None and s.params.world_key() == self.params.world_key()) else None
            pack_dir = res['pack'] if res['pack_ok'] else None
            self.phase = 'generate'
            self.task = Task('generate', _generate_worker, self.params, pack_dir, reuse).start()
        else:
            self._begin_build(s.gen, s.world, s.region, changed=None)
        return self

    def _fail(self, text, exc=None):
        self.state, self.phase, self.error_text, self.message = 'error', 'done', text, text
        self.error_exc = exc
        return self

    # --- опрос (главный поток) ---
    @property
    def fraction(self):
        if self.phase == 'generate' and self.task:
            return 0.8 * self.task.fraction
        if self.phase == 'build' and self.sink:
            return 0.8 + 0.2 * self.sink.progress
        return 1.0 if self.state in ('done', 'noop') else 0.0

    @property
    def finished(self):
        return self.state in ('done', 'error', 'cancelled', 'noop')

    def poll(self, budget_s=0.03):
        """Один шаг конвейера; вызывать по таймеру. Возвращает self.state."""
        if self.finished:
            return self.state
        try:
            if self.phase == 'generate':
                t = self.task
                self.message = t.message
                if not t.finished:
                    return self.state
                if self.cancel_requested or t.state == 'cancelled':          # отмена нажата — результат (даже готовый) выбрасывается, сцена не строится
                    self._discard_generated(t)
                    self.state, self.phase, self.message = 'cancelled', 'done', 'Cancelled'
                    return self.state
                if t.state == 'error':
                    return self._fail(t.error_text, t.error)
                r = t.result
                self.t_gen = r['times'].get('generate', 0.0)
                self._gen_result = r
                s = self.sess
                changed = None
                if self.mode == 'update' and s.region is not None and not self.plan['view']:
                    changed = changed_chunks(s.region, r['region'])         # None, если высоты/слои несовместимы -> строим всё
                self._begin_build(r['gen'], r['world'], r['region'], changed)
            if self.phase == 'build':
                done = self.sink.step(budget_s)
                self.message = f'Building scene {int(100 * self.sink.progress)}%'
                if done:
                    self._finish()
        except Exception as e:      # noqa: BLE001 - любая ошибка сборки сцены -> в интерфейс, а не в консоль
            import traceback
            self.error_text = f'{type(e).__name__}: {e}'
            self.error_exc = e
            self.tb = traceback.format_exc()
            self.state, self.phase = 'error', 'done'
        return self.state

    def _discard_generated(self, t):
        """Освобождает регион и мир, созданные отменённой генерацией (кроме принадлежащих сессии)."""
        r = t.result if isinstance(getattr(t, 'result', None), dict) else None
        if not r:
            return
        s = self.sess
        for key, keep in (('region', s.region), ('world', s.world)):             # gen — общий экземпляр из кэша backend: его закрывать нельзя
            obj = r.get(key)
            if obj is not None and obj is not keep:
                try:
                    obj.close()
                except Exception:      # noqa: BLE001 - освобождение необязательно
                    pass

    def _begin_build(self, gen, world, region, changed):
        s = self.sess
        # прежний регион освобождаем только после успешной смены (нужен для diff)
        self._new = (gen, world, region)
        if changed is not None and len(changed) == 0 and self.mode == 'update':
            self._swap_session(); self.state, self.phase, self.message = 'noop', 'done', 'No chunk changed'
            return
        self.sink = pick_sink(self.res['assets_ok'], self.sink_pref)
        ctx = BuildContext(self.scene, self.params, gen, world, region, self.res['pack'], self.res['assets'], self.collection_name, changed,
                           self.params.view_dict(), prev_sink=s.sink if type(s.sink) is type(self.sink) else None)
        self._ctx = ctx
        self.t_build0 = time.perf_counter()
        self.sink.begin(ctx)
        self.phase = 'build'

    def _swap_session(self):
        s = self.sess
        gen, world, region = self._new
        if s.region is not None and s.region is not region:
            try:
                s.region.close()
            except Exception:
                pass
        s.gen, s.world, s.region, s.params = gen, world, region, self.params
        if self.sink is not None:
            s.sink = self.sink
        s.backend_name = backend.name()

    def _finish(self):
        self.t_build = time.perf_counter() - self.t_build0
        self._swap_session()
        reg = self.sess.region
        st = dict(self.sink.stats() if self.sink else {})
        r = self._gen_result or {}
        st.update(backend=backend.name(), sink=self.sink.name if self.sink else '', t_generate=self.t_gen, t_build=self.t_build,
                  t_total=time.perf_counter() - self._t0, chunks=reg.info.nx * reg.info.nz, memory=reg.memory_bytes(),
                  stage_times=dict(r.get('stage_times', {})), mode=self.mode, reasons=list(self.plan.get('reasons', [])),
                  rss=sysinfo.process_memory()[0], peak_rss=sysinfo.process_memory()[1])      # ПОСЛЕ построения сцены (до этого замер был после генерации и занижал пик вдвое-втрое)
        starts = r.get('structures')
        if starts is None and self.mode == 'update' and self.sess.stats:
            st['structures_total'] = self.sess.stats.get('structures_total', 0)
            st['structure_types'] = self.sess.stats.get('structure_types', {})
            st['structure_starts'] = self.sess.stats.get('structure_starts', [])
        else:
            starts = starts or []
            types = {}
            for sx in starts:
                types[sx.id] = types.get(sx.id, 0) + 1
            st['structures_total'] = len(starts)
            st['structure_types'] = dict(sorted(types.items(), key=lambda kv: -kv[1]))
            st['structure_starts'] = [(sx.id, sx.chunk_x, sx.chunk_z, tuple(sx.bb), sx.piece_count) for sx in starts[:1000]]
        self.sess.stats = self.stats = st
        self.state, self.phase, self.message = 'done', 'done', 'Done'

    def cancel(self):
        """Запрос отмены. Фаза генерации: рабочий поток останавливается на ближайшей контрольной точке (библиотека проверяет отмену между шагами прогресса), а если он уже
        закончил — poll() выбрасывает результат; сцена не строится. Фаза построения: останавливается сразу."""
        if self.finished:
            return
        self.cancel_requested = True
        self.message = 'Cancelling …'
        if self.task and not self.task.finished:
            self.task.cancel()
        if self.phase == 'build':
            if self.sink:
                self.sink.abort()
            self._swap_session()
            self.sess.params = None            # сцена построена частично: следующий Update пересоберёт всё
            self.state, self.phase, self.message = 'cancelled', 'done', 'Cancelled'

    def run_blocking(self, budget_s=0.05, on_tick=None):
        if self.state == 'new':
            self.start()
        while not self.finished:
            self.poll(budget_s)
            if on_tick:
                on_tick(self)
            if self.phase == 'generate':
                time.sleep(0.01)
        return self


# ---- Biome Map -----------------------------------------------------------------------------------------------------------

def biome_map_worker(task, params, pack_dir, step, y):
    """Рабочий поток: сетка биомов по области (mcgen_biome_grid). Возвращает (grid u8 (h, w), names, x0, z0, step, секунды)."""
    t0 = time.perf_counter()
    gen = backend.open_gen(pack_dir, params.version)
    world = gen.world(params.dimension, params.preset, params.seeds, params.tweaks_dict())
    x0, z0 = params.cx0 * 16, params.cz0 * 16
    w, h = params.nx * 16 // step, params.nz * 16 // step
    # порциями по строкам: прогресс и отмена
    out = np.empty((h, w), np.uint8)
    rows = max(1, min(h, 256))
    for r0 in range(0, h, rows):
        task.check()
        n = min(rows, h - r0)
        out[r0:r0 + n] = world.biome_grid(x0, z0 + r0 * step, w, n, step, y)
        task.report((r0 + n) / h, 'Sampling biomes')
    return out, gen.biome_names(), x0, z0, step, time.perf_counter() - t0


def auto_step(params, requested):
    """Шаг (блоков на пиксель) с ограничением числа пикселей ≈ 4096²."""
    step = max(1, int(requested))
    while (params.nx * 16 // step) * (params.nz * 16 // step) > 4096 * 4096:
        step *= 2
    return step

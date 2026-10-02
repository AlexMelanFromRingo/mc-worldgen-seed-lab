"""Интерфейс «приёмника сцены» (SceneSink): превращает регион libmcgen в объекты Blender. Все методы — из ГЛАВНОГО потока.

Реализации:
  * core/fallback_preview.PreviewSink — временный запасной «предпросмотр по карте высот» (колонки одного цвета без текстур);
  * render.scene.SceneBuilder (поток W4: настоящие меши блоков, текстуры, оттенки) — через адаптер W4Sink ниже.

Оператор Generate ведёт приёмник порциями на таймере: begin() -> step(бюджет) … пока не вернёт True -> finish().
"""
import importlib
from dataclasses import dataclass, field


@dataclass
class BuildContext:
    scene: object                    # bpy.types.Scene
    params: object                   # core.params.GenParams
    gen: object                      # McGen (имена блоков/биомов)
    world: object                    # McWorld
    region: object                   # McRegion (массивы блоков/биомов/высот)
    pack_dir: str = None
    assets_dir: str = None
    collection_name: str = 'MC World'
    changed: object = None           # множество (cx, cz) чанков, изменившихся с прошлой сборки; None = строить всё
    view: dict = field(default_factory=dict)
    prev_sink: object = None         # приёмник прошлой сборки (его построитель можно переиспользовать для частичного обновления)


class SceneSink:
    """Базовый класс приёмника."""
    name = 'base'

    def begin(self, ctx):
        raise NotImplementedError

    def step(self, budget_s):
        """Выполнить часть работы не дольше budget_s секунд. True — всё построено."""
        raise NotImplementedError

    @property
    def progress(self):
        return 0.0

    def stats(self):
        """{'objects': n, 'vertices': n, 'faces': n}"""
        return {}

    def clear(self, scene, collection_name):
        raise NotImplementedError

    def abort(self):
        pass


def w4_available():
    """Есть ли модуль render.scene с SceneBuilder (поток W4)."""
    pkg = __package__.rsplit('.', 1)[0]
    try:
        m = importlib.import_module(pkg + '.render.scene')
        return hasattr(m, 'SceneBuilder')
    except Exception:       # noqa: BLE001 - модуль ещё не готов / не импортируется -> запасной предпросмотр
        return False


def pick_sink(assets_ok, prefer='AUTO'):
    """Приёмник сцены: SceneBuilder W4 (если модуль есть и ресурсы клиента готовы), иначе запасной предпросмотр."""
    from . import fallback_preview
    if prefer != 'FALLBACK' and assets_ok and w4_available():
        from .w4_adapter import W4Sink
        return W4Sink()
    return fallback_preview.PreviewSink()

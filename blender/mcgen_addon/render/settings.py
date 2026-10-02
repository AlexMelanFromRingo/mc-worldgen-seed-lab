"""Настройки вида сцены (без bpy): то, что W5 берёт из свойств Scene → «MC World» → «Вид» и передаёт в `SceneBuilder`."""
import os

__all__ = ['ViewSettings']

_DEFAULTS = dict(
    assets_dir='',            # каталог ресурсов клиента (run/assets-26.3)
    pack_dir='',              # pack-каталог (run/pack-26.3)
    cache_dir='',             # каталог кэша аддона (таблица состояний, PNG атласа)
    version='',               # строка версии игры (для имён/сообщений)
    chunks_per_object=1,      # 1 | 2 | 4 | 8: сколько чанков по стороне в одном объекте Blender
    biome_blend=2,            # радиус смешивания биомов (окно (2r+1)²), как «Biome blend» игры
    cutout_leaves=True,       # «красивая» листва (cutout); False — сплошные кубы листвы, листва к листве скрыта
    bake_shade=False,         # запечь направленное затенение игры в цвета вершин
    pixel_style=True,         # Closest (пиксельный) вместо Linear
    shading='lit',            # 'lit' (Principled, освещение сцены) | 'emission' (без освещения, как игра без теней)
    scale=1.0,                # единиц Blender на блок
    collection='MC World',    # имя коллекции сцены
    threads=0,                # потоки меширования (0 = по числу ядер)
    merge_flat=False,         # M5: жадное слияние плоских граней (повтор тайла в шейдере)
    lod=False,                # M5: дальний LOD по карте высот
    lod_distance=8,           # чанков от центра до границы LOD
    lod_stride=2,             # блоков в ячейке LOD (1, 2, 4, 8, 16)
    show_special=True,        # рисовать заглушки для особых блоков (сундуки, кровати… — при наличии)
)


class ViewSettings:
    """Контейнер настроек с умолчаниями. Принимает dict, другой ViewSettings или любой объект (PropertyGroup) с теми же именами атрибутов."""

    def __init__(self, src=None, **kw):
        for k, v in _DEFAULTS.items():
            setattr(self, k, v)
        if src is not None:
            self.update_from(src)
        for k, v in kw.items():
            if k not in _DEFAULTS:
                raise TypeError('неизвестная настройка вида: %s' % k)
            setattr(self, k, v)

    def update_from(self, src):
        for k in _DEFAULTS:
            if isinstance(src, dict):
                if k in src:
                    setattr(self, k, src[k])
            elif hasattr(src, k):
                try:
                    setattr(self, k, getattr(src, k))
                except Exception:
                    pass
        return self

    @classmethod
    def coerce(cls, src):
        return src if isinstance(src, cls) else cls(src)

    def threads_resolved(self):
        return self.threads if self.threads > 0 else max(1, (os.cpu_count() or 2))

    def as_dict(self):
        return {k: getattr(self, k) for k in _DEFAULTS}

    def __repr__(self):
        return 'ViewSettings(%s)' % ', '.join('%s=%r' % kv for kv in self.as_dict().items())

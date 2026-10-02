"""MC Worldgen — генерация мира Minecraft 26.x в Blender нашей C-библиотекой libmcgen (расширение Blender 4.2+).

Структура: core/ (мост ctypes lib.py, макет mock.py, ресурсы пользователя pack.py, сиды, палитры, задачи — без bpy),
ui/ (свойства, операторы, панели, пресеты, перевод), lib/<платформа>/ (бинарники libmcgen, в git не входят),
assets/, mesh/, render/ (потоки ресурсов и мешей: настоящие блоки, текстуры). Подробности — docs/blender/addon.md.
"""
import sys

try:
    import bpy  # noqa: F401
    _HAVE_BPY = True
except ImportError:                      # тесты ядра вне Blender импортируют только core.*
    _HAVE_BPY = False

if _HAVE_BPY:
    from . import ui


def register():
    if not _HAVE_BPY:
        raise RuntimeError('bpy недоступен: аддон запускается только внутри Blender')
    ui.register()


def unregister():
    if not _HAVE_BPY:
        return
    from .core import backend, jobs
    ui.unregister()
    jobs.drop_sessions()
    backend.release_all()

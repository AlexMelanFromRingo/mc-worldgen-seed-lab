"""Интерфейс аддона: свойства, операторы, панели, пресеты, перевод. Порядок регистрации важен (свойства -> операторы -> панели)."""
import bpy
from bpy.app.handlers import persistent

from .. import render
from . import ops, panels, presets, props, translations


@persistent
def _on_load_post(_dummy):
    """После открытия .blend воксельные данные прошлого файла недействительны (в .blend сохраняются только настройки и меши)."""
    from ..core import jobs
    ops.stop_all()
    jobs.drop_sessions()
    render.detach()


def register():
    translations.register()
    props.register()
    ops.register()
    render.register()                # инструменты строительства/разрушения (mcgen.edit_*)
    presets.register()
    panels.register()
    if _on_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load_post)


def unregister():
    if _on_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load_post)
    panels.unregister()
    presets.unregister()
    render.unregister()
    ops.unregister()
    props.unregister()
    translations.unregister()

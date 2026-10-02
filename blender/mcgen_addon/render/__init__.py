"""Слой Blender (bpy): материалы, построение сцены, выбор лучом, инструменты редактирования.

    from mcgen_addon.render.scene import SceneBuilder, ViewSettings
    from mcgen_addon.render import edit_ops          # edit_ops.register() / edit_ops.attach(scene_builder)

Модули импортируют bpy только внутри Blender; `settings.py` — без bpy.
"""


def register():
    """Регистрирует операторы редактирования (вызывается из register() расширения)."""
    from . import edit_ops
    edit_ops.register()


def unregister():
    from . import edit_ops
    edit_ops.unregister()

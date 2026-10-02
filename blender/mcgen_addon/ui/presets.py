"""Пресеты настроек (как у Render properties): сохранить / загрузить / удалить. Хранятся в user-пресетах Blender (scripts/presets/mcgen),
готовые наборы лежат в каталоге presets/mcgen аддона и подключаются через bpy.utils.register_preset_path."""
import os

import bpy
from bl_operators.presets import AddPresetBase
from bpy.types import Operator

from ..core import paths
from . import props


class MCGEN_OT_preset_add(AddPresetBase, Operator):
    bl_idname = 'mcgen.preset_add'
    bl_label = 'Add MC World Preset'
    bl_description = 'Save the current MC World settings as a preset (or delete the selected one with the minus button)'
    preset_menu = 'MCGEN_PT_presets'
    preset_subdir = 'mcgen'
    preset_defines = ['s = bpy.context.scene.mcgen']
    preset_values = []                  # заполняется в register(): включает динамические настройки мира


_path_registered = []


def register():
    MCGEN_OT_preset_add.preset_values = props.preset_values()
    bpy.utils.register_class(MCGEN_OT_preset_add)
    d = paths.addon_dir()
    if os.path.isdir(os.path.join(d, 'presets', 'mcgen')):
        try:
            bpy.utils.register_preset_path(d)
            _path_registered.append(d)
        except (RuntimeError, ValueError):
            pass


def unregister():
    for d in _path_registered:
        try:
            bpy.utils.unregister_preset_path(d)
        except (RuntimeError, ValueError):
            pass
    _path_registered.clear()
    try:
        bpy.utils.unregister_class(MCGEN_OT_preset_add)
    except RuntimeError:
        pass

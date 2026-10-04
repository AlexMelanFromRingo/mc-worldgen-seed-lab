"""Общий загрузчик для тестов: делает каталог blender/mcgen_addon доступным как пакет «mcgen_addon».

Вне Blender подставляется пакет-пустышка (чтобы не исполнять __init__ с bpy) — импортируются только core.*; внутри Blender
(bpy доступен) аддон импортируется обычным образом и регистрируется.
"""
import importlib
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
BLENDER_DIR = os.path.dirname(os.path.dirname(HERE))
ADDON_DIR = os.path.join(BLENDER_DIR, 'mcgen_addon')
REPO = os.path.dirname(BLENDER_DIR)
JARS = os.path.join(REPO, 'jars')
SCRATCH = os.environ.get('MCGEN_SCRATCH') or os.path.join(tempfile.gettempdir(), 'mcgen-tests')


def load_core():
    """core.* без bpy: возвращает пакет mcgen_addon (пустышка) — доступны mcgen_addon.core.lib / mock / pack / …"""
    if 'mcgen_addon' not in sys.modules:
        pkg = types.ModuleType('mcgen_addon')
        pkg.__path__ = [ADDON_DIR]
        sys.modules['mcgen_addon'] = pkg
    return sys.modules['mcgen_addon']


def load_addon():
    """Полный пакет аддона (нужен bpy)."""
    if BLENDER_DIR not in sys.path:
        sys.path.insert(0, BLENDER_DIR)
    return importlib.import_module('mcgen_addon')

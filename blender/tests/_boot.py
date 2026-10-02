"""Загрузка пакетов аддона W4 (assets, mesh, render) в тестах БЕЗ выполнения mcgen_addon/__init__.py (там может быть bpy).

Подменяет пакет `mcgen_addon` пустым пространством имён с правильным __path__; подпакеты импортируются как
`mcgen_addon.assets.*`, `mcgen_addon.mesh.*`, `mcgen_addon.render.*`. Внутри модулей используются только относительные импорты.
"""
import os
import sys
import types

BLENDER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # .../blender
ADDON_DIR = os.path.join(BLENDER_DIR, 'mcgen_addon')
REPO = os.path.dirname(BLENDER_DIR)


def boot():
    if 'mcgen_addon' not in sys.modules or not hasattr(sys.modules['mcgen_addon'], '__path__'):
        pkg = types.ModuleType('mcgen_addon')
        pkg.__path__ = [ADDON_DIR]
        sys.modules['mcgen_addon'] = pkg
    elif ADDON_DIR not in list(sys.modules['mcgen_addon'].__path__):
        sys.modules['mcgen_addon'].__path__.append(ADDON_DIR)
    return sys.modules['mcgen_addon']


boot()

# где лежат ресурсы пользователя (НЕ в git): переопределяется переменными окружения
RUN = os.environ.get('MCGEN_RUN', os.path.join(REPO, 'run'))
VERSION = os.environ.get('MCGEN_VERSION', '26.3')
ASSETS_DIR = os.environ.get('MCGEN_ASSETS', os.path.join(RUN, 'assets-' + VERSION))
PACK_DIR = os.environ.get('MCGEN_PACK', os.path.join(RUN, 'pack-' + VERSION))
SERVER_DIR = os.environ.get('MCGEN_SERVER', os.path.join(RUN, 'server-' + VERSION))
SCRATCH = os.environ.get('MCGEN_SCRATCH', os.path.join('/tmp', 'mcgen-w4'))


def have_resources():
    return os.path.isdir(os.path.join(ASSETS_DIR, 'assets', 'minecraft', 'blockstates')) and \
        os.path.isfile(os.path.join(PACK_DIR, 'reports', 'blocks.json'))

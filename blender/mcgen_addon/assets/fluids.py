"""Жидкости: классификация состояний (вода/лава, уровень, падающая), текстуры (still/flow/overlay), константы рендера.
Геометрию поверхности строит C-ядро (mesh.c) и эталон на numpy (mesh/mesher.py) по алгоритму FluidRenderer игры 26.x. Без bpy."""

from .blockprops_data import ALWAYS_WATER

__all__ = ['FLUID_NONE', 'FLUID_WATER', 'FLUID_LAVA', 'fluid_state', 'FLUID_TEXTURES', 'MAX_FLUID_HEIGHT']

FLUID_NONE, FLUID_WATER, FLUID_LAVA = 0, 1, 2
MAX_FLUID_HEIGHT = 8.0 / 9.0     # 0.8888889

# (still, flowing, overlay) по виду жидкости — как в FluidStateModelSet
FLUID_TEXTURES = {
    FLUID_WATER: ('minecraft:block/water_still', 'minecraft:block/water_flow', 'minecraft:block/water_overlay'),
    FLUID_LAVA: ('minecraft:block/lava_still', 'minecraft:block/lava_flow', None),
}


def fluid_state(block_name, props):
    """Состояние жидкости блока: (вид, количество 0..8, падающая). Количество — FluidState.getAmount(): источник 8, течение 1..7,
    падающая 8. Для блоков без жидкости (FLUID_NONE, 0, False). `props` — {имя: строка}."""
    n = block_name[10:] if block_name.startswith('minecraft:') else block_name
    if n == 'water' or n == 'lava':
        kind = FLUID_WATER if n == 'water' else FLUID_LAVA
        lv = int(props.get('level', '0'))
        if lv == 0:
            return kind, 8, False
        if lv < 8:
            return kind, 8 - lv, False
        return kind, 8, True
    if n in ALWAYS_WATER or props.get('waterlogged') == 'true':
        return FLUID_WATER, 8, False
    return FLUID_NONE, 0, False

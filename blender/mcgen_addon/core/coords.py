"""Система координат: мир Minecraft (x на восток, y вверх, z на юг; правая тройка) -> Blender (X восток, Y север, Z вверх).

    Blender = (x, -z, y)

Начало сцены Blender — юго-западный нижний угол области генерации по X/Z (cx0*16, cz0*16), по Y — абсолютная высота Minecraft
(уровень моря 63 лежит на Z = 63). Сдвиг области хранится в пользовательских свойствах коллекции: mcgen_cx0, mcgen_cz0, mcgen_min_y.
"""


def mc_to_blender(x, y, z):
    """Точка мира (x, y, z) в системе Blender (без учёта сдвига начала области)."""
    return (x, -z, y)


def blender_to_mc(bx, by, bz):
    return (bx, bz, -by)


def chunk_origin_blender(cx, cz, cx0, cz0):
    """Позиция минимального угла чанка (cx, cz) относительно начала области (cx0, cz0) в Blender (X, Y)."""
    return ((cx - cx0) * 16, -((cz - cz0) * 16))

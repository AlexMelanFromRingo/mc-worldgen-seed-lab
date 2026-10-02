"""Ресурсы клиента → таблицы для меширования (чистый Python, без bpy): PNG, blockstates, модели, атлас, оттенки, жидкости, таблица состояний.

    from mcgen_addon.assets import state_table
    table = state_table.load(assets_dir, pack_dir, cache_dir)

Текстуры и модели Mojang НЕ входят в аддон: читаются на лету из каталога ресурсов пользователя. Подробности — docs/blender/assets-mesh.md.
"""

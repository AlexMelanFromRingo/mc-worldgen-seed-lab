# Ресурсы, меши, редактирование (поток W4)

Статус: **в работе**. Этот документ — контракт для W5 (аддон/UI) и отчёт W4; числа и ссылки на картинки заполняются по мере готовности.
Все пути — от корня репозитория. Ресурсы Mojang (текстуры, модели, blockstates, colormaps) в репозиторий не кладутся: читаются из каталога
ресурсов пользователя (`run/assets-26.3/…`, `run/pack-26.3/…`).

## 1. API для W5

### 1.1. Ресурсы (чистый Python, без bpy) — `blender/mcgen_addon/assets/`

```python
from mcgen_addon.assets import state_table

table = state_table.load(assets_dir, pack_dir, cache_dir, version=None, progress=None, force=False)
```

* `assets_dir` — каталог ресурсов клиента: `run/assets-26.3` (или сразу `…/assets/minecraft`);
* `pack_dir` — pack-каталог (`run/pack-26.3`: `reports/blocks.json`, `data/minecraft/**`);
* `cache_dir` — каталог кэша аддона; таблица кладётся в `statetable-<sha1>.npz` (ключ — SHA-1 содержимого ресурсов);
* `progress(доля 0..1, текст)` — необязательный колбэк; `force=True` — пересобрать, игнорируя кэш.
* Возвращает `StateTable` (поля — таблицы для C-ядра, плюс `names`, `block_names`, `stats`, `atlas_image` и т. д.).
  `table.state_id("minecraft:oak_stairs[facing=north,…]") -> id | -1`, `table.classify(id)` → `air|cube|partial|cutout|translucent|fluid|special|nomodel`,
  `table.describe(id)`. `table.set_biomes(biome_names, assets_dir, pack_dir)` строит таблицу цветов по id биомов (порядок = порядок списка).

### 1.2. Сцена (bpy) — `blender/mcgen_addon/render/scene.py`

```python
from mcgen_addon.render.scene import SceneBuilder, ViewSettings

sb = SceneBuilder(view_settings)          # ViewSettings | dict | любой объект с теми же именами атрибутов (PropertyGroup)
sb.build(blocks_by_chunk, biomes_by_chunk, region_info, block_names, biome_names=None, progress=None)
sb.update_chunk(cx, cz)                   # пересобрать меш чанка (и соседей на границе) по текущим массивам
sb.clear()                                # удалить объекты, меши, материалы-кэш
```

* `blocks_by_chunk`: `{(cx, cz): np.ndarray(uint16)}`, размер `height*256`, порядок `[y][z][x]` (как `mcgen_region_blocks`). **Массивы не копируются**:
  правка элемента + `update_chunk` — основной путь редактирования.
* `biomes_by_chunk`: `{(cx, cz): np.ndarray(uint8)}` размер `(height/4)*16`, порядок `[qy][qz][qx]`; `None` — биом 0 везде.
* `region_info`: объект/словарь с `min_y`, `height` (и по возможности `cx0, cz0, nx, nz` — `McRegionInfo`).
* `block_names`: список имён состояний по id библиотеки (`mcgen_block_state_name`); если порядок совпадает с `reports/blocks.json` — идентичное отображение,
  иначе строится перекодировка по именам. `None` = порядок blocks.json.
* `biome_names`: имена биомов по id (`mcgen_biome_name`); `None` — отсортированный список биомов pack-каталога.

`ViewSettings` (все поля с умолчаниями): `assets_dir, pack_dir, cache_dir, version, chunks_per_object (1|2|4|8), biome_blend (0..7, по умолчанию 2),
cutout_leaves (True), bake_shade (False), pixel_style (True), scale (1.0), collection (имя), merge_flat (M5), lod (M5)`.

### 1.3. Ядро меширования — `blender/mcgen_addon/mesh/mesher.py`, `libmcgen/src/mesh/`

Описано в разделе «C-ядро» ниже (заполняется на вехе M2).

## 2. Что дальше в документе

Разделы «Измерения», «Отличия от игры», «Ворота G7/G8», картинки рендера — по мере готовности вех M1–M5.

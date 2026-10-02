# Эталоны (ground truth): настоящие чанки ванильного сервера с изоляцией стадий

Поток W6. Всё, что здесь написано, измерено на сервере 26.3 (`jars/server-26.3.jar`, Java 25), если не оговорено иное.
Инструменты — `tools/gt/`, данные — `run/gt/` (вне git), результаты ворот — `docs/blender/accuracy.md`.

## 0. Быстрый старт (для W1 и остальных)

```bash
# эталон «чистое заполнение шумом» уже сгенерирован: run/gt/26.3/raw/<dim>-s<seed>-c<cx>_<cz>-r10/world
ls run/gt/26.3/raw/
# сравнить дамп libmcgen с эталоном (код возврата 0/1/2, ASCII-карта чанков, топ пар «наше -> эталон», полосы по y)
python3 tools/gt/diff.py --ref run/gt/26.3/raw/overworld-s12345-c0_0-r10 --mcr region.mcr --mask-ext --biomes --heightmaps
# прогнать ворота по матрице (mcgen-cli или ctypes), таблица -> docs/blender/accuracy.md
python3 tools/gt/run_gate.py --gate G2 [--profile core|land|all] [--seeds 12345] [--dims overworld]
# сгенерировать новый эталон (под flock /tmp/mcgen-server.lock)
python3 tools/gt/gen_world.py --version 26.3 --variant raw --dim overworld --seed 12345 --cx 0 --cz 0 --radius 10
# самопроверка инструментов без libmcgen
python3 tools/gt/selftest.py
```

Для G2 в 26.3 дамп просить так: `mcgen-cli … --stages 0x3 --tweak ore_veins=0` (жилы руды в 26.3 лежат в `material_rule`, в `raw` их нет;
вариант `veins` — те же жилы, но без бедрока и поверхности).

## 1. Механизм изоляции датапаком (M1, измерено)

Датапак-переопределение worldgen-реестров работает на **новом** мире при условии, что каталог пака лежит в `<мир>/datapacks/<имя>/` ДО первого запуска.

* Эксперимент 1 (`run/gt/_exp/m1`): `world/datapacks/gt/{pack.mcmeta, data/minecraft/worldgen/material_rule/overworld.json = пустая sequence}`,
  `initial-enabled-packs` по умолчанию (`vanilla`). Лог сервера: `Found new data pack file/gt, loading it automatically`; `level.dat` →
  `DataPacks: {Enabled: [vanilla, file/gt], Disabled: [minecart_improvements, redstone_experiments, trade_rebalance]}`; `/datapack list` — «2 data pack(s) enabled:
  vanilla, file/gt (world)». В чанках нет бедрока, глубинного сланца и травы, т. е. переопределение действует на уже встроенное измерение Overworld
  (никакого `dimension`/`world_preset` в паке не нужно).
* Эксперимент 2 (рабочий вариант, им пользуется `gen_world.py`): то же + `initial-enabled-packs=vanilla,file/gt` в `server.properties` — работает так же
  (сообщения «Found new data pack» нет, пак включён явно). Пишем оба: явное перечисление защищает от изменения автоподхвата в будущих версиях.
* `pack.mcmeta`: `{"pack": {"description": "...", "min_format": N, "max_format": N}}`, где N = `data_major` из `run/pack-<V>/version.json`
  (26.3 — 121; 26.1 — 101; 26.2 — 107; 26.4-snapshot-2 — 122). Поля `pack_format` в 26.x нет.
* Переопределяется тот же путь, что и в ванили (`data/minecraft/worldgen/<реестр>/<id>.json`) — файл пака заменяет файл ванили целиком.
* Сервер из bundler-jar: `java -DbundlerRepoDir=<общий каталог> -jar jars/server-<V>.jar nogui` (**имя свойства — `bundlerRepoDir`**, не `bundlerrepodir`),
  cwd = каталог запуска мира; библиотеки (`libraries/`, `versions/`) распаковываются один раз в `run/gt/_srv/<V>/`.
* Команды консоли ждём до строки `Done (`; затем `gamerule random_tick_speed 0` … и **`tick freeze`**: чанки продолжают генерироваться (`forceload` работает),
  но нет случайных тиков и потоков жидкости (эталон остаётся «как после генерации»). `/forceload add` берёт БЛОЧНЫЕ координаты, максимум 256 чанков за команду
  (окна 16x16 чанков).

### Структура стадий в 26.3 (чтение `src/dec/26.3`) и что это значит

* Цепочка статусов: `empty → structure_starts → structure_references → biomes → terrain → features → initialize_light → light → spawn → full`.
  Статусов `noise`/`surface`/`carvers` в 26.3 **нет**: `NoiseBasedChunkGenerator.buildTerrain` за один статус `terrain` делает
  `doFill` (density + aquifer) → `buildSurface` (`MaterialSystem`: правило `material_rule`) → `generateCarvers`. Промежуточные стадии из .mca не достать —
  только изоляцией датапаком (отсюда варианты).
* `noise_settings.material_rule` (26.3) = единая последовательность: `bedrock_floor`, жилы меди/железа (`ore_vein`), поверхность
  (`above_preliminary_surface → overworld/surface`), `underground` (глубинный сланец, полосы серы). Поэтому `raw` = пустая `sequence` в этих файлах
  (`material_rule/{overworld,overworld_caves,overworld_floating_islands,nether,end}.json`) — остаётся чистый `doFill`: камень/вода/лава/воздух.
  В 26.1/26.2 то же даёт `surface_rule` = пустая sequence + `ore_veins_enabled=false` в `noise_settings`.
* **Жилы руды в 26.3 — часть правила материала**, а не заполнения (в `mcgen.h` они приписаны стадии TERRAIN): вариант `veins` оставляет в правиле только
  `copper_ore_vein` и `iron_ore_vein`; `raw` их не содержит.
* **Расширения «по биому» в коде, которые не отключаются данными:** `MaterialSystem.buildSurface` при любом правиле вызывает
  `erodedBadlandsExtension` (биом `eroded_badlands`: столбы из `default_block`) и `frozenOceanExtension` (`frozen_ocean`, `deep_frozen_ocean`: `packed_ice` и
  `snow_block` — айсберги). Поэтому в `raw`/`veins` эти столбцы — «грязь»; `diff.py --mask-ext` исключает столбцы, у которых в 3x3 клетках вокруг (по x, z; qy ±1 у
  поверхности) эталон имеет эти биомы, и пишет число замаскированных столбцов. В `surface` и выше расширения — часть стадии (маска не нужна).
* `generate-structures=false` отключает постройки целиком (Beardifier тоже); у биомов остаются те же списки биомов.
* Биомы в `raw` совпадают с ванильными (биомный источник датапаком не менялся) — `--biomes` сравнивает и их.

## 2. Варианты (`tools/gt/variants/<имя>/variant.json`)

Каталоги вариантов содержат только декларативное описание (`variant.json`): датапак строится из `run/pack-<V>/data/minecraft/**` при каждой генерации
(`tools/gt/datapack.py`; ресурсы Mojang в репозиторий не попадают), хэш собранного пака пишется в `manifest.json`.

| вариант | что включено поверх чистого заполнения | как делается |
|---|---|---|
| `raw` | ничего: камень/вода/лава/воздух (aquifer), без бедрока/сланца/жил/травы | material_rule пуст; `carvers=[]`, `features=[[]…]` у всех биомов; `generate-structures=false` |
| `veins` | + большие жилы руды | material_rule = только `ore_vein` |
| `surface` | + настоящее material_rule (бедрок, жилы, поверхность, сланец) | карверы/фичи пусты |
| `carvers` | + карверы | фичи пусты |
| `features` | + все декорации биомов, без построек | `generate-structures=false` |
| `full` | полная ваниль | без пака (`initial-enabled-packs=vanilla`), `generate-structures=true` |
| `feature:<placed_feature_id>` | `surface` + ровно одна placed_feature (на своём шаге у биомов, где она есть), карверов нет | остальные списки пусты |
| `structure:<structure_set_id>` | `surface` + ровно один набор построек, фич/карверов нет | у остальных `structure_set` `structures=[]`, `generate-structures=true` |

Список — `python3 tools/gt/datapack.py --list`; собрать пак вручную — `datapack.py --version 26.3 --variant raw --out DIR`.

## 3. Раскладка и манифест

`run/gt/<V>/<вариант>/<dim>-s<seed>-c<cx>_<cz>-r<R>/` — каталог запуска сервера: `world/` (level-name=world; Overworld
`world/dimensions/minecraft/overworld/region/*.mca`, Nether — `the_nether`, End — `the_end`), `world/datapacks/gt/`, `server.properties`, `server.log`,
`manifest.json` (версия, вариант, seed, область в чанках, число чанков full, время старта/генерации, java, хэш датапака, свойства). `anvilcache/` — npz-кэш читателя.
Область — квадрат (2R+1)² чанков вокруг (cx, cz). Матрица областей — `tools/gt/matrix.json` (генерирует `pick_regions.py`: `tools/mcquery`, y=68,
окна 21x21 чанков; для каждого seed — spawn (0,0), ocean, mountains, desert, jungle; Nether (0,0) и (60,-60); End (0,0), внешние острова (90,0) и (-40,-90)).

## 4. Читатель Anvil и сравнение

* `tools/gt/anvil.py`: блоки `u16[y][z][x]` (id по `reports/blocks.json`), биомы `u8[qy][qz][qx]`, статус, карты высот (абсолютные y; порядок как в MCR1);
  npz-кэш по region-файлу. Формат 26.x, найденный в данных: запись палитры блоков — `{"": "minecraft:stone"}` (состояние по умолчанию; NBT не смешивает строки
  и компаунды) либо `{id, properties{полный набор}}`; в `sections` есть «световые» секции вне диапазона блоков (`Y=-1` у End/Nether — только SkyLight) — они пропускаются;
  высоту берём из `dimension_type` (Overworld −64/384, Nether 0/256, End 0/256).
* `tools/gt/mcr.py`: MCR1 (чтение через memmap, запись); `from-world` строит дамп из эталона (самопроверки).
* `tools/gt/diff.py`: сопоставление состояний по именам (свойства упорядочиваются); метрики и маски — в `--help`. Коды возврата: 0 — совпадение ≥ порога, 1 — ниже, 2 — структурная
  ошибка (нет чанка, другая высота/min_y, 0 блоков).
* `tools/gt/run_gate.py`: ворота G2…G6 (см. `--list`), секции в `docs/blender/accuracy.md` между маркерами `<!-- gate:G2 begin/end -->`.

## 5. Статус и замеры

(заполняется по ходу работ — см. ниже)

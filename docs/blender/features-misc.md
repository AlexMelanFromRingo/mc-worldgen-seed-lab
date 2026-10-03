# W12 «Подземные, ледяные и особые фичи, Nether/End» (ворота G5-misc)

Отчёт потока W12 аддона «MC Worldgen». Каркас стадии FEATURES, порядок фич и общие типы — `docs/blender/features.md` (W8); растительность — `features-veg.md`, деревья/грибы — `features-trees.md`.
Здесь — всё, что не относится к ним: геоды, дрипстоун, монстр-комнаты, окаменелости, скалк, ледяные фичи, замораживание верхнего слоя, колодцы пустыни, Nether (базальт, дельты, светящийся камень),
End (шипы, острова, шлюзы, хорус), а также «собирательные» типы 26.3 (`overlay`, `template`, `random_neighbor_spread`, `speleothem*`…). Все числа ниже измерены (`libmcgen/tests/g5_features.py`,
`libmcgen/tests/g5_misc_order.py`, `tools/gt/diff.py`); команды воспроизведения — в §6.

## 1. Что сделано (26.3 проверено блок-в-блок; 26.1/26.2/26.4 — реализовано по различиям, эталонных миров ещё нет)

| файл | типы фич (`FeatType`) | placed_feature |
|---|---|---|
| `libmcgen/src/feature_nether.c` | `overlay`, `random_neighbor_spread`, `single_block_pillar`, `projected_random_patchy_square`, `netherrack_replace_blobs`, `delta_feature`, `stepped_column_cluster` | `glowstone`, `glowstone_extra`, `basalt_pillar`, `basalt_blobs`, `blackstone_blobs`, `delta`, `small_basalt_columns`, `large_basalt_columns` (и `overlay` у `weeping_vines`/`desert_well`) |
| `libmcgen/src/feature_end.c` | `end_spike`, `end_island`, `end_gateway`, `end_platform`, `chorus_plant` | `end_spike`, `end_island_decorated`, `end_gateway_return`, `end_platform`, `chorus_plant` |
| `libmcgen/src/feature_ice.c` | `blue_ice`, `spike`, `iceberg` | `blue_ice`, `ice_spike`, `iceberg_packed`, `iceberg_blue` |
| `libmcgen/src/feature_geode.c` | `geode` | `amethyst_geode` |
| `libmcgen/src/feature_drip.c` | `speleothem`, `speleothem_cluster`, `large_dripstone` (+ имена 26.1/26.2 `pointed_dripstone`, `dripstone_cluster`) | `pointed_dripstone`, `dripstone_cluster`, `large_dripstone`, `sulfur_spike`, `sulfur_spike_cluster` |
| `libmcgen/src/feature_misc_sculk.c` | `multiface_growth`, `sculk_patch` (SculkSpreader мира, курсоры заряда, `SculkVeinBlock`, `MultifaceSpreader`) | `sculk_vein`, `sculk_patch_deep_dark`, `glow_lichen` (его берёт растительность, тип общий) |
| `libmcgen/src/feature_misc_tmpl.c` | `fossil`, `template` (на API шаблонов W9: `template.h`/`processor.h`) | `fossil_upper`, `fossil_lower`, `desert_well` (26.3) |
| `libmcgen/src/feature_miscx.c` | `monster_room`, `underwater_magma`, `freeze_top_layer`, `fill_layer`, `void_start_platform`, `block_pile`, `replace_single_block`; единая регистрация `feature_register_misc_all()` | `monster_room`, `monster_room_deep`, `underwater_magma`, `freeze_top_layer`, `void_start_platform` |
| `libmcgen/src/feature_misc_old.c` | 26.1/26.2: `glowstone_blob`, `basalt_pillar`, `basalt_columns`, `desert_well` (в 26.3 переписаны через общие типы) | те же placed_feature в 26.1/26.2 |
| `libmcgen/src/feature_misc.h` | общие итераторы `BlockPos` (`manhattanOrdered`, `betweenClosed`) и мелочи | — |

Общие файлы — только точечные правки: `feature.c` (одна строка регистрации + экспериментальные режимы обхода чанков для анализа недетерминизма, см. §4), `surface.c/.h` (экспорт `surface_biome_temperature`, загрузка температур
биомов для всех биомов — причина см. §3.2), `template.h/.c`, `processor.h/.c` (ГСЧ настроек шаблона, §3.3), `tests/g5_features.py` (путь к `mcgen-cli` через `MCGEN_CLI`). ABI `mcgen.h` не менялся.
Не реализованы как неиспользуемые в генерации биомов: `bonus_chest` (только при создании мира `/place`), `end_podium` (бой с драконом), `coral_*`/`bamboo`/`vines`/`root_system`/`vegetation_patch`/`block_column` — группа W10, `tree`/`huge_*` — W11.

## 2. Что изменилось в 26.3 по сравнению с 26.1/26.2 (важно для «по различиям»)

В 26.3 фичи Nether и подземелий переписаны на обобщённые типы: `GlowstoneFeature` → `random_neighbor_spread`; `BasaltPillarFeature` → `overlay`[`single_block_pillar` + `projected_random_patchy_square` + `simple_block`…];
`BasaltColumnsFeature` → `stepped_column_cluster` внутри `weighted_random_selector`; `DesertWellFeature` → `overlay`[`template`…]; `PointedDripstoneFeature`/`DripstoneClusterFeature` → `speleothem`/`speleothem_cluster`
(блок и теги задаются в данных, свойство `vertical_direction` общее для версий); `GeodeFeature` — тот же алгоритм, но шум `createParity` на `float`-стеке (26.1/26.2 — `NormalNoise` на `double`); `LargeDripstoneFeature` в 26.3
ограничивает смещение ветра `16 − radius` (в 26.1/26.2 не ограничено). Алгоритмы остальных (`delta_feature`, `netherrack_replace_blobs`, `iceberg`, `spike`, `monster_room`, `fossil`, `underwater_magma`,
`sculk_patch`, `multiface_growth`, `end_*`, `chorus_plant`) совпадают с точностью до обёртки `config` — реализация общая, `fp_cfg()` снимает обёртку.

## 3. Важные детали

### 3.1. ГСЧ и порядок вызовов
Все фичи пишутся «по поведению и числам игры»: порядок и число вызовов `WorldgenRandom` (на Xoroshiro, но методы `LegacyRandomSource`, см. `features.md` §3.2) сохранены, в том числе «пустые» траты:
`EndSpikeFeature` тратит `nextFloat()` на угол кристалла, `MonsterRoomFeature` — `nextLong()` на `LootTableSeed` сундука и `nextInt(4)` на тип моба спавнера, `TemplateFeature` — `nextInt(сумма весов)` на выбор шаблона
(даже при единственном), `fossil` — `nextInt(4)` поворот, `nextInt(n)` индекс, `nextInt(10)` глубина. Float/double — как в Java (`Mth.sin/cos` — таблица; `Math.pow(x, 2.0)` = `x*x`; `iceberg`, `spike`, `geode`,
`large_dripstone` проверены на этом блок-в-блок).

### 3.2. Температура биомов и замораживание (`freeze_top_layer`)
`Biome.shouldFreeze/shouldSnow` требуют температуру биома `getTemperature(pos, seaLevel)` (базовая температура, модификатор `frozen`, снижение выше `seaLevel + 17` по шуму `SimplexNoise(seed 1234)`).
Эта величина уже была в `surface.c` (условие `temperature` правил поверхности и расширение айсбергов), но **таблица температур биомов загружалась только если правило поверхности использует условие `temperature`** — у остальных биомов
было `0.0`, и `freeze_top_layer` замораживал океаны и заснеживал пустыни. Теперь температуры грузятся для всех биомов, функция доступна как `surface_biome_temperature()`. Эффект в сборе (`features`, Overworld r = 10, 3 сида):
`ice → water` 88 980 / 52 337 / 45 920 → 0, `snow[layers=1] → air` и `grass_block[snowy] → grass_block` — исчезли (остаток — листва и деревья чужих групп); общая доля s12345: 99,434 % → 99,742 %.

### 3.3. Шаблоны NBT в фичах (fossil, template)
* `StructurePlaceSettings.setRandom(random)` делает `getRandom(pos)` общим потоком фичи: **выбор палитры** (`nextInt(число палитр)` тратится всегда, даже при одной палитре) и процессор `block_rot` читают
  `random.nextFloat()` из ГСЧ фичи, а не из LCG по позиции, как в постройках. Для этого в API W9 добавлено `TSettings.srnd` (отдельно от `rnd` — параметра `random` у `placeInWorld`, который идёт на `LootTableSeed`) и `PEnv.rnd`.
  Измерено на `fossil_upper`: без `fossil` — 43 расхождения, с шаблонами без общего ГСЧ — 16 (гниение `block_rot` по-разному), с общим ГСЧ — 0.
* **Взаимная блокировка:** `features_world_get` держит `w->lock` во время разбора фич, а `template_get`/`proclist_*` берут `w->lock` через `structures_world_get`. Поэтому шаблоны и списки процессоров разрешаются **лениво
  при первом `place()`** (разбор хранит только идентификаторы и `Js`-узлы). Любой новый тип фичи на шаблонах должен делать так же.

### 3.4. Недетерминизм игры у границ чанков (ice_patch, ice_spike, chorus, дрипстоун, скалк, базальтовые блобы)
Фичи соседних чанков, чьи окна пересекаются, дают порядок-зависимый результат, а настоящий сервер выполняет `FEATURES` чанков в порядке, зависящем от потоков (`ground-truth.md` §2.4). Мы обходим чанки в порядке билетов `forceload`
(x внешний, z внутренний, волнами — результат одинаков при любом числе потоков). Для фич, где одно изменение порядка каскадно меняет число вызовов ГСЧ в соседнем чанке, расхождение растёт лавинообразно (`netherrack_replace_blobs`:
`findTarget` зависит от соседних блобов, поэтому один «не тот» порядок пары чанков сдвигает весь поток ГСЧ чанка).
Инструменты (не часть ABI):
* `MCGEN_FEATURES_SEQ=<xz|zx|xz-|zx-|ring|randN>` — другой последовательный обход; `MCGEN_FEATURES_BEFORE="ax,az>bx,bz;…"` — чанк A декорируется непосредственно перед чанком B.
* `libmcgen/tests/g5_misc_order.py --feature <id>` — прогон в нескольких порядках (4 направления + случайные), подсчёт «неустойчивых» клеток (где результаты libmcgen в разных порядках различаются) и расхождений с эталоном
  вне неустойчивых клеток и вне течения воды/лавы; ненулевое число вне неустойчивых клеток — признак логической ошибки.
* `libmcgen/tests/g5_misc_view.py` — просмотр колонки/среза эталон|наше вокруг точки.
Примеры: `ice_spike` — расхождение исчезает (411 → 0), если пара соседних чанков обработана в обратном порядке; `chorus_plant` — тоже; `large_dripstone` — в порядке `zx` остаётся 2 блока (вода).

## 4. Результаты G5i (26.3, изоляция по одной фиче)

@@ТАБЛИЦА@@

## 5. Что осталось / ограничения

@@ОГРАНИЧЕНИЯ@@

## 6. Воспроизведение

```bash
make -C libmcgen
python3 libmcgen/tests/g5_features.py --only minecraft:amethyst_geode,minecraft:fossil_upper --report libmcgen/tests/results/g5i-misc-26.3.json
python3 libmcgen/tests/g5_misc_order.py --feature minecraft:large_dripstone         # порядок чанков: устойчивые/неустойчивые клетки
python3 libmcgen/tests/g5_misc_view.py --ref run/gt/26.3/feature_minecraft_ice_spike/<мир> --mcr f.mcr --at X Y Z --r 8 --mode slice
MCGEN_FEATURES_ONLY=minecraft:ice_spike MCGEN_FEATURES_SEQ=xz MCGEN_FEATURES_BEFORE="-135,-149>-135,-150" libmcgen/build/mcgen-cli …
```

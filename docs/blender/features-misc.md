# W12 «Подземные, ледяные и особые фичи, Nether/End» (ворота G5-misc)

Отчёт потока W12 аддона «MC Worldgen». Каркас стадии FEATURES, порядок фич и общие типы — `docs/blender/features.md` (W8); растительность — `features-veg.md`, деревья/грибы — `features-trees.md`.
Здесь — всё, что не относится к ним: геоды, дрипстоун, монстр-комнаты, окаменелости, скалк, ледяные фичи, замораживание верхнего слоя, колодцы пустыни, Nether (базальт, дельты, светящийся камень),
End (шипы, острова, шлюзы, хорус), а также «собирательные» типы 26.3 (`overlay`, `template`, `random_neighbor_spread`, `speleothem*`…). Все числа ниже измерены (`libmcgen/tests/g5_features.py`,
`libmcgen/tests/g5_misc_order.py`, `tools/gt/diff.py`); команды воспроизведения — в §6.

## 1. Что сделано (26.3 проверено блок-в-блок; 26.1/26.2/26.4-snapshot-2 — по различиям и проверено на тех же эталонах, §4.3)

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

Общие файлы — только точечные правки: `region.c` (две строки: вызов `features_post_chunk`), `feature.c` (одна строка регистрации + экспериментальные режимы обхода чанков для анализа недетерминизма, см. §4), `surface.c/.h` (экспорт `surface_biome_temperature`, загрузка температур
биомов для всех биомов — причина см. §3.2), `template.h/.c`, `processor.h/.c` (ГСЧ настроек шаблона, §3.3), `tests/g5_features.py` (путь к `mcgen-cli` через `MCGEN_CLI`). ABI `mcgen.h` не менялся.
Не реализованы как неиспользуемые в генерации биомов: `bonus_chest` (только при создании мира `/place`), `end_podium` (бой с драконом), `coral_*`/`bamboo`/`vines`/`root_system`/`vegetation_patch`/`block_column` — группа W10, `tree`/`huge_*` — W11.

## 2. Что изменилось между версиями (важно для «по различиям»)

* **26.1 → 26.2 → 26.3 переписаны на обобщённые типы:** `GlowstoneFeature` → `random_neighbor_spread` (26.3); `BasaltPillarFeature` → `overlay`[`single_block_pillar` + `projected_random_patchy_square` + `simple_block`…] (26.3);
  `BasaltColumnsFeature` → `stepped_column_cluster` внутри `weighted_random_selector` (26.3); `DesertWellFeature` → `overlay`[`template`…] (26.3); `PointedDripstoneFeature`/`DripstoneClusterFeature` (26.1) →
  `speleothem`/`speleothem_cluster` (уже в 26.2, ещё с обёрткой `config`; в 26.1 другие имена ключей — `chance_of_taller_dripstone`, `dripstone_block_layer_thickness`, …). Старые типы 26.1/26.2 реализованы
  в `feature_misc_old.c` и `feature_drip.c` (имена `pointed_dripstone`, `dripstone_cluster`).
* `GeodeFeature` — один алгоритм, шум `createParity` на `float`-стеке в 26.3+ и `NormalNoise` на `double` в 26.1/26.2 (обе ветки в `feature_geode.c`).
* `LargeDripstoneFeature`: смещение ветра ограничено `16 − radius` с 26.2; в 26.1 не ограничено; в 26.1 нет `replaceable_blocks` (тег `dripstone_replaceable_blocks`).
* `SculkSpreader`: в 26.3+ курсор в генерации мира ходит только в радиусе² ≤ 144 от центра (`canMoveToPos`) и гаснет, если допустимого хода нет; в 26.1/26.2 хода без ограничения, но курсор гаснет, если после хода ушёл
  от центра на ≥ 15 по x/z (`closerThan(…, 15.0)`). Обе ветки в `feature_misc_sculk.c`.
* Алгоритмы остальных (`delta_feature`, `netherrack_replace_blobs`, `iceberg`, `spike`, `monster_room`, `fossil`, `underwater_magma`, `multiface_growth`, `end_*`, `chorus_plant`, `freeze_top_layer`, `block_pile`, `fill_layer`,
  `void_start_platform`) в четырёх версиях совпадают с точностью до обёртки `config` (сверено построчно нормализованным diff исходников игры) — реализация общая, `fp_cfg()` снимает обёртку.

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
  вне неустойчивых клеток и вне течения воды/лавы; ненулевое число вне неустойчивых клеток — подозрение на логику, но у хаотичных фич (блобы, скалк, `dripstone_cluster`) остаётся ненулевой остаток, потому что порядок ref не входит в наш набор из 16 порядков (каскад по числу вызовов ГСЧ); надёжное доказательство — миры r = 5 без конфликта порядка и `_rep1` эталона.
* `MCGEN_FEATURES_SEQ=file` + `MCGEN_FEATURES_ORDER=<файл «cx cz»>` — обход в заданном порядке (запись порядка сервера — `tools/gt/jfr_order.py`; подбор порядка под эталон — `libmcgen/tests/g5_misc_ordsearch.py`, §4.5); `MCGEN_FEATURES_OUTER=1|2|3` — эксперимент: внешнее кольцо без surface/карверов (по умолчанию выключен, на результат не влиял); `MCGEN_FEATURES_AREA`/`MCGEN_FEATURES_LASTD`/`MCGEN_FEATURES_RING` — §3.6.
* `libmcgen/tests/g5_misc_view.py` — просмотр колонки/среза эталон|наше вокруг точки.
Примеры: `ice_spike` — расхождение исчезает (411 → 0), если пара соседних чанков обработана в обратном порядке; `chorus_plant` — тоже; `large_dripstone` — в порядке `zx` остаётся 2 блока (вода).

### 3.5. Пузырьковые столбцы над магмой (пост-обработка)
`underwater_magma` в эталоне сопровождается `bubble_column[drag=true]` над каждым блоком магмы, у которого выше — источник воды (`LiquidBlock.onPlace/tick` → `BubbleColumnBlock.updateColumn`; для песка душ — `drag=false`).
Измерено на эталоне: **прямо над магмой воды не остаётся ни разу** (41 столбец, высота 1–18), в том числе у краевых чанков области, которые не проходят пост-обработку жидкостей по `pp-margin`. Реализовано отдельным шагом
`features_post_chunk()` (feature_miscx.c), вызываемым из `region_postprocess` (region.c, две строки) для всех чанков региона при стадии FEATURES: вода-источник `water[level=0]` над магмой/песком душ и выше по столбцу заменяется
на пузыри, пока столбец состоит из источников воды/пузырей. Эффект: изоляция `underwater_magma` 279 + 12 → 0 блоков (оба мира, края включены); в сборе `features` (Overworld r = 10) исчезли `water → bubble_column`:
1 208 / 1 310 / 947 блоков (s12345 / s8675309 / s−7048…); общая доля s12345 99,7415 → 99,7443 %, s8675309 99,864 → 99,867 %.

### 3.6. Порядок декорации чанков у края области: две волны
Измерено на эталоне `features` (Overworld, r = 10, 441 чанк): игра декорирует чанки «полной» области (до r + 2 от билета) в порядке «x внешний, z внутренний», а **внешнее кольцо r + 3 — после всех остальных**.
В `feature.c` это две волны: сначала чанки на расстоянии (Чебышёв) < `MCGEN_FEATURES_LASTD` (по умолчанию 3) от «области билета», затем остальные; внутри волны порядок (x, затем z), параллельный обход по диагональному
фронту t = iz + 3·ix даёт тот же результат, что последовательный. «Область билета» по умолчанию — запрошенный регион; `MCGEN_FEATURES_AREA=x0,z0,x1,z1` (в чанках) задаёт её явно, а `tools/gt/run_gate.py` теперь
выставляет её равной центр ± радиус эталона (иначе кольцо pp-margin считалось бы частью области). Значение `none` — одна волна (прежнее поведение). Эффект: расхождения Overworld s12345 в сборе 66 913 → 54 901 блоков;
кольцо декорации (`MCGEN_FEATURES_RING`, по умолчанию 1) больше 1 результат не меняет (проверено на `lush_caves_clay`: 1 030 при кольце 1…5), кольцо 0 — хуже (1 837).
Порядок «внутри» области всё равно зависит от мира (у отдельных миров эталона лучше подходит z-затем-x, а полная запись порядка сервера, `tools/gt/jfr_order.py`, воспроизводится через
`MCGEN_FEATURES_SEQ=file MCGEN_FEATURES_ORDER=<файл «cx cz»>`), поэтому хаотичные фичи (§4.1) воспроизводятся до уровня шума игры, но не побитно.

## 4. Результаты G5i (изоляция по одной фиче)

### 4.0. 26.3 (68 прогонов, 38 placed_feature; `--stable`: клетки, где эталон не совпадает с его повтором `_rep1/_rep2`, маскируются)

| placed_feature | измерение | миров | блоков сравнено | эффект в эталоне | расхождений | худший мир | примечание |
|---|---|---:|---:|---:|---:|---:|---|
| `amethyst_geode` | overworld | 1 | 11 894 781 | 12 651 | 0 | 100.000000 % |  |
| `blue_ice` | overworld | 2 | 23 789 553 | 810 | 0 | 100.000000 % |  |
| `desert_well` | overworld | 2 | 208 600 978 | 216 | 2 | 99.999999 % | 2 блока воды (растекание игры) |
| `dripstone_cluster` | overworld | 1 | 11 892 477 | 39 153 | 840 | 99.992937 % | порядок чанков (каскад по числу вызовов ГСЧ); эталон против своего повтора — 2 278 |
| `forest_rock` | overworld | 1 | 11 894 761 | 2 880 | 0 | 100.000000 % |  |
| `fossil_lower` | overworld | 2 | 94 568 416 | 546 | 0 | 100.000000 % |  |
| `fossil_upper` | overworld | 2 | 94 568 416 | 815 | 0 | 100.000000 % |  |
| `freeze_top_layer` | overworld | 2 | 23 789 557 | 49 237 | 0 | 100.000000 % |  |
| `glow_lichen` | overworld | 1 | 11 894 781 | 204 | 0 | 100.000000 % |  |
| `ice_patch` | overworld | 3 | 98 598 813 | 7 981 | 16 | 99.999865 % | порядок двух соседних дисков (W8) |
| `ice_spike` | overworld | 1 | 11 894 370 | 36 897 | 0 | 100.000000 % |  |
| `iceberg_blue` | overworld | 2 | 23 789 553 | 850 | 0 | 100.000000 % |  |
| `iceberg_packed` | overworld | 2 | 23 789 553 | 8 379 | 0 | 100.000000 % |  |
| `large_dripstone` | overworld | 1 | 11 894 755 | 14 712 | 552 | 99.995359 % | порядок чанков: в порядке zx — 0 вне воды; эталон и повтор совпадают |
| `monster_room` | overworld | 2 | 28 508 134 | 237 | 0 | 100.000000 % |  |
| `monster_room_deep` | overworld | 1 | 11 894 781 | 402 | 0 | 100.000000 % |  |
| `pointed_dripstone` | overworld | 1 | 11 894 755 | 7 504 | 2 | 99.999983 % | 2 блока воды (растекание игры) |
| `sculk_patch_deep_dark` | overworld | 2 | 23 783 781 | 52 820 | 1 446 | 99.987839 % | порядок чанков (патчи пересекаются); эталон против повтора — 4 215  второй мир — 0 при том же порядке |
| `sculk_vein` | overworld | 2 | 23 789 562 | 632 | 0 | 100.000000 % |  |
| `sulfur_pool` | overworld | 2 | 23 789 546 | 27 132 | 5 | 99.999958 % | 5 блоков края озёр (порядок) |
| `sulfur_spike` | overworld | 2 | 23 789 547 | 3 452 | 1 | 99.999992 % | 1 блок (порядок) |
| `sulfur_spike_cluster` | overworld | 2 | 23 789 547 | 15 177 | 0 | 100.000000 % |  |
| `underwater_magma` | overworld | 2 | 23 789 533 | 439 | 0 | 100.000000 % | столбцы пузырьков над магмой — пост-обработка (§3.5) |
| `chorus_plant` | end | 1 | 7 929 856 | 2 531 | 132 | 99.998335 % | порядок пары чанков (-41 -91) (-41 -90): в обратном порядке 0; эталон и повтор совпадают |
| `end_gateway_return` | end | 4 | 128 712 704 | 26 | 0 | 100.000000 % |  |
| `end_island_decorated` | end | 1 | 7 929 856 | 5 592 | 0 | 100.000000 % |  |
| `end_platform` | end | 2 | 13 238 272 | 25 | 0 | 100.000000 % |  |
| `end_spike` | end | 1 | 7 929 856 | 40 665 | 0 | 100.000000 % |  |
| `basalt_blobs` | nether | 2 | 36 530 307 | 5 409 407 | 47 726 | 99.833129 % | порядок чанков: расхождение каскадное (r5-мир 100 %  r10 — 99 83 % при маскировке повтора; эталон против повтора — 300 925) |
| `basalt_pillar` | nether | 2 | 36 831 232 | 1 297 | 0 | 100.000000 % |  |
| `blackstone_blobs` | nether | 2 | 36 790 451 | 2 660 100 | 765 | 99.997349 % | то же (r5 — 100 %; эталон против повтора — 40 781) |
| `delta` | nether | 2 | 36 830 586 | 30 406 | 86 | 99.999702 % | порядок чанков (r5 — 100 %) |
| `glowstone` | nether | 3 | 65 732 608 | 3 198 | 0 | 100.000000 % |  |
| `glowstone_extra` | nether | 3 | 65 732 608 | 628 | 0 | 100.000000 % |  |
| `large_basalt_columns` | nether | 2 | 36 821 912 | 482 881 | 1 431 | 99.995047 % | порядок чанков (r5 — 100 %; эталон против повтора — 9 320) |
| `ore_ancient_debris_large` | nether | 1 | 7 929 856 | 131 | 0 | 100.000000 % |  |
| `ore_debris_small` | nether | 1 | 7 929 856 | 71 | 0 | 100.000000 % |  |
| `small_basalt_columns` | nether | 2 | 36 830 926 | 96 771 | 119 | 99.999588 % | порядок чанков (r5 — 100 %) |

Метод — как в `features.md` §4: датапак эталона оставляет одну `placed_feature`, `--stages 0x17`, `MCGEN_FEATURES_ONLY`, `--pp-margin 1`, блок-в-блок через `tools/gt/diff.py` (маскируются только «текущие» жидкости игры).
«Эффект» — число блоков, которые фича меняет в эталоне (сумма по мирам; 0 — мир ничего не доказывает). Итого: 68 прогонов, 38 placed_feature; без расхождений — 24 placed_feature (все с ненулевым эффектом): `amethyst_geode`, `fossil_upper/lower` (миры r = 5 и r = 14), `monster_room`, `monster_room_deep`,
`freeze_top_layer`, `underwater_magma`, `iceberg_blue/packed`, `blue_ice`, `ice_spike`, `end_spike`, `end_island_decorated`, `end_platform`, `end_gateway_return` (26 блоков в четырёх мирах), `glowstone`, `glowstone_extra`, `basalt_pillar`, `sculk_vein`, `glow_lichen`,
`forest_rock`, `sulfur_spike_cluster`, `ore_ancient_debris_large`, `ore_debris_small`. Остальные 14 placed_feature — недетерминизм порядка чанков и растекание воды игры (примечания в таблице, §3.4, §4.1); у `dripstone_cluster`, `sculk_patch_deep_dark`, `basalt_blobs`, `blackstone_blobs`, `*_basalt_columns`, `delta` расхождение с эталоном **меньше расхождения эталона с его собственным повтором** (§4.1), у `large_dripstone` и `chorus_plant` повтор совпадает с эталоном, а расхождение исчезает при другом порядке пары чанков (zx; обратный).

### 4.1. Недетерминизм как нижняя граница
Расхождение эталона с его же повтором (`_rep1`, тот же сид, тот же датапак) — число блоков, ниже которого побитное совпадение с одним запуском игры недостижимо. Измерено (`tools/gt`-чтение обоих миров, сравнение всех чанков `full`) и сопоставлено с нашим результатом в порядке `xz` без маскировки:

| placed_feature (мир) | эталон ↔ повтор | мы ↔ эталон (xz) | вне «неустойчивых к порядку» клеток |
|---|---:|---:|---:|
| `dripstone_cluster` (s12345 r5) | 2 278 | 1 635 | 291 |
| `sculk_patch_deep_dark` (s12345 r5) | 4 215 | 3 866 | 1 733 |
| `sculk_patch_deep_dark` (s8675309 r5) | 1 566 | 0 | 0 |
| `basalt_blobs` (nether r10) | 300 925 | 175 593 | 134 691 |
| `blackstone_blobs` (nether r10) | 40 781 | 16 871 | 14 767 |
| `large_basalt_columns` (nether r10) | 9 320 | 3 220 | 2 476 |
| `small_basalt_columns` (nether r10) | 306 | 64 | 64 |
| `delta` (nether r10) | 646 | 48 | 46 |
| `ice_spike` (s−7048… r5) | 411 | 411 | 411 (с `_rep1`: 0) |
| `large_dripstone` (s12345 r5) | 0 | 550 | 0 (порядок zx: 0) |
| `chorus_plant` (end r5) | 0 | 132 | 0 (порядок xz−/zx−: 0) |

* `ice_spike`: наш результат совпадает с `_rep1` до блока, т. е. это один из двух исходов самой игры; `chorus_plant`: повтор совпал с эталоном, а обход с обратным порядком пары чанков (-41 -91) (-41 -90) даёт 0 — в игре порядок зависит от планировщика; `large_dripstone` — то же (zx).
* Для хаотичных фич доказательством логики служат миры r = 5 без конфликта порядка (`basalt_blobs`, `blackstone_blobs`, `*_basalt_columns`, `delta`, `sculk_patch_deep_dark` s8675309 — 0 расхождений вне неустойчивых клеток) и `g5_misc_order.py`.
* `dripstone_cluster` (мир s12345, 183 экземпляра во внутренней области): локальная проверка по экземплярам (окно экземпляра ± радиус, все y): **6 изолированных экземпляров** (без другого кластера в радиусе взаимодействия) — 0 расхождений;
  из 177 взаимодействующих 130 совпали полностью, 47 — расхождения порядка (каскад по числу вызовов ГСЧ). Т. е. логика кластера точна; остаток — порядок чанков.
* Сборка (`features`, Overworld r = 10): эталон против `_rep1` у s12345 — 111 908 расхождений (**99,7419 %**), у нас против эталона — 112 046 (99,7415 %) до двухволнового порядка (§3.6), после — см. §4.4.
* End (`features`, r = 10, два мира) — **100 %** (28,9·10⁶ блоков каждый), в том числе хорус, острова и шипы. Nether: расхождения — в основном грибы/гигантские грибы (W11); из моих — `basalt_blobs`/`blackstone_blobs`
  (каскад порядка), у остальных мелочи (magma/gravel у границ).
* Overworld, ледяные/снежные расхождения G5 (`ice → water` 88 980, `snow → air`, `snowy`-трава) устранены (§3.2); в сборе остаются вклады деревьев, мха/глины пещер и общего недетерминизма.

### 4.2. Скорость
`features` (все реализованные фичи, Overworld, 25×25 чанков с окном 27×27, 1 поток, машина под нагрузкой load≈14): декорация 1,93 с = **378 чанков/с на поток**, 8 потоков — 524 чанков/с (упирается в кольца и
карты высот); Nether — 4 962 чанков/с. Стоимость моих фич в этом числе малая (самые дорогие — `freeze_top_layer`: 256 колонок × 2 температуры на чанк, и `large_dripstone`/`geode`: объёмные циклы).


### 4.3. 26.1, 26.2, 26.4-snapshot-2 (те же 24/24/23 фичи группы, изоляция, `libmcgen/tests/results/g5i-misc-<версия>.json`)
Эталоны этих версий (`run/gt/<V>/feature_<id>/`) получены; для 26.1/26.2 повторов `_rep1` нет, поэтому порядко-неустойчивые фичи оценены `g5_misc_order.py` (10 порядков: xz, zx, xz−, zx−, rand1…6): «вне» — расхождения вне клеток, где результаты разных порядков различаются между собой.

| версия | прогонов | без расхождений | расхождения (блоков, порядок xz, без маскировки) |
|---|---:|---:|---|
| 26.1 | 24 | 16 | `basalt_blobs` 134 928, `sculk_patch_deep_dark` 13 510, `large_basalt_columns` 5 079, `dripstone_cluster` 2 415, `large_dripstone` 552, `small_basalt_columns` 149, `delta` 404, `pointed_dripstone` 2 |
| 26.2 | 24 | 16 | `basalt_blobs` 167 513, `sculk_patch_deep_dark` 13 446, `large_basalt_columns` 4 890, `dripstone_cluster` 840, `large_dripstone` 552, `small_basalt_columns` 115, `delta` 432, `pointed_dripstone` 2 |
| 26.4-snapshot-2 | 23 | 15 | `basalt_blobs` 250 487, `sculk_patch_deep_dark` 4 039, `large_basalt_columns` 5 022, `dripstone_cluster` 961, `large_dripstone` 552, `small_basalt_columns` 108, `delta` 358, `pointed_dripstone` 2 |

Остальные фичи версий — **100 %**: 26.1/26.2 — `amethyst_geode`, `basalt_pillar`, `chorus_plant`, `desert_well`, `end_island_decorated`, `end_spike`, `forest_rock`, `fossil_upper`, `freeze_top_layer`, `glowstone`, `glowstone_extra`, `ice_spike`, `iceberg_blue`, `monster_room_deep`, `sculk_vein`, `underwater_magma`;
26.4-snapshot-2 — тот же набор без `forest_rock`. Среди них проверены «рискованные места»: `geode` на `double`-шуме (26.1/26.2), `desert_well`/`basalt_pillar`/`glowstone` старыми классами (26.1/26.2), `pointed_dripstone` с ключами config 26.1 (2 блока воды — как в 26.3). Эталонов остальных placed_feature (`ice_patch`, `sulfur_*`, `end_gateway_return`, `end_platform`, `fossil_lower`, `monster_room`, ледяные `blue_ice`/`iceberg_packed`…) для этих версий нет.
Порядок-зависимые фичи — те же, что в 26.3, расхождения вне порядко-неустойчивых клеток (`g5_misc_order.py`, мир r = 5/10 как в §4.1):

| фича | 26.1 | 26.2 | 26.4-snapshot-2 |
|---|---:|---:|---:|
| `dripstone_cluster` | 83 | 2 | 7 |
| `large_dripstone` | 0 (zx: 0) | 0 (zx: 0) | 0 (zx: 0) |
| `sculk_patch_deep_dark` | 1 380 | 1 422 | 387 |
| `large_basalt_columns` | 146 | 623 | 44 |
| `small_basalt_columns` | 0 | 0 | 0 |
| `delta` | 6 | 0 | 0 |
| `basalt_blobs` | 14 682 | 16 649 | 26 518 |

`sculk_patch_deep_dark` 26.1/26.2 (старая логика курсора, радиус 15, катализатор и шрикеры по `extra_rare_growths`): расхождение в порядке xz 13,4 тыс. блоков (патчи больше и сильнее пересекаются через границы чанков), при учёте порядко-неустойчивых клеток остаётся ≈1,4 тыс. — уровень 26.3 (1 733). Новая логика (`MCGEN_SCULK_NEW=1`) на этих версиях хуже (xz 18 957 против 13 510 в 26.1).

### 4.4. Сборка G5 (`tools/gt/run_gate.py --gate G5 --profile core`, 26.3, r = 10, 625 чанков с margin 2, порог 99,9 %)
Итоговый прогон после всех правок группы (11/12 строк PASS):

| seed | Overworld | Nether | End (main_island / outer_east) |
|---|---:|---:|---:|
| 12345 | **99,91025 %** (54 897 из 61,2·10⁶) PASS | 99,85925 % (57 466) FAIL | 100 % / 100 % |
| 8675309 | **99,95251 %** (29 127) PASS | 99,96922 % (12 587) PASS | 100 % / 100 % |
| −7048155917072976836 | **99,98418 %** (9 710) PASS | 99,97820 % (8 909) PASS | 100 % / 100 % |

Для сравнения, до работ группы (независимый прогон 20:20 UTC): Overworld 99,889 / 99,923 / 99,976 %, а до исправления льда/снега (§3.2) — 99,54–99,61 %. Единственная строка FAIL — Nether s12345: 20 586 `nether_wart_block → air`, 15 030 `air → nether_wart_block`, 3 202 `warped_wart_block → air` — гигантские грибы (`huge_fungus`, группа W11), недетерминизм игры у них такой же (эталон против повтора ≈ 99,88–99,91 %). Фич группы W12 в главных расхождениях нет.

Вклад шагов в Overworld s12345: ice/snow (§3.2) 99,434 → 99,742 %; пузырьковые столбцы (§3.5) +0,003 п. п.; двухволновой порядок (§3.6) 66 913 → 54 901 расхождений; остаток — деревья/листва (W11), мох и глина пещер lush и сколк (порядок чанков, §4.5).

### 4.5. Глина и мох пещер lush: расхождение — только порядок чанков
`lush_caves_clay` (`vegetation_patch`/`waterlogged_vegetation_patch`, W10) в изоляции (s12345, r = 5): 1 030 блоков при полу эталона против повтора 50 (т. е. игра здесь детерминирована). Проверено и **исключено**: число шагов `environment_scan` (+1 шаг — хуже: 2 258), кольцо декорации 1…5 (без изменений), хэш-порядок `HashSet<BlockPos>` (алгоритм патча сверен с исходниками `VegetationPatchFeature`/`WaterloggedVegetationPatchFeature` 26.3), `vertical_range`. Окончательный довод — поиск порядка
(`libmcgen/tests/g5_misc_ordsearch.py`): начиная с обхода zx, жадная перестановка соседних чанков **за четыре шага даёт 0 расхождений вне жидкостей** (556 → 506 → 403 → 184 → 0) и 4 блока воды вместе с жидкостями; итоговый порядок отличается от zx в десяти парах соседних чанков и записан в
`libmcgen/tests/results/order-lush_caves_clay-s12345-c0_0-r5.txt` (воспроизведение: `MCGEN_FEATURES_ONLY=minecraft:lush_caves_clay MCGEN_FEATURES_SEQ=file MCGEN_FEATURES_ORDER=<файл>`). Это означает: логика фичи (позиции попыток, ГСЧ патча, порядок растительности, вода) побитно точна, а остаток — порядок, в котором сервер декорирует соседние чанки (зависит от планировщика; для данного мира он воспроизводим, но не сводится к xz/zx).
Мох (`lush_caves_vegetation`, `lush_caves_ceiling_vegetation`) у эталона нестабилен сам (эталон против повтора 2 375 и 1 895; у нас xz 4 323 и 4 599) — вероятно, тот же механизм (поиск порядка для мха не запускался: у эталона нет стабильного порядка). Чтобы закрыть этот остаток в сборе, нужна запись реального порядка шагов FEATURES сервера для каждого эталонного мира (`tools/gt/jfr_order.py`) и режим `MCGEN_FEATURES_SEQ=file`, а не новая логика фич.

## 5. Что осталось / ограничения

* **Эталонов нет (не проверено блок-в-блок):** `void_start_platform`, `fill_layer`, `block_pile`, `replace_single_block` (в генерации биомов 26.3 не участвуют — только в структурах/пресетах); `end_gateway_return` проверен (4 мира, 26 блоков эффекта, 0 расхождений), `monster_room` на поверхности — 237 блоков эффекта, 0 расхождений.
* **26.1, 26.2, 26.4-snapshot-2:** проверены на эталонах W6 (§4.3): все непорядковые фичи — 100 %, порядко-неустойчивые — на уровне 26.3. Версии без эталонов (отдельные placed_feature, не вошедшие в 24/24/23) остаются непроверенными.
* **Повторить проверки версий:** `python3 libmcgen/tests/g5_features.py --version 26.2 --only <список через запятую> --stable --report libmcgen/tests/results/g5i-misc-26.2.json`, затем `g5_misc_order.py --version <V> --feature <id>` для порядко-неустойчивых и `g5_misc_table.py` для таблицы.
* **Недетерминизм порядка чанков:** `basalt_blobs`, `blackstone_blobs`, `*_basalt_columns`, `delta`, `dripstone_cluster`, `sculk_patch` у границ чанков не воспроизводятся блок-в-блок в больших областях — это свойство игры (повтор эталона
  даёт то же). Для таких фич доказательством логики служат миры r = 5 (100 % у `basalt_blobs`, `blackstone_blobs`, `*_basalt_columns`, `delta`, `sculk_patch_deep_dark`) и `g5_misc_order.py`.
* **Глина/мох пещер lush и сколк в сборе:** остаток — порядок декорации соседних чанков, а не логика фич (§4.5: для `lush_caves_clay` порядок из десяти пар даёт 0 расхождений). Закрывается только воспроизведением порядка сервера (запись порядка по каждому эталонному миру), не правкой фич.
* **Растекание воды:** единичные клетки воды (2 блока) у `pointed_dripstone`/`desert_well` — след растекания игры после пометок пост-обработки (область W1/`fluidpp.c`).
* **Приближения:** `underwater_magma.isVisibleFromOutside` — по флагу «полный непрозрачный куб» состояния (`BSF_SOLID_RENDER`), а не по форме окклюзии грани (на данных 26.3 форма магмы совпала блок-в-блок);
  освещение (`getBrightness(BLOCK) < 10`) в `freeze_top_layer` не считается (на этой стадии света нет — как в игре); `scheduleTick` жидкостей у трещин геод и `setEntityId` спавнера/`LootTable` сундука не воспроизводятся (нет блок-сущностей),
  но расход ГСЧ сохранён.
* **Память:** сборка с `-fsanitize=address,undefined` и прогон `features` (0x1f, 8×8 чанков, 4 потока) в 5 областях × 4 версиях (Overworld ×3 — геоды/дрипстоун/скалк/пустыня/окаменелости, Nether, End) — 20 прогонов, ошибок AddressSanitizer
  и UBSan в файлах группы нет (единичные предупреждения UBSan «left shift of negative value» — в `terrain.c`/ядре, не в фичах).
* **Скалк 26.3:** `sculk_patch` не реализует слияние курсоров (в генерации мира игра его и не делает, `isWorldGeneration`).


## 6. Воспроизведение

```bash
make -C libmcgen
python3 libmcgen/tests/g5_features.py --only minecraft:amethyst_geode,minecraft:fossil_upper --stable --report libmcgen/tests/results/g5i-misc-26.3.json   # --stable: маскировать недетерминизм по _rep1/_rep2
python3 tools/gt/run_gate.py --gate G5 --profile core --no-doc                          # G5 в сборе (двухволновой порядок, §3.6)
python3 libmcgen/tests/g5_misc_order.py --feature minecraft:large_dripstone         # порядок чанков: устойчивые/неустойчивые клетки
python3 libmcgen/tests/g5_misc_ordsearch.py --feature minecraft:lush_caves_clay --world s12345-c0_0-r5 --out order.txt   # подбор порядка чанков, при котором фича совпадает с эталоном побитно
python3 libmcgen/tests/g5_misc_view.py --ref run/gt/26.3/feature_minecraft_ice_spike/<мир> --mcr f.mcr --at X Y Z --r 8 --mode slice
MCGEN_FEATURES_ONLY=minecraft:ice_spike MCGEN_FEATURES_SEQ=xz MCGEN_FEATURES_BEFORE="-135,-149>-135,-150" libmcgen/build/mcgen-cli …
```

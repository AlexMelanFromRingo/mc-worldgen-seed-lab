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
* `libmcgen/tests/g5_misc_view.py` — просмотр колонки/среза эталон|наше вокруг точки.
Примеры: `ice_spike` — расхождение исчезает (411 → 0), если пара соседних чанков обработана в обратном порядке; `chorus_plant` — тоже; `large_dripstone` — в порядке `zx` остаётся 2 блока (вода).

### 3.5. Пузырьковые столбцы над магмой (пост-обработка)
`underwater_magma` в эталоне сопровождается `bubble_column[drag=true]` над каждым блоком магмы, у которого выше — источник воды (`LiquidBlock.onPlace/tick` → `BubbleColumnBlock.updateColumn`; для песка душ — `drag=false`).
Измерено на эталоне: **прямо над магмой воды не остаётся ни разу** (41 столбец, высота 1–18), в том числе у краевых чанков области, которые не проходят пост-обработку жидкостей по `pp-margin`. Реализовано отдельным шагом
`features_post_chunk()` (feature_miscx.c), вызываемым из `region_postprocess` (region.c, две строки) для всех чанков региона при стадии FEATURES: вода-источник `water[level=0]` над магмой/песком душ и выше по столбцу заменяется
на пузыри, пока столбец состоит из источников воды/пузырей. Эффект: изоляция `underwater_magma` 279 + 12 → 0 блоков (оба мира, края включены); в сборе `features` (Overworld r = 10) исчезли `water → bubble_column`:
1 208 / 1 310 / 947 блоков (s12345 / s8675309 / s−7048…); общая доля s12345 99,7415 → 99,7443 %, s8675309 99,864 → 99,867 %.

## 4. Результаты G5i (26.3, изоляция по одной фиче)

| placed_feature | измерение | миров | блоков сравнено | эффект в эталоне | расхождений | худший мир | примечание |
|---|---|---:|---:|---:|---:|---:|---|
| `amethyst_geode` | overworld | 1 | 11 894 781 | 12 651 | 0 | 100.000000 % |  |
| `blue_ice` | overworld | 2 | 23 789 553 | 810 | 0 | 100.000000 % |  |
| `desert_well` | overworld | 2 | 208 600 978 | 216 | 2 | 99.999999 % | 2 блока воды (растекание) |
| `dripstone_cluster` | overworld | 1 | 11 894 755 | 39 153 | 1 673 | 99.985935 % | порядок чанков (хаотичный каскад по числу вызовов ГСЧ) |
| `forest_rock` | overworld | 1 | 11 894 761 | 2 880 | 0 | 100.000000 % |  |
| `fossil_lower` | overworld | 1 | 11 894 781 | 33 | 0 | 100.000000 % |  |
| `fossil_upper` | overworld | 1 | 11 894 781 | 43 | 0 | 100.000000 % |  |
| `freeze_top_layer` | overworld | 2 | 23 789 557 | 49 237 | 0 | 100.000000 % |  |
| `glow_lichen` | overworld | 1 | 11 894 781 | 204 | 0 | 100.000000 % |  |
| `ice_patch` | overworld | 3 | 98 598 813 | 7 981 | 16 | 99.999865 % | порядок двух соседних дисков (W8) |
| `ice_spike` | overworld | 1 | 11 894 781 | 36 897 | 411 | 99.996545 % | эталон против rep1 = ровно наши 411 блоков (недетерминизм игры); обратный порядок пары чанков — 0 |
| `iceberg_blue` | overworld | 2 | 23 789 553 | 850 | 0 | 100.000000 % |  |
| `iceberg_packed` | overworld | 2 | 23 789 553 | 8 379 | 0 | 100.000000 % |  |
| `large_dripstone` | overworld | 1 | 11 894 755 | 14 712 | 552 | 99.995359 % | порядок чанков: в порядке zx — 2 блока (вода) |
| `monster_room` | overworld | 1 | 11 894 781 | 0 | 0 | 100.000000 % |  |
| `monster_room_deep` | overworld | 1 | 11 894 781 | 402 | 0 | 100.000000 % |  |
| `pointed_dripstone` | overworld | 1 | 11 894 755 | 7 504 | 2 | 99.999983 % | 2 блока воды (растекание игры) |
| `sculk_patch_deep_dark` | overworld | 2 | 23 789 562 | 52 820 | 3 866 | 99.967498 % | порядок чанков (курсоры ≤ 12 блоков  патчи пересекаются); второй мир — 100 % |
| `sculk_vein` | overworld | 2 | 23 789 562 | 632 | 0 | 100.000000 % |  |
| `sulfur_pool` | overworld | 2 | 23 789 546 | 27 132 | 5 | 99.999958 % | 5 блоков края озёр (порядок) |
| `sulfur_spike` | overworld | 2 | 23 789 547 | 3 452 | 1 | 99.999992 % | 1 блок (порядок) |
| `sulfur_spike_cluster` | overworld | 2 | 23 789 547 | 15 177 | 0 | 100.000000 % |  |
| `underwater_magma` | overworld | 2 | 23 789 533 | 439 | 0 | 100.000000 % | столбцы пузырьков над магмой — пост-обработка (`features_post_chunk`  §3.5); без неё 279 + 12 блоков `water → bubble_column` |
| `chorus_plant` | end | 1 | 7 929 856 | 2 531 | 132 | 99.998335 % | порядок пары чанков (-41 -91) (-41 -90); в обратном порядке 0; эталон и его rep1 совпадают |
| `end_gateway_return` | end | 2 | 118 095 872 | 0 | 0 | 100.000000 % |  |
| `end_island_decorated` | end | 1 | 7 929 856 | 5 592 | 0 | 100.000000 % |  |
| `end_platform` | end | 2 | 13 238 272 | 25 | 0 | 100.000000 % |  |
| `end_spike` | end | 1 | 7 929 856 | 40 665 | 0 | 100.000000 % |  |
| `basalt_blobs` | nether | 2 | 36 831 232 | 5 409 407 | 175 748 | 99.391904 % | порядок чанков: `findTarget` зависит от соседних блобов  расхождение каскадное (r5-мир 100 %  r10 — 99 39 %) |
| `basalt_pillar` | nether | 2 | 36 831 232 | 1 297 | 0 | 100.000000 % |  |
| `blackstone_blobs` | nether | 2 | 36 831 232 | 2 660 100 | 16 911 | 99.941487 % | то же (r5 — 100 %  r10 — 99 94 %) |
| `delta` | nether | 2 | 36 831 232 | 30 406 | 403 | 99.998606 % | порядок чанков (r5 — 100 %) |
| `glowstone` | nether | 3 | 65 732 608 | 3 198 | 0 | 100.000000 % |  |
| `glowstone_extra` | nether | 3 | 65 732 608 | 628 | 0 | 100.000000 % |  |
| `large_basalt_columns` | nether | 2 | 36 831 232 | 482 881 | 5 803 | 99.979921 % | порядок чанков (r5 — 100 %) |
| `ore_ancient_debris_large` | nether | 1 | 7 929 856 | 131 | 0 | 100.000000 % |  |
| `ore_debris_small` | nether | 1 | 7 929 856 | 71 | 0 | 100.000000 % |  |
| `small_basalt_columns` | nether | 2 | 36 831 232 | 96 771 | 293 | 99.998986 % | порядок чанков (r5 — 100 %) |

Метод — как в `features.md` §4: датапак эталона оставляет одну `placed_feature`, `--stages 0x17`, `MCGEN_FEATURES_ONLY`, `--pp-margin 1`, блок-в-блок через `tools/gt/diff.py` (маскируются только «текущие» жидкости игры).
«Эффект» — число блоков, которые фича меняет в эталоне (сумма по мирам; 0 — мир ничего не доказывает). Итого: 63 мира-прогона, 38 placed_feature; без расхождений — 23 (21 из них с ненулевым эффектом; `end_gateway_return` и `monster_room` пока без эффекта): `amethyst_geode`, `fossil_upper/lower`, `monster_room_deep`,
`freeze_top_layer`, `underwater_magma`, `iceberg_blue/packed`, `blue_ice`, `end_spike`, `end_island_decorated`, `end_platform`, `glowstone`, `glowstone_extra`, `basalt_pillar`, `sculk_vein`, `glow_lichen`, `forest_rock`, `sulfur_spike_cluster`,
`ore_ancient_debris_large`, `ore_debris_small`. Остальные 15 placed_feature (15 прогонов) — недетерминизм порядка чанков и растекание воды/пузырьков игры (примечания в таблице, §3.4, §4.1).

### 4.1. Недетерминизм как нижняя граница
* `ice_spike`: расхождение эталона с его же повтором (`_rep1`) = **411 блоков, те же пары**, что у нас с эталоном, т. е. наш результат — один из двух исходов самой игры; `chorus_plant`: повтор совпал с эталоном, а обход
  с обратным порядком одной пары чанков даёт 0 — в игре порядок зависит от планировщика.
* `dripstone_cluster` (мир s12345, 183 экземпляра во внутренней области): локальная проверка по экземплярам (окно экземпляра ± радиус, все y): **6 изолированных экземпляров** (без другого кластера в радиусе взаимодействия) — 0 расхождений;
  из 177 взаимодействующих 130 совпали полностью, 47 — расхождения порядка (каскад по числу вызовов ГСЧ). Т. е. логика кластера точна; остаток — порядок чанков.
* Сборка (`features`, Overworld r = 10): эталон против `_rep1` у s12345 — 111 908 расхождений (**99,7419 %**), у нас против эталона — 112 046 (99,7415 %): мы на уровне собственного шума игры; основной вклад —
  мох/глина/деревья/скалк (чужие и общие хаотичные группы).
* End (`features`, r = 10, два мира) — **100 %** (28,9·10⁶ блоков каждый), в том числе хорус, острова и шипы. Nether: расхождения — в основном грибы/гигантские грибы (W11); из моих — `basalt_blobs`/`blackstone_blobs`
  (каскад порядка), у остальных мелочи (magma/gravel у границ).
* Overworld, ледяные/снежные расхождения G5 (`ice → water` 88 980, `snow → air`, `snowy`-трава) устранены (§3.2); сейчас в сборе остаются только вклады деревьев и общего недетерминизма.

### 4.2. Скорость
`features` (все реализованные фичи, Overworld, 25×25 чанков с окном 27×27, 1 поток, машина под нагрузкой load≈14): декорация 1,93 с = **378 чанков/с на поток**, 8 потоков — 524 чанков/с (упирается в кольца и
карты высот); Nether — 4 962 чанков/с. Стоимость моих фич в этом числе малая (самые дорогие — `freeze_top_layer`: 256 колонок × 2 температуры на чанк, и `large_dripstone`/`geode`: объёмные циклы).


## 5. Что осталось / ограничения

* **Эталонов нет (не проверено блок-в-блок):** `end_gateway_return` (в обоих 26.3-мирах эффект 0; наш libmcgen ставит шлюзы в чанках (57,46) и (0,70) сида 12345 — миры запрошены у W6), `monster_room` на поверхности
  (эффект 0, та же логика проверена на `monster_room_deep`), `void_start_platform`, `fill_layer`, `block_pile`, `replace_single_block` (в генерации биомов 26.3 не участвуют — только в структурах/пресетах).
* **26.1, 26.2, 26.4-snapshot-2:** реализация по различиям (§2) собрана, все placed_feature версий разбираются (`MCGEN_FEATURES_DUMP_ORDER`: 203/207/210/210 из 203/207/210/210 placed_feature в 26.1/26.2/26.3/26.4 имеют реализованный тип; прогоны `features` по трём измерениям каждой версии — без ошибок и зависаний), но эталонных `feature_<id>` для этих версий у W6 пока нет
  (очередь поставлена: 23 фичи моей группы, затем rep1 для порядко-неустойчивых). Точность для них не заявляется. Особо рискованные места: `geode` на `double`-шуме (26.1/26.2), `sculk_patch` с радиусом ходов 15 (26.1/26.2),
  `large_dripstone` без ограничения ветра (26.1), `desert_well`/`basalt_*`/`glowstone` старыми классами.
* **Как закрыть проверки версий, когда у W6 появятся миры:** `python3 libmcgen/tests/g5_features.py --version 26.2 --only minecraft:glowstone,minecraft:large_dripstone,… --report libmcgen/tests/results/g5i-misc-26.2.json`
  (то же для 26.1 и 26.4-snapshot-2), затем `g5_misc_order.py --version <V> --feature <id>` для порядко-неустойчивых и `g5_misc_table.py` для таблицы. Ожидаемые первые подозреваемые при расхождении: 26.1 — `dripstone_cluster`/`pointed_dripstone`
  (ключи config), `large_dripstone` (ветер); 26.1/26.2 — `geode` (double-шум), `sculk_patch` (радиус 15), `desert_well`, `basalt_columns`, `glowstone_blob`.
* **Недетерминизм порядка чанков:** `basalt_blobs`, `blackstone_blobs`, `*_basalt_columns`, `delta`, `dripstone_cluster`, `sculk_patch` у границ чанков не воспроизводятся блок-в-блок в больших областях — это свойство игры (повтор эталона
  даёт то же). Для таких фич доказательством логики служат миры r = 5 (100 % у `basalt_blobs`, `blackstone_blobs`, `*_basalt_columns`, `delta`, `sculk_patch_deep_dark`) и `g5_misc_order.py`.
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
python3 libmcgen/tests/g5_features.py --only minecraft:amethyst_geode,minecraft:fossil_upper --report libmcgen/tests/results/g5i-misc-26.3.json
python3 libmcgen/tests/g5_misc_order.py --feature minecraft:large_dripstone         # порядок чанков: устойчивые/неустойчивые клетки
python3 libmcgen/tests/g5_misc_view.py --ref run/gt/26.3/feature_minecraft_ice_spike/<мир> --mcr f.mcr --at X Y Z --r 8 --mode slice
MCGEN_FEATURES_ONLY=minecraft:ice_spike MCGEN_FEATURES_SEQ=xz MCGEN_FEATURES_BEFORE="-135,-149>-135,-150" libmcgen/build/mcgen-cli …
```

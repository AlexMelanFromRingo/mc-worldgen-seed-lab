# W8 «Декорации»: каркас стадии FEATURES, минеральные и «блоб-подобные» фичи (ворота G5)

Отчёт потока W8 аддона «MC Worldgen». Спецификация — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md` (G5), справка по игре —
`docs/00-worldgen-guide.md` §15, правила размещения — `docs/08-height-temperature-rules.md`. Все числа ниже измерены; команды воспроизведения — в §6.

## 1. Итог

| что | результат |
|---|---|
| Каркас стадии `MC_STAGE_FEATURES` (M1) | готов для 26.1/26.2/26.3/26.4: окно чанков (`WorldGenRegion`), `applyBiomeDecoration`, `FeatureSorter`, 18 модификаторов размещения, провайдеры значений/состояний, предикаты, реестр типов фич |
| Порядок фич по шагам (глобальный индекс — в seed каждой фичи) | **9 из 9** выгрузок реального кода совпали (26.1: 164, 26.2: 168, 26.3: 171 фича Overworld; Nether 37, End 5; `tests/g5_order.py`) |
| Реализованные типы (M2) | `ore`, `scattered_ore`, `disk`, `block_blob`, `spring_feature`, `lake`, `simple_block`, `sequence`, `random_selector`, `simple_random_selector`, `random_boolean_selector`, `weighted_random_selector`, `no_op` |
| **G5i** — каждая фича в изоляции против настоящих чанков сервера 26.3 (`run/gt/26.3/feature_*`) | **72 мира, 54 фичи, 1 049 458 523 блоков сравнено, 18 расхождений.** 50 фич совпали на 100 % при непустом эффекте (суммарно 814 114 блоков, изменённых фичами в эталоне); `ice_patch` — 16 блоков из 7 981 (перестановка двух соседних чанков, недетерминизм игры, §5.2); `ore_copper_large` — 2 блока из 11 714 (растекание воды, не фича); `lake_lava_surface`, `spring_lava_frozen` — в имеющихся мирах эффект 0 (проверка пустая; запрошены другие области у W6) |
| G5 «в сборе» (`features`, r = 10, 441 чанк на мир, **реализовано 108 из 171 фичи Overworld и 25 из 37 Nether, остальное — деревья, трава-блок-колонны, геоды… не поставлено**) | Overworld: 99,10 / 99,56 / 99,61 % блоков (3 seed), Nether: 99,38 / 99,47 / 99,72 %. Порог ≥ 99,9 % достижим только после остальных групп фич, и даже тогда ограничен недетерминизмом игры (повтор эталона: 0,25 % блоков) |
| Скорость стадии (26.3, Overworld, область 32×32 + кольцо, все стадии 0x1f, 12 потоков) | декорация одна: **4 515 чанков/с** (1 поток — 920); вся стадия FEATURES с кольцами соседей 0,91 с на 1 156 чанков; полный конвейер 64×64 чанков — 26 с (12 потоков, машина занята другими потоками) |


## 2. Где код

| файл | что |
|---|---|
| `libmcgen/src/feature.h` | внутренний интерфейс каркаса: ГСЧ декорации, окно чанков, провайдеры, предикаты, реестр типов фич |
| `libmcgen/src/feature.c` | стадия `MC_STAGE_FEATURES`: окно чанков региона, цикл `applyBiomeDecoration`, параллельный обход волнами, реестр типов (`feature_register_all`), разбор конфигураций из JSON, мир фич (`FWorld`) |
| `libmcgen/src/feature_sort.c` | списки фич биомов и `FeatureSorter` (глобальный индекс фич по шагам) |
| `libmcgen/src/feature_region.c` | «WorldGenRegion»: чтение/запись блоков окна 3×3, карты высот (6 типов, ленивые WG), биом блока (BiomeManager) |
| `libmcgen/src/feature_prov.c` | IntProvider / FloatProvider / HeightProvider / VerticalAnchor / WeightedList, `Mth.sin`, шум `BIOME_INFO_NOISE` |
| `libmcgen/src/feature_bpred.c` | BlockPredicate, RuleTest, `BlockState.canSurvive` (для нужных классов) |
| `libmcgen/src/feature_bsp.c` | BlockStateProvider (simple, weighted, rule_based, randomized_int, rotated, noise, noise_threshold, dual_noise) |
| `libmcgen/src/placement.c` | PlacementModifier'ы и `FeaturePlacer` |
| `libmcgen/src/feature_ore.c` | `ore`, `scattered_ore` |
| `libmcgen/src/feature_disk.c` | `disk`, `block_blob`, `spring_feature`, `lake` |
| `libmcgen/src/feature_misc.c` | `simple_block`, `sequence`, `random_selector`, `simple_random_selector`, `random_boolean_selector`, `weighted_random_selector`, `no_op` |
| `libmcgen/src/blockstate.[ch]` | свойства состояний блока (`set_value` по имени), состояние по умолчанию, состояния из JSON всех форматов, свойства из `reports/block_flags.json` |
| `libmcgen/tests/g5_blockflags/BlockFlags.java`, `tests/g5_blockflags.py` | выгрузка закодированных в Java свойств состояний блоков из настоящих классов игры |
| `libmcgen/tests/g5_features.py` | ворота G5i: изолированные фичи против `run/gt/<V>/feature_<id>/` |
| `libmcgen/tests/g5_order.py` | порядок фич по шагам против выгрузки игры `data/featureorder-<V>.json` |
| `libmcgen/tests/g5_table.py` | таблица §7 |

Правки общих файлов (минимальные): `mcgen_internal.h` (поля `McGen.bs_tab`, `McWorld.features`, прототипы), `region.c` (стадия в `generate()`, доступ к стадиям/пометкам региона),
`pack.c`/`world.c` (освобождение). ABI `mcgen.h` не менялся.

## 3. Как устроено

### 3.1. Положение в конвейере
`mcgen_generate_region(…, stages & MC_STAGE_FEATURES)`: `region.c` сначала считает чанки региона (BIOMES → TERRAIN → SURFACE → CARVERS по запрошенным стадиям,
пул потоков), затем вызывает `features_apply_region()` (feature.c) и только потом растекание жидкостей и карты высот. `FEATURES` включает `TERRAIN`+`BIOMES`,
но не включает `SURFACE`/`CARVERS` — так получается и эталон `feature:<id>` (поверхность без карверов: стадии `0x17`), и полный `features` (`0x1f`).

`features_apply_region` строит **окно чанков**: чанки региона (их блоки/биомы/пометки — прямо в `McRegion`) + кольцо в 2 чанка вокруг, которое
дособирается теми же стадиями (параллельно). Декорация применяется к чанкам региона и к кольцу шириной 1 (их фичи заходят в регион); кольцо шириной 2 даёт им
исходные блоки (как «соседние чанки» игры). Читать и писать можно только в 3×3 чанка вокруг обрабатываемого (как `WorldGenRegion`, `ensureCanWrite`);
чтение вне окна даёт воздух, запись вне окна игнорируется.

### 3.2. Цикл чанка (`ChunkGenerator.applyBiomeDecoration`)
1. `WorldgenRandom` на Xoroshiro: `setDecorationSeed(McSeeds.features, x0, z0)` → `decSeed`; для каждой фичи шага `setFeatureSeed(decSeed, индекс, шаг)` =
   `seed = decSeed + индекс + 10000·шаг`. **Важно:** `WorldgenRandom` наследует `LegacyRandomSource`, поэтому `nextInt(bound)`, `nextLong`, `nextDouble`, `nextBoolean`
   строятся на `next(bits) = (int)(xoroshiro.nextLong() >>> 64 − bits)` по алгоритмам `BitRandomSource` (например `nextDouble` — две выборки, `nextBoolean` — старший бит),
   а не на собственных методах `XoroshiroRandomSource`; состояние гауссиана не сбрасывается `setSeed` (`FRnd` в feature.h).
2. Множество биомов — из палитр биомов чанков окна 3×3 (пересечение с биомами источника). Для шага: объединение списков фич этих биомов, сортировка по глобальному индексу шага.
3. `FeatureSorter.buildFeaturesPerStep` (feature_sort.c): списки фич всех биомов источника (порядок `possibleBiomes()`: первое вхождение в таблице параметров; End — 5 биомов в порядке
   игры), обход в глубину с рёбрами «фича → следующая в списке биома», вершины/рёбра в порядке (шаг, индекс первого обнаружения), результат — обратный порядок завершения.
   Проверка: 9 из 9 (26.1/26.2/26.3 × 3 измерения) совпадений с выгрузкой реального кода (`tests/g5_order.py`, 164/168/171 фич Overworld).
4. **Порядок чанков.** Фичи соседних чанков, пересекающиеся в пространстве (руды разных видов, диски, озёра), порождают порядок-зависимый результат; игра при этом
   недетерминирована у границ чанков (повтор `features` Overworld r=10: 0,25 % блоков, W6, `ground-truth.md` §2.4). Мы обходим чанки как билеты `forceload` (x внешний, z внутренний;
   `ForceLoadCommand`), параллельно «волнами» `t = iz + 3·ix`: чанки одной волны не пересекаются окнами 3×3 (|dx| ≥ 3), а все пересекающиеся более ранние чанки имеют меньшее `t`,
   поэтому результат побитово равен последовательному обходу при любом числе потоков (проверено `cmp`). Перебор порядков на r=10 по пересечению множеств блоков угля
   (`tests/g5_blockset.py`): x,z — 91,9 %; z,x — 90,4 %; от центра кольцами — 87,0 %; обратный порядок — 73,4 %; т. е. «вперёд» верно по существу, остаток — недетерминизм игры.
   Единственный случай расхождения изолированной фичи (`ice_patch`, 16 блоков из 7981) — именно перестановка двух соседних чанков (подтверждено трассировкой `MCGEN_TRACE_PM`).
5. `PlacedFeature` (placement.c): цепочка `PlacementModifier` (count, count_on_every_layer, noise_based_count, noise_threshold_count, rarity_filter, random_chance,
   in_square, height_range (uniform/biased/very_biased/trapezoid/weighted_list/constant; якоря absolute/above_bottom/below_top/relative_to_sea_level),
   heightmap (все 6 типов), biome, environment_scan, surface_relative_threshold_filter, surface_water_depth_filter, block_predicate_filter, offset/random_offset,
   fixed_placement, randomly_selected, cuboid), обход в глубину (`FeaturePlacer`), `placeWithBiomeCheck` для верхней фичи.

### 3.3. Карты высот (важно для `heightmap`-модификаторов и руд)
Шесть типов `Heightmap.Types`. «Финальные» (`WORLD_SURFACE`, `OCEAN_FLOOR`, `MOTION_BLOCKING`, `MOTION_BLOCKING_NO_LEAVES`) вычисляются по блокам после карверов и обновляются на
каждой записи через `fc_set` (точно `Heightmap.update`); запись «секцией напрямую» (`fc_set_raw`: руды, `OreFeature`) карты не трогает, как в игре. **WG-карты (`*_WG`)**:
в 26.3 при первом запросе чанка (после уже сделанных соседями записей) и больше не обновляются; в 26.1/26.2 вычислены по блокам после карверов и заморожены.
Предикаты «непрозрачности»: 26.3 — теги `blocks_motion_in_heightmap[_no_leaves]` пака, 26.1/26.2 — `blocksMotion()` из `block_flags.json`.

### 3.4. Данные блоков, закодированные в Java (`reports/block_flags.json`)
Пакет `run/pack-<V>/reports` не содержит `isSolid`, `canBeReplaced`, жидкости состояния, «прочных» граней и т. п. Их выгружает
`python3 libmcgen/tests/g5_blockflags.py <V>` (Java-класс запускается на `game-<V>.jar` пользователя; в репозитории — только код). Без файла включаются грубые эвристики.
**Для интеграции:** `tools/make_pack.py` должен вызывать этот скрипт после `--reports`.

### 3.5. Как добавить…
* **тип фичи** — функция `parse(FParse*, cfg)` (cfg — объект «config» в 26.1/26.2 или сама фича в 26.3) и `place(FCtx*, cfg, x, y, z)`; объявить `FeatType` и добавить
  `feature_register_<группа>()` в `feature_register_all()` (feature.c); файл — `feature_<группа>.c`. Вложенные фичи/провайдеры — `fp_placed`, `fp_feature`, `fp_bsprov`, `fp_bpred`,
  `fp_intprov`. Блоки читать/писать через `fc_get`/`fc_set(…, flags)` (флаг 2/3 — как в игре) и `fc_set_raw`; пометки пост-обработки — `fc_mark_above`.
  Нереализованный тип не ломает порядок: фича занимает свой индекс, но ничего не ставит (в таблице §7 — «не начата»/«частично»).
* **PlacementModifier** — ветка в `parse_one` и `pm_run` (placement.c).
* **BlockPredicate / RuleTest / BlockStateProvider** — feature_bpred.c / feature_bsp.c; **`canSurvive` блока** — `block_can_survive` (по классу Java из `block_flags.json`, `bs_is_a`).
* **изоляция фичи при отладке** — `MCGEN_FEATURES_ONLY=minecraft:ore_diamond[,…]` (остаются только эти placed_feature, индексы пересчитываются, как у датапак-эталона);
  `MCGEN_FEATURES_DEBUG=1` — ошибки разбора и скорость; `MCGEN_FEATURES_DUMP_ORDER=файл` — порядок фич и статус реализации (JSON).

## 4. Результаты G5i (изоляция по одной фиче)

Метод: датапак эталона (W6, `tools/gt`, вариант `feature:<id>`) оставляет у биомов одну `placed_feature`, поверхность настоящая, карверов и построек нет; libmcgen —
`--stages 0x17` (BIOMES|TERRAIN|SURFACE|FEATURES) с `MCGEN_FEATURES_ONLY=<id>`, `--pp-margin 1`, сравнение блок-в-блок (`tools/gt/diff.py`, область = все чанки статуса full, маски
только «текущих» жидкостей игры). «Эффект» — число блоков, которые фича меняет в эталоне (сравнение эталона с нашим дампом без стадии FEATURES): если 0, мир ничего не доказывает.
Воспроизведение: `python3 libmcgen/tests/g5_features.py --report libmcgen/tests/results/g5i-26.3.json` (JSON с результатами — там же).

| placed_feature | измерение | миров | блоков сравнено | эффект в эталоне | расхождений | совпадение |
|---|---|---:|---:|---:|---:|---:|
| `disk_clay` | overworld | 3 | 35 684 316 | 201 | 0 | 100 % |
| `disk_grass` | overworld | 1 | 11 894 767 | 14 798 | 0 | 100 % |
| `disk_gravel` | overworld | 3 | 35 684 316 | 771 | 0 | 100 % |
| `disk_sand` | overworld | 3 | 35 684 316 | 2 946 | 0 | 100 % |
| `forest_rock` | overworld | 1 | 11 894 761 | 2 880 | 0 | 100 % |
| `ice_patch` | overworld | 3 | 98 598 813 | 7 981 | 16 | 99.99998 % |
| `lake_lava_surface` | overworld | 3 | 98 598 813 | 0 | 0 | 100 % |
| `lake_lava_underground` | overworld | 3 | 98 598 813 | 5 470 | 0 | 100 % |
| `nether_sprouts` | the_nether | 1 | 7 929 856 | 3 223 | 0 | 100 % |
| `ore_ancient_debris_large` | the_nether | 1 | 7 929 856 | 131 | 0 | 100 % |
| `ore_andesite_lower` | overworld | 1 | 11 894 781 | 86 887 | 0 | 100 % |
| `ore_andesite_upper` | overworld | 1 | 11 894 781 | 621 | 0 | 100 % |
| `ore_blackstone` | the_nether | 1 | 7 929 856 | 20 997 | 0 | 100 % |
| `ore_clay` | overworld | 1 | 11 894 767 | 168 236 | 0 | 100 % |
| `ore_coal_lower` | overworld | 1 | 11 894 781 | 8 856 | 0 | 100 % |
| `ore_coal_upper` | overworld | 3 | 35 684 343 | 961 | 0 | 100 % |
| `ore_copper` | overworld | 1 | 11 894 770 | 6 889 | 0 | 100 % |
| `ore_copper_large` | overworld | 1 | 11 894 755 | 11 714 | 2 | 99.99998 % |
| `ore_debris_small` | the_nether | 1 | 7 929 856 | 71 | 0 | 100 % |
| `ore_diamond` | overworld | 2 | 16 711 677 | 994 | 0 | 100 % |
| `ore_diamond_buried` | overworld | 1 | 11 894 781 | 1 110 | 0 | 100 % |
| `ore_diamond_large` | overworld | 1 | 11 894 781 | 89 | 0 | 100 % |
| `ore_diamond_medium` | overworld | 1 | 11 894 781 | 1 111 | 0 | 100 % |
| `ore_diorite_lower` | overworld | 1 | 11 894 781 | 86 887 | 0 | 100 % |
| `ore_diorite_upper` | overworld | 1 | 11 894 781 | 621 | 0 | 100 % |
| `ore_dirt` | overworld | 1 | 11 894 781 | 22 533 | 0 | 100 % |
| `ore_emerald` | overworld | 1 | 11 894 784 | 337 | 0 | 100 % |
| `ore_gold` | overworld | 1 | 11 894 781 | 2 144 | 0 | 100 % |
| `ore_gold_deltas` | the_nether | 1 | 7 929 856 | 417 | 0 | 100 % |
| `ore_gold_extra` | overworld | 1 | 11 894 741 | 4 443 | 0 | 100 % |
| `ore_gold_lower` | overworld | 1 | 11 894 781 | 253 | 0 | 100 % |
| `ore_gold_nether` | the_nether | 1 | 7 929 856 | 7 643 | 0 | 100 % |
| `ore_granite_lower` | overworld | 1 | 11 894 781 | 86 851 | 0 | 100 % |
| `ore_granite_upper` | overworld | 1 | 11 894 781 | 621 | 0 | 100 % |
| `ore_gravel` | overworld | 1 | 11 894 781 | 43 199 | 0 | 100 % |
| `ore_gravel_nether` | the_nether | 1 | 7 929 856 | 19 826 | 0 | 100 % |
| `ore_infested` | overworld | 1 | 11 894 784 | 8 359 | 0 | 100 % |
| `ore_iron_middle` | overworld | 1 | 11 894 781 | 4 167 | 0 | 100 % |
| `ore_iron_small` | overworld | 1 | 11 894 781 | 1 464 | 0 | 100 % |
| `ore_iron_upper` | overworld | 1 | 11 894 781 | 14 | 0 | 100 % |
| `ore_lapis` | overworld | 1 | 11 894 781 | 604 | 0 | 100 % |
| `ore_lapis_buried` | overworld | 1 | 11 894 781 | 1 540 | 0 | 100 % |
| `ore_magma` | the_nether | 1 | 7 929 856 | 17 642 | 0 | 100 % |
| `ore_quartz_deltas` | the_nether | 1 | 7 929 856 | 1 227 | 0 | 100 % |
| `ore_quartz_nether` | the_nether | 1 | 7 929 856 | 22 141 | 0 | 100 % |
| `ore_redstone` | overworld | 1 | 11 894 781 | 1 863 | 0 | 100 % |
| `ore_redstone_lower` | overworld | 1 | 11 894 781 | 2 117 | 0 | 100 % |
| `ore_soul_sand` | the_nether | 1 | 7 929 856 | 9 900 | 0 | 100 % |
| `ore_tuff` | overworld | 1 | 11 894 781 | 118 885 | 0 | 100 % |
| `spring_closed` | the_nether | 1 | 7 928 413 | 1 443 | 0 | 100 % |
| `spring_lava` | overworld | 1 | 11 894 770 | 11 | 0 | 100 % |
| `spring_lava_frozen` | overworld | 4 | 110 493 594 | 0 | 0 | 100 % |
| `spring_open` | the_nether | 1 | 7 929 842 | 14 | 0 | 100 % |
| `spring_water` | overworld | 1 | 11 894 770 | 11 | 0 | 100 % |

Что проверено этими числами (кроме самих фич): порядок и число вызовов ГСЧ (`setDecorationSeed`/`setFeatureSeed`, `BitRandomSource`), глобальный индекс в шаге (у изолированной фичи он 0), `count`/`count_on_every_layer`/`in_square`/
`height_range` (uniform, trapezoid, biased, very_biased)/`heightmap` (в т. ч. ленивые WG-карты 26.3)/`offset`/`rarity_filter`/`environment_scan`/`surface_relative_threshold_filter`/
`block_predicate_filter`, BlockPredicate (`matching_blocks/tag/fluids`, `solid`, `all_of/any_of/not`), RuleTest (`tag_match`, `random_*`, `height_match`, `any_of`), `discard_chance_on_air_exposure`,
запись «секцией напрямую» (руды) и через регион (диски, блобы, озёра, источники, `simple_block`), `markAboveForPostProcessing`, `canSurvive` по тегам (`nether_sprouts`), провайдеры состояний.

### 4.1. Фичи в сборе (`features`, r = 10)
`mcgen-cli --stages 0x1f --pp-margin 1` против `run/gt/26.3/features/*-r10` (все декорации, без построек; реализованы 108/171 Overworld и 25/37 Nether, поэтому расхождения — в основном деревья, трава,
блок-колонны, геоды, мох, скалк и т. п.): Overworld 99,098655 % (s12345), 99,560755 % (s8675309), 99,610887 % (s−7048155917072976836); Nether 99,382456 % / 99,471913 % / 99,716049 %.
Для руд в сборе (пересечение множеств позиций с эталоном, `tests/g5_blockset.py`, s12345): железо 99,89 %, медь 99,93 %, лазурит 99,96 %, алмазы (глубинные) 99,49 %, гравий 99,97 %, гранит 99,67 %, туф 99,99 %, лава 100 %;
уголь 90–92 % (зависит от порядка чанков: 73 % при обратном порядке), остаток объясняется порядком у границ чанков и ещё не реализованными геодами/подземными фичами, идущими раньше руд.

## 5. Решения и особенности

1. **ABI `mcgen.h` не менялся.** Общие файлы — минимальные правки (см. §2). Фильтры/отладка — переменные окружения `MCGEN_FEATURES_*` (не часть контракта).
2. **Порядок чанков — недетерминизм игры.** Фичи соседних чанков, пересекающиеся в пространстве, дают порядок-зависимый результат, а игра выполняет `FEATURES` соседних чанков в порядке, зависящем от потоков
   (повтор эталона: 0,25 % блоков, `ground-truth.md` §2.4). Из перебранных порядков лучшим оказался порядок билетов `forceload` (x, затем z) — он принят по умолчанию; остальные доступны через `MCGEN_FEATURES_SEQ`.
   Изолированные фичи от порядка почти не зависят (`ice_patch`: 16 блоков — один конфликт двух соседних дисков).
3. **`WorldgenRandom` ≠ `XoroshiroRandomSource`** по методам (§3.2 п. 1) — иначе ни одна фича не совпадёт; это самое важное наблюдение каркаса.
4. **Карты высот зависят от версии**: 26.3 праймит `*_WG` при первом запросе и не обновляет; 26.1/26.2 хранят WG-карты после карверов. 26.4 принята как 26.3 (эталона нет).
5. **Освещение в декорациях не считается** (свет = 0, как в игре на этой стадии — освещение считается позже, поэтому условие грибов `getRawBrightness < 13` всегда выполнено). Планируемые тики (`scheduleTick`) не выполняются — как в эталоне с `tick freeze`.
6. **Пометки пост-обработки** (`markAboveForPostProcessing`, `postProcess` блоков) пишутся в `PPMarks` региона, растекание — W1 (`fluidpp.c`); `updateFromNeighbourShapes` для не-жидкостей (потеря растений на диске и т. п.) пока не реализован — нужен группе «растительность».
7. **26.4-snapshot-2**: биомы блоков в игре хранятся поблочно; каркас берёт клетки 4×4×4 региона (как 26.3) — без эталона точность не заявляется; `feature.c` использует `surface_apply_chunk_ex`/`surface_carves_inside`.
8. **Нереализованный тип** не ломает порядок: фича занимает индекс и ничего не ставит; вложенные нереализованные фичи в реализованном селекторе тоже молча пропускаются (в таблице — «частично»).
9. **Проверки версий 26.1/26.2** выполнены на уровне порядка фич (9/9) и разбора всех форматов JSON (`config`-обёртка, `Name`/`Properties`, `configured_feature`); эталонных миров фич для них у W6 пока нет (запрошены `ore_diamond`, `ore_coal_lower`, `disk_grass`, `forest_rock`).

## 6. Воспроизведение

```bash
make -C libmcgen                                    # сборка
python3 libmcgen/tests/g5_blockflags.py 26.3        # reports/block_flags.json (один раз на версию; нужен jar игры)
python3 libmcgen/tests/g5_order.py                  # порядок фич: 9/9
python3 libmcgen/tests/g5_features.py               # G5i: все feature_<id> из run/gt/26.3 (есть --only, --report, --version)
python3 libmcgen/tests/g5_table.py --results libmcgen/tests/results/g5i-26.3.json > таблица.md
# в сборе:
libmcgen/build/mcgen-cli --pack run/pack-26.3 --version 26.3 --seed 12345 --cx0 -10 --cz0 -10 --nx 21 --nz 21 --stages 0x1f --pp-margin 1 --out f.mcr
python3 tools/gt/diff.py --ref run/gt/26.3/features/overworld-s12345-c0_0-r10 --mcr f.mcr --margin 0
```

## 7. Таблица placed_feature × статус (26.3)

Таблица строится из выгрузки libmcgen (`g5_table.py`): «проверена» — изолированная фича совпала с эталоном при ненулевом эффекте; «реализована» — тип и все вложенные типы реализованы, эталона ещё нет;
«частично» — реализован верхний тип, но вложенная фича (дерево, патч, блок-колонна…) не реализована и молча пропускается; «не начата» — тип не реализован. В таблице 210 placed_feature, достижимых из биомов трёх измерений
(остальные из 273 — вложенные в селекторы/постройки).

Всего placed_feature в биомах измерений (26.3): 210; реализована: 91, проверена: 50, не начата: 39, частично: 28, реализована (расхождения): 2

| placed_feature | тип фичи | измерения | шаг | статус | G5i (изоляция) |
|---|---|---|---|---|---|
| `amethyst_geode` | `geode` | overworld | LOCAL_MODIFICATIONS | не начата | — |
| `bamboo` | `bamboo` | overworld | VEGETAL_DECORATION | не начата | — |
| `bamboo_light` | `bamboo` | overworld | VEGETAL_DECORATION | не начата | — |
| `bamboo_vegetation` | `random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `basalt_blobs` | `netherrack_replace_blobs` | nether | UNDERGROUND_DECORATION | реализована | — |
| `basalt_pillar` | `overlay` | nether | LOCAL_MODIFICATIONS | реализована | — |
| `birch_tall` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `blackstone_blobs` | `netherrack_replace_blobs` | nether | UNDERGROUND_DECORATION | реализована | — |
| `blue_ice` | `blue_ice` | overworld | SURFACE_STRUCTURES | не начата | — |
| `brown_mushroom_dappled_forest` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `brown_mushroom_nether` | `simple_block` | nether | UNDERGROUND_DECORATION | реализована | — |
| `brown_mushroom_normal` | `simple_block` | nether,overworld | VEGETAL_DECORATION | реализована | — |
| `brown_mushroom_old_growth` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `brown_mushroom_swamp` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `brown_mushroom_taiga` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `cave_vines` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `chorus_plant` | `chorus_plant` | end | VEGETAL_DECORATION | реализована | — |
| `classic_vines_cave_feature` | `vines` | overworld | VEGETAL_DECORATION | не начата | — |
| `crimson_forest_vegetation` | `simple_block` | nether | VEGETAL_DECORATION | реализована | — |
| `crimson_fungi` | `huge_fungus` | nether | VEGETAL_DECORATION | не начата | — |
| `dark_forest_vegetation` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `delta` | `delta_feature` | nether | SURFACE_STRUCTURES | реализована | — |
| `desert_well` | `overlay` | overworld | SURFACE_STRUCTURES | частично | — |
| `disk_clay` | `disk` | overworld | UNDERGROUND_ORES | проверена | 100 % (168 блоков эффекта), 100 % (33 блоков эффекта) |
| `disk_grass` | `disk` | overworld | UNDERGROUND_ORES | проверена | 100 % (14798 блоков эффекта) |
| `disk_gravel` | `disk` | overworld | UNDERGROUND_ORES | проверена | 100 % (684 блоков эффекта), 100 % (87 блоков эффекта) |
| `disk_sand` | `disk` | overworld | UNDERGROUND_ORES | проверена | 100 % (2389 блоков эффекта), 100 % (557 блоков эффекта) |
| `dripstone_cluster` | `speleothem_cluster` | overworld | UNDERGROUND_DECORATION | не начата | — |
| `end_gateway_return` | `end_gateway` | end | SURFACE_STRUCTURES | реализована | — |
| `end_island_decorated` | `end_island` | end | RAW_GENERATION | реализована | — |
| `end_platform` | `end_platform` | end | TOP_LAYER_MODIFICATION | реализована | — |
| `end_spike` | `end_spike` | end | SURFACE_STRUCTURES | реализована | — |
| `flower_cherry` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_default` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_flower_forest` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_forest_flowers` | `simple_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_meadow` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_pale_garden` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_plains` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_swamp` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `flower_warm` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `forest_flowers` | `simple_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `forest_rock` | `block_blob` | overworld | LOCAL_MODIFICATIONS | проверена | 100 % (2880 блоков эффекта) |
| `fossil_lower` | `fossil` | overworld | UNDERGROUND_STRUCTURES | не начата | — |
| `fossil_upper` | `fossil` | overworld | UNDERGROUND_STRUCTURES | не начата | — |
| `freeze_top_layer` | `freeze_top_layer` | overworld | TOP_LAYER_MODIFICATION | не начата | — |
| `glow_lichen` | `multiface_growth` | overworld | VEGETAL_DECORATION | не начата | — |
| `glowstone` | `random_neighbor_spread` | nether | UNDERGROUND_DECORATION | реализована | — |
| `glowstone_extra` | `random_neighbor_spread` | nether | UNDERGROUND_DECORATION | реализована | — |
| `ice_patch` | `disk` | overworld | SURFACE_STRUCTURES | реализована (расхождения) | 99.9999 % |
| `ice_spike` | `spike` | overworld | SURFACE_STRUCTURES | не начата | — |
| `iceberg_blue` | `iceberg` | overworld | LOCAL_MODIFICATIONS | не начата | — |
| `iceberg_packed` | `iceberg` | overworld | LOCAL_MODIFICATIONS | не начата | — |
| `kelp_cold` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `kelp_warm` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `lake_lava_surface` | `lake` | overworld | LAKES | реализована | эталон без эффекта |
| `lake_lava_underground` | `lake` | overworld | LAKES | проверена | 100 % (2546 блоков эффекта), 100 % (939 блоков эффекта), 100 % (1985 блоков эффекта) |
| `large_basalt_columns` | `weighted_random_selector` | nether | SURFACE_STRUCTURES | реализована | — |
| `large_dripstone` | `large_dripstone` | overworld | LOCAL_MODIFICATIONS | не начата | — |
| `lush_caves_ceiling_vegetation` | `vegetation_patch` | overworld | VEGETAL_DECORATION | не начата | — |
| `lush_caves_clay` | `random_boolean_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `lush_caves_vegetation` | `vegetation_patch` | overworld | VEGETAL_DECORATION | не начата | — |
| `monster_room` | `monster_room` | overworld | UNDERGROUND_STRUCTURES | не начата | — |
| `monster_room_deep` | `monster_room` | overworld | UNDERGROUND_STRUCTURES | не начата | — |
| `mushroom_island_vegetation` | `random_boolean_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `nether_sprouts` | `simple_block` | nether | VEGETAL_DECORATION | проверена | 100 % (3223 блоков эффекта) |
| `ore_ancient_debris_large` | `scattered_ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (131 блоков эффекта) |
| `ore_andesite_lower` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (86887 блоков эффекта) |
| `ore_andesite_upper` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (621 блоков эффекта) |
| `ore_blackstone` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (20997 блоков эффекта) |
| `ore_clay` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (168236 блоков эффекта) |
| `ore_coal_lower` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (8856 блоков эффекта) |
| `ore_coal_upper` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (961 блоков эффекта) |
| `ore_copper` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (6889 блоков эффекта) |
| `ore_copper_large` | `ore` | overworld | UNDERGROUND_ORES | реализована (расхождения) | 100.0000 % |
| `ore_debris_small` | `scattered_ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (71 блоков эффекта) |
| `ore_diamond` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (268 блоков эффекта), 100 % (726 блоков эффекта) |
| `ore_diamond_buried` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (1110 блоков эффекта) |
| `ore_diamond_large` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (89 блоков эффекта) |
| `ore_diamond_medium` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (1111 блоков эффекта) |
| `ore_diorite_lower` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (86887 блоков эффекта) |
| `ore_diorite_upper` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (621 блоков эффекта) |
| `ore_dirt` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (22533 блоков эффекта) |
| `ore_emerald` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (337 блоков эффекта) |
| `ore_gold` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (2144 блоков эффекта) |
| `ore_gold_deltas` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (417 блоков эффекта) |
| `ore_gold_extra` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (4443 блоков эффекта) |
| `ore_gold_lower` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (253 блоков эффекта) |
| `ore_gold_nether` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (7643 блоков эффекта) |
| `ore_granite_lower` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (86851 блоков эффекта) |
| `ore_granite_upper` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (621 блоков эффекта) |
| `ore_gravel` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (43199 блоков эффекта) |
| `ore_gravel_nether` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (19826 блоков эффекта) |
| `ore_infested` | `ore` | overworld | UNDERGROUND_DECORATION | проверена | 100 % (8359 блоков эффекта) |
| `ore_iron_middle` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (4167 блоков эффекта) |
| `ore_iron_small` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (1464 блоков эффекта) |
| `ore_iron_upper` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (14 блоков эффекта) |
| `ore_lapis` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (604 блоков эффекта) |
| `ore_lapis_buried` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (1540 блоков эффекта) |
| `ore_magma` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (17642 блоков эффекта) |
| `ore_quartz_deltas` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (1227 блоков эффекта) |
| `ore_quartz_nether` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (22141 блоков эффекта) |
| `ore_redstone` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (1863 блоков эффекта) |
| `ore_redstone_lower` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (2117 блоков эффекта) |
| `ore_soul_sand` | `ore` | nether | UNDERGROUND_DECORATION | проверена | 100 % (9900 блоков эффекта) |
| `ore_tuff` | `ore` | overworld | UNDERGROUND_ORES | проверена | 100 % (118885 блоков эффекта) |
| `pale_garden_flowers` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `pale_garden_vegetation` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `pale_moss_patch` | `vegetation_patch` | overworld | VEGETAL_DECORATION | не начата | — |
| `patch_berry_common` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_berry_rare` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_bush` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_cactus_decorated` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `patch_cactus_desert` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `patch_crimson_roots` | `simple_block` | nether | UNDERGROUND_DECORATION | реализована | — |
| `patch_dead_bush` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_dead_bush_2` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_dead_bush_badlands` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_dry_grass_badlands` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_dry_grass_desert` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_fire` | `simple_block` | nether | UNDERGROUND_DECORATION | реализована | — |
| `patch_firefly_bush_near_water` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_firefly_bush_near_water_swamp` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_firefly_bush_swamp` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_badlands` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_forest` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_jungle` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_meadow` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_normal` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_plain` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_savanna` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_taiga` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_grass_taiga_2` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_large_fern` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_leaf_litter` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_melon` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_melon_sparse` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_pumpkin` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_red_shrub` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_soul_fire` | `simple_block` | nether | UNDERGROUND_DECORATION | реализована | — |
| `patch_sugar_cane` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `patch_sugar_cane_badlands` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `patch_sugar_cane_desert` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `patch_sugar_cane_swamp` | `block_column` | overworld | VEGETAL_DECORATION | не начата | — |
| `patch_sunflower` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_tall_grass` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_tall_grass_2` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `patch_waterlily` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `pointed_dripstone` | `simple_random_selector` | overworld | UNDERGROUND_DECORATION | частично | — |
| `red_mushroom_nether` | `simple_block` | nether | UNDERGROUND_DECORATION | реализована | — |
| `red_mushroom_normal` | `simple_block` | nether,overworld | VEGETAL_DECORATION | реализована | — |
| `red_mushroom_old_growth` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `red_mushroom_swamp` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `red_mushroom_taiga` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `rooted_azalea_tree` | `root_system` | overworld | VEGETAL_DECORATION | не начата | — |
| `rooted_sulfur_spring` | `root_system` | overworld | LAKES | не начата | — |
| `sculk_patch_deep_dark` | `sequence` | overworld | UNDERGROUND_DECORATION | частично | — |
| `sculk_vein` | `multiface_growth` | overworld | UNDERGROUND_DECORATION | не начата | — |
| `sea_pickle` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_cold` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_deep` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_deep_cold` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_deep_warm` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_normal` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_river` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_swamp` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `seagrass_warm` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `small_basalt_columns` | `weighted_random_selector` | nether | SURFACE_STRUCTURES | реализована | — |
| `spore_blossom` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `spring_closed` | `spring_feature` | nether | UNDERGROUND_DECORATION | проверена | 100 % (1443 блоков эффекта) |
| `spring_closed_double` | `spring_feature` | nether | UNDERGROUND_DECORATION | реализована | — |
| `spring_delta` | `spring_feature` | nether | UNDERGROUND_DECORATION | реализована | — |
| `spring_lava` | `spring_feature` | nether,overworld | FLUID_SPRINGS,VEGETAL_DECORATION | проверена | 100 % (11 блоков эффекта) |
| `spring_lava_frozen` | `spring_feature` | overworld | FLUID_SPRINGS | реализована | эталон без эффекта |
| `spring_open` | `spring_feature` | nether | UNDERGROUND_DECORATION | проверена | 100 % (14 блоков эффекта) |
| `spring_water` | `spring_feature` | overworld | FLUID_SPRINGS | проверена | 100 % (11 блоков эффекта) |
| `sulfur_pool` | `sequence` | overworld | LAKES | реализована | — |
| `sulfur_spike` | `simple_random_selector` | overworld | UNDERGROUND_DECORATION | частично | — |
| `sulfur_spike_cluster` | `speleothem_cluster` | overworld | UNDERGROUND_DECORATION | не начата | — |
| `trees_badlands` | `random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `trees_birch` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_birch_and_oak_leaf_litter` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_cherry` | `tree` | overworld | VEGETAL_DECORATION | не начата | — |
| `trees_dappled_forest` | `weighted_random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_flower_forest` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_grove` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_jungle` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_mangrove` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_meadow` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_old_growth_pine_taiga` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_old_growth_spruce_taiga` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_plains` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_savanna` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_snowy` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_sparse_jungle` | `random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `trees_swamp` | `tree` | overworld | VEGETAL_DECORATION | не начата | — |
| `trees_taiga` | `random_selector` | overworld | VEGETAL_DECORATION | реализована | — |
| `trees_water` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_windswept_forest` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_windswept_hills` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `trees_windswept_savanna` | `random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `twisting_vines` | `block_column` | nether | VEGETAL_DECORATION | не начата | — |
| `underwater_magma` | `underwater_magma` | overworld | UNDERGROUND_ORES | не начата | — |
| `vines` | `vines` | overworld | VEGETAL_DECORATION | не начата | — |
| `warm_ocean_vegetation` | `simple_random_selector` | overworld | VEGETAL_DECORATION | частично | — |
| `warped_forest_vegetation` | `simple_block` | nether | VEGETAL_DECORATION | реализована | — |
| `warped_fungi` | `huge_fungus` | nether | VEGETAL_DECORATION | не начата | — |
| `weeping_vines` | `overlay` | nether | VEGETAL_DECORATION | частично | — |
| `wildflowers_birch_forest` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |
| `wildflowers_meadow` | `simple_block` | overworld | VEGETAL_DECORATION | реализована | — |


## 8. Приоритетный список оставшихся типов (для групп фич)

> Пункт 1 (деревья, грибы-деревья, фунги, корневые системы) выполнен потоком W11: см. `docs/blender/features-trees.md` (устройство, таблица placed_feature × статус, недетерминизм игры, воспроизведение).

Число — размещённых `placed_feature` 26.3, использующих тип (прямо или во вложенных селекторах); порядок — по числу и по тому, что блокирует остальное.

1. **Деревья** (`tree`: 46 placed, `fallen_tree` 6, `huge_fungus` 2, огромные грибы): TreeFeature (+`featuresize`, 10 trunk placers: straight, forking, giant, mega_jungle, dark_oak, bending, upwards_branching, fancy, cherry, poplar; 12 foliage placers; 8 decorators:
   beehive, place_on_ground, trunk_vine/leave_vine, alter_ground, shelf_mushroom, attached_to_leaves/logs, cocoa, pale_moss, creaking_heart; `root_system`/`mangrove_root_placer`). Блокирует все `trees_*` и большинство селекторов («частично» 19 `random_selector`). Нужна `updateLeaves`-логика (distance) и `canSurvive` саженцев (уже есть, `supports_vegetation`).
2. **Растительность поверхности**: `block_column` (11: кельп, лианы пещер, сахарный тростник, кактус, твистинг/уипинг-лозы, сморщенные…), `vegetation_patch` (мох, глина, лаш-пещеры), `multiface_growth` (светящийся лишайник, жилы скалка), `vines`, `bamboo`, `block_pile`, `overlay`, `random_neighbor_spread`, `freeze_top_layer` (снег/лёд; нужны `Biome.getHeightAdjustedTemperature`, `TEMPERATURE_NOISE`, `FROZEN_TEMPERATURE_NOISE`), морские фичи (морская трава, огурцы, кораллы), `sculk_patch`. Нужно: `updateFromNeighbourShapes` пометок (§5 п. 6), `block_can_survive` для классов Seagrass/SeaPickle/Mushroom/LilyPad/Crop/…
3. **Подземные**: `geode` (аметист; идёт ДО руд — влияет на воздух рядом с рудами и на уголь), `speleothem_cluster`/`large_dripstone` (дрипстоун), `monster_room` (2), `fossil` (2, шаблоны NBT — `nbt.[ch]`), `underwater_magma`, `lake` с водой + `Biome.shouldFreeze`.
4. **Вода/лёд**: `iceberg` (2), `blue_ice`, `ice_spike`(`spike`), `freeze_top_layer`.
5. **Nether/End**: `delta_feature`, `netherrack_replace_blobs` (2), basalt pillar/columns, glowstone blob, `huge_fungus`, `nether_forest_vegetation`, `end_spike`, `end_island`, `chorus_plant`, `void_start_platform`, `end_gateway`, `end_platform`.
6. Пробелы каркаса, которые закроют группы: `MossyCarpetBlock.placeAt`, `Biome.shouldFreeze`/`getHeightAdjustedTemperature`, свет (`getRawBrightness`) для грибов и посевов, поблочные биомы 26.4, постройки в цикле шага (`structure.placeInChunk` — W9).


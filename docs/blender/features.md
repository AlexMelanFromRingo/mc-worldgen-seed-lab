# W8 «Декорации»: каркас стадии FEATURES, минеральные и «блоб-подобные» фичи (ворота G5)

Отчёт потока W8 аддона «MC Worldgen». Спецификация — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md` (G5), справка по игре —
`docs/00-worldgen-guide.md` §15, правила размещения — `docs/08-height-temperature-rules.md`. Все числа ниже измерены; команды воспроизведения — в §6.

@@ИТОГ@@

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
4. Порядок чанков — построчно (z, затем x); параллельно «волнами» `t = ix + 3·iz`: чанки одной волны не пересекаются окнами 3×3, а все пересекающиеся
   более ранние чанки имеют меньшее `t`, поэтому результат побитово равен последовательному обходу при любом числе потоков (проверено `cmp`). Игра при этом
   недетерминирована у границ чанков (W6, `ground-truth.md` §2.4); из перебранных порядков (zx, xz, обратные, от центра) построчный — один из двух лучших.
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

@@РЕЗУЛЬТАТЫ@@

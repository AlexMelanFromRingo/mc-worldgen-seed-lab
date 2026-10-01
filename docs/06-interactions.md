# 06. Систематический аудит взаимосвязей генерации (версия 26.3; слои L1 + L2)

Цель: не полагаться на память о «приколах» генерации, а **вывести все связи из кода и данных автоматически**. Три слоя: **L1** — граф данных датапака (`tools/audit_interactions.py` → `data/interactions-<V>.json`); **L2** — статический скан Java-классов фич/карверов/размещения/структур/материалов (`tools/audit_code_reads.py` → `data/code-reads-<V>.json`); **L3** — эмпирика на настоящем сервере 26.3 (§10; EULA принят пользователем устно).

Отчёт генерируется скриптом `tools/audit_report.py`; сырые данные — в `data/`. Связи в L1 — **потенциальные** (пересечение «пишет/читает» по блокам и тегам с учётом порядка), реальные зависят от геометрии.

## 1. Охват

| | 26.1 | 26.2 | 26.3 |
|---|---|---|---|
| размещаемых фич (`placed_feature`) | 258 | 262 | 273 |
| из них подключено хотя бы к одному биому | 204 | 208 | 211 |
| потенциальных влияний «ранняя фича → поздняя» (Overworld) | 317 | 336 | 279 |
| структур / наборов | 34 / 20 | 34 / 20 | 52 / 21 |
| шумов, на которые есть ссылки в данных | 53 | 54 | 56 |
| Java-классов в скане L2 (26.3) | | | 189 |

## 2. Карты высот: какие фичи зависят от того, что уже поставили другие

Карты высот `*_WG` считаются **только по рельефу** (до фич). Остальные (`MOTION_BLOCKING`, `OCEAN_FLOOR`, `WORLD_SURFACE`, `MOTION_BLOCKING_NO_LEAVES`) — «живые»: обновляются по мере установки блоков, поэтому **фича, использующая живую карту, видит результат работы более ранних фич** (деревья, структуры, озёра). Это порядок «чтение-после-записи» (см. `docs/00` §16).

| Карта высот | Тип | Фич | Примеры |
|---|---|---|---|
| `MOTION_BLOCKING` | живая | 47 | `bamboo_light`, `brown_mushroom_dappled_forest`, `brown_mushroom_normal`, `brown_mushroom_old_growth`, `brown_mushroom_swamp`, `brown_mushroom_taiga` |
| `OCEAN_FLOOR` | живая | 37 | `bamboo_vegetation`, `birch_tall`, `dark_forest_vegetation`, `kelp_cold`, `kelp_warm`, `pale_garden_vegetation` |
| `WORLD_SURFACE_WG` | рельеф | 19 | `bamboo`, `lake_lava_surface`, `patch_berry_common`, `patch_berry_rare`, `patch_dead_bush`, `patch_dead_bush_2` |
| `OCEAN_FLOOR_WG` | рельеф | 8 | `disk_clay`, `disk_grass`, `disk_gravel`, `disk_sand`, `glow_lichen`, `lake_lava_underground` |
| `MOTION_BLOCKING_NO_LEAVES` | живая | 3 | `pale_garden_flowers`, `pale_moss_patch`, `patch_firefly_bush_near_water` |

## 3. Потенциальные влияния между фичами Overworld (L1)

Критерий: фича *A* стоит раньше фичи *B* в глобальном порядке (`FeatureSorter`) и **пишет** блок (не вездесущий: исключены воздух, вода, лава, камень, глубинный сланец, земля, трава, бедрок, гравий, песок, туф), который *B* **читает** (предикаты, теги, `target`). Ниже — сводка по классам причин, полный список — `data/interactions-26.3.json → feature_edges`.

### 3.1. Самые «зависимые» фичи (читают результаты наибольшего числа ранних фич)

| Читающая фича | Шаг | Сколько ранних фич влияет | Типичные блоки-связки |
|---|---|---|---|
| `rooted_azalea_tree` | vegetal_decoration | 33 | `oak_leaves`, `short_grass`, `granite`, `diorite`, `andesite` |
| `lush_caves_clay` | vegetal_decoration | 12 | `granite`, `diorite`, `andesite`, `clay`, `pale_moss_block` |
| `spring_water` | fluid_springs | 10 | `packed_ice`, `granite`, `diorite`, `andesite`, `calcite` |
| `lush_caves_vegetation` | vegetal_decoration | 10 | `granite`, `diorite`, `andesite`, `pale_moss_block`, `moss_block` |
| `lush_caves_ceiling_vegetation` | vegetal_decoration | 9 | `granite`, `diorite`, `andesite`, `pale_moss_block` |
| `pale_moss_patch` | vegetal_decoration | 8 | `granite`, `diorite`, `andesite` |
| `dripstone_cluster` | underground_decoration | 7 | `granite`, `diorite`, `andesite`, `dripstone_block` |
| `pointed_dripstone` | underground_decoration | 7 | `granite`, `diorite`, `andesite`, `dripstone_block` |
| `ore_infested` | underground_decoration | 7 | `granite`, `diorite`, `andesite` |
| `spring_lava` | fluid_springs | 7 | `granite`, `diorite`, `andesite`, `calcite` |
| `ore_tuff` | underground_ores | 6 | `granite`, `diorite`, `andesite` |
| `ore_coal_upper` | underground_ores | 6 | `granite`, `diorite`, `andesite` |
| `ore_coal_lower` | underground_ores | 6 | `granite`, `diorite`, `andesite` |
| `ore_iron_upper` | underground_ores | 6 | `granite`, `diorite`, `andesite` |
| `ore_iron_middle` | underground_ores | 6 | `granite`, `diorite`, `andesite` |

### 3.2. Классы связей

| Класс связи | Рёбер | Примеры |
|---|---|---|
| жилы камня/руд перезаписывают друг друга (общий тег `stone_ore_replaceables`) | 215 | `ore_granite_upper` → `ore_granite_lower`; `ore_granite_upper` → `ore_diorite_upper`; `ore_granite_lower` → `ore_diorite_upper` |
| цепочка лаш-пещер: глина/мох → растительность → азалия (`azalea_grows_on`) | 20 | `ore_clay` → `disk_clay`; `pale_moss_patch` → `dark_forest_vegetation`; `pale_moss_patch` → `lush_caves_ceiling_vegetation` |
| прочее | 12 | `glow_lichen` → `rooted_azalea_tree`; `flower_warm` → `rooted_azalea_tree`; `patch_grass_savanna` → `rooted_azalea_tree` |
| растительность на поверхности: растения ↔ растения | 10 | `bamboo_vegetation` → `rooted_azalea_tree`; `patch_tall_grass` → `rooted_azalea_tree`; `patch_grass_jungle` → `rooted_azalea_tree` |
| деревья: листья/стволы мешают позднейшим кустам/цветам | 9 | `trees_windswept_savanna` → `rooted_azalea_tree`; `trees_sparse_jungle` → `rooted_azalea_tree`; `trees_jungle` → `rooted_azalea_tree` |
| лёд/айсберги ↔ последующие фичи | 8 | `iceberg_packed` → `amethyst_geode`; `iceberg_blue` → `amethyst_geode`; `iceberg_packed` → `spring_water` |
| сульфурные пещеры (26.2+): источник → бассейн | 5 | `rooted_sulfur_spring` → `sulfur_pool`; `rooted_sulfur_spring` → `sulfur_spike_cluster`; `sulfur_pool` → `sulfur_spike_cluster` |

Шаги (writer → reader): underground_ores→underground_ores: 142, vegetal_decoration→vegetal_decoration: 50, underground_ores→vegetal_decoration: 34, underground_ores→underground_decoration: 18, underground_ores→fluid_springs: 12, underground_decoration→vegetal_decoration: 5, lakes→underground_decoration: 4, surface_structures→fluid_springs: 4.

## 4. Структуры: шаг, адаптация рельефа, биомы

| Шаг генерации | `terrain_adaptation` | Структур | Примеры |
|---|---|---|---|
| `surface_structures` | `beard_thin` | 24 | `abandoned_camp_bamboo_jungle`, `abandoned_camp_birch_forest`, `abandoned_camp_cherry_grove`, `abandoned_camp_dappled_forest`, `abandoned_camp_flower_forest` |
| `surface_structures` | `bury` | 1 | `stronghold` |
| `surface_structures` | `none` | 19 | `bastion_remnant`, `desert_pyramid`, `end_city`, `igloo`, `jungle_pyramid` |
| `underground_decoration` | `beard_box` | 1 | `ancient_city` |
| `underground_decoration` | `beard_thin` | 1 | `nether_fossil` |
| `underground_decoration` | `none` | 1 | `fortress` |
| `underground_structures` | `bury` | 1 | `trail_ruins` |
| `underground_structures` | `encapsulate` | 1 | `trial_chambers` |
| `underground_structures` | `none` | 3 | `buried_treasure`, `mineshaft`, `mineshaft_mesa` |

Смысл: `terrain_adaptation` ≠ `none` ⇒ структура **меняет плотность рельефа** вокруг себя (beardifier) ещё *до* этапа рельефа; шаг (`step`) задаёт, **когда** блоки структуры ставятся относительно фич (структуры шага ставятся перед фичами того же шага). Структуры с `bury`/`encapsulate`/`beard_box` «врастают» в землю — их видимость зависит от рельефа.

Наборы с зависимостями между собой: `pillager_outposts` (exclusion → `minecraft:villages` r=10).

## 5. Общие шумы и density-функции (одна функция — несколько потребителей)

Каждый шум (`worldgen/noise/*.json`) может читаться density-функциями (данные) и/или напрямую Java-кодом (`Noises.X`). Таблица — шумы с **двумя и более** потребителями (это и есть места, где подсистемы делят один и тот же шум):

| Шум | Density-функции/конфиги (данные) | Java-классы (`Noises.X`) |
|---|---|---|
| `aquifer_barrier` | amplified, large_biomes, overworld | — |
| `aquifer_fluid_level_floodedness` | amplified, large_biomes, overworld | — |
| `aquifer_fluid_level_spread` | amplified, large_biomes, overworld | — |
| `aquifer_lava` | amplified, large_biomes, overworld | — |
| `cave_cheese` | overworld/final_density, overworld_amplified/final_density, overworld_large_biomes/final_density | — |
| `cave_layer` | overworld/final_density, overworld_amplified/final_density, overworld_large_biomes/final_density | — |
| `ice` | overworld/biome_surface/frozen_peaks, overworld/under_biome_surface/frozen_peaks | — |
| `jagged` | overworld/sloped_cheese, overworld_amplified/sloped_cheese, overworld_large_biomes/sloped_cheese | — |
| `offset` | shift_x, shift_z | — |
| `packed_ice` | overworld/biome_surface/frozen_peaks, overworld/under_biome_surface/frozen_peaks | — |
| `powder_snow` | overworld/powder_snow_surface, overworld/powder_snow_under_surface | — |
| `surface` | overworld/biome_surface/old_growth_pine_taiga, overworld/biome_surface/windswept_gravelly_hills, overworld/biome_surface/windswept_hills, overworld/biome_surface/windswept_savanna, overworld/surface… | MaterialSystem |

Density-функции, на которые ссылаются **разные виды** конфигов (общий «источник правды»):

| Функция | Кто использует |
|---|---|
| `overworld/ridges` | density_function: overworld/factor, overworld/jaggedness, overworld/ridges_folded…; noise_settings: amplified, large_biomes, overworld |
| `overworld/continents` | density_function: overworld/factor, overworld/jaggedness, overworld/offset…; noise_settings: amplified, overworld |
| `overworld/erosion` | density_function: overworld/factor, overworld/jaggedness, overworld/offset…; noise_settings: amplified, overworld |
| `overworld_large_biomes/continents` | density_function: overworld_large_biomes/factor, overworld_large_biomes/jaggedness, overworld_large_biomes/offset; noise_settings: large_biomes |
| `overworld_large_biomes/erosion` | density_function: overworld_large_biomes/factor, overworld_large_biomes/jaggedness, overworld_large_biomes/offset; noise_settings: large_biomes |
| `overworld_amplified/preliminary_surface_level` | density_function: overworld_amplified/chunk_surface_level; noise_settings: amplified |
| `overworld_amplified/depth` | density_function: overworld_amplified/sloped_cheese; noise_settings: amplified |
| `overworld/preliminary_surface_level` | density_function: overworld/chunk_surface_level; noise_settings: overworld |
| `overworld/depth` | density_function: overworld/sloped_cheese; noise_settings: overworld |
| `overworld_large_biomes/preliminary_surface_level` | density_function: overworld_large_biomes/chunk_surface_level; noise_settings: large_biomes |
| `overworld_large_biomes/depth` | density_function: overworld_large_biomes/sloped_cheese; noise_settings: large_biomes |
| `end/islands` | density_function: end/sloped_cheese; noise_settings: end |
| `end/base_3d_noise` | density_function: end/sloped_cheese; noise_settings: floating_islands |

## 6. Карверы

| Карвер | Тип | Вероятность | y | lava_level | В биомах |
|---|---|---|---|---|---|
| `cave` | cave | 0.15 | `{"type": "minecraft:uniform", "max_inclusive": {"absolute": ` | `None` | 56 |
| `cave_extra_underground` | cave | 0.07 | `{"type": "minecraft:uniform", "max_inclusive": {"absolute": ` | `None` | 56 |
| `nether_cave` | cave | 0.2 | `{"type": "minecraft:uniform", "max_inclusive": {"below_top":` | `None` | 5 |
| `canyon` | canyon | 0.01 | `{"type": "minecraft:uniform", "max_inclusive": {"absolute": ` | `None` | 56 |

## 7. Скан Java-кода (L2): кто что читает и пишет

### 7.1. Классы фич, читающие окружение и пишущие блоки (топ-20 по числу чтений)

| Класс | Чтения (по API) | Записи | Теги блоков | Карты высот | RNG-вызовов |
|---|---|---|---|---|---|
| `RootSystemFeature` | getBlockState×7, getFluidState×2, isEmptyBlock/isAir×5, getHeight(heightmap)×1 | setBlock/setBlockState×2 | — | `WORLD_SURFACE` | 10 |
| `SpringFeature` | getBlockState×8, isEmptyBlock/isAir×6 | setBlock/setBlockState×1, postProcessing/fluid tick×1 | — | — | 0 |
| `IcebergFeature` | getBlockState×9, isEmptyBlock/isAir×2, sea level / y bounds×1 | setBlock/setBlockState×8 | — | — | 34 |
| `EndSpikeFeature` | getHeight(heightmap)×4, sea level / y bounds×6, world seed×1 | setBlock/setBlockState×5, entities×1 | — | — | 2 |
| `GeodeFeature` | getBlockState×2, getFluidState×2, isEmptyBlock/isAir×1, noise (Perlin/Simplex/Normal)×5, world seed×1 | postProcessing/fluid tick×1 | — | — | 5 |
| `HugeFungusFeature` | getBlockState×4, isStateAtPosition×1, isEmptyBlock/isAir×5 | setBlock/setBlockState×9 | — | — | 14 |
| `SpeleothemClusterFeature` | getBlockState×4, getFluidState×2, getHeight(heightmap)×2, sea level / y bounds×2 | setBlock/setBlockState×1 | `BASE_STONE_OVERWORLD` | — | 5 |
| `TreeFeature` | getBlockState×2, isStateAtPosition×3, isEmptyBlock/isAir×2, sea level / y bounds×2 | setBlock/setBlockState×5 | `LEAVES`, `REPLACEABLE_BY_TREES` | — | 0 |
| `MonsterRoomFeature` | getBlockState×4, isEmptyBlock/isAir×3, sea level / y bounds×1 | setBlock/setBlockState×1, blockEntity/loot×4, entities×3 | `FEATURES_CANNOT_REPLACE` | — | 5 |
| `SpikeFeature` | getBlockState×3, isEmptyBlock/isAir×4, sea level / y bounds×1 | setBlock/setBlockState×3 | — | — | 9 |
| `BlueIceFeature` | getBlockState×5, isEmptyBlock/isAir×1, sea level / y bounds×1 | setBlock/setBlockState×2 | — | — | 6 |
| `LargeDripstoneFeature` | getBlockState×3, getHeight(heightmap)×2, sea level / y bounds×2 | setBlock/setBlockState×4 | `BASE_STONE_OVERWORLD` | `WORLD_SURFACE_WG` | 1 |
| `SpeleothemUtils` | getBlockState×2, isStateAtPosition×2, isEmptyBlock/isAir×3 | setBlock/setBlockState×2 | — | — | 0 |
| `SteppedColumnClusterFeature` | getBlockState×2, isEmptyBlock/isAir×2, sea level / y bounds×3 | setBlock/setBlockState×1 | — | — | 0 |
| `AbstractHugeMushroomFeature` | getBlockState×2, isEmptyBlock/isAir×2, sea level / y bounds×2 | setBlock/setBlockState×1 | `LEAVES`, `REPLACEABLE_BY_MUSHROOMS` | — | 2 |
| `FossilFeature` | getBlockState×1, isEmptyBlock/isAir×1, getHeight(heightmap)×1, sea level / y bounds×3 |  | — | `OCEAN_FLOOR_WG` | 2 |
| `LakeFeature` | getBlockState×2, getFluidState×1, isEmptyBlock/isAir×1, getBiome×1, sea level / y bounds×1 | setBlock/setBlockState×3, postProcessing/fluid tick×1 | — | — | 8 |
| `MultifaceGrowthFeature` | getBlockState×4, isEmptyBlock/isAir×1, getChunk/hasChunk×1 | postProcessing/fluid tick×1 | — | — | 1 |
| `VegetationPatchFeature` | getBlockState×2, isStateAtPosition×2, isEmptyBlock/isAir×2 | setBlock/setBlockState×1 | — | — | 3 |
| `ReplaceBlobsFeature` | getBlockState×2, sea level / y bounds×3 | setBlock/setBlockState×1 | — | — | 0 |

### 7.2. Классы, использующие мировой seed напрямую (а не только через переданный `RandomSource`)

`EndSpikeFeature`, `GeodeFeature`, `DesertPyramidPiece`, `DesertPyramidStructure`, `JigsawStructure`, `NetherFossilPieces`, `StrongholdStructure`

Это «прямые» потребители seed: именно они дают дополнительные независимые потоки (см. `docs/03`, таблицу бит).

### 7.3. Классы, меняющие поведение по биому в точке

`LakeFeature`, `SnowAndFreezeFeature`, `BiomeFilter`, `MineshaftPieces`, `MaterialSystem`, `MatchingBiomesPredicate`

### 7.4. Классы, которые смотрят на структуры рядом

`BuriedTreasurePieces`, `DesertPyramidPiece`, `DesertPyramidStructure`, `IglooPieces`, `JungleTemplePiece`, `MineshaftPieces`, `NetherFortressPieces`, `NetherFossilPieces`, `OceanMonumentPieces`, `OceanRuinPieces`, `RuinedPortalPiece`, `ShipwreckPieces`, `StrongholdPieces`, `SwampHutPiece`, `WoodlandMansionStructure`

## 8. Что изменилось между версиями (L1)

* **26.1 → 26.2:** рёбер 317 → 336; новых 19, исчезнувших 0. Примеры новых: `ore_andesite_lower`→`dripstone_cluster`, `ore_andesite_lower`→`pointed_dripstone`, `ore_andesite_upper`→`dripstone_cluster`, `ore_andesite_upper`→`pointed_dripstone`, `ore_diorite_lower`→`dripstone_cluster`.
* **26.2 → 26.3:** рёбер 336 → 279; новых 0, исчезнувших 57. Примеры новых: .

## 9. Ограничения и что не сделано

* **L1 — потенциальные связи.** Пересечение по блокам не значит, что в реальной геометрии фичи пересекаются; зато *отсутствие* ребра означает, что данные-связи нет (кроме связей через Java-код, их ловит L2, и через общую геометрию — не ловит никто, кроме L3).
* **L3 выполнена только для одного сида и одной версии** (12345, 26.3, 6561 чанк) — см. §10; для других сидов/версий нужно повторить (`tools/l3_gen.py`).
* L1 не разбирает NBT-шаблоны структур (1511 файлов) — только их параметры и пулы.
* Java-код вне перечисленных каталогов (например `NoiseChunk`, `Aquifer`, `SurfaceSystem`) в L2 не сканируется — их связи описаны вручную в `docs/00` §10–13.

## 10. L3 — эмпирика на настоящем сервере (seed 12345, Java 26.3, 81×81 = 6561 чанк)

Метод: `tools/l3_gen.py` запускает ванильный `server-26.3.jar` (EULA принят пользователем устно 2026-10-01, только для локального аудита), форс-загружает чанки (`/forceload`, ≈ 30 чанков/с), `save-all flush`; `tools/l3_anvil.py` читает `.mca` (палитры блоков, 26.x хранит палитру как строки/`{"": имя}`); анализ — `tools/l3_analyze.py`, `l3_clay_diamond.py`, `l3_azalea.py`, `l3_rivers.py`. Всего разобрано 6561 чанк, 154 065 блоков алмазной руды, 1 914 650 блоков глины.

| Утверждение | Метод | Результат | Вывод |
|---|---|---|---|
| **«Вскрытая воздухом руда отбрасывается»** (`discard_chance_on_air_exposure`) | доля алмазов, касающихся воздуха, против доли обычных твёрдых блоков (камень/глубинный сланец/туф/гранит/диорит/андезит) в y∈[−64; 16] | алмазы **0.968 %** (1 491 из 154 065); твёрдые **2.943 %**; отношение **0.33** | подтверждено: у стен пещер алмазов втрое меньше |
| **«Глина → алмаз на фиксированном смещении»** | число пар (глина, алмаз) по смещениям (dx, dz) ∈ [−14; 14]², dy ∈ [−135; −25]; нулевое распределение — 24 циклических сдвига глины внутри области (сохраняют распределение по y и кластеризацию) | пар: наблюдено **177 799**, контроль **184 651 ± 4 769**; макс. \|z\| по ячейкам: наблюдено **4.31**, в контролях 4.55 (макс 5.76); для глины на дне рек/океанов (y 40…64): 220 648 против 231 987 ± 6 236, \|z\|max 4.50 против 4.51 | **связи нет** (наблюдений даже чуть меньше ожидаемого) |
| **«Под азалией с корнями — пещера»** | кластеры `rooted_dirt` (расстояние ≤ 3): у каждого нижний слой — воздух/признаки лаш-пещеры? | 29 кластеров, 28 крупных: **28/28** — пещера или лаш-признаки (мох, лианы, корни, глина) под нижним слоем, у **23** — воздух непосредственно под ним | подтверждено; обратное неверно: из 16 357 блоков корней лишь 28 блоков листьев азалии (многие системы не доходят до поверхности за 100 блоков и остаются без дерева) |
| **«Реки замкнуты»** | топология речной маски на карте 6144×6144 блоков (движок) + сверка с chunkbase | 192 компонента; в 81 крупном — 120 петель; 14 не касаются океана/болота/края | подтверждено (`docs/00` §16) |

Замечания: (1) для глины/алмазов тест восстановил бы эффект, если бы он был (мощность: ≈ 10⁵ пар в окне, чувствительность |z|≈5 при пуассоновском разбросе); (2) первый вариант контроля (сдвиги на сотни блоков) был **неверным** — «глина» уходила за границу области и пар становилось в 15 раз меньше; исправлено циклическим сдвигом — типичная ловушка таких тестов; (3) граничные соседи чанков не учитывались (ошибка одинакова для алмазов и контроля); (4) одна версия и один сид. Правило «алмазы рядом с глиной» существовало в **1.13.2–1.17.1** (по x в 1.13.2/1.14.4, по x и z в 1.15.2–1.17.1; в 1.7.10–1.12.2 и с 1.18 — нет); проверено на 14 настоящих серверах, механизм (LCG-seed'ы соседних фич) воспроизведён арифметически — `docs/07` и `docs/09`. В 26.x связи нет, и тест её не видит (так и должно быть).

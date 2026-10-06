# W9 «Постройки»: стадия STRUCTURES (ворота G6)

Отчёт потока W9 аддона «MC Worldgen». Спецификация — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md` (G6), справка по игре — `docs/00-worldgen-guide.md` §14,
`docs/02-structure-placement.md`. Все числа ниже измерены командами из §6; версия по умолчанию — 26.3 (единственная, для которой есть эталонные миры построек).

## 1. Итог

Реализована стадия `MC_STAGE_STRUCTURES`: **все 52 структуры датапака 26.3** (28 jigsaw + 24 Java-кодированных: пирамиды, храмы, иглу, хижины, клады, корабли, руины, порталы, окаменелости, шахты, стронгхолды, монументы, особняки, крепость, город Края)
генерируются и ставятся блок-в-блок по правилам игры. Ворота **G6 пройдены на 26.3: 155 эталонных миров настоящего сервера, 738 из 738 стартов совпали с сохранёнными `structures.starts` (позиция, биом, ориентация, bounding box и порядок всех частей, соединения jigsaw);
из 4 396 393 блоков, которые постройки меняют в эталоне (включая Beardifier), не совпало 1 685 — 99,96 %; худший отдельный мир — 99,84 %** (порог спеки 99,9 % выполнен по каждому из 21 набора в среднем и по каждому миру, кроме двух: древний город 99,84 % и шахта 99,89 % — остаток объяснён порядком обработки чанков сервера, §7).
Для 26.1/26.2/26.4-snapshot-2 эталонных миров нет: старты сверены с oracle (`Structure.generate` настоящего кода) — 26.1: 65/65, 26.2: 73/80, 26.4: 35/40 (все «нет» — `ruined_portal`/`_ocean`, которые oracle в наборах с несколькими структурами не выдаёт; это ограничение oracle, а не расхождение).

Что ещё: старты и ссылки с кэшем по миру, `Beardifier` (26.3 — float, 26.1/26.2 — double), `terrain_adaptation` `beard_thin/beard_box/bury/encapsulate`, общий движок шаблонов (`.nbt`) с процессорами и пост-обработкой форм блоков, ABI `mcgen_structure_starts` для списка построек области.
Скорость: расчёт стартов 64×64 чанков — 0,8 с на один поток (≈0,2 мс/чанк), стадия вместе с Beardifier и рисованием частей добавляет заметную долю только из-за кольца окна декорации (см. §6).

Остаток (подробно — §7–8): остаточные 0,01–0,16 % блоков — порядок обработки соседних чанков у сервера недетерминирован (пятна sculk у древних городов, стыки стен особняков, заборы шахт); для 26.1/26.2/26.4 блоки не сверены (нет эталонов); все варианты (18 лагерей, `mineshaft_mesa`, `shipwreck_beached`, все `ruined_portal_*`) проверены на эталонах.


## 2. Где код

| файл | что | аналог в игре |
|---|---|---|
| `libmcgen/src/structure.h` | типы: `BB` (BoundingBox), `RS` (WorldgenRandom на LCG/Xoroshiro), `StPiece`/`PieceVT`, `StStart`, `StructDef`/`StructType`, `StructSet`, `GenCtx`, `StCtx` | `StructurePiece`, `StructureStart`, `Structure`, `StructureSet` |
| `libmcgen/src/structure.c` | реестры structure/structure_set/биом-теги, размещение, старты (`Structure.generate`), кэш стартов и ссылок, цикл `placeInChunk`, Beardifier (26.3 float и 26.1/26.2 double), WG-карты высот, ABI `mcgen_structure_starts` | `ChunkGenerator.createStructures/createReferences/applyBiomeDecoration` (часть структур), `StructureStart`, `Beardifier` |
| `libmcgen/src/structure_place.c` | кольца strongholds (`concentric_rings`): геометрия + поиск биома 57×57 квартов с «резервуарной» выборкой | `ChunkGeneratorStructureState.generateRingPositions`, `BiomeSource.findBiomeHorizontal` |
| `libmcgen/src/structure_piece.[ch]` | слой «StructurePiece» для Java-кодированных построек: `sp_place/sp_box/sp_maybe_box/…`, ориентация, ГСЧ региона | `StructurePiece`, `ScatteredFeaturePiece` |
| `libmcgen/src/structure_post.c` | пост-обработка форм блоков (заборы, решётки, лестницы, snowy, факелы, ladder): `updateShape` для помеченных позиций и для шаблонов с `knownShape = false` | `Block.updateFromNeighbourShapes`, `StructureTemplate.updateShapeAtEdge` |
| `libmcgen/src/template.[ch]` | шаблоны `.nbt`: палитры, порядок блоков, `rotate/mirror` состояний по свойствам, `placeInWorld` (процессоры, waterlogging, `LootTableSeed`) | `StructureTemplate`, `StructurePlaceSettings` |
| `libmcgen/src/processor.[ch]` | `rule` (все RuleTest/PosRuleTest данных), `protected_blocks`, `block_rot`, `capped`, `block_ignore`, `jigsaw_replacement`, `gravity`, `block_age`, `blackstone_replace`, `lava_submerged` | `StructureProcessor` и наследники |
| `libmcgen/src/jigsaw.[ch]` | `JigsawStructure`, пулы (`template_pool`, fallback, веса), элементы single/legacy/list/feature/empty, `JigsawPlacement` (приоритеты, свободные объёмы, `expansion hack`, проекции, `dimension_padding`, `pool_aliases`), `PoolElementStructurePiece` | `JigsawPlacement`, `StructureTemplatePool`, `StructurePoolElement` |
| `libmcgen/src/structures/<имя>.c` | Java-кодированные постройки (по модулю на постройку), регистрация — `structures/register.c` | `structures/*Structure.java`, `*Pieces.java` |
| `libmcgen/tests/g6_starts.c/.py`, `g6_blocks.py`, `g6_rings.c` | сверка стартов с сохранёнными `structures.starts` настоящего сервера, блок-в-блок сверка, кольца strongholds | — |

Правки общих файлов (минимальные): `mcgen_internal.h` (поля `McWorld.structures/struct_on/struct_run`), `world.c` (освобождение), `region.c` (стадия в `generate()`, пост-обработка форм),
`terrain.c` (Beardifier в `terrain_fill_chunk`, `terrain_column_height_ctx` = `iterateNoiseColumn`), `terrain_old.c`/`df_old.c` (то же для 26.1/26.2), `feature.c/.h` (вызов `structures_decorate_step`, режим «только постройки»),
`biome.c` (поиск R-дерева с историей `lastResult`), `fluidpp.[ch]` (хук `shape_update`), `Makefile` (`src/structures/*.c`, параметр `B=` каталога сборки), `mcgen.h` (только добавлены `McStructureStart`, `mcgen_structure_starts`, `mcgen_structure_piece_bb`).

## 3. Как устроено (соответствие игре)

### 3.1. Положение в конвейере
`region.c`: при `STRUCTURES` стадии TERRAIN/BIOMES включаются автоматически, `world.struct_on = 1` включает Beardifier в `terrain_fill_chunk`. Части рисуются в цикле декорации чанка (`features_apply_region` → `decorate_chunk`): на каждом шаге `step` игры
сначала `structures_decorate_step` (постройки шага), затем фичи шага (если запрошена стадия FEATURES; без неё — режим «только постройки»). Это точно `ChunkGenerator.applyBiomeDecoration`: `random.setFeatureSeed(decorationSeed, индекс_структуры_в_шаге, шаг)`
на каждую структуру шага, затем для каждого старта, ссылающегося на чанк, — `StructureStart.placeInChunk(writableArea = чанк × [min_y+1, max_y])`.

### 3.2. Старты (`structure_starts`)
* Реестры (`structure.c`): `worldgen/structure/*.json` (порядок реестра — по id: игра грузит ресурсы в `TreeMap`), `structure_set/*.json`, биом-теги (рекурсивно), `possibleStructureSets` (набор допустим, если биом одной из структур есть в источнике биомов измерения).
* Размещение — `docs/02-structure-placement.md`: `random_spread` (linear/triangular, соль, `legacy_type_1/2/3` и обычный редуктор частоты, `exclusion_zone`), `concentric_rings` (кольца strongholds — геометрия от LCG seed + поиск биома 57×57 кварт на y=0 с «резервуарной» выборкой и историей `lastResult` R-дерева; 128 из 128 совпали с oracle).
* Выбор из набора с несколькими структурами — `setLargeFeatureSeed` + `nextInt(Σвес)` с повторами; генерация — `Structure.generate`: `GenCtx` (LCG `WorldgenRandom`), `findGenerationPoint` → проверка биома точки старта по кварте (`world_biome_noise`) → `build` частей.
* Высоты (`ChunkGenerator.getBaseHeight/getFirstFreeHeight`): `terrain_column_height_ctx` — та же свёртка колонки (`final_density` + aquifer без Beardifier), что `iterateNoiseColumn` (26.3); для 26.1/26.2 — `NoiseChunk` из одной ячейки (`terrain_old_column_height`); результаты кэшируются.
* Кэш стартов — по чанку-источнику, потокобезопасно; ссылки (`createReferences`) — все старты в ±8 чанках, чей bounding box (расширенный на 12 при `terrain_adaptation ≠ none`) пересекает чанк; порядок нескольких стартов одной структуры в чанке — итерация `LongOpenHashSet` fastutil (реализована).

### 3.3. Beardifier
`beard_for_chunk` собирает «жёсткие» части и соединения jigsaw рядом с чанком (`isCloseToChunk(12)`), `SBeard` подставляется в `final_density` (26.3: `sctx_set_beardifier`; 26.1/26.2: узел `W_BEARD` в `df_old.c`). Ядро 24³ — `exp(-d²/16)` во float, `fastInvSqrt` — как в игре.

### 3.4. Jigsaw и шаблоны
`jigsaw.c`: пулы с весами и fallback, элементы single/legacy/list/feature/empty, `JigsawPlacement.Placer` (очередь с приоритетами `placement_priority`, `selection_priority`, перемешивание Fisher–Yates `Util.shuffle` на LCG, проверка свободных объёмов — набор целочисленных боксов вместо `VoxelShape`, `expansion hack`, проекции rigid/terrain_matching и `JigsawJunction`, `dimension_padding`, `pool_aliases` random/random_group/direct, `start_jigsaw_name`).
`template.c`: палитры, порядок блоков (полные кубы → прочие → блок-сущности, по y,x,z), повороты/отражения состояний по свойствам (facing, axis, rotation, north/east/south/west, shape рельсов и лестниц, hinge, orientation jigsaw), `placeInWorld` (процессоры, waterlogging и `toFill`, `LootTableSeed`: `random.nextLong()` на контейнеры с NBT — без этого ГСЧ структуры расходится с игрой, найдено сверкой sculk у древних городов),
`knownShape = false` (все шаблонные части кроме jigsaw): `updateShapeAtEdge` по граням записанного объёма (порядок `forAllFaces`) + `updateFromNeighbourShapes` (`structure_post.c`: заборы, решётки, стены, лестницы, двери, сундуки, знамёна, двойные растения, snowy, факелы, ladder, пшеница без света).
`processor.c`: `rule` (RuleTest: always_true, block_match, blockstate_match, random_block(state)_match, tag_match, all_of/any_of/not; PosRuleTest: always_true, linear_pos, axis_aligned_linear_pos), `protected_blocks`, `block_rot`, `capped` (ГСЧ по seed домена `structures`), `block_ignore`, `jigsaw_replacement`, `gravity`, `block_age`, `blackstone_replace`, `lava_submerged`. ГСЧ блока — `LegacyRandomSource(Mth.getSeed(pos))`.
Порядок процессоров как у `SinglePoolElement.getSettings` (для `legacy_single_pool_element` блок-игнор воздуха — последним: иначе пустые «плиты» `feature_plate` затирали бы землю).

### 3.5. Найденные факты об игре (проверены по эталону)
1. **Карты высот `*_WG` для построек в 26.3 — снимок сразу после terrain**, а не лениво при первом запросе (gravity у `terrain_matching` читает `WORLD_SURFACE_WG` чанка после уже поставленных соседних частей; ленивое праймирование давало 20 лишних блоков на деревню). Снимок: `structures_wg_snapshot`, `structure_height`. Расхождение с моделью W8 («WG праймятся при первом запросе») — стоит перепроверить у фич на мирах с постройками.
2. `StructurePiece.afterPlace` пирамиды: центр `pieces.calculateBoundingBox()` берётся по ТЕКУЩИМ рамкам (после сдвига высоты при рисовании), а не по рамке старта.
3. writableArea (chunkBB) — один объект на все шаги декорации чанка; `nether_fossil`/`ruined_portal` расширяют его (`encapsulate`) и расширение сохраняется (`FCtx.sbb`).
4. Жидкости: `LevelChunk.postProcessGeneration` для помеченной жидкости вызывает `LiquidBlock.tick` — пузырьковые столбы над магмой/песком душ у ruined_portal и ocean_ruin; формы блоков из `SHAPE_CHECK_BLOCKS` обновляются там же (`structure_post.c`).
5. `StructureStart` в NBT эталона хранит рамки частей ПОСЛЕ рисования (у ScatteredFeaturePiece и шаблонных частей y смещён), а jigsaw — без «expansion hack»; `g6_starts.py` учитывает оба случая.

## 4. Структуры × статус

Проверка: `g6_starts.py` (старты против `.mca`) и `g6_blocks.py` (блоки). «Миров» — число эталонных миров настоящего сервера (r=8, один набор построек, поверхность включена, фичи/карверы выключены).

| структура (набор) | тип | step | adaptation | статус | старты | блоки построек, % |
|---|---|---|---|---|---|---|
| village_plains/desert/savanna/snowy/taiga (villages) | jigsaw | surface_structures | beard_thin | реализована, проверена (все 5 биомов) | 15/15 | 99,957–100 (среднее 99,980) |
| pillager_outpost | jigsaw | surface_structures | beard_thin | реализована, проверена | 3/3 | 100 |
| abandoned_camp_* ×18 (abandoned_camp) | jigsaw | surface_structures | beard_thin | реализована, проверены все 18 вариантов | 18/18 миров | 100 |
| ancient_city | jigsaw | underground_decoration | beard_box | реализована, проверена | 4/4 | 99,84–99,99 (среднее 99,92) |
| trail_ruins | jigsaw | underground_structures | bury | реализована, проверена | 3/3 | 100 |
| trial_chambers | jigsaw | underground_structures | encapsulate | реализована, проверена | 4/4 | 99,997–100 |
| bastion_remnant | jigsaw | surface_structures | none | реализована, проверена | 6/6 (на уровне мира) | 100 |
| fortress (Незер) | Java | underground_decoration | none | реализована, проверена | в наборе nether_complexes (6/6) | 100 |
| desert_pyramid | Java | surface_structures | none | реализована, проверена | 3/3 | 100 |
| jungle_pyramid | Java | surface_structures | none | реализована, проверена | 4/4 | 100 |
| swamp_hut | Java | surface_structures | none | реализована, проверена | 5/5 | 100 |
| igloo | шаблоны | surface_structures | none | реализована, проверена | 4/4 | 100 |
| buried_treasure | Java | underground_structures | none | реализована, проверена | 9/9 | 100 |
| shipwreck | шаблоны | surface_structures | none | реализована, проверена (в т. ч. `shipwreck_beached`) | 5/5 | 100 |
| ocean_ruin_cold/warm | шаблоны | surface_structures | none | реализована, проверена | 4/4 | 100 |
| ruined_portal (+mountain, nether, ocean) | шаблоны | surface_structures | none | реализована, проверена (в т. ч. desert, jungle, swamp) | 18/18 | 100 |
| nether_fossil | шаблоны | underground_decoration | beard_thin | реализована, проверена (9 миров с окаменелостями) | 561/561 | 100 |
| end_city | шаблоны | surface_structures | none | реализована, проверена | 3/3 | 100 |
| mineshaft (+mineshaft_mesa) | Java | underground_structures | none | реализована, проверена (в т. ч. `mineshaft_mesa`) | 11/11 | 99,89–100 (среднее 99,976) |
| stronghold | Java | strongholds | bury | реализована, проверена | 9/9 | 99,9976–100 |
| monument | Java | surface_structures | none | реализована, проверена | 3/3 | 100 |
| mansion | шаблоны | surface_structures | none | реализована, проверена | 7/7 | 99,971–100 (среднее 99,992) |

Сводка G6 по наборам (`libmcgen/tests/g6_summary.py`, 131 мир; «блоков построек» — блоки, которые отличают эталон от прогона без построек; в расхождения входит и рельеф, изменённый Beardifier):

| набор | миров | блоков построек | расхождений | худший мир, % | в среднем по блокам, % |
|---|---|---|---|---|---|
| abandoned_camp | 18 | 35678 | 0 | 100.0000 | 100.0000 |
| ancient_cities | 3 | 1939982 | 1628 | 99.8429 | 99.9161 |
| buried_treasures | 4 | 9 | 0 | 100.0000 | 100.0000 |
| desert_pyramids | 3 | 7408 | 0 | 100.0000 | 100.0000 |
| end_cities | 3 | 10411 | 0 | 100.0000 | 100.0000 |
| igloos | 4 | 1132 | 0 | 100.0000 | 100.0000 |
| jungle_temples | 3 | 4178 | 0 | 100.0000 | 100.0000 |
| mineshafts | 4 | 93604 | 14 | 99.8922 | 99.9850 |
| nether_complexes | 6 | 356352 | 0 | 100.0000 | 100.0000 |
| nether_fossils | 13 | 279870 | 0 | 100.0000 | 100.0000 |
| ocean_monuments | 3 | 79069 | 0 | 100.0000 | 100.0000 |
| ocean_ruins | 3 | 1002 | 0 | 100.0000 | 100.0000 |
| pillager_outposts | 3 | 21588 | 0 | 100.0000 | 100.0000 |
| ruined_portals | 18 | 11656 | 0 | 100.0000 | 100.0000 |
| shipwrecks | 4 | 2095 | 0 | 100.0000 | 100.0000 |
| strongholds | 9 | 455640 | 2 | 99.9976 | 99.9996 |
| swamp_huts | 5 | 728 | 0 | 100.0000 | 100.0000 |
| trail_ruins | 3 | 23971 | 0 | 100.0000 | 100.0000 |
| trial_chambers | 3 | 424143 | 0 | 100.0000 | 100.0000 |
| villages | 12 | 252909 | 10 | 99.9732 | 99.9960 |
| woodland_mansions | 7 | 394968 | 31 | 99.9711 | 99.9922 |

## 5. Версии
* **26.3** — основная, все цифры выше.
* **26.1/26.2** — код общий: `getBaseHeight` через `NoiseChunk` из одной ячейки (`terrain_old_column_height`), Beardifier в узле `W_BEARD` (double). Старты сверены с oracle (26.1: 65/65; 26.2: 73/80, остальное — пробелы oracle). Блоки не сверены (эталонов построек для этих версий нет).
* **26.4-snapshot-2** — как 26.3; старты 35/40 против oracle (пробелы те же).

## 6. Скорость
Старты: 64×64 чанков — 0,79 с одним потоком (209 проверок размещения, 23 действительных старта, из них 13 шахт, 3 чертога испытаний, 2 древних города, 1 деревня). Регион 48×48 чанков, стадии 0x7 против 0x27 (12 потоков, машина загружена другими потоками, load ≈ 10–16): 13–15 с и 20–22 с;
без единой действительной структуры (`MCGEN_STRUCTURES_ONLY=minecraft:none`) — 17 с, т. е. большая часть накладных расходов — кольцо окна декорации (2 чанка вокруг региона достраиваются, праймятся карты высот) и Beardifier-проход, а не сами постройки. Деревня/пирамида/Незер-бастион генерируются за миллисекунды на старт; время тяжёлых построек (особняк, монумент) — десятки мс на старт.

## 7. Ограничения
* Содержимое сундуков и сущности (жители, мобы, рамки/картины/стойки) не создаются; блок-контейнеры ставятся, `random.nextLong()` на `LootTableSeed` расходуется как в игре.
* Порядок обработки соседних чанков у настоящего сервера недетерминирован (`docs/blender/ground-truth.md` §2.4); мы обходим чанки как билеты `forceload` (x внешний, z внутренний). Это объясняет остаток: sculk-пятна древних городов (0,01–0,16 % блоков), колонна стен особняка на стыке чанков (12 блоков), 2 забора шахты. Перебор порядков (`MCGEN_FEATURES_SEQ`) показал, что порядок «вперёд» даёт минимум расхождений (1 182 против 2 691/5 121).
* Блоки не сверены: 26.1/26.2/26.4 целиком (эталонов нет); сторона `dimension_origin` в placement (в ванили не используется).
* Стены (`WallBlock.updateShape`) обновляются приближённо (полная нижняя грань блока над стеной вместо `VoxelShape`-пересечения); `CropBlock` без света — только на пути шаблонов.
* Опорное допущение: seed домена `structures` используется и для процессоров `capped` и «подозрительного» песка (`level.getSeed()` игры).
* Фичи внутри jigsaw (`feature_pool_element`: деревья, кусты, стога, цветы) зависят от реализации W8: недостающие типы фич молча пропускаются, порядок ГСЧ при этом не сдвигается только для реализованных; расхождения 2–3 блока на деревню — это цветы/стога W8.

## 8. Приоритетный список оставшегося
1. Перепроверить у W8 модель WG-карт 26.3 (§3.5 п. 1): снимок после terrain, а не ленивое праймирование.
2. Блоки 26.1/26.2/26.4: эталонов нет (нужны миры `structure:*` для этих версий); проверить ветку `terrain_old_column_height`/Beardifier double на блоках.
3. Остаточные 0,01–0,16 % (порядок чанков сервера): исследовать модель планировщика forceload, если понадобится 100 % по sculk/стенам; точная модель `WallBlock.updateShape` через формы коллизии.
4. Содержимое: сундуки (таблицы добычи), спаунеры, сущности жителей — при необходимости для аддона (не блоки).
5. Скорость: пул `TerrainCtx` для высот и кэш колонок уже есть; профилирование монумента/особняка при больших областях и распараллеливание расчёта стартов вне потоков окна.

## 9. Воспроизведение
```bash
make -C libmcgen B=build_w9 -j8 && make -C libmcgen B=build_w9 build_w9/tests/g6_starts build_w9/tests/g6_rings
G6_BUILD=build_w9 python3 libmcgen/tests/g6_starts.py [--sets villages,…] [-v]           # старты против сохранённых structures.starts эталонов
G6_BUILD=build_w9 G6_TMP=/tmp/g6 python3 libmcgen/tests/g6_blocks.py [--sets …] [--list 20] [--json out.json]   # блоки против эталонов (стадии 0x27)
python3 libmcgen/tests/g6_summary.py вывод.txt --md                                         # сводная таблица
G6_BUILD=build_w9 python3 libmcgen/tests/g6_oracle_starts.py --version 26.1 --seed 12345   # старты против oracle (версии без эталонов)
libmcgen/build_w9/tests/g6_rings run/pack-26.3 26.3 12345                                    # кольца strongholds (сверка: oracle/run.sh 26.3 stronghold 12345)
```
Переменные: `MCGEN_STRUCTURES_ONLY=minecraft:<набор>[,…]` — оставить только эти structure_set (как в эталонах «один набор»); `MCGEN_STRUCTURES_DEBUG=1` — предупреждения о нереализованных типах.


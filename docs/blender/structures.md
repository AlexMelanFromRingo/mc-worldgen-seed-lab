# W9 «Постройки»: стадия STRUCTURES (ворота G6)

Отчёт потока W9 аддона «MC Worldgen». Спецификация — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md` (G6), справка по игре — `docs/00-worldgen-guide.md` §14,
`docs/02-structure-placement.md`. Все числа ниже измерены командами из §6; версия по умолчанию — 26.3 (единственная, для которой есть эталонные миры построек).

@@ИТОГ@@

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

@@ОСТАЛЬНОЕ@@

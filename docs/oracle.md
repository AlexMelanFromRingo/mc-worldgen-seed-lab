# Эталон (oracle): настоящий код Mojang + настоящий датапак, внутри процесса

Каталог `oracle/`. Java-программа, которая в одном процессе (без сервера) загружает реальный worldgen-датапак и классы игры из
`jars/game-<V>.jar` (V ∈ 26.1, 26.2, 26.3) и по 64-битному seed выдаёт «эталонные» результаты генерации для overworld / the_nether / the_end.
Ничего из алгоритмов не переписано руками: вызываются `RandomState`, `Climate.Sampler`, `MultiNoiseBiomeSource`, `TheEndBiomeSource`,
`ChunkGeneratorStructureState`, `StructurePlacement.isStructureChunk`, `Structure.generate`, `BiomeManager`, `NoiseBasedChunkGenerator.getBaseHeight`, ...
Единственный «glue»-код, скопированный из игры: цикл выбора структуры из набора в `structstart` (`ChunkGenerator.createStructures`).

## Сборка и запуск

```bash
oracle/build.sh 26.3                 # компиляция (атомарно обновляет oracle/build/26.3/classes); для 26.1/26.2 то же
oracle/run.sh 26.3 climate overworld 12345 100 16 -200        # одна команда (JSON в stdout)
oracle/run.sh 26.3 serve < команды.txt                         # режим serve: команды построчно из stdin, ответ — 1 JSON-строка на команду
oracle/gen_vectors.sh [26.1 26.2 26.3]                         # тест-векторы -> tests/vectors/<V>/
python3 oracle/compare_versions.py                             # сравнение версий (числа из раздела «Расхождения»)
```
* Старт JVM + Bootstrap + загрузка реестров: 10–15 с на версию (26.3 дольше) — для пакетной работы используйте `serve`.
* stdout принадлежит только JSON-ответам (stdout процесса захватывается до `Bootstrap`, лог идёт в stderr — `oracle/log4j2.xml`).
* Классы программы в пакете `oracle`; версионные слои: `oracle/src/common` (общий код), `oracle/src/v261_262`, `oracle/src/v263` (класс `oracle.compat.Compat`).
* Аргументы: `dim` = `overworld|nether|end` (для `noise/df/climate/biome/structs/...` можно опустить — тогда overworld);
  `seed` = десятичное знаковое число (также беззнаковое и `0x…`) либо `s:текст` (`String.hashCode()` как в игре);
  опции: `--preset normal|large_biomes|amplified` (меняет noise_settings overworld; nether/end одинаковы во всех пресетах), `--mode point|chunk`.

## Команды

| Команда | Что делает | 26.1/26.2 | 26.3 |
|---|---|---|---|
| `info` | версия, world_version | + | + |
| `list noises\|dfs\|presets\|biomes\|structure_sets\|structures\|noise_settings` | id из реестров | + | + |
| `params <dim> [--fmt json\|tsv] [--out f]` | полный дамп `Climate.ParameterList` (порядок сохранён): 6 квантованных интервалов [min,max] ×10000, offset, биом. End — описание правил | + (ow/nether/end) | + |
| `rtree <dim> [--out f]` | дамп внутреннего R-дерева `Climate.RTree` (рефлексия) — для точного воспроизведения обхода и тай-брейка | + | + |
| `climate <dim> <seed> <qx> <qy> <qz> [--mode]` | 6 квантованных значений `TargetPoint` + сырые значения (double в 26.1/26.2, float в 26.3) + биты + биом. End: `height_value` в weirdBlock | + | + (`point`/`chunk`) |
| `climategrid <dim> <seed> <qx0> <qz0> <nx> <nz> <qy> [--raw] [--biome]` | то же по сетке (индекс `iz*nx+ix`) | + | + |
| `biome <dim> <seed> <qx0> <qz0> <nx> <nz> <qy> [--fmt json\|csv\|bin\|hash] [--out f] [--brute]` | сетка «сырых» биомов (без зума). `hash` = FNV-1a64 по именам (не зависит от палитры); `--brute` сравнивает с `findValueBruteForce` | + | + |
| `noise <dim> <seed> <noise_id> x y z [x y z…] [--twod]` | значение именованного NormalNoise, построенного как в `RandomState` (`Noises.instantiate`; nether/temperature и nether/vegetation — как в проводке). Значение + `dbits` + `fbits` | + | + (float-нативно) |
| `df <dim> <seed> <density_function_id> x y z […]` | значение зарегистрированной density-функции в блоковой точке без кэшей | + (через подстановку в NoiseRouter копии settings и настоящий `RandomState`) | + (`RandomState.sampleBlockValueUncached`) |
| `structs <dim> <seed> <set_id\|all> <cx0> <cz0> <nx> <nz>` | «потенциальные» чанки структур: реальный `ChunkGeneratorStructureState` + `StructurePlacement.isStructureChunk` | + | + |
| `structsets <dim> <seed>` | параметры `StructureSet` (placement через реальный codec), структуры и веса | + | + |
| `stronghold <seed>` | 128 позиций колец `ConcentricRingsStructurePlacement` (зависят от биомов) | + | + |
| `structstart <dim> <seed> <cx> <cz> [set\|all]` | реальный `Structure.generate` (jigsaw, высоты, биом старта) с настоящим `StructureTemplateManager` — валидность старта, bbox, куски | + | + |
| `pillars <seed>` | шипы End: центры, радиусы, высоты, guarded + геометрия клетки (`EndSpikeFeature.getSpikesForLevel`) | + | + |
| `slime <seed> <cx0> <cz0> <nx> <nz>` | слайм-чанки: `WorldgenRandom.seedSlimeChunk(cx,cz,seed,987234911L).nextInt(10)==0` | + | + |
| `hashed <seed>` | `BiomeManager.obfuscateSeed(seed)` | + | + |
| `blockbiome <dim> <seed> <x> <y> <z> [nx nz [step]]` | биом на блок-уровне через `BiomeManager.getBiome` (зум + obfuscated seed) | + | + |
| `height <dim> <seed> <x> <z> [heightmap]` / `heightgrid … <x0> <z0> <nx> <nz> <step> [hm]` | `NoiseBasedChunkGenerator.getBaseHeight/getFirstFreeHeight/getFirstOccupiedHeight` без генерации чанков | + | + |
| `defaultspawn <dim> <seed>` | климатическая точка спавна (`findSpawnPosition` / `NoiseSpawnFinder`) + высота WORLD_SURFACE_WG в центре чанка | + | + |

Форматы: каждый ответ — одна JSON-строка `{"ok":true,"cmd":...}` либо `{"ok":false,"error":...}`. Сетки — плоские массивы, индекс `iz*nx+ix`.
Кварты: `block = quart*4`. Все long-значения климата — квантованные (`(long)(float*10000)`).

## Различия API между версиями (и как они закрыты)

| Место | 26.1 / 26.2 | 26.3 | Слой |
|---|---|---|---|
| Density-функции | `levelgen/DensityFunction(s)`, `double compute(ctx)` | `levelgen/densityfunction/*`, `float sampleValue(...)`, `DensityFunctionCompiler` | `Compat.climate/df` |
| `RandomState` | `create(settings, noises, seed)`; `sampler()` | `create(noises, seed, settings)`; `createClimateSampler(SamplerContext)`, `sampleBlockValueUncached` | `Compat.newRandomState` |
| Шумы | `NormalNoise.getValue` (double), реестр `NoiseParameters` | `Noise.get` (float, `NoiseStack`), реестр `NormalNoise` | `Compat.noise` |
| Биомы | `BiomeSource.getNoiseBiome(qx,qy,qz,sampler)` | `createResolver(sampler)` / `createResolverForChunk` (пакетный `sampleVolume`) | `Compat.climate` |
| Реестры | `RegistryLayer.WORLDGEN`, `WORLDGEN_REGISTRIES` | `RegistryLayer.WORLD`, `WORLD_REGISTRIES` | `Compat.loadRegistries` |
| Структуры | `StructurePlacement` — абстрактный класс; `Structure.generate(...)` без `climateSampler` | `StructurePlacement` — интерфейс (+`AbstractSpreadingStructurePlacement`); `ChunkGeneratorStructureState` получил `origin`; `generate(..., climateSampler, ...)` | `Compat.generateStructure` |
| `BiomeManager` | `BiomeManager(NoiseBiomeSource, seed)` | `BiomeManager(BiomeResolver, seed)` | `Compat.biomeManager` |
| Спавн | `Climate.Sampler.findSpawnPosition` | `NoiseSpawnFinder` + `SpawnTargetPoint` (на density-функциях) | `Compat.spawnTarget` |
| R-дерево климата | `CHILDREN_PER_NODE = 6` | `19` (`Climate.RTree.create(values, childrenPerNode)`) | — (важно для тай-брейка) |

## Известные ограничения

* **Режим `chunk` реализован только для 26.3** (там реальная генерация чанка считает климат пакетно через `sampleVolume` — см. «Расхождения»).
  В 26.1/26.2 `--mode chunk` = `point`: реальный путь (`NoiseChunk.cachedClimateSampler`) отличается от point лишь FlatCache (значения 2D-функций в квартах) — математически то же; прямая проверка через `NoiseChunk` не делалась.
* **Тай-брейк R-дерева недетерминирован в самой игре**: `Climate.RTree.search` начинает с `lastResult` (ThreadLocal) и использует строгие сравнения — при равенстве fitness результат зависит от предыдущего запроса в потоке.
  Эталон вызывает точки последовательно в порядке сетки (детерминированно), `--brute` сравнивает с `findValueBruteForce` (первый минимум в порядке таблицы).
* `stronghold` / `structs` для strongholds используют `Util.backgroundExecutor()` (многопоточность игры) — позиции зависят от биомов через те же RTree/ties.
* `defaultspawn`: финальный спавн игрока (`PlayerSpawnFinder.getSpawnPosInChunk`) требует блоков сгенерированных чанков — не воспроизводится; даётся климатическая часть.
* `structstart`: структуры, чья генерация требует уже сгенерированных соседних чанков/`StructureManager` (напр. проверка пересечений), тут не учитываются; цикл выбора из набора — копия glue игры.
* `height`: высоты по `getBaseHeight` (шум ± aquifer), без поверхностных правил/деревьев/карверов.
* `preset`: `flat`, `debug_all_block_states`, `single_biome_surface` не поддержаны (генератор не multi-noise/noise-based для biome params).
* `rtree`/`params` для End — описание правил (константы из `TheEndBiomeSource`), таблицы параметров там нет.

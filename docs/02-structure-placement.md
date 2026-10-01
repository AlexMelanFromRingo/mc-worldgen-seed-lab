# 02. Размещение структур (StructurePlacement / structure_set), 26.1 / 26.2 / 26.3

Ссылки `V:путь:строка` — от `src/dec/V/net/minecraft/...`; `V:data:...` — `src/data-V/data/minecraft/...`.
Таблицы сформированы скриптом `tools/extract_structure_sets.py` (-> `data/structure_sets-26.{1,2,3}.json`; `--diff`, `--md-all`).
**Проверено запуском реального кода** (`tools/verify/*`): (а) C-порт RandomSpread/reducer'ов/slime совпадает с игрой на 1280 строках дампа во всех трёх версиях;
(б) Python-порт колец strongholds (`tools/verify/ringport.py`) совпал с настоящим `ChunkGeneratorStructureState.getRingPositionsFor` в 26.2 и 26.3 для 5 seed × 128 позиций (реальный код, но с подменным `BiomeSource`, см. §4.4).

## 1. Архитектура
* Реестр `structure_set`: `{ "structures": [{structure, weight}...], "placement": {...} }`; каждая `structure` ссылается на `worldgen/structure/*.json` (тип, `biomes`, `step`, `terrain_adaptation`, ...). Все 34 (26.1/26.2) и 52 (26.3) структуры входят в какой-либо set (скрипт: 0 предупреждений) — «структур вне сета» нет.
* Типы placement: `random_spread`, `concentric_rings` (все версии); `dimension_origin` — только 26.3 (26.3:levelgen/structure/placement/DimensionOriginStructurePlacement.java:7-24, регистрация StructurePlacements.java:8-10). Ванильные данные `dimension_origin` не используют (grep по data-26.3 — 0). Он помещает структуру в чанк `ChunkGeneratorStructureState.getDimensionOrigin()` (26.3:chunk/ChunkGeneratorStructureState.java:176-178) = `ChunkGenerator.getOrigin(randomState)` = чанк мирового спавна (если у noise_settings есть `spawn_target`, 26.3:levelgen/NoiseBasedChunkGenerator.java:112-120) либо (0,0).
* 26.1/26.2: `StructurePlacement` — абстрактный класс + `StructurePlacementType` (26.2:.../placement/StructurePlacement.java:23-179). 26.3: интерфейс `StructurePlacement` (:11-29) + `AbstractSpreadingStructurePlacement` (:21-176), общая логика (salt/frequency/exclusion_zone/reducers) перенесена без изменения семантики (diff: только смена базового класса и `type()`→`codec()`).
* Общий порядок проверок: `isStructureChunk = isPlacementChunk && applyAdditionalChunkRestrictions(frequency) && applyInteractionsWithOtherStructures(exclusion_zone)` (26.3:AbstractSpreadingStructurePlacement.java:84-97; 26.2:StructurePlacement.java:82-96).

## 2. Конвейер «чанк → структура» (ChunkGenerator.createStructures, 26.3:chunk/ChunkGenerator.java:501-588)
```
для каждого set из state.possibleStructureSets():           // только sets, у которых хоть одна структура имеет биом из biomeSource.possibleBiomes() (ChunkGeneratorStructureState.java:66-72)
  если в чанке уже есть валидный start любой структуры сета -> пропуск
  если placement.isStructureChunk(state, cx, cz):
     если в сете 1 структура: tryGenerateStructure(она)
     иначе: options = копия списка (порядок как в JSON); rnd = WorldgenRandom(Legacy); rnd.setLargeFeatureSeed(seed, cx, cz); total = sum(weight)
            пока options не пуст: choice = rnd.nextInt(total); выбрать по накопленному весу (choice -= w; if choice<0 break);
                   if tryGenerateStructure(выбранная): return;  иначе options.remove(выбранная); total -= её вес   // СЛЕДУЮЩИЙ бросок из того же rnd
tryGenerateStructure: structure.generate(...): context.random = WorldgenRandom(Legacy) + setLargeFeatureSeed(seed,cx,cz) (Structure.java:246-249) — тот же seed, что у rnd выбора!
   -> findGenerationPoint(context) -> .filter(isValidBiome(stub)) -> pieces -> StructureStart.isValid()   (Structure.java:99-122, 26.3)
```
Следствия: (1) для сетов с несколькими структурами результат = «первая по броску структура, у которой биом валиден и generate успешен»; при непересекающихся биом-тегах (villages, ocean_ruins, shipwrecks, abandoned_camp) итог от порядка бросков не зависит, но nether_complexes (fortress вес 2, bastion_remnant вес 3, `nextInt(5)`: <2 → fortress) и ruined_portals (7 вариантов) зависят от биома на старте. (2) Первые значения `rnd` выбора и `context.random` структуры совпадают (один и тот же `setLargeFeatureSeed`) — корреляция между выбором типа и позицией внутри чанка. (3) Биом старта проверяется в точке `stub.position()` по сетке 4×4×4: `biomeResolver.getNoiseBiome(x>>2, y>>2, z>>2)` (26.3:Structure.java:233-239; 26.1/26.2: `isValidBiome` статический, `biomeSource.getNoiseBiome(..., randomState.sampler())`) — БЕЗ клиентского «зума/размывания» BiomeManager.
Не выяснено: построчная сверка `createStructures` 26.1/26.2 с 26.3 (структурно логика та же: stronghold/биом-пробы дают ожидаемые результаты во всех версиях, но diff файла не снимался).

## 3. random_spread
Поля (26.3:RandomSpreadStructurePlacement.java:15-26; AbstractSpreadingStructurePlacement.java:32-43): `spacing`,`separation` ∈ [0,4096], **spacing > separation**; `spread_type` ∈ {linear (по умолч.), triangular}; `salt` ≥ 0; `frequency` ∈ [0,1] (1.0); `frequency_reduction_method` ∈ {default, legacy_type_1, legacy_type_2, legacy_type_3}; `exclusion_zone {other_set, chunk_count∈[1,16]}`; `locate_offset` (только для /locate и карт: `getLocatePos`, StructurePlacement.java:20-22; на генерацию не влияет).
```c
// getPotentialStructureChunk(seed, cx, cz)  — RandomSpreadStructurePlacement.java:67-76 (идентично во всех версиях)
gx = floorDiv(cx, spacing); gz = floorDiv(cz, spacing);
wg = WorldgenRandom(Legacy); wg.setLargeFeatureWithSalt(seed, gx, gz, salt);  // seed' = gx*341873128712 + gz*132897987541 + seed + salt
limit = spacing - separation;
linear:     sx = nextInt(limit);                         sz = nextInt(limit);                        // RandomSpreadType.java:23-28
triangular: sx = (nextInt(limit) + nextInt(limit)) / 2;  sz = (nextInt(limit) + nextInt(limit)) / 2;   // порядок: X (2 броска), затем Z (2 броска); деление целочисленное
return (gx*spacing + sx, gz*spacing + sz);
isPlacementChunk: (cx,cz) == getPotentialStructureChunk(levelSeed, cx, cz)                                  // :78-82
```
Поток зависит только от `seed mod 2^48` (проверено). `nextInt(limit)`: при `limit`=степень двойки (ancient_cities 16, nether_fossils 1, buried/mineshaft 1) — ветка `limit*next(31)>>31`, иначе rejection-sampling (см. doc 01, §2). Вырожденные случаи: `limit = 1` (nether_fossils spacing 2/sep 1; buried_treasures и mineshafts spacing 1/sep 0) ⇒ позиция НЕ зависит от seed: nether_fossils — всегда чанки `(2gx, 2gz)`; buried/mineshaft — любой чанк, фильтр целиком в reducer'е.
Reducers (применяются при `frequency < 1`; 26.3:AbstractSpreadingStructurePlacement.java:91-93,101-126; 26.1/26.2:StructurePlacement.java:88-90,104-129), все на `WorldgenRandom(Legacy(0))`, C-код — doc 01 §5.3:
* `default`: `setLargeFeatureWithSalt(seed, salt, cx, cz)` (**salt на месте x, cx на месте z, cz — в blend**: состояние = salt·341873128712 + cx·132897987541 + seed + cz), `nextFloat() < frequency`;
* `legacy_type_1` (pillager_outposts, p=0.2): `setSeed((cx>>4) ^ ((cz>>4)<<4) ^ seed); nextInt(); nextInt((int)(1/p)) == 0` — решение общее для всего региона 16×16 чанков;
* `legacy_type_2` (buried_treasures, p=0.01): `setLargeFeatureWithSalt(seed, cx, cz, 10387320); nextFloat() < p` (salt из JSON игнорируется);
* `legacy_type_3` (mineshafts, p=0.004): `setLargeFeatureSeed(seed, cx, cz); nextDouble() < (double)p`.
`exclusion_zone`: `hasStructureChunkInRange(other_set, cx, cz, r)` перебирает квадрат (2r+1)² и вызывает `other.placement.isStructureChunk` (ChunkGeneratorStructureState.java:202-214; ExclusionZone :140-142 в Abstract…): pillager_outposts запрещены в радиусе 10 чанков от любого **потенциального** чанка деревни (биом деревни не проверяется!).

## 4. concentric_rings (strongholds)
Поля: `distance`∈[0,1023], `spread`∈[0,1023], `count`∈[1,4095], `preferred_biomes` (тег), + общие. Данные: distance=32, spread=3, count=128, `#minecraft:stronghold_biased_to` (data:worldgen/structure_set/strongholds.json).
### 4.1. Позиции (ChunkGeneratorStructureState.generateRingPositions, 26.3:114-174; 26.1/26.2 — тот же алгоритм, отличие только `randomState.sampler()` вместо `randomState` в вызове поиска)
```
rnd = LegacyRandomSource; rnd.setSeed(concentricRingsSeed)   // = levelSeed для обычных миров (createForNormal), 0 для flat (createForFlat)  :57-64,126
angle = rnd.nextDouble() * PI * 2;  pic = 0; circle = 0
for i in 0..count-1:
    dist = 4*distance + distance*circle*6 + (rnd.nextDouble() - 0.5) * (distance * 2.5)     // int-часть + double
    ix = (int)Math.round(cos(angle)*dist);  iz = (int)Math.round(sin(angle)*dist)
    biomeRnd = rnd.fork()                                     // LegacyRandomSource(rnd.nextLong()): ПОТРЕБЛЯЕТ 2 next(32) у основного rnd
    pos[i] = findBiomeHorizontal(ix*16+8, 0, iz*16+8, 112, preferred::contains, biomeRnd, ...)  ? chunk(pos) : (ix, iz)
    angle += 2*PI/spread;  if (++pic == spread) { circle++; pic = 0; spread += 2*spread/(circle+1) /*int-деление*/; spread = min(spread, count - i); angle += rnd.nextDouble()*PI*2 }
```
Размеры колец при (distance 32, spread 3, count 128): 3, 6, 10, 15, 21, 28, 36, 9 (итого 128).
### 4.2. Биом-поиск (BiomeSource.findBiomeHorizontal, 26.3:BiomeSource.java:93-151; 26.1=26.2 идентичны между собой)
Вызов с `skip=1, findClosest=false`: `noiseRadius = 112>>2 = 28`; `startRadius = noiseRadius` ⇒ сканируется ВЕСЬ квадрат 57×57 квартов (z от −28 до 28 внешний цикл, x от −28 до 28 внутренний) на **quart-Y = 0 (блоки y=0..3)** вокруг `(ix*16+8)>>2`. Выбор — reservoir-sampling: для каждого попадания в предпочитаемый биом `if (result == null || biomeRnd.nextInt(found+1) == 0) result = (quartX*4, y, quartZ*4); found++`. Итоговый чанк = `(quartX*4)>>4 = quartX>>2`. Если попаданий нет — остаётся `(ix, iz)`. Значит позиция страхолда зависит от (а) 48-битной геометрии (seed mod 2^48) и (б) карты биомов на y≈0 в окне ±112 блоков (то есть от климатических шумов, 64 бита, и от того, попадают ли пещерные биомы dripstone_caves/lush_caves/sulfur_caves в тег `stronghold_biased_to`).
### 4.3. Проверка
`isPlacementChunk = ringPositions.contains(ChunkPos(cx,cz))` (ConcentricRings…java:86-89). Порт геометрии+резервуара: `tools/verify/ringport.py` (псевдокод выше).
### 4.4. Что проверено запуском
Настоящий `getRingPositionsFor` с подменным `BiomeSource` (`FakeBS`: детерминированная хэш-карта «предпочитаемый/нет» на квартах) совпал с `ringport.py` по всем 128 позициям для seed ∈ {0, 1, −1, 123456789, −4172144997902289642} в 26.2 и 26.3 (`tools/verify/RingFake.java`). Это проверяет: геометрию, `fork()`-потребление RNG, порядок сканирования, резервуар, округление, преобразование quart→chunk.
### 4.5. Эмпирика «тот же seed — те же страхолды?» (реальные биомы, `tools/verify/SH.java`, 24 seed × 128)
26.1→26.2: отличаются позиции **116 из 3072 (3,8 %)**, у всех 24 seed хотя бы один страхолд иной; 26.2→26.3: **58 из 3072 (1,9 %)**, снова у всех 24; 26.1→26.3: 173 (5,6 %). Причины (гипотезы по коду, не разложены экспериментально): 26.2 — биом `sulfur_caves` добавлен в `stronghold_biased_to` (и подземные биомы на y≈0 изменились), 26.3 — численные различия шумов (float) и новый биом `dappled_forest`. Вывод: кольца 48-битной геометрии одинаковы во всех версиях, а итоговые чанки страхолдов — НЕТ; для кракинга используйте биом-независимую часть (±7 чанков от `(ix,iz)`) либо считайте климат реальным кодом нужной версии.

## 5. Таблицы structure_set (из `data/structure_sets-*.json`)
Все числовые параметры placement **идентичны** в 26.1, 26.2, 26.3; различаются только биом-теги (sulfur_caves в 26.2, dappled_forest в 26.3) и новый set `abandoned_camp` (26.3).
| set | структуры в сете (вес) | placement | spacing | sep | limit=sp-sep | spread | salt | frequency | freq-метод | exclusion_zone | locate_offset | измерение | версии |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| abandoned_camp | 18 структур abandoned_camp_* (вес 1 каждая) | random_spread | 37 | 8 | 29 | linear | 91231127 | 1.0 | default | - | (0, 0, 0) | overworld | 26.3 |
| ancient_cities | ancient_city(1) | random_spread | 24 | 8 | 16 | linear | 20083232 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| buried_treasures | buried_treasure(1) | random_spread | 1 | 0 | 1 | linear | 0 | 0.01 | legacy_type_2 | - | (9, 0, 9) | overworld | 26.1,26.2,26.3 |
| desert_pyramids | desert_pyramid(1) | random_spread | 32 | 8 | 24 | linear | 14357617 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| end_cities | end_city(1) | random_spread | 20 | 11 | 9 | triangular | 10387313 | 1.0 | default | - | (0, 0, 0) | the_end | 26.1,26.2,26.3 |
| igloos | igloo(1) | random_spread | 32 | 8 | 24 | linear | 14357618 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| jungle_temples | jungle_pyramid(1) | random_spread | 32 | 8 | 24 | linear | 14357619 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| mineshafts | mineshaft(1), mineshaft_mesa(1) | random_spread | 1 | 0 | 1 | linear | 0 | 0.004 | legacy_type_3 | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| nether_complexes | fortress(2), bastion_remnant(3) | random_spread | 27 | 4 | 23 | linear | 30084232 | 1.0 | default | - | (0, 0, 0) | the_nether | 26.1,26.2,26.3 |
| nether_fossils | nether_fossil(1) | random_spread | 2 | 1 | 1 | linear | 14357921 | 1.0 | default | - | (0, 0, 0) | the_nether | 26.1,26.2,26.3 |
| ocean_monuments | monument(1) | random_spread | 32 | 5 | 27 | triangular | 10387313 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| ocean_ruins | ocean_ruin_cold(1), ocean_ruin_warm(1) | random_spread | 20 | 8 | 12 | linear | 14357621 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| pillager_outposts | pillager_outpost(1) | random_spread | 32 | 8 | 24 | linear | 165745296 | 0.2 | legacy_type_1 | villages r=10 | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| ruined_portals | ruined_portal(1), ruined_portal_desert(1), ruined_portal_jungle(1), ruined_portal_swamp(1), ruined_portal_mountain(1), ruined_portal_ocean(1), ruined_portal_nether(1) | random_spread | 40 | 15 | 25 | linear | 34222645 | 1.0 | default | - | (0, 0, 0) | overworld/the_nether | 26.1,26.2,26.3 |
| shipwrecks | shipwreck(1), shipwreck_beached(1) | random_spread | 24 | 4 | 20 | linear | 165745295 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| strongholds | stronghold(1) | concentric_rings: distance=32 spread=3 count=128 preferred=#minecraft:stronghold_biased_to | - | - | - | - | 0 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| swamp_huts | swamp_hut(1) | random_spread | 32 | 8 | 24 | linear | 14357620 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| trail_ruins | trail_ruins(1) | random_spread | 34 | 8 | 26 | linear | 83469867 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| trial_chambers | trial_chambers(1) | random_spread | 34 | 12 | 22 | linear | 94251327 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| villages | village_plains(1), village_desert(1), village_savanna(1), village_snowy(1), village_taiga(1) | random_spread | 34 | 8 | 26 | linear | 10387312 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |
| woodland_mansions | mansion(1) | random_spread | 80 | 20 | 60 | triangular | 10387319 | 1.0 | default | - | (0, 0, 0) | overworld | 26.1,26.2,26.3 |

| structure | type | step | terrain_adaptation | биом-тег | измерение | в каких версиях | изменился тег/состав биомов? |
|---|---|---|---|---|---|---|---|
| abandoned_camp_bamboo_jungle | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_bamboo_jungle | overworld | 26.3 | нет |
| abandoned_camp_birch_forest | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_birch_forest | overworld | 26.3 | нет |
| abandoned_camp_cherry_grove | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_cherry_grove | overworld | 26.3 | нет |
| abandoned_camp_dappled_forest | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_dappled_forest | overworld | 26.3 | нет |
| abandoned_camp_flower_forest | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_flower_forest | overworld | 26.3 | нет |
| abandoned_camp_forest | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_forest | overworld | 26.3 | нет |
| abandoned_camp_meadow | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_meadow | overworld | 26.3 | нет |
| abandoned_camp_old_growth_birch_forest | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_old_growth_birch_forest | overworld | 26.3 | нет |
| abandoned_camp_old_growth_pine_taiga | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_old_growth_pine_taiga | overworld | 26.3 | нет |
| abandoned_camp_old_growth_spruce_taiga | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_old_growth_spruce_taiga | overworld | 26.3 | нет |
| abandoned_camp_pale_garden | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_pale_garden | overworld | 26.3 | нет |
| abandoned_camp_savanna | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_savanna | overworld | 26.3 | нет |
| abandoned_camp_snowy_taiga | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_snowy_taiga | overworld | 26.3 | нет |
| abandoned_camp_sparse_jungle | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_sparse_jungle | overworld | 26.3 | нет |
| abandoned_camp_swamp | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_swamp | overworld | 26.3 | нет |
| abandoned_camp_taiga | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_taiga | overworld | 26.3 | нет |
| abandoned_camp_windswept_forest | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_windswept_forest | overworld | 26.3 | нет |
| abandoned_camp_wooded_badlands | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/abandoned_camp_wooded_badlands | overworld | 26.3 | нет |
| ancient_city | jigsaw | underground_decoration | beard_box | #minecraft:has_structure/ancient_city | overworld | 26.1,26.2,26.3 | нет |
| bastion_remnant | jigsaw | surface_structures | none | #minecraft:has_structure/bastion_remnant | the_nether | 26.1,26.2,26.3 | нет |
| buried_treasure | buried_treasure | underground_structures | none | #minecraft:has_structure/buried_treasure | overworld | 26.1,26.2,26.3 | нет |
| desert_pyramid | desert_pyramid | surface_structures | none | #minecraft:has_structure/desert_pyramid | overworld | 26.1,26.2,26.3 | нет |
| end_city | end_city | surface_structures | none | #minecraft:has_structure/end_city | the_end | 26.1,26.2,26.3 | нет |
| fortress | fortress | underground_decoration | none | #minecraft:has_structure/nether_fortress | the_nether | 26.1,26.2,26.3 | нет |
| igloo | igloo | surface_structures | none | #minecraft:has_structure/igloo | overworld | 26.1,26.2,26.3 | нет |
| jungle_pyramid | jungle_temple | surface_structures | none | #minecraft:has_structure/jungle_temple | overworld | 26.1,26.2,26.3 | нет |
| mansion | woodland_mansion | surface_structures | none | #minecraft:has_structure/woodland_mansion | overworld | 26.1,26.2,26.3 | нет |
| mineshaft | mineshaft | underground_structures | none | #minecraft:has_structure/mineshaft | overworld | 26.1,26.2,26.3 | 26.2: +['sulfur_caves'] -[]; 26.3: +['dappled_forest'] -[] |
| mineshaft_mesa | mineshaft | underground_structures | none | #minecraft:has_structure/mineshaft_mesa | overworld | 26.1,26.2,26.3 | нет |
| monument | ocean_monument | surface_structures | none | #minecraft:has_structure/ocean_monument | overworld | 26.1,26.2,26.3 | нет |
| nether_fossil | nether_fossil | underground_decoration | beard_thin | #minecraft:has_structure/nether_fossil | the_nether | 26.1,26.2,26.3 | нет |
| ocean_ruin_cold | ocean_ruin | surface_structures | none | #minecraft:has_structure/ocean_ruin_cold | overworld | 26.1,26.2,26.3 | нет |
| ocean_ruin_warm | ocean_ruin | surface_structures | none | #minecraft:has_structure/ocean_ruin_warm | overworld | 26.1,26.2,26.3 | нет |
| pillager_outpost | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/pillager_outpost | overworld | 26.1,26.2,26.3 | нет |
| ruined_portal | ruined_portal | surface_structures | none | #minecraft:has_structure/ruined_portal_standard | overworld | 26.1,26.2,26.3 | 26.2: +['sulfur_caves'] -[]; 26.3: +['dappled_forest'] -[] |
| ruined_portal_desert | ruined_portal | surface_structures | none | #minecraft:has_structure/ruined_portal_desert | overworld | 26.1,26.2,26.3 | нет |
| ruined_portal_jungle | ruined_portal | surface_structures | none | #minecraft:has_structure/ruined_portal_jungle | overworld | 26.1,26.2,26.3 | нет |
| ruined_portal_mountain | ruined_portal | surface_structures | none | #minecraft:has_structure/ruined_portal_mountain | overworld | 26.1,26.2,26.3 | нет |
| ruined_portal_nether | ruined_portal | surface_structures | none | #minecraft:has_structure/ruined_portal_nether | the_nether | 26.1,26.2,26.3 | нет |
| ruined_portal_ocean | ruined_portal | surface_structures | none | #minecraft:has_structure/ruined_portal_ocean | overworld | 26.1,26.2,26.3 | нет |
| ruined_portal_swamp | ruined_portal | surface_structures | none | #minecraft:has_structure/ruined_portal_swamp | overworld | 26.1,26.2,26.3 | нет |
| shipwreck | shipwreck | surface_structures | none | #minecraft:has_structure/shipwreck | overworld | 26.1,26.2,26.3 | нет |
| shipwreck_beached | shipwreck | surface_structures | none | #minecraft:has_structure/shipwreck_beached | overworld | 26.1,26.2,26.3 | нет |
| stronghold | stronghold | surface_structures | bury | #minecraft:has_structure/stronghold | overworld | 26.1,26.2,26.3 | 26.2: +['sulfur_caves'] -[]; 26.3: +['dappled_forest'] -[] |
| swamp_hut | swamp_hut | surface_structures | none | #minecraft:has_structure/swamp_hut | overworld | 26.1,26.2,26.3 | нет |
| trail_ruins | jigsaw | underground_structures | bury | #minecraft:has_structure/trail_ruins | overworld | 26.1,26.2,26.3 | нет |
| trial_chambers | jigsaw | underground_structures | encapsulate | #minecraft:has_structure/trial_chambers | overworld | 26.1,26.2,26.3 | 26.2: +['sulfur_caves'] -[]; 26.3: +['dappled_forest'] -[] |
| village_desert | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/village_desert | overworld | 26.1,26.2,26.3 | нет |
| village_plains | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/village_plains | overworld | 26.1,26.2,26.3 | нет |
| village_savanna | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/village_savanna | overworld | 26.1,26.2,26.3 | нет |
| village_snowy | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/village_snowy | overworld | 26.1,26.2,26.3 | нет |
| village_taiga | jigsaw | surface_structures | beard_thin | #minecraft:has_structure/village_taiga | overworld | 26.1,26.2,26.3 | нет |



### 5.1. Различия между версиями (вывод `tools/extract_structure_sets.py --diff`)
* 26.1→26.2: к биомам mineshaft, ruined_portal (standard), stronghold (structure.biomes И preferred_biomes), trial_chambers добавлен `minecraft:sulfur_caves` — nothing else.
* 26.2→26.3: добавлен `minecraft:dappled_forest` в те же теги (mineshaft, ruined_portal, stronghold, trial_chambers); новый set **abandoned_camp**; у ruined_portal/_jungle/_mountain/_nether `air_pocket_probability` setup'ов `on_land_surface`/`in_nether`/jungle изменено 0.5 → 0.0 (косметика пост-обработки; влияет ли на потребление RNG — не выяснено, проверить RuinedPortalStructure/RuinedPortalPiece).

## 6. abandoned_camp (26.3)
data:worldgen/structure_set/abandoned_camp.json: `random_spread`, spacing 37, separation 8 (limit 29), linear, salt 91231127, frequency 1, 18 структур веса 1 (по одной на биом: bamboo_jungle, birch_forest, cherry_grove, dappled_forest, flower_forest, forest, meadow, old_growth_{pine_taiga,birch_forest,spruce_taiga}, pale_garden, savanna, snowy_taiga, sparse_jungle, swamp, taiga, windswept_forest, wooded_badlands). Теги биомов попарно не пересекаются (проверено скриптом) ⇒ тип определяется биомом старта. Каждая структура — `jigsaw`: `start_pool = minecraft:abandoned_camp/tent/<биом>`, `size 2`, `start_height {absolute 0}`, `project_start_to_heightmap WORLD_SURFACE_WG`, `max_distance_from_center 80`, `use_expansion_hack true`, `terrain_adaptation beard_thin`, `step surface_structures`. Это палатки/стоянки (лагерь) в лесных биомах. Не выяснено: содержимое template_pool/processor_list, какие именно элементы (нужно читать data-26.3/data/minecraft/worldgen/template_pool/abandoned_camp/**).

## 7. Чьё положение определяется не «обычным» random_spread
* **Все** структуры ванили идут через structure_set; нестандартное — вырожденные `spacing 1/2` (buried_treasure, mineshaft, nether_fossil), `concentric_rings` (strongholds), общий set с весами (nether_complexes; ruined_portals — один и тот же набор потенциальных чанков в Overworld и Nether: совпадают chunk-координаты, биом-проверка своя, `ruined_portal_nether` в Nether).
* Не структуры (не в этом документе): шипы/шлюзы End, жеоды, dungeons — см. `docs/03`.
* End city: set `end_cities` spacing 20/sep 11/triangular/salt 10387313 (limit 9); биом-тег has_structure/end_city; требования центрального острова/высоты — в классе EndCityStructure: не выяснено (не читался в этой сессии).

## 8. Что посчитать, чтобы подтвердить структуру в чанке (общий чек-лист)
1. `getPotentialStructureChunk` == чанк (только 48 бит seed) [rings: геометрия + биомы]. 2. reducer (frequency) и exclusion_zone (тоже 48 бит). 3. Выбор типа: `setLargeFeatureSeed(seed,cx,cz)` + взвешенные `nextInt(total)` (48 бит). 4. `findGenerationPoint` (позиция в чанке, высоты по heightmap = нужен рельеф ⇒ density/noise, 64 бита) и биом старта (quart-сетка, climate, 64 бита); `terrain_adaptation`/`start_height` влияют на y и пост-форму, не на факт появления. Структуры, определяемые ТОЛЬКО RNG (без биомов/рельефа) до проверки самого факта появления: шаги 1–3; но факт появления почти всегда требует биома (1 бит «есть/нет» + тип).
Детальный разбор по типам структур (джигсоу: `start_height`/heightmap; fortress/monument/mansion/stronghold/end city и т.д.): не выяснено — делегировался параллельному агенту (результат, если появится: scratchpad `parts/structgen.md`).

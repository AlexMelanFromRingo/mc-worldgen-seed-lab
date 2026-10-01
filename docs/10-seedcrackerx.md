# 10. SeedcrackerX: полный разбор конвейера (ветка master, mod 2.16.2, MC 26.3)

Источник: `refs/SeedcrackerX/src/main/java/kaptainwutax/seedcrackerX/` (далее `S/`), библиотеки
`refs/seedfinding/*` (клоны `SeedFinding/mc_*_java`, `mjtb49/LattiCG`; хэши совпадают с `gradle.properties`).
Всё ниже проверено чтением кода; числа скорости — только мои измерения (помечены), остальное — «оценка».

## 1. Конвейер (диаграмма)

```
 клиент получает чанки (ClientPacketListenerMixin.onChunkData)           вход: ТОЛЬКО то, что видит игрок
        |                                                                (блоки, биомы, пакет логина)
        v
 FinderQueue.onChunkData -> Finder.Type (S/finder/Finder.java:~98-115) -> пул из 5 потоков
        |   структуры: BuriedTreasure, DesertTemple, EndCity, JungleTemple, Monument, SwampHut,
        |              Shipwreck, Outpost, Igloo, TrialChambers        (Category.STRUCTURES)
        |   декораторы: EndPillars, EndGateway, Dungeon, EmeraldOre, DesertWell, WarpedFungus
        |   биомы: BiomeFinder (по умолчанию выключен)
        v
 DataStorage.add{Base,Pillar,Biome,HashedSeed}Data  (S/cracker/storage/DataStorage.java:108-151)
        |   baseSeedData = ScheduledSet<TreeSet по SEED_DATA_COMPARATOR>  (структуры раньше декораторов,
        |   затем по убыванию «битов»); добавление откладывается (scheduleAdd) и сливается dump() в tick()
        |   каждое добавление ставит в очередь DataAddedEvent -> TimeMachine.poke(Phase)
        v
 TimeMachine.poke  (S/cracker/storage/TimeMachine.java:55)   фазы: PILLARS -> STRUCTURES -> BIOMES;
        |                                                     LIFTING -> STRUCURE_REDUCE -> BIOMES  (:503-515)
        |  (1) PILLARS   : 2^16 pillar seed'ов по высотам 10 столбов           (:95, PillarData.java)
        |  (2) STRUCTURES: 2^32 (=4 потока * 2^30) при известном pillar seed   (:181, timeMachine():492)
        |      LIFTING   : 2^19 младших бит * 2^29 старших (если liftBits>=40) (:118)
        |  (3) REDUCE    : пересечение кандидатов с pillar seed / со всеми данными (:421)
        |  (4) BIOMES    : structure seed (48 бит) -> world seed (64 бита)     (:262)
        |        a) декораторы 1.18+ (Xoroshiro) 2^16 верхних бит  (если decoratorBits>32)   (:270-312)
        |        b) hashed seed из пакета логина: SHA-256 на 2^16 кандидатах                (:314-334)
        |        c) биомы (нужно >=7 BiomeData): «fuzzy» по StructureSeed.toRandomWorldSeeds  (:336-366)
        |        d) «deep» по 2^16 верхним битам, если structureSeeds<=10                   (:370-402)
        v
 SeedCracker.entrypoints.pushWorldSeed(seed)  (:79-81)  [+ необязательная выгрузка в Google-таблицу, Database.java]
```

## 2. Что собирается и сколько «бит» это даёт

`DataStorage.getBits` (`S/cracker/storage/DataStorage.java:60-77`) — оценка информации одного наблюдения:

| наблюдение | формула бит | пример (26.x) |
|---|---|---|
| UniformStructure (linear) | log2(offset^2), offset = spacing-separation | desert/igloo/jungle/swamp 24 -> 9.17; trial 22 -> 8.9 |
| TriangularStructure | log2(peak^2) (верхняя оценка: реальная энтропия ниже) | monument 27 -> 9.5; end city 9 -> 6.3 |
| BuriedTreasure | log2(100) = 6.64 (шанс 1% на чанк) | |
| DesertWell 1000*16*16; Dungeon 256*16*16*0.125; EndGateway 700*16*16*7; EmeraldOre... | по формулам, но для **1.18+ декораторы дают 0** в фазе structure seed (`:66-67`), т.к. используют Xoroshiro (64 бита) | |

Пороги:
* **regular bits**: `getBaseBits()` (`:161`) — сумма бит всех структур (кроме PillagerOutpost) >= **`getWantedBits()` = 32** (`:196`). 32 бита + 16 бит pillar seed = 48.
* **liftable bits**: `getLiftingBits()` (`:172`) — сумма log2(offset^2) только по `OldStructure` (desert/igloo/jungle/swamp) и `Shipwreck`; нужно **>= 40** (`TimeMachine.java:119`) — тогда pillar seed не нужен.
* биомы: `notEnoughBiomeData()` = меньше 7 записей (`:200`).

Как собирается (детали):
* **Структуры** ищутся не по метаданным, а по *блочным шаблонам* (`PieceFinder`/`JigsawFinder`: `fillWithOutline`, 4 поворота); в `addBaseData` пишется `RegionStructure.Data(chunkX, chunkZ)`. Для End city/monument есть сдвиги координат чанка (`EndCityFinder.java:107` для >=1.19 +1 блок; `MonumentFinder.java:~73` чанк+1). Для buried treasure шаблон — сундук в (9,9) чанка (`BuriedTreasureFinder.java:~45`).
* **TrialChambers** — собственный класс `S/structures/TrialChambers.java`: `Config(34, 12, 94251327)` — совпадает с датапаком 26.1–26.3 (`structure_set/trial_chambers.json`). Шаблоны — конечные комнаты с ominous vault (`buildEnd1/buildEnd2`, `TrialChambersFinder.java`).
* **End pillars** (`EndPillarsFinder.java`): 10 маркеров бедрока на радиусе 42, высоты -> `PillarData(List<Integer>)`.
* **Gateway/Dungeon/DesertWell/Emerald/Fungus** — декораторы (позиция внутри чанка + y + биом); сохраняются как `Decorator.Data`.
* **Hashed seed**: `ClientPacketListenerMixin.java:57-66`: `packet.commonPlayerSpawnInfo().seed()` при `handleLogin` и `handleRespawn` -> `HashedSeedData`. В 26.3 сервер по-прежнему отправляет его (`ServerPlayer.java:2209`, `BiomeManager.obfuscateSeed` = SHA-256 от 8 байт LE, берутся первые 8 байт — `WorldSeed.toHash`).
* **BiomeData** (`BiomeFinder`): (biome, x>>2, z>>2), проверка `source.getBiomeForNoiseGen`.
* Сохранение: `StructureSave` пишет строки `имя;chunkX;chunkZ` — удобный формат входа для нашего CLI.

## 3. Математика восстановления structure seed

Формулы (`mc_core/.../ChunkRand.java`, подтверждены `WorldgenRandom.java` игры и 455 векторами из `crack/experiments/vectors/`):

```
regionSeed  = W + rx*341873128712 + rz*132897987541 + salt            (RegionSeed.A,B; setLargeFeatureWithSalt)
state0      = (regionSeed ^ 0x5DEECE66D) & (2^48-1)                   (Random.setSeed)
linear      : ox = nextInt(spacing-sep); oz = nextInt(spacing-sep)
triangular  : ox = (nextInt(p)+nextInt(p))/2; oz = (nextInt(p)+nextInt(p))/2        p = spacing-sep
chunk       = (rx*spacing + ox, rz*spacing + oz)   [rx = floorDiv(chunk, spacing)]
```
Проверка кандидата — `UniformStructure.canStart` / `TriangularStructure.canStart` (mc_feature): `rand.setSeed(baseRegionSeed + structureSeed); nextInt(...)==offsetX && nextInt(...)==offsetZ`, где `baseRegionSeed = setRegionSeed(0, rx, rz, salt)` — XOR выполняется *после* сложения, поэтому seed входит аддитивно только до XOR с 0x5DEECE66D (младшие 35 бит).

Соли (26.1–26.3, `src/data-*/.../structure_set`, идентичны во всех трёх): desert 14357617 (32/8), igloo 14357618, jungle 14357619, swamp 14357620, shipwreck 165745295 (24/4), village 10387312 (34/8), trial 94251327 (34/12), ancient city 20083232 (24/8), monument 10387313 (32/5 tri), mansion 10387319 (80/20 tri), end city 10387313 (20/11 tri), ruined portal 34222645 (40/15), nether complex 30084232 (27/4), trail ruins 83469867 (34/8); **новое в 26.3**: `abandoned_camp` salt 91231127 (37/8) — SeedcrackerX его не знает.

### 3.1 Пилларный путь (End)
`PillarSeed.fromStructureSeed` (`mc_core/.../PillarSeed.java`): `pillarSeed = Random(W).nextLong() & 0xFFFF`; высоты = `76 + 3*idx`, idx — перетасовка 0..9 через `Random(pillarSeed)` (проверено против игры: `EndSpikeFeature.java:49-51, 187-197`, `Util.toShuffledList`). 16 бит nextLong = биты 16..31 второго состояния LCG => `TimeMachine.timeMachine()` (`:492-501`) собирает state2 из (верхние 16 | pillar<<16 | нижние 16) и откатывает LCG на 2 шага (`LCG.JAVA.combine(-2)`), XOR 0x5DEECE66D. Остаётся **2^32** значений partial — на каждое проверяются все структуры (`testStart`). Итого 48 = 16 (pillar) + 32 (структуры).

### 3.2 Lifting (без End)
`pokeLifting` (`TimeMachine.java:118-178`): для bound = 24 (desert/igloo/jungle/swamp) `nextInt(24) % 4 == r & 3` зависит только от **младших 19 бит** seed (биты 17..18 состояния после шага); перебираем 2^19, фильтруем по `% 4` (`:141-151`), затем достраиваем 2^29 старших бит (`:153-157`) и проверяем все структуры. Комментарий `:138-140` признаёт, что можно `%8` на 2^20. *Моё измерение ниже показывает, что это верное и очень быстрое решение.*

### 3.3 Decoder библиотеки LattiCG (используется только для Dungeon 1.13–1.17.1)
`Dungeon.Data.onDataAdded` (`S/cracker/decorator/Dungeon.java:~150-264`): `DynamicProgram.create(LCG.JAVA)`, `JavaCalls.nextInt(bound).equalTo(v)`, `filteredSkip` для «mossy»-ветвления -> `device.reverse()` -> `JavaRandomReverser.findAllValidSeeds` (`LattiCG/.../RandomReverser.java:57-90`): решётка строится так — для i-го измеренного вызова строка с `MULT^(callIndex_i - callIndex_0) mod 2^48`, последняя строка — модуль; для `nextInt(n)` **не** степень двойки добавляются 2 измерения (`addModuloMeasuredSeed`: «seed < MOD - MOD mod (n*2^17)» и «seed mod n*2^17 в [v*2^17, (v+1)*2^17)»); затем `LLL.reduce` (delta = `Params.recommendedDelta`) и перебор точек решётки `Enumerate` (ветви и границы), итог — стрим seed'ов. Для pow2-bound: интервал верхних бит `[v*2^(48-k), (v+1)*2^(48-k))`. **Для 26.x это не применяется**: `if (version.isNewerThan(v1_17_1)) return;` (`Dungeon.java:160`) — декорирование с 1.18 идёт на Xoroshiro128++ от *полного* 64-битного seed (`WorldgenRandom.setDecorationSeed`, `ChunkGenerator.applyBiomeDecoration`).
Population-реверс (`mc_reversal/PopulationReverser.java`) — шаговое решение по 16 бит (Hensel): для chunk-seed'а (48 бит) находит world seed за O(2^16) — тоже только для LCG-декораторов <=1.17.1.
`Lattice2D` (`ChunkRandomReverser.REGION_LATTICE(341873128712, 132897987541, 2^48)`) — 2D-редукция Лагранжа-Гаусса: по значению `x*A + z*B mod 2^48` находит *малые* (x,z) (terrain seed / region seed обратно к координатам).

### 3.4 От structure seed к world seed
* `WorldSeed.fromHash(structureSeed, hashed)` (`mc_core/.../WorldSeed.java`): перебор 2^16 верхних бит, `SHA-256(seed, 8 байт little-endian)[0..8]` == hashed (коллизии практически невозможны). **Это основной путь для 26.x.**
* Декораторы 1.18+ (`Decorator.Data.testStart(worldSeed, WorldgenRandom(Xoroshiro))`, `TimeMachine.java:270-312`): по 2^16 верхних бит на каждый structure seed; индексы `(step,index)` жёстко заданы в `Dungeon.java:28-31` (3,2) — **риск для 26.x**: порядок features мог смещаться (FeatureSorter), индексы надо сверять по oracle.
* Биомы: `OverworldBiomeSource` из `mc_biome@17af8cb` (2023, «Bump to 1.17.1») — **только слоистый генератор <=1.17.1, нет 1.18+ multi-noise** (проверено grep). Для 26.x фаза «биомы» в SeedcrackerX, по-видимому, не даёт верных результатов (вероятно; не запускал). `MCVersion.latest()` = 1.21.3 (`mc_core/.../MCVersion.java:9`), конфиги структур берутся оттуда; TrialChambers — свой класс.
* `StructureSeed.toRandomWorldSeeds` (`NextLongReverser`) — если world seed был сгенерирован случайно (`Random().nextLong()`), верхние 16 бит однозначно определяются 48 младшими => 1–2 кандидата. Не верно для seed'ов, введённых вручную (строка/число).

## 4. Мои измерения для механизма lifting (crack/experiments/cuda/struct48.cu, RTX 4080 SUPER, nvcc 12.0, Ryzen 5 5500 12 потоков)

* Реализация `check_obs` сверена с игрой: `crack/experiments/c/selftest` -> 965/965 векторов совпали для 26.1, 26.2, 26.3 (`vectors/gameref-26.{1,2,3}.txt` идентичны побайтово: структуры/слайм/пиллары/бедрок/mineshaft/стронгхолд в этих версиях НЕ менялись).
* Полный перебор 2^48 «в лоб» (ранний выход, 6 структур линейных): **1.83e11 W/с** на GPU (2^38 за 1.50 с) => 2^48 за **~1538 с = 25.6 мин** (экстраполяция измеренной скорости); с треугольными структурами (mansion/monument/end city, 8 штук) 1.15e11 W/с => ~41 мин. CPU (12 потоков): 3.4e8 W/с => ~9.6 суток.
* Lifting (L=20, 6 структур: 3 храма + shipwreck + trial + village): 1 кандидат младших бит, **всего 0.039 с на GPU / 0.727 с на CPU (12 потоков)**; найден верный seed.
* Статистика по 12 случайным seed'ам (`results/struct_lift_stats.txt`): 4 храма/корабль (36.2 бит) — ~4095 кандидатов (упёрлись в лимит вывода); 5 (45.3 бит) — большие списки (десятки–тысячи); 6 смешанных (54.5 бит) — **в среднем 0.92 ложных кандидата, верный найден 12/12**; 8 структур (73 бит) — 0 ложных.
* Важный вывод: «ложные» кандидаты — это сиблинги с теми же 20 младшими битами. Эффективная информация = info − max(0, lift_info − L). Для храмов (bound 24) 6 бит из 9.17 — «младшие» и избыточны после 20 бит; каждая дополнительная такая структура добавляет лишь ~3.2 бита. Поэтому порог SeedcrackerX «40 бит» — эвристика; реально нужно ≈ 28+ бит «не младшей» информации.

## 5. Что в SeedcrackerX для 26.x важно и слабо

1. Поддержка 26.3 формально есть (pom: Fabric 26.3, Loom), но ядро — библиотеки 2021–2023 (`mc_biome`, `mc_feature`), `MCVersion.latest()` = 1.21.3; новые структуры 26.x (abandoned_camp, ancient_city, ruined_portal, village, mansion, trail_ruins) не поддержаны (Config: только 10 структур, `Config.java:~21-33`).
2. Нет GPU, Java parallel stream, для pillar-пути 4 потока x 2^30 (`TimeMachine.java:202-236`): на CPU-часы уходят минуты.
3. Нет учёта того, что structure seed ≠ единственный seed для 1.18+ Overworld (кроме hashed seed).
4. `PillagerOutpost` исключён из bits («todo remove this when libs are updated», `:166`, `:132`).
5. База данных (Google Sheet, `Database.java`) — если включена, отправляет найденные seed'ы серверов (нам не нужна).

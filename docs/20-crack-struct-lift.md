# 20. crack-struct + crack-lift64: восстановление seed по структурам (26.1 / 26.2 / 26.3)

Законченный инструмент: наблюдения (позиции структур) -> 48-битный **structure seed** (`crack-struct`, CUDA, fallback CPU/OpenMP) ->
64-битный **world seed** (`crack-lift64`: hashed seed / биомы / «случайный» seed игры). Все параметры размещения читаются из
`data/structure_sets-<V>.json`, ничего не захардкожено; версии 26.1/26.2/26.3 различаются только таблицами.
Эталон корректности — реальный код игры (`oracle/`, векторы `tests/vectors/`). Всё, что ниже названо «измерено», запущено на этой машине
(RTX 4080 SUPER, Ryzen 5 5500 12 потоков, nvcc 12.0, GPU делят несколько агентов — скорости ядер с большим разбросом, берётся лучший прогон).

```
crack/
  Makefile                 make (crack-struct + crack-lift64), make cpu, make unit, make test, make e2e, make bench
  bin/crack-struct         structure seed по позициям структур            (src/crack_struct.cu, src/{lcg48,pred,sets,mini_json}.h)
  bin/crack-lift64         structure seed -> world seed                   (src/crack_lift64.c + engine/)
  bin/crack-struct-cpu     сборка без CUDA (make cpu)
  tests/e2e.py             сквозной тест против oracle (реальный код Mojang)
  tests/test_kernels.py    все пути поиска (lift/full × драйверы × GPU/CPU), без oracle
  tests/unit_pred.cpp, unit_lift64.c   юнит-тесты математики / SHA-256 / тай-брейков
  tests/stats.py           статистика «сколько структур нужно / сколько кандидатов»
  tests/bench_struct.sh    скорость ядер полного перебора
  tests/oracle_client.py   клиент oracle serve
```

## 1. Быстрый старт

```bash
cd crack && make            # bin/crack-struct (nvcc, sm_89), bin/crack-lift64 (gcc + engine/)
bin/crack-struct --list-sets --version 26.3                      # какие наборы/структуры известны, их параметры из JSON
bin/crack-struct --version 26.3 obs.txt > cands.txt              # 1) structure seed (48 бит), по одному на строку
bin/crack-lift64 --version 26.3 --seeds cands.txt --hashed H     # 2) world seed (hashed seed из пакета логина)
bin/crack-lift64 --version 26.3 --seeds cands.txt --biomes b.txt # 2') или по биомам
bin/crack-lift64 --version 26.3 --seeds cands.txt                # 2'') seed, сгенерированный игрой при пустом поле seed
```

Реальный пример (26.3; наблюдения сгенерированы `crack-struct --gen-obs ... --seed 4611686018427387905`; чанки — старт структур):

```
$ cat obs.txt                       $ bin/crack-struct --version 26.3 obs.txt
desert_pyramid;-767;-1072           версия 26.3: 8 наблюдений, 8 предикатов, информация ~ 71.2 бит, ожидаемо ложных кандидатов ~ 1.07e-07
igloo;-763;1222                     [lift] L=20 бит: 1 кандидатов младших бит (0.009 с); перебор старших 28 бит: 2.684e+08 проверок
swamp_hut;1158;-382                 режим lift/gpu: кандидатов 1 ...; поиск 0.021 с, всего 0.030 с
shipwreck;63;91                     1                                       <- structure seed (stdout)
trial_chambers;587;1100
village;-92;-1209                   $ bin/crack-lift64 --seeds cands.txt --hashed -8950587753039679121
ancient_city;576;29                 4611686018427387905  0x4000000000000001  # struct=1 hi16=0x4000 hashed
ocean_monument;1096;177
```

Корень проекта (где `data/`) определяется по расположению бинарника (`bin/../..`), либо `$MCGEN_ROOT`, либо `--data-dir DIR` (crack-struct: каталог `data/`; crack-lift64: `--root`/`--data-dir`, принимается и корень, и `data/`).
Новая версия игры для crack-struct: положить `data/structure_sets-<V>.json` (или `--sets FILE`) — перекомпиляция не нужна.

## 2. Форматы входных файлов

### 2.1. Наблюдения структур (crack-struct)
Строки `имя;chunkX;chunkZ` — формат `StructureSave` SeedcrackerX (разделитель `;`, также `,`/пробел), `#` — комментарий.
* `имя` — id structure_set (`desert_pyramids`, `nether_complexes`, `end_cities`...) **или** id структуры (`desert_pyramid`, `monument`,
  `mansion`, `fortress`, `bastion_remnant`, `ruined_portal_nether`, `end_city`...) с/без `minecraft:`, плюс синонимы SeedcrackerX/seedfinding
  (`village`, `outpost`, `ocean_ruin`...). Список — `crack-struct --list-sets`. Набор, которого нет в версии (напр. `abandoned_camp` в 26.1/26.2), даёт ошибку.
* `chunkX;chunkZ` — координаты **чанка старта** (то, что игра сравнивает в `isStructureChunk`; для random_spread — результат `getPotentialStructureChunk`).
  `--block-coords`: числа — блоковые координаты, чанк = floor(x/16) (годится для `/locate`: она возвращает `chunk*16 + locate_offset`, offset < 16).
* Проверки на входе: смещение в регионе обязано быть `< spacing-separation` (иначе «структура не может стартовать в этом чанке»); противоречие двух
  чанков одного набора в одном регионе; дубликаты; `nether_fossils` (spacing-separation=1, позиция не зависит от seed) — предупреждение «не несёт информации».
* `strongholds;cx;cz` (стартовый чанк, как отдаёт `/locate`) — **только фильтр** (см. §4.5); сам по себе поиск не запускает.
* Все измерения комбинируются в одном файле (Overworld + Nether + End): seed один и тот же, placement всех наборов зависит от одних и тех же 48 бит.

### 2.2. Биомы (crack-lift64 `--biomes`)
Строки `x z y biome` (блоки; `x z biome` — y по умолчанию `--y 64`), `#` — комментарий. Директивы: `@dim overworld|nether|end`, `@preset normal|large_biomes|amplified`,
`@quart` (координаты в квартах, 1 кварт = 4 блока: биом «сырого» шума без зума) / `@block`. Имя биома — как в игре (`plains`, `minecraft:nether_wastes`, `small_end_islands`).
Блок-уровень проходит через **BiomeManager** (зум по 8 ячейкам + obfuscated seed), поэтому зависит и от верхних 16 бит.

### 2.3. Кандидаты и hashed seed
`--struct-seed S` либо `--seeds FILE|-` (число на строку, `#` — комментарий; десятичное/0x; берутся младшие 48 бит) — годится вывод crack-struct.
`--hashed H` — `long` из пакета логина/респауна (десятичное со знаком или `0x...`).

## 3. Как игра считает позицию (проверено по исходникам и запуском)

Ссылки — `src/dec/<V>/net/minecraft/...`. Тексты `WorldgenRandom`, `RandomSpreadType`, `LegacyRandomSource`, `BitRandomSource`, `RandomSource`,
`LinearCongruentialGenerator`, а также тела всех четырёх reducer'ов **построчно идентичны в 26.1, 26.2, 26.3** (`diff`: 0 строк; 26.3 лишь вынес общую логику
в `AbstractSpreadingStructurePlacement` вместо `StructurePlacement`; `BiomeManager` — только сигнатуры/перегрузка `getBiome(x,y,z)`). Таблицы чисел для 26.1/26.2/26.3 совпадают, отличаются биом-теги и
новый набор `abandoned_camp` (26.3: spacing 37, separation 8, salt 91231127, linear — читается из JSON).

| что | формула (всё зависит только от `seed mod 2^48`) | источник |
|---|---|---|
| регион и смещение | `gx=floorDiv(cx,spacing)`; `setLargeFeatureWithSalt(seed,gx,gz,salt)`: `state0=((seed+gx*341873128712+gz*132897987541+salt)^0x5DEECE66D)&(2^48-1)`; linear: `ox=nextInt(L), oz=nextInt(L)`; **triangular**: `ox=(nextInt(L)+nextInt(L))/2`, затем так же `oz` (порядок: два броска X, два Z); `L=spacing-separation` | `RandomSpreadStructurePlacement.java:67-76`, `RandomSpreadType.java:23-28`, `WorldgenRandom.java:66-69` |
| `isStructureChunk` | `isPlacementChunk && applyAdditionalChunkRestrictions(reducer) && !exclusionZone` | 26.3 `AbstractSpreadingStructurePlacement.java:84-97`; 26.1/26.2 `StructurePlacement.java:82-96` |
| reducer `default` (при `frequency<1`) | `setLargeFeatureWithSalt(seed, **salt**, cx, cz)` — **salt стоит на месте x, cx на месте z, cz — «blend»**: `state=salt*A+cx*B+seed+cz`; `nextFloat() < f` | `:101-106` |
| `legacy_type_1` (outposts, f=0.2) | `setSeed((cx>>4) ^ ((cz>>4)<<4) ^ seed)` (int-XOR, затем `^ long seed`); `nextInt(); nextInt((int)(1.0F/f))==0` -> `nextInt(5)==0` | `:119-125` |
| `legacy_type_2` (buried_treasure, f=0.01) | `setLargeFeatureWithSalt(seed, cx, cz, 10387320)` (salt из JSON **игнорируется**); `nextFloat()<f` | `:113-117` |
| `legacy_type_3` (mineshaft, f=0.004) | `setLargeFeatureSeed(seed,cx,cz)`: `s=Random(seed); xs=nextLong(); zs=nextLong(); setSeed(cx*xs ^ cz*zs ^ seed)`; `nextDouble() < (double)f` (f — float: порог `0.004F` как double = 0.00400000018998980522) | `:107-111`, `WorldgenRandom.java:58-64` |
| exclusion_zone | `hasStructureChunkInRange(other_set, cx, cz, r)`: любой чанк квадрата (2r+1)² — `isStructureChunk` другого набора (outposts: villages, r=10, **без проверки биома деревни**) | `ChunkGeneratorStructureState.java:202-214` |
| lim=1 | `nether_fossils` (2/1): позиция `(2gx,2gz)` не зависит от seed; `buried_treasure`/`mineshaft` (1/0): любой чанк, вся информация — в reducer'е (~6.6 / ~8 бит на чанк) | doc 02 §3 |
| concentric_rings | `ChunkGeneratorStructureState.generateRingPositions`: `Random(seed)`: `angle=nextDouble()*PI*2`; на кольцо `dist=4*32+32*circle*6+(nextDouble()-0.5)*80`, `ix=round(cos*dist)`, `fork()` потребляет `nextLong`; итоговый чанк — результат поиска биома в окне ±112 блоков => **±7 чанков** от `(ix,iz)` | `:114-174`, doc 02 §4 |

Проверка запуском: `crack-struct --selftest tests/vectors/<V>/structs.tsv` сравнивает `isStructureChunk` моей host-транскрипции (все наборы, **с reducer'ами и exclusion zone**) с
реальным кодом игры на окнах 64×64: 26.1 — 120 (dim,seed,set), 7563 чанков, 0 расхождений; 26.2 — 120 / 7563 / 0; 26.3 — 126 / 7604 / 0 (Overworld, Nether, End; seed 0, ±1, ±2^63, 12345, ...).
`tests/unit_pred.cpp` (`make unit`): `fastmod`/тест делимости для **всех** lim 2..4096, `draw_lim` против `nextInt` (включая границу отбраковки), пороги reducer'ов,
`pred_eval` против транскрипции игры на 3×~8000 предикатах — 0 ошибок.

## 4. Алгоритмы crack-struct

Наблюдение даёт 1–2 **предиката** над 48-битным `W`: `spread` (linear/triangular), `reducer` (4 метода), плюс пост-фильтры (exclusion, strongholds). Информация оценивается
как `-log2 P(случайный W проходит)`; печатается `ожидаемо ложных ~ 2^(48-info)`. При ожидаемо > 4·10^6 кандидатов инструмент отказывается (`--force` снимает).

### 4.1. Lifting (по умолчанию, если есть liftable-наблюдения)
Для linear с `lim = 2^t·q` (q нечётно, не степень двойки: desert/igloo/jungle/swamp 24 (t=3), shipwreck 20 и ocean_ruins 12 (t=2), village 26, trial 22, trail 26 (t=1)):
`nextInt(lim)` без отбраковки равно `r % lim`, `r = state >> 17` (31 бит), поэтому `result mod 2^t = r mod 2^t` — это биты 17..17+t-1 состояния после шага, а они зависят
только от **младших 17+t бит** `W` (сложение и XOR сохраняют «младшие биты зависят от младших»). Для каждой координаты — t бит информации из L=17+t ≤ 20 бит `W`.
Реализация: побитовое наращивание кандидатов младших L бит с отсечением (≤2^20 узлов, миллисекунды), затем перебор 2^(48-L) старших бит для каждого низа на GPU (ядро `k_lift`, полная проверка всех предикатов).
Ограничение честно: отбраковка `nextInt` (вероятность ≈ lim/2^31 на бросок) в lifting-фильтре младших бит не моделируется — шанс потерять верный seed ≈ 2·10^-8 на структуру (в полном переборе обрабатывается точно, см. DK_EDGE).
**Эффективная информация меньше номинальной**: после lifting «в младших битах» остаётся только `Π P_eff`; поэтому число ложных кандидатов печатается отдельной строкой `[lift] ожидаемо ложных`, учитывающей число найденных низов.

### 4.2. Полный перебор 2^48 (нет liftable-структур: Nether, End, ancient_city, ruined_portal, monument/mansion, reducers) — «перечисление решений первого ограничения»
Прототип перебирал все `W` (1.8·10^11 W/с, 25 мин на 2^48). Теперь:
`W <-> s0 = ((W+cst)&M)^0x5DEECE66D <-> s1 = A·s0+C (mod 2^48)` — биекции. Первая координата linear-наблюдения — условие на `r = s1>>17`: `r mod lim == ox` (или старшие биты при lim=2^k).
Перечисляем **только** такие s1: `r = ox + k·lim`, внутри — 2^17 значений `m` (младшие биты s1), `s2 = A·s1+C` обновляется сложением `s2 += A`, вторая координата проверяется одним умножением
(тест делимости, Hacker's Delight 10-17: `rotr((r2-oz)·q^-1, t) <= floor((2^32-1)/lim)`), и только прошедшие (~1/lim) идут в «тяжёлую» проверку остальных предикатов
(`s1 -> W = ((((s1-C)·A^-1)^MUL) - cst) mod 2^48`). Пространство сжимается в lim раз (для lim=23 — в 23 раза), цена одного кандидата ≈ 10 инструкций.
На GPU прошедшие кандидаты складываются в очередь в shared-памяти варпа и обрабатываются пачками по 32 (без дивергенции «тяжёлого» пути; измерено ×5,6: 1.1·10^12 -> 6.2·10^12 «эквивалентных» значений/с на DIV-драйвере).
Драйверы (выбирается минимум оценки «число кандидатов × стоимость»):

| драйвер | когда | что перечисляется |
|---|---|---|
| `enumerate-residues` (DK_DIV) | linear, lim не степень двойки | `r = ox + k·lim`, 2^48/lim значений |
| `enumerate-range` (DK_POW2) | linear, lim = 2^k (ancient_city 16) | непрерывный блок s1 со старшими k битами = ox |
| `enumerate-range (reducer)` (DK_RANGE) | `default`/`legacy_type_2` reducer (buried_treasure, f=0.01) | `s1 < T24<<24` (T24 = ceil(f·2^24)), 2^48·f значений |
| `triangular` (DK_TRI) | triangular, lim не степень двойки | `r1 = a + k·lim` по допустимым `a` (≤2 допустимых `b` на `a`), 2^48·|A|/lim |
| `edge` (DK_EDGE) | всегда вместе с DIV/TRI | `r ∈ [2^31-lim, 2^31)` (возможна отбраковка `nextInt`) — точный перебор, поэтому полный режим **не теряет** seed'ы из-за отбраковки |
| `generic` (DK_GEN) | только `legacy_type_1/3` (mineshaft, outposts) без linear/triangular | прямой перебор W |

CPU-версия (OpenMP) — те же функции. `--part i/n` режет пространство индексов драйвера на n частей (длинный запуск можно разбить/возобновить).

### 4.3. Пост-фильтры (и на устройстве, и на хосте)
* **exclusion_zone** (pillager_outposts: villages, r=10): `PK_EXCL` — проверка потенциальных чанков другого набора в квадрате (≤4 региона), на устройстве для простого случая (другой набор без reducer'а/exclusion), на хосте — точная рекурсия по `isStructureChunk`.
* **Результаты перепроверяются на хосте** по всем предикатам (и по тем, что не влезли в `MAXP=28` на устройстве). Буфер устройства 4M кандидатов; при переполнении — предупреждение (список неполон).

### 4.4. Что не покрывается как драйвер
`dimension_origin` (26.3; в ванили не используется, не зависит от seed) — ошибка «не поддержан». `concentric_rings` — только фильтр (§4.5). Слайм-чанки, шахты по чанкам-слайм, End-острова, бедрок Nether, пиллары — отдельные инструменты других агентов (`crack-slime`, `crack-mineshaft`, `crack-pillars`, `crack-nether-bedrock`); их можно комбинировать, пересекая списки кандидатов.

### 4.5. Strongholds (concentric_rings) — только фильтр
Позиции колец (128 штук) зависят от биомов (64 бита), но геометрия `(ix,iz)` — только от 48 бит. Наблюдение `strongholds;cx;cz` = «в пределах ±7 чанков от одной из позиций колец» ≈ 8 бит на позицию (кольцо 0: угол ~5.7 бит + расстояние ~2.4 бита).
Как драйвер бесполезно (3 позиции кольца 0 ≈ 13 бит), как фильтр — сильно: см. §6.3 (4 храма + 3 страхолда: медиана кандидатов 8·10^4 -> 12 на 26.3, 8·10^5 -> 99 на 26.1).
Устройство проверяет с допуском 8 (надмножество, `double cos/sin`), хост — точно 7.

## 5. crack-lift64: 2^16 верхних бит

Для каждого кандидата `W = (hi<<48) | S48`, hi ∈ [0, 65536). Проверки по возрастанию стоимости:
1. **hashed seed.** `BiomeManager.obfuscateSeed(seed) = Hashing.sha256().hashLong(seed).asLong()` (`BiomeManager.java:23`): Guava `hashLong` кладёт 8 байт **little-endian**, `asLong()` читает первые 8 байт дайджеста **little-endian**.
   Проверено против `oracle hashed` (seed 0 -> 8794265229978523055, 12345 -> 293737985876514017) и в e2e на случайных seed. Реализация: SHA-256 одного блока, при наличии — **SHA-NI** (74 нс/хэш против 790 нс скалярного, совпадение на 3·10^6 значений); параллельно по кандидатам: **1000 кандидатов за 0.9 с** (65 млн хэшей).
2. **Биомы Nether/End** — зависят только от 48 бит (Nether: `LegacyRandomSource(seed)`, `seed+1`; End: `LegacyRandomSource(seed)` + `consumeCount(17292)`), поэтому на **кварт-уровне** (`@quart`) это фильтр structure seed, а верхние 16 бит не определяются
   (вердикт `# UNDETERMINED struct_seed=... hi16=any`). Блок-уровень (зум) слабо зависит от hi (только у границ биомов).
3. **Биомы Overworld** — Xoroshiro-климат от всех 64 бит: на каждый из 65536 `W` инициализация 6 шумов (≈25 мкс) + точка (≈5 мкс) с ранним выходом; 12 потоков: **≈0.7–1 с на кандидата при 14 точках**.
   Блок-уровень: реализован `BiomeManager.getBiome` (`BiomeManager.java:35-87`: сдвиг на 2, `fiddle` от hashed seed, 8 ячеек, `Mth.square`-сумма в порядке z,y,x) — в e2e совпадает с реальной игрой (`oracle blockbiome`).
   **Тай-брейки R-дерева:** `Climate.RTree.search` зависит от `lastResult` потока (недетерминированно при равном fitness), поэтому принимается **любой из биомов с минимальным расстоянием**: при несовпадении проверяется, есть ли у наблюдаемого биома лист на том же расстоянии (`unit_lift64`: из 40000 случайных точек климата у 507 были равные листья разных биомов; все принимались, прочие — нет).
4. **«Случайный» seed игры** (`--random`, включён по умолчанию, если нет hashed/биомов). `WorldOptions.randomSeed() = RandomSource.create().nextLong()` (`WorldOptions.java:88`), а `RandomSource.create(long) = new LegacyRandomSource(seed)` (`RandomSource.java:23`) — **во всех трёх версиях**:
   `W = (a<<32) + b`, `a=(int)(s1>>16)`, `b=(int)(s2>>16)`, `s2=A·s1+C` — весь W определяется 48-битным `s1`. По нижним 48 битам W верхние 16 восстанавливаются (аналог `NextLongReverser` SeedcrackerX): 2^16 перебор младших 16 бит `s1` + линейное уравнение для верхних 16 бит `a`.
   Проверено: 300/300 случайных seed восстановлены, кандидатов: 1 (77%) или 2 (23%). Верно **только** для seed, созданных игрой при пустом поле (по умолчанию в «Новый мир»); для введённых вручную — `--hashed`/`--biomes`.
   (Согласуется с `docs/13` §2 п.3 и `docs/10` §3.4: для 26.x это подтверждено исходниками — `RandomSource.create(long)` возвращает `LegacyRandomSource`, а не Xoroshiro.)
5. **Малые/текстовые seed.** `WorldOptions.parseSeed`: число -> `Long.parseLong`, иначе `String.hashCode()` (int, расширяется со знаком) — поэтому `S48 < 2^31` или `S48 >= 2^48-2^31` печатается как `# GUESS small/text seed`.

Вывод: на stdout строка `мировой_seed(со знаком) <TAB> 0x... <TAB> # struct=... hi16=... [hashed] [biomes] [random-seed-assumption]`; `# ...` — комментарии (вердикты, подсказки). Код возврата: 0 — найден world seed или подтверждён structure seed; 1 — ничего; 2 — ошибка входа.

## 6. Результаты

### 6.1. Сквозной тест `crack/tests/e2e.py` (oracle = реальный код игры)
Для каждой версии и случайных 64-битных seed: oracle `structs` -> потенциальные чанки, oracle `structstart` — реально сгенерированные структуры (где oracle способен; ruined_portal и reducer-наборы — потенциальные чанки, это отражено в колонке real);
файл наблюдений (имена чередуются: id набора / id структуры) -> `crack-struct` -> «истинный structure seed среди кандидатов» -> `crack-lift64` (a) `--hashed` из `oracle hashed`, (b) 16 биомов из `oracle blockbiome` (Overworld, блок-уровень) / 24 кварт-биома `oracle biome` (Nether/End) -> восстановлен ли исходный world seed;
для половины seed (сгенерированных как в игре) — `crack-lift64` без hashed/биомов.
Сценарии: `ow-lift` (8 liftable-структур), `ow-mixed` (9: + monument, mansion, ancient_city, ruined_portal, outposts с exclusion zone, buried_treasure), `ow-sh` (4 liftable + 3 страхолда кольца 0 из `oracle stronghold`),
`ow-full` (12 без liftable: ancient_city, ruined_portal, monument, mansion, buried_treasure, mineshaft [+abandoned_camp]) — полный перебор в окне 2^34 значений вокруг истинного seed, `nether` (10: nether_complexes + ruined_portal), `end` (10 end_city) — окно 2^34;
плюс по одному **реальному полному прогону 2^48** (Nether, 10 структур) на версию.

@@E2E@@

### 6.2. Сколько структур нужно, сколько кандидатов остаётся
@@STATS@@

### 6.3. Страхолды как фильтр (e2e `ow-sh`, 4 liftable-структуры = 36 бит + 3 страхолда кольца 0)
@@SH@@

### 6.4. Скорость
@@BENCH@@

## 7. Ограничения и честные оценки

* **Мало наблюдений — много кандидатов.** Нужно ≥ ~55 бит информации (печатается). Ориентиры: 4 liftable-структуры (36 бит) — тысячи кандидатов; 6 смешанных — в среднем ~1; 8 — 1. Ложные кандидаты — «сиблинги» с теми же 20 младшими битами. В Nether нужно ≥ 6 структур (≈9.2 бит каждая), в End — ≥ 9–10 end_city (≈5–6 бит каждая).
* **Lifting** может потерять верный seed с вероятностью ≈ 2·10^-8 на структуру (отбраковка `nextInt` в фильтре младших бит); полный перебор (`--mode full`) точен.
* **Реальность наблюдений.** Нужны чанки именно **старта** структуры. Для структур, которые игра могла не сгенерировать (биом/рельеф), «потенциальный чанк» без самой структуры — ложное наблюдение: добавляйте только реально виденные.
* **Нет эквивалентов SeedcrackerX-фаз по декораторам** (dungeon/emerald/gateway для 1.18+ идут через Xoroshiro, индексы нужно сверять по oracle) и End-пиллара (16 бит): это `crack-pillars` другого агента — комбинируйте списки кандидатов.
* **Strongholds** — фильтр, не драйвер (§4.5); если игра не нашла предпочтительный биом, позиция = `(ix,iz)` точно, это входит в допуск ±7.
* **Биомы Overworld** требуют ≥ 8–14 разнообразных точек: вероятность случайного совпадения на точку = доля биома (5–30%). `crack-lift64` тратит ≈ 0.7–1 с на кандидата (12 потоков), для 10^4+ кандидатов сначала сузьте `--hashed`/`--random`.
* **Версии.** `crack-struct` принимает любой `--version` с файлом `data/structure_sets-<V>.json` (или `--sets FILE`); новые версии добавляются таблицей (`tools/extract_structure_sets.py`). `crack-lift64` ограничен версиями движка `engine/` (26.1–26.3).
* **GPU общий.** Скорости ядер ниже получены на загруженной другими агентами видеокарте (лучший из N прогонов); при простое — выше. Без GPU (`--dev cpu`/`make cpu`) полный 2^48: **≈1.5–2 ч** (linear/pow2/reducer), **≈10 ч** (triangular), **≈дни** (generic: только mineshaft/outposts без linear) на 12 потоках; lifting на CPU — доли секунды.
* `crack-struct --max-out` (100000) усекает вывод по возрастанию; буфер устройства 4M — при переполнении список неполон (предупреждение). 
* Не реализовано: GPU-версия SHA/биомов в `crack-lift64` (CPU достаточно); перечисление «по обоим координатам» (решётка/сортировка вычетов дало бы ещё ×lim, но 2^48 уже за десятки секунд).

## 8. Воспроизведение
```bash
cd crack
make                                  # сборка
make unit                             # юнит-тесты (host, ~1 мин)
make test                             # unit + selftest placement (3 версии) + ядра lift/full × GPU/CPU без oracle
python3 tests/e2e.py --seeds 6 --full-real 1 --json /tmp/e2e.json   # сквозной, ~40–60 мин (oracle, JVM 15–35 с на версию)
python3 tests/stats.py --n 30 --json /tmp/stats.json                # статистика кандидатов
flock /tmp/gpu.lock tests/bench_struct.sh 44 3                      # скорость ядер (GPU общий -> замок)
```

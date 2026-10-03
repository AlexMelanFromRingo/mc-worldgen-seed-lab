# W3 «Пещеры и каньоны»: стадия CARVERS libmcgen

Отчёт потока W3 аддона «MC Worldgen» (спецификация — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md`, ворота G4).
Все числа — измеренные; команды воспроизведения — в §6. Код — `libmcgen/src/carver.c`, `carver.h`; тесты — `libmcgen/tests/g4_*`.

## 1. Итог

| что | результат |
|---|---|
| **G4, 26.3, настоящий сервер** (`run/gt/26.3/{carve_raw,carvers}`, строго: все чанки дампа, в том числе ниже full, без масок жидкостей, `--pp-margin margin−1`) | **61 эталон, 50 436 чанков, 4 127 080 320 блоков, 1 расхождение (99.99999998 %)**; 3 seed (12345, 8675309, −7048155917072976836) × 3 измерения |
| вариант `carve_raw` (пустая поверхность + карверы; маска расширений ext-биомов) | 30 эталонов, 24 981 чанков, 2 038 120 320 блоков, **0** расхождений |
| вариант `carvers` (поверхность W2 + карверы) | 31 эталон, 25 455 чанков, 2 088 960 000 блоков, 1 расхождение (причина — порядок растекания жидкости, §5) |
| официальные ворота `tools/gt/run_gate.py` (`docs/blender/accuracy.md`) | **G4: 30/30 PASS** (25 230 чанков), **G4c (carve_raw): 30/30 PASS** (24 981 чанков), биомы 100 %, карты высот 0 расхождений |
| **26.1, 26.2, 26.4-snapshot-2, любые пресеты — против настоящих классов игры** (`libmcgen/tests/g4_java.py`, без сервера) | 26.1: **0 расхождений** на 1 928 чанках (165.9 · 10⁶ блоков; carve_raw и surface+carve; Overworld/Nether/End, 3 seed); 26.2: **0** на 1 800 чанках (153.4 · 10⁶); 26.3 (перекрёстно с сервером): **0** на 776 чанках; 26.4: **0** на 1 172 чанках carve_raw (кроме столбцов расширений поверхности, §3.5) и 131 блок `dirt ↔ grass` на 324 чанках surface+carve (§3.5); пресеты `large_biomes`, `amplified`, `caves`, `floating_islands` (26.1, 26.3, 26.4, 2 seed): **0** |
| скорость (26.3, Overworld, 1 поток, CPU-время, машина загружена) | CARVERS **≈ 0.55 мс/чанк** (≈ 1 800 чанков/с на поток) при 12.4 мс/чанк у TERRAIN (+4 %); Nether ≈ 0.4 мс/чанк (§7) |

Домен seed: **карверы используют `McSeeds.terrain`** (`RandomState.seed()` = seed мира; `setLargeFeatureSeed(seed + индекс_карвера, cx, cz)`). Тест `g4_seeds`: маска зависит от `terrain`
и не зависит от `climate`/`structures`/`features`.

## 2. Где код

| файл | что |
|---|---|
| `libmcgen/src/carver.c` | вся стадия: ГСЧ `java.util.Random` (inline), `Mth.sin/cos` (две таблицы: ≤26.3 и 26.4), поставщики значений (`IntProvider`, `FloatProvider`, `HeightProvider`, `VerticalAnchor`), разбор карверов (26.3+ `worldgen/carver/<имя>.json`; 26.1/26.2 `worldgen/configured_carver/<имя>.json`) и списков по биомам, пещеры/каньоны/комнаты, маска, применение маски (26.3+), «жадное» применение (26.1/26.2) |
| `libmcgen/src/carver.h` | внутренний интерфейс, контракт со SURFACE (`mcgen_surface_top_material`), экспорт маски (`carvers_chunk_mask`, `carvers_mask_alloc`) |
| `libmcgen/src/terrain.c`, `terrain_old.c` | **точечное расширение W1**: `TerrainCtx` сохраняет aquifer последнего заполненного чанка (`terrain_carve_substance`, `terrain_carve_sched`, `terrain_marks_rw`); `terrain_old.c` — `aq_substance_xyz` (позиция явно; барьерный шум по точке `SinglePointContext`) |
| `libmcgen/src/region.c` | хук после TERRAIN/SURFACE: `carvers_apply_chunk` на том же `TerrainCtx` (aquifer и кэши сэмплеров чанка — как в игре); то же для «гало»-чанков растекания |
| `libmcgen/src/mcgen_internal.h`, `world.c` | поле `McWorld.carvers` (строится лениво при первом чанке, потокобезопасно), освобождение |
| `libmcgen/tests/g4_carvers.py` | ворота G4 по эталонам сервера: CLI + `tools/gt/diff.py`, строго, JSON-отчёт |
| `libmcgen/tests/g4_java.py`, `g4_java/{old,new,v264}/CarveRef.java`, `g4_java/{build,run}.sh` | G4 без сервера: Java-эталон на настоящих классах игры (doFill + [buildSurface] + applyCarvers/generateCarvers/generateCarvingMask+ChunkTerrainBuilder одного чанка), сравнение с libmcgen блок-в-блок |
| `libmcgen/tests/g4_eager.c`, `g4_seeds.c` | самопроверки: два пути применения маски дают одинаковые блоки; домен seed |
| `tools/gt/run_gate.py` (правка W6, 2 строки) | `G4c`: стадии `0xb` (без SURFACE — иначе поверхность накладывается на эталон без неё); запуск libmcgen с `--pp-margin margin−1` |

Публичный ABI (`include/mcgen.h`) **не менялся**.

## 3. Как устроено

### 3.1. Цепочка и зависимости
В игре (26.3) `NoiseBasedChunkGenerator.buildTerrain` = `doFill` → `buildSurface` → `generateCarvers` в одном статусе. Карверы работают над готовым
(заполненным + покрытым поверхностью) чанком, используют **тот же `NoiseChunk`** (его aquifer и кэши) и пишут жидкости/воздух по правилам aquifer.
Поэтому `carvers_apply_chunk` вызывается на том же `TerrainCtx`, на котором только что заполнен чанк (aquifer `Aq` сохраняется до следующего заполнения),
после SURFACE и до копирования пометок пост-обработки жидкостей. Растекание жидкостей (`region_postprocess`) идёт после всех стадий.

**Зависимости чанка:** только его собственные блоки + биомы исходных чанков радиуса 8 (список карверов биома — по «сырому» шумовому биому клетки
(x_min/4, qy = 0, z_min/4), `world_biome_noise`). Блоки соседних чанков **не нужны**, «гало» блоков для стадии нет. Если у всех возможных биомов измерения
одинаковый список карверов (в ванили всегда: Overworld — `[cave, cave_extra_underground, canyon]`, Nether — `nether_cave`, End — пусто), поиск биомов
исходных чанков пропускается (экономия 289 расчётов климата на чанк); при разных списках (датапак) — честный поиск.

### 3.2. Маска (26.3+)
1. `CarvingMask`: биты по (x, z, y), y ∈ [minGenY+1, minGenY+genDepth−1−7] (`protectedBlocksOnTop = 7`). `minGenY/genDepth` — как `WorldGenerationContext`:
   `max(minY измерения, minY noise_settings)`, `min(высота измерения, высота noise_settings)` (Nether: 0/128, хотя высота типа измерения 256).
2. Обход исходных чанков `dx, dz ∈ [−8, 8]`, `dx` — внешний цикл. Для i-го карвера списка биома: `setLargeFeatureSeed(seed + i, sx, sz)`
   (`setSeed(seed); xs = nextLong(); zs = nextLong(); setSeed(sx·xs ^ sz·zs ^ seed)`), старт — `nextFloat() <= probability`.
3. `cave`: число пещер `count` (в ванили `very_biased_to_bottom` 0..14, Nether 0..9), на каждую: x, y (`HeightProvider`), z, множители радиусов, `floor_level`;
   с вероятностью 1/4 — «комната» (эллипсоид, `room_vertical_radius_multiplier`) и до 3 дополнительных тоннелей; тоннель — `createTunnel` на отдельном ГСЧ
   (`SingleThreadedRandomSource(random.nextLong())`), ветвление в `splitPoint`, `canReach`, `carveEllipsoid`.
   `canyon`: один путь на исходный чанк, `initWidthFactors` (ширина по высоте — до 384 значений ГСЧ), `shape` (`thickness`, `distance_factor`,
   `width_smoothness`, `vertical_radius_*`).
4. **Float-арифметика Java** повторена выражение в выражение: `float` там, где в игре `float`, `double` — где `double`; порядок вызовов ГСЧ внутри одного
   выражения задан явными временными (в C порядок вычисления аргументов не определён); `-ffp-contract=off`.
   `Mth.sin/cos` — таблица `SIN[65536]` из `(float)Math.sin(i / 10430.378350470453)` (свёртка хэша всех 65 536 значений совпала с Java `Math.sin` и `StrictMath.sin`).
   **В 26.4 `Mth.sin/cos` изменены**: индекс с округлением (`+ 0.5`), нечётная симметрия для i < 0, узлы `sin[0] = 0`, `sin[16384] = 1`, `sin[32768] = 0`, `sin[49152] = −1`
   (без этого 26.4 давал изолированные расхождения ~0.002 % блоков; найдено Java-эталоном) — вторая таблица и флаг версии.

### 3.3. Применение маски (26.3+, `applyCarvingMask`)
Столбцы по x (внешний), z; в столбце — непрерывные отрезки маски **снизу вверх**, внутри отрезка **сверху вниз**; флаг `hasGrass` сбрасывается на каждом отрезке:
* блок из тега `minecraft:uncarvable` (бедрок) — пропуск (проверяется текущий блок);
* блок — трава/мицелий → `hasGrass = true`;
* `Aquifer.computeSubstance(x, y, z, 0.0)` (aquifer W1: `terrain_carve_substance`): `null` (барьер) — блок остаётся; иначе записывается воздух/вода/лава;
  пометка пост-обработки, если `shouldScheduleFluidUpdate` и состояние — жидкость;
* если `hasGrass` и под вырезанным блоком `dirt` — он заменяется материалом поверхности (`topMaterial`, **стадия W2** — слабый символ
  `mcgen_surface_top_material`: правило поверхности в одной точке с `stoneDepth = 1`, `waterHeight = y+1 | −∞`, градиенты по карте WORLD_SURFACE_WG).
  Путь реально выполняется: 567 вызовов (все с заменой блока) на 256 чанках Overworld около (−3, −3) seed 12345.

Aquifer-жидкости в вырезанном объёме: вода выше уровня водоносного слоя и барьерные «стенки» определяет aquifer; ниже −54 — лава (глобальный выбор жидкости);
в Nether водоносного слоя нет (`createDisabled`): воздух при y ≥ 32 и лава ниже (глобальный `fluidRule`: уровень моря 32, `default_fluid` лава).

### 3.4. 26.1 / 26.2 («жадный» путь)
Карвер сразу меняет чанк (`WorldCarver.carveBlock`), маска служит лишь отметкой «блок уже вырезан» (`mask.get/set` в `carveEllipsoid`); параметры в `configured_carver`
(`{"type", "config": {…}}`, поле `yScale`, `lava_level`, `replaceable`). Классы Java `getCaveBound/getThickness/getYScale` переведены в поля определения:
* `cave`: `count = very_biased_to_bottom(0, 14)`, толщина `trapezoid(0, 3, 1)` (≡ `nextFloat()·2 + nextFloat()`), «странная» толщина всегда;
  `nether_cave`: `count = (0, 9)`, толщина `trapezoid(0, 6, 2)` (≡ `(nextFloat()·2 + nextFloat())·2`), без странной толщины, `yScale` тоннелей 5.0;
* `carveBlock`: `hasGrass` по трава/мицелий; блок обязан входить в `replaceable` (теги блоков датапака; `nether_cave` — `nether_carver_replaceables`);
  `y <= lava_level` → лава, иначе aquifer (`null` — без изменения); пометка по **значению `shouldScheduleFluidUpdate` последнего вызова aquifer** (в ветке лавы оно «старое» — повторено);
  `Nether`: `y <= minGenY + 31` → лава, иначе `cave_air`, без aquifer;
* aquifer — W1 (`terrain_old.c`): позиция передаётся явно, барьерный шум — по точке (`SinglePointContext`), как в игре.
Проверка: Java-эталон на настоящих классах 26.1/26.2 — 0 расхождений (§1, §6). Отметка о тестовой оснастке: у эталона Oracle теги статических реестров не применяются
(`BlockState.is(тег)` всегда `false`), поэтому `CarveRef` применяет `PendingTags.apply()`; чтение блоков — через секцию (`ProtoChunk.getBlockState` у секции из одного воздуха возвращает `air`
вместо `cave_air`).

### 3.5. 26.4-snapshot-2
Классы и данные карверов совпадают с 26.3 (отличаются `VerticalAnchor.Context` вместо `WorldGenerationContext` и `Mth.sin/cos`, §3.2); маска строится тем же кодом.
Но карвинг встроен в `ChunkTerrainBuilder.fillColumn` (проход поверхности по столбцу): вырезаются только «твёрдые» блоки шума (воздух и жидкости шума не трогаются),
запрет — по результату правила материала (а не по текущему блоку), вместо `hasGrass` — `carvedTopBlock` (повторный расчёт правила для `dirt` с `stoneDepthAbove = 1`).
Реализовано: применение маски только к блокам, которые не воздух и не жидкость (`apply_mask`, ветка `v264`) — **для стадий без SURFACE (`carve_raw`) совпадает с игрой: 0 расхождений на 1 172 чанках** (все
остаточные различия `carve_raw` — столбцы расширений поверхности `frozen_ocean`/`eroded_badlands`, которые игра выполняет при любом правиле материала; при SURFACE они совпадают).
С полной поверхностью остаётся **131 блок `dirt → grass_block/gravel` на 324 чанках** (0.0005 %): это `carvedTopBlock` (материал блока под вырезанным верхним слоем).
Правка для **W2** (`surface_apply_chunk`, проход по столбцу, ~15 строк): получить маску `carvers_mask_alloc(w, cx, cz, …)` и `TerrainCtx` (aquifer `terrain_carve_substance`) и в ветках
`SOLID` делать то, что описано в комментарии к `carvers_mask_alloc` (`carved` → aquifer: AIR — удалить блок, жидкость — записать, SOLID — обычный путь; `carvedTopBlock |= stone_above == 1`; в ветках воздуха/жидкости
`carvedTopBlock &= carved`; при `carvedTopBlock` и `dirt` — перерасчёт правила с `update_y(1, stone_below, water_h, y)`). После этого пост-обработку `carvers_apply_chunk` для 26.4 с SURFACE нужно отключить.

## 4. Тонкие настройки (стадия carvers)
| id | что делает | по умолчанию |
|---|---|---|
| `canyon_frequency` | вероятность каньонов × k (≤ 1; 0 — никогда) | 1.0 — побитово ваниль |
| `cave_density` | вероятность карверов-пещер × D (≤ 1; 0 — нет классических пещер) | 1.0 |
| `cave_size` | множитель радиусов (`horizontal/vertical_radius_multiplier`) и толщины пещер-карверов (тоннели и комнаты) | 1.0 |
При значениях по умолчанию результат побитово ванильный (G4 идёт без настроек). Эталона для не-ванильных значений нет (у игры такой настройки нет) — только по определению.

## 5. Единственное расхождение G4 и его причина
Overworld, seed −7048155917072976836, область (0, 0): блок (94, 16, −80) — у нас `water[level=0]`, в эталоне `water[level=1]`.
* Вырезание ни при чём: до растекания (`--tweak fluid_flow=0`) в клетке воздух, как и в эталоне; вырезанные блоки совпадают.
* Клетка стоит на границе чанков (5, −5) / (5, −6), рядом два источника: (93, 16, −80) и (94, 16, −81)/(94, 16, −82). Если сначала растекается чанк (5, −6), клетка получает двух соседей-источников и по
  правилу «бесконечной воды» становится источником (наш результат); в эталоне чанк (5, −5) растёкся раньше, клетка получила уровень 1.
* Порядок `region_postprocess` (cz, затем cx) — допущение W1. Проверена альтернатива «по расстоянию от центра области» (как волна загрузки): на 16 эталонах `carvers` Overworld она исправляет
  эту клетку, но даёт 3 новых расхождения (две области) — настоящий порядок ни тем, ни другим не воспроизводится (зависит от планировщика загрузки чанков сервера).
  Остаточный шум — 1 блок на 4.1 · 10⁹ (строго); в официальных воротах `run_gate.py` такие клетки идут под маску «текущих жидкостей» (G4: 30/30 PASS).

## 6. Тесты и воспроизведение
```bash
make -C libmcgen
# G4 по всей матрице W6 (carve_raw: стадии 0xb; carvers: 0xf), строго
python3 libmcgen/tests/g4_carvers.py --version 26.3 --variants carve_raw carvers --report libmcgen/tests/results/g4-26.3.json
python3 tools/gt/run_gate.py --gate G4c --profile all      # официальная таблица (docs/blender/accuracy.md): карверы без поверхности
python3 tools/gt/run_gate.py --gate G4  --profile all      # с поверхностью
# G4 без сервера (любая версия/пресет; Java-эталон на классах игры; растекание выключено)
python3 libmcgen/tests/g4_java.py --version 26.1 --dim overworld --seeds 12345,8675309 --cx0 20 --cz0 -40 --nx 12 --nz 12            # carve_raw
python3 libmcgen/tests/g4_java.py --version 26.4-snapshot-2 --dim nether --preset normal --seeds 12345 --nx 6 --nz 6 --surface   # + поверхность
# согласованность двух путей применения маски; домен seed
make -C libmcgen build/tests/g4_eager build/tests/g4_seeds
libmcgen/build/tests/g4_eager run/pack-26.3 26.3 minecraft:overworld 12345 -4 -4 10
libmcgen/build/tests/g4_seeds run/pack-26.3 26.3 minecraft:overworld 12345 777 -6 -6 12
```
Результаты — `libmcgen/tests/results/g4-26.3.{json,log}`, `g4-java*.log`. Стадии для `carve_raw` — **0xb** (BIOMES|TERRAIN|CARVERS, без SURFACE).

## 7. Скорость и ограничения
* Измерения при загруженной машине (load average 15–25 на 12 ядрах, одновременно работали сервер W6 и Java-эталоны), CPU-время процесса, минимум из 5 прогонов, 256 чанков Overworld (0, 0), 1 поток, `fluid_flow=0`:
  TERRAIN 3.17 с (12.4 мс/чанк); TERRAIN+CARVERS 3.31 с → CARVERS ≈ 0.55 мс/чанк (+4 %). Nether: 0.59 → 0.69 с (≈ 0.4 мс/чанк). Построение маски — 0.2 мс/чанк (50 мс на 256 чанков), остальное — aquifer
  (`computeSubstance` в ≈ 1 490 блоках маски на чанк) и запись. callgrind (5×5 чанков): +7.0 · 10⁶ инструкций на чанк при 124 · 10⁶ у TERRAIN. Результат не зависит от числа потоков (дамп 32×32 чанков, стадии 0xf: одинаковый md5 при 1, 5, 12 потоках).
* **26.4 + полная поверхность**: 131 блок на 324 чанка (§3.5) — нужна правка W2 в `surface_apply_chunk` по контракту §3.5.
* Эталоны карверов ванильного сервера для 26.1, 26.2, 26.4-snapshot-2 у W6 отсутствуют; проверка этих версий — Java-эталоном на настоящих классах игры (§1). Для серверной сверки:
  `python3 tools/gt/gen_queue.py --version <V> --variants carve_raw,carvers --profile core --seeds 12345` (V = 26.1, 26.2, 26.4-snapshot-2).
* Порядок растекания жидкостей на границах чанков (§5) — вне стадии; тонкая настройка для W1/W6.

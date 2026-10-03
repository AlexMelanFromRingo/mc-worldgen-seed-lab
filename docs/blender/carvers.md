# W3 «Пещеры и каньоны»: стадия CARVERS libmcgen

Отчёт потока W3 аддона «MC Worldgen» (спецификация — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md`, ворота G4).
Все числа — измеренные; команды воспроизведения — в §6. Код — `libmcgen/src/carver.c`, `carver.h`; тесты — `libmcgen/tests/g4_carvers.py`, `g4_eager.c`.

## 1. Итог

| что | результат |
|---|---|
| **G4 (26.3)** — блок-в-блок против настоящего сервера (`run/gt/26.3/{carvers,carve_raw}`) | @G4_TOTAL@ |
| вариант `carve_raw` (пустая поверхность + карверы) | @G4_RAW@ |
| вариант `carvers` (поверхность W2 + карверы) | @G4_CARVERS@ |
| сравнение | строгое: все чанки дампа (в том числе ниже full), без масок жидкостей, растекание — `--pp-margin margin−1`; для `carve_raw` маска столбцов расширений бесплодных земель/айсбергов (`--mask-ext`, как в G2) |
| 26.1 / 26.2 | реализовано («жадный» путь `ConfiguredWorldCarver`, §3.4), **эталонов карверов для 26.1/26.2 у W6 пока нет** — не сверено с сервером (§7) |
| 26.4-snapshot-2 | маска карверов — как в 26.3 (код тот же, данные те же); применение встроено в проход поверхности (§3.5): для `carve_raw` реализовано, **эталонов нет**; для полной поверхности нужна стадия W2 `surface_264` |
| скорость, 26.3, Overworld, 1 поток | @SPEED@ |

Домены seed: **карверы используют `McSeeds.terrain`** (`RandomState.seed()` = seed мира, `setLargeFeatureSeed(seed + индекс_карвера, cx, cz)`).
Все четыре поля `McSeeds` в ванили равны; при разных значениях карвер следует `terrain` (пещеры, aquifer, жилы, шумы поверхности — один домен).

## 2. Где код

| файл | что |
|---|---|
| `libmcgen/src/carver.c` | вся стадия: ГСЧ `java.util.Random` (inline), `Mth.sin/cos` (таблица 65536 float), поставщики значений (`IntProvider`, `FloatProvider`, `HeightProvider`, `VerticalAnchor`), разбор карверов (26.3+ `worldgen/carver/*.json`; 26.1/26.2 `worldgen/configured_carver/*.json`) и списков по биомам, пещеры/каньоны/комнаты, маска, применение маски (26.3+), «жадное» применение (26.1/26.2) |
| `libmcgen/src/carver.h` | внутренний интерфейс стадии, контракт со SURFACE (`mcgen_surface_top_material`), экспорт маски `carvers_chunk_mask` |
| `libmcgen/src/terrain.c`, `terrain_old.c` | **точечное расширение W1**: `TerrainCtx` сохраняет aquifer последнего заполненного чанка (`terrain_carve_substance`, `terrain_carve_sched`, `terrain_marks_rw`); `terrain_old.c` — `aq_substance_xyz` (позиция явно, барьерный шум по точке `SinglePointContext`) |
| `libmcgen/src/region.c` | хук после TERRAIN/SURFACE: `carvers_apply_chunk` на том же `TerrainCtx` (aquifer и кэши сэмплеров чанка — как в игре); то же для «гало»-чанков растекания |
| `libmcgen/src/mcgen_internal.h`, `world.c` | поле `McWorld.carvers` (строится лениво при первом чанке, потокобезопасно), освобождение |
| `libmcgen/tests/g4_carvers.py` | ворота G4: генерация CLI + `tools/gt/diff.py`, таблица, JSON-отчёт |
| `libmcgen/tests/g4_eager.c` | самопроверка: два пути применения маски (отложенный 26.3+ и «жадный» 26.1/26.2) дают одинаковые блоки на заполнении без травы |

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
   выражения задан явными временными (в C порядок вычисления аргументов не определён); `-ffp-contract=off`. `Mth.sin/cos` — таблица `SIN[65536]`
   из `(float)Math.sin(i / 10430.378350470453)` (свёрнутый хэш всех 65536 значений совпал с Java `Math.sin` и `StrictMath.sin`).

### 3.3. Применение маски (26.3+, `applyCarvingMask`)
Столбцы по x (внешний), z; в столбце — непрерывные отрезки маски **снизу вверх**, внутри отрезка **сверху вниз**; флаг `hasGrass` сбрасывается на каждом отрезке:
* блок из тега `minecraft:uncarvable` (бедрок) — пропуск (проверяется текущий блок);
* блок — трава/мицелий → `hasGrass = true`;
* `Aquifer.computeSubstance(x, y, z, 0.0)` (aquifer W1: `terrain_carve_substance`): `null` (барьер) — блок остаётся; иначе записывается воздух/вода/лава;
  пометка пост-обработки, если `shouldScheduleFluidUpdate` и состояние — жидкость;
* если `hasGrass` и под вырезанным блоком `dirt` — он заменяется материалом поверхности (`topMaterial`, **стадия W2** — слабый символ
  `mcgen_surface_top_material`, правило поверхности в одной точке с `stoneDepth = 1`, `waterHeight = y+1 | −∞`, градиенты по карте WORLD_SURFACE_WG).
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

### 3.5. 26.4-snapshot-2
Классы и данные карверов совпадают с 26.3 (отличается только `VerticalAnchor.Context` вместо `WorldGenerationContext`); маска строится тем же кодом.
Но карвинг встроен в `ChunkTerrainBuilder.fillColumn` (проход поверхности по столбцу): вырезаются только «твёрдые» блоки шума (воздух и жидкости шума не трогаются),
запрет — по результату правила материала (а не по текущему блоку), вместо `hasGrass` — `carvedTopBlock` (повторный расчёт правила для `dirt` с `stoneDepthAbove = 1`).
Реализовано: для стадий без SURFACE (`carve_raw`) — применение маски только к блокам, которые не воздух и не жидкость (`apply_mask`, ветка `v264`).
Для полной поверхности 26.4 нужна `surface_264` у W2; она получает маску через `carvers_chunk_mask` и aquifer через `terrain_carve_substance`.

## 4. Тонкие настройки (стадия carvers)
| id | что делает | по умолчанию |
|---|---|---|
| `canyon_frequency` | вероятность каньонов × k (≤ 1; 0 — никогда) | 1.0 — побитово ваниль |
| `cave_density` | вероятность карверов-пещер × D (≤ 1; 0 — нет классических пещер) | 1.0 |
| `cave_size` | множитель радиусов (`horizontal/vertical_radius_multiplier`) и толщины пещер-карверов (тоннели и комнаты) | 1.0 |
При значениях по умолчанию результат побитово ванильный (G4 идёт без настроек).

## 5. Расхождения и их причины
Разбор единственного расхождения G4 (Overworld, seed −7048155917072976836, область (0,0), блок (94, 16, −80): у нас `water[level=0]`, в эталоне `water[level=1]`):
вырезанные блоки до растекания совпадают (воздух у нас и в эталоне); различие — порядок обработки чанков при `postProcessGeneration`: в игре чанк (5, −5) растёкся раньше
соседнего (5, −6), поэтому клетка (94, 16, −80) получила уровень 1 (при обратном порядке она — второй источник рядом и по правилу «бесконечной воды» становится источником).
Порядок `region_postprocess` (cz, затем cx) — допущение W1. Проверена альтернатива «по расстоянию от центра области» (как волна загрузки): она исправляет этот блок, но даёт
3 новых расхождения на 16 эталонах `carvers` Overworld (против 1) — настоящий порядок не воспроизводится ни тем, ни другим (зависит от планировщика загрузки чанков сервера).
Остаточный шум: 1 блок на 3.2 · 10⁹ (строгое сравнение), в официальных воротах `run_gate.py` такие клетки попадают под маску «текущих жидкостей». Карверы к расхождению не причастны.

## 6. Тесты и воспроизведение
```bash
make -C libmcgen
# ворота G4 по всей матрице W6 (carve_raw: стадии 0xb; carvers: 0xf), строго
python3 libmcgen/tests/g4_carvers.py --version 26.3 --variants carve_raw carvers --report g4-26.3.json
python3 tools/gt/run_gate.py --gate G4c --profile all      # официальная таблица (docs/blender/accuracy.md): карверы без поверхности
python3 tools/gt/run_gate.py --gate G4  --profile all      # с поверхностью
# один эталон вручную (--pp-margin = margin − 1, margin = 4)
libmcgen/build/mcgen-cli --pack run/pack-26.3 --version 26.3 --dim minecraft:overworld --seed 12345 --cx0 -14 --cz0 -14 --nx 29 --nz 29 \
        --stages 0xb --tweak ore_veins=0 --pp-margin 3 --out /tmp/cr.mcr
python3 tools/gt/diff.py --ref run/gt/26.3/carve_raw/overworld-s12345-c0_0-r10 --mcr /tmp/cr.mcr --mask-ext --no-mask-flow
# согласованность двух путей применения маски (26.1–26.4, все измерения)
make -C libmcgen build/tests/g4_eager && libmcgen/build/tests/g4_eager run/pack-26.3 26.3 minecraft:overworld 12345 -4 -4 10
```
Стадии для `carve_raw` — **0xb** (BIOMES|TERRAIN|CARVERS, без SURFACE): `G4c` в `tools/gt/run_gate.py` исправлен с 0xf (после подключения SURFACE W2 она бы
накладывала поверхность на эталон без неё).

## 7. Что осталось / ограничения
* Эталоны карверов для 26.1, 26.2, 26.4-snapshot-2 — у W6 нет (есть только `raw`). Нужны `carve_raw` и `carvers` (хотя бы r = 2…3, Overworld + Nether, 1–2 seed).
  До этого «жадный» путь и ветка 26.4 проверены только косвенно: `g4_eager` (на данных 26.3 «жадный» и отложенный путь дают одинаковые блоки — 0 расхождений на Overworld и Nether), разбор кода игры.
* 26.4 + полная поверхность — за W2 (`surface_264`, контракт §3.5).
* Тонкие настройки `cave_density`/`cave_size` для карверов не имеют эталона (у игры такой настройки нет) — только по определению.

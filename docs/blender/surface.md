# W2 «Поверхность»: стадия SURFACE libmcgen

Отчёт потока W2 аддона «MC Worldgen» (спецификация — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md`, ворота G3).
Все числа ниже — измеренные; команды воспроизведения — в §5.

## 1. Итог

| что | результат |
|---|---|
| Стадия `MC_STAGE_SURFACE` (бит 4) | реализована для Overworld (normal, large_biomes, amplified, caves, floating_islands, single_biome_surface), Nether, End; версии 26.1, 26.2, 26.3, 26.4-snapshot-2 |
| **G3 против настоящего сервера** (эталоны W6, вариант `surface`, 26.3) | G3_SERVER |
| Против настоящих классов игры без сервера (Java-эталон `libmcgen/tests/g3_java`, §5.2) | G3_JAVA |
| Карты высот (WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES) после стадии | 0 расхождений во всех сравнениях (сервер и Java) |
| Растекание жидкостей, `raw`/`veins` (G2, G2v) | не изменились (стадия SURFACE включается только по запросу бита) |
| Скорость | G3_SPEED |

ABI `mcgen.h` не менялся. Для потока W3 (карверы) экспортирована `mcgen_surface_top_material` (§3.7).

## 2. Где код

| файл | что |
|---|---|
| `libmcgen/src/surface.[ch]` | интерпретатор правил, система поверхности, расширения, зум биомов, `topMaterial` |
| `libmcgen/src/region.c` (точечные правки) | вызов стадии в рабочем потоке региона и для «гало»-чанков, неявные зависимости SURFACE → TERRAIN+BIOMES, точные классы состояний для карт высот |
| `libmcgen/src/world.c`, `mcgen_internal.h` (точечные правки) | `McWorld.surface`, создание/освобождение в `mcgen_world_new/free` |
| `libmcgen/tests/g3_surface.py` | G3 по эталонным мирам сервера (`run/gt/<V>/surface/*`) |
| `libmcgen/tests/g3_java.py`, `libmcgen/tests/g3_java/{old,new,v264}/SurfRef.java`, `build.sh`, `run.sh` | G3 против настоящих классов игры (JVM без сервера), поиск областей по биомам |
| `libmcgen/tests/g3_rules.c` | самопроверка без эталонов: разбор правил всех версий/измерений/пресетов, независимость от числа потоков и окружения чанка, `topMaterial` |

## 3. Как устроено

### 3.1. Данные и правила
Дерево правил читается из `noise_settings`: 26.1/26.2 — встроенное поле `surface_rule`; 26.3+ — ссылка `material_rule` на
`worldgen/material_rule/*.json`, условия — `worldgen/material_condition/*.json` (ссылки подставляются при разборе). Поддержаны
все типы из данных четырёх версий: правила `block`, `sequence`, `condition`, `bandlands`, `ore_vein` (26.3+); условия `biome`,
`noise_threshold` (в т. ч. `is_3d`), `vertical_gradient`, `y_above`, `water`, `temperature`, `steep`, `not`, `hole`,
`above_preliminary_surface`, `stone_depth` (floor/ceiling, `add_surface_depth`, `secondary_depth_range`). Состояния блоков —
`{"Name","Properties"}` или строка; недостающие свойства берутся из состояния по умолчанию. Дерево неизменяемо и общее для
потоков; у потока своя «лень»: кэши условий по колонке и по шагу y (как `LazyXZCondition`/`LazyYCondition`), кэш значений
шумов по слотам (одно значение на колонку/позицию для всех условий с одним шумом).

### 3.2. Обход столбцов (SurfaceSystem / MaterialSystem.buildSurface)
Порядок — как у игры: x снаружи, z внутри. Для столбца: высота `WORLD_SURFACE_WG + 1`; биом поверхности (зум в точке
(x, начальная_высота, z); 26.1/26.2 с `legacy_random_source` — y = 0); расширение бэдлендов; градиенты; затем сверху вниз
`stoneAboveDepth`, `waterHeight`, потолок камня (`nextCeilingStoneY` с просмотром вниз), правило в каждой «твёрдой» клетке;
в конце расширение замёрзшего океана (айсберги). `surface` (глубина: `(int)(n·2.75 + 3 + rnd·0.25)`, `rnd` — позиционная
фабрика домена `terrain`), `surface_secondary`, глинистые полосы (`clay_bands` по `fromHashOf`, 192 слоя) — как в игре.
Карта высот `WORLD_SURFACE_WG` ведётся по ходу работы (`Heightmap.update` с правилом «у − 2» и перескоком вниз при снятии верхнего
блока): расширения и `hole` меняют её, а градиенты соседних столбцов читают текущее состояние.

### 3.3. Различия версий (поведение, закодированное в Java)
| | 26.1 / 26.2 | 26.3 | 26.4-snapshot-2 |
|---|---|---|---|
| точность шумов | `double` (`NormalNoise.getValue`) | `float` (`Noise.get`, `NoiseStack`), `Math.round(float)` для полос | как 26.3 |
| к какому блоку применяется правило | только `default_block` (`old == defaultBlock`); жилы руды — в заполнении | ко всем «камням» (не воздух и не жидкость); жилы — правило `ore_vein` в той же последовательности | как 26.3 |
| preliminary surface (`above_preliminary_surface`) | `NoiseChunk.preliminarySurfaceLevel` в 4 углах ячейки 16×16, билинейно (`Mth.lerp2` в `double`) | `chunk_surface_level` объёмом 16×1×16 | как 26.3 |
| градиенты `steep` | по текущей карте высот (ленивая проверка) | по текущей карте высот на старте столбца | по высотам шума **до** расширений (`NoiseColumn.columnMaxYs`) |
| потолок камня у дна мира | просмотр до minY − 1 | то же | `getCeilingBelowIndex`: без «пустоты» под миром → потолка нет |
| айсберги | всегда выполняют цикл и расход случайных (при `top ≤ 2` — с нулевыми границами) | при `top ≤ 2` выход без расхода | как 26.3 |
| температура биома | `PerlinSimplexNoise` (`double`) | `SimplexNoise`/`NoiseStack` (`float`) | как 26.3 |
| источник биома | `BiomeManager` + соседние чанки | то же | блочные биомы чанка (совпадают с зумом, см. `terrain.md` §3.7) |

### 3.4. Биомы для правил
`BiomeManager.getBiome` повторён целиком (зум 8 углов с «размытием» по obfuscated-seed). Клетки собственного чанка берутся из
биомов стадии BIOMES, соседние — `world_biome_cell` (то, что игра берёт из соседних чанков). Ускорение: кэш «размытия» по угловым
клеткам, кэш клеток соседей, быстрый отказ условия `biome`, если биома нет среди клеток чанка на этом уровне.

### 3.5. Жилы руды и стадия TERRAIN
В 26.3+ `material_rule` содержит `ore_vein` (медь, железо) **до** правил поверхности: первое правило, вернувшее блок, побеждает.
Жилы ставит стадия TERRAIN (`terrain_veins.c`, проверена вариантом `veins`), поэтому правило `ore_vein` в SURFACE «узнаёт» уже
поставленный блок жилы (руда / сырой блок / наполнитель своего правила) и сохраняет его; глубинный сланец и бедрок
(`bedrock_floor` стоит раньше) перекрывают, как в игре. Твик `ore_veins = 0` работает так же, как и раньше.

### 3.6. Температура и айсберги
Условие `temperature` и растаивание айсбергов (`shouldMeltFrozenOceanIcebergSlightly`) требуют `Biome.getTemperature`: базовая
температура и `temperature_modifier` читаются из `worldgen/biome/*.json`, `frozen` — шумы `FROZEN_TEMPERATURE_NOISE`/`BIOME_INFO_NOISE`
(Simplex 2D с фиксированными seed 3456/2345; 1234 — снежная линия), версионные реализации — `PerlinSimplexNoise` (26.1/26.2) и `NoiseStack`
(26.3+).

### 3.7. Интерфейс со стадиями выше
* **Конвейер** (`region.c`): TERRAIN → SURFACE → CARVERS → … → растекание жидкостей → карты высот. Пометки пост-обработки
  (жидкости, поставленные поверхностью) копятся вместе с пометками заполнения. SURFACE неявно включает BIOMES и TERRAIN.
  Гало-чанки (нужны растеканию за краем региона) тоже проходят SURFACE.
* **`mcgen_surface_top_material(w, t, cx, cz, blocks, x, y, z, under_fluid)`** (для карверов W3): `topMaterial` игры — правило
  в одной точке с `stoneDepthAbove = stoneDepthBelow = 1`, `waterHeight = under_fluid ? y + 1 : MIN`, градиенты по карте
  высот чанка в момент вызова. Координаты **абсолютные**. Возвращает id состояния или −1 (`Optional.empty`). Контекст потока
  принадлежит миру и освобождается вместе с ним.
* **Карты высот**: после всех стадий считаются из блоков; классы состояний — по тегам датапака
  `blocks_motion_in_heightmap[_no_leaves]` (26.3+), для 26.1/26.2 — эвристика по именам (исправлены `grass_block`, листва
  цветущей азалии и `powder_snow`).

## 4. Тонкие настройки
Настройки W1 стадию не ломают: `sea_level_offset` сдвигает уровень моря системы (айсберги, снежная линия) вместе с заполнением,
числа в правилах — абсолютные y датапака и от настроек не зависят. Отдельных настроек поверхности нет.

## 5. Тесты и воспроизведение

```bash
make -C libmcgen && make -C libmcgen test              # g3_rules: 32 комбинации версия×измерение×пресет, потоки, окружение, topMaterial
# 5.1 G3 против настоящего сервера (эталоны W6, 26.3): все чанки дампа r+4, без масок жидкостей, карты высот, биомы
python3 libmcgen/tests/g3_surface.py --version 26.3 --report libmcgen/tests/results/g3-26.3.json
python3 tools/gt/run_gate.py --gate G3                  # то же через инструмент W6, секция G3 в docs/blender/accuracy.md
# 5.2 G3 против настоящих классов игры (без сервера): любые области, версии и пресеты
libmcgen/tests/g3_java/build.sh 26.4-snapshot-2
python3 libmcgen/tests/g3_java.py --version 26.4-snapshot-2 --dim overworld --seeds 12345,8675309 \
    --find frozen_ocean,eroded_badlands,mushroom_fields,ice_spikes --radius 220 --nx 6 --nz 6 --stats packed_ice,terracotta
```

G3_RESULTS

## 6. Решения и находки

G3_FINDINGS

## 7. Что не сделано / ограничения

G3_LIMITS

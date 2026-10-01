# 05. Точная математика шума/климата/End и верификация C-движка (26.1 / 26.2 / 26.3)

Этот документ закрывает пробел `docs/04 §4` («точная математика … не выяснено»). Всё здесь **проверено запуском**: C-движок `engine/` совпадает с реальным кодом игры (oracle) — см. §7.
Пути к исходникам — `src/dec/<V>/net/minecraft/…`. Реализация — `engine/mc_rng.h`, `mc_noise.h`, `mc_climate.h`, `mc_biomes.h`, `mc_end.h`, `mc_data.h`.

## 1. Как seed превращается в климат Overworld (quart → 6 параметров → биом)

1. `RandomState` (26.2: `levelgen/RandomState.java:36`; 26.3: `RandomState.java:41-100`): `random = XoroshiroRandomSource(seed).forkPositional()`.
   `XoroshiroRandomSource(long)` = `upgradeSeedTo128bit`: `lo = seed ^ 0x6A09E667F3BCC909`, `hi = lo + 0x9E3779B97F4A7C15`, оба пропускаются через `mixStafford13` (→ `mc_rng.h: xoro_from_long_seed`). `forkPositional` = два `nextLong` → `(seedLo, seedHi)` фабрики.
2. Каждый именованный шум: `positional.fromHashOf("minecraft:<имя>")` = `Xoroshiro(md5lo(name) ^ seedLo, md5hi(name) ^ seedHi)` (MD5 от UTF-8, два big-endian long). **Порядок создания шумов не важен** — каждый определяется только именем и seed.
3. `NormalNoise`: два `PerlinNoise` (first/second); для каждого `random.forkPositional()` (2 nextLong из одного и того же потока шума: сначала first, потом second), а октава `i` — `positional.fromHashOf("octave_<firstOctave+i>")`. Октава с нулевой амплитудой пропускается (не создаётся).
4. `ImprovedNoise`/`GradientNoise`: `xo,yo,zo = nextDouble()*256` ×3, затем перестановка Фишера–Йейтса `nextInt(256-i)` (i=0..255). `nextDouble() = (nextLong()>>>11) * 2^-53`; `nextInt(bound)` у Xoroshiro — умножение 32 бит на bound с отбраковкой (`XoroshiroRandomSource.nextInt`).
5. Шесть используемых шумов Overworld (пресет → имя): temperature, vegetation (humidity), continentalness, erosion, ridge (weirdness), + `offset` (для shift). `large_biomes` подменяет `*_large`, `amplified` меняет только сплайн offset.
   Параметры: `worldgen/noise/*.json` (26.1/26.2: `firstOctave`+`amplitudes`; 26.3: `base_octave`, `octave_count`, `amplitude_modifiers`, `base_amplitude`, `normalize`). Экстрактор: `tools/extract_climate.py` → `data/climate-<V>.txt`.
6. Климат в точке `(bx,by,bz) = 4*(qx,qy,qz)` (`Climate.Sampler.sample`):
   * `shift_x = 4*offset(bx/4, 0, bz/4)`, `shift_z = 4*offset(bz/4, bx/4, 0)` (ShiftA/ShiftB: оси переставлены);
   * `x = bx*0.25 + shift_x`, `z = bz*0.25 + shift_z`, `y = by*0.0` (y_scale = 0 ⇒ **климат 2D кроме depth**);
   * `temperature, vegetation, continentalness, erosion, ridges = NormalNoise(x, y, z)`;
   * `ridges_folded = (|(|ridges| + c1)| + c2) * c3` (c1 = −2/3, c2 = −1/3, c3 = −3);
   * `offset = −0.50375 + spline_offset(continents, erosion, ridges, ridges_folded)` (blend_alpha = 1, blend_offset = 0 для новых миров);
   * `depth = gradient_y(by) + offset`, `gradient: y=-64 → 1.5, y=320 → −1.5` (clamped);
   * квантование `Climate.quantizeCoord(float) = (long)(v * 10000.0f)` (усечение к нулю).
7. Биом: `ParameterList.findValue` → `Climate.RTree` (7 измерений: 6 + offset; 26.1/26.2 `CHILDREN_PER_NODE = 6`, 26.3 — 19; `Climate.java:249` / `:152`). Порядок точек: `OverworldBiomeBuilder` (дамп: `data/params/<V>/overworld.tsv`). Поиск эквивалентен «минимум суммы квадратов расстояний до коробок»; **при равном fitness результат зависит от `lastResult` (ThreadLocal) предыдущего поиска — в самой игре недетерминирован** (доля ничьих в тестах ≈ 0.03 %).

## 2. Два численных режима шума

### 26.1/26.2 — double (`mc_noise.h: improved_noise_d`, `normal_get_d`)
* `ImprovedNoise.noise`: `x = _x + xo`, `xf = floor(x)`, `xr = x − xf` (double); градиент `dot = g0*x + g1*y + g2*z`; `smoothstep(t) = t³(6t²−15t+10)`; `lerp3` = `lerp(z, lerp(y, lerp(x,…), lerp(x,…)), lerp(y, …))`.
* `PerlinNoise.getValue`: для каждого уровня `i` (0..octaves−1): `factor = 2^firstOctave·2^i`, `noise(wrap(x*factor), wrap(y*factor), wrap(z*factor))`, `value += amp[i]·noise·valueFactor_i`, где `valueFactor_i = 2^(n−1)/(2^n−1)/2^i` (n — размер массива амплитуд), `wrap(x) = x − lfloor(x/2^25 + 0.5)·2^25`.
* `NormalNoise.getValue = (first(x,y,z) + second(1.0181268882175227·x, …)) * valueFactor`, `valueFactor = (1/6)/(0.1·(1 + 1/(maxOct−minOct+1)))` по ненулевым амплитудам.
* Спайн — `float` (`CubicSpline`), координаты сплайна — `(float)` от double; `offset` складывается в double: `−0.5037500262260437 + (double)spline`.

### 26.3 — float (`improved_noise_f`, `normal_get_f`)
* `GradientNoise/PerlinNoise.get`: `x = wrap(_x) + offsetX` (double; `wrap` не трогает |x| < nextDown(2^24)), `floor`, `rel = (float)(x − floor)`, градиенты и `lerp/smoothstep` — **float**: `gradDot = gx*x + gy*y + gz*z` во float.
* `NormalNoise.create` строит `NoiseStack` — **слои в порядке** `first_0, second_0, first_1, second_1, …`; слой: `value += amp_f · noise.get(x·freq, y·freq, z·freq)` (float), `second.freq = freq·1.0181268882175227` (double), `amp_f = (float)(normalizationFactor · octave.amplitude)`.
  `octave.amplitude = baseAmplitude · 2^(n−1)/(2^n−1) · 0.5^i · modifier` (при `normalize`), `normalizationFactor = (Σ|amp|·1/3) / (√2·√Σ(0.27022478·|amp|)²)`, сумма — `DoubleStream.sum()` (Kahan! `mc_stream_sum`). Параметры JSON 26.3 подобраны «parity»-образом, чтобы воспроизводить старые значения.
* Константы ridges_folded — float (`−0.6666667f`…); `depth`: `grad = from + (clamp(y) − from)·((to−from)/range)` во float; `offset = −0.50375f + spline`.
* **Главное следствие:** значения 26.3 отличаются от 26.2 на ~1e-7 (float), что после квантования ×10⁴ меняет биом лишь на границах (и отдельно — другая таблица биомов: `dappled_forest`, `sulfur_caves`).

**FMA:** Java не сливает операции; C/CUDA надо собирать с `-ffp-contract=off` / `nvcc --fmad=false`, иначе бит-точность теряется.

## 3. Nether (48 бит seed)
* `noise_settings/nether.json`: `legacy_random_source = true`; температура/влажность — шумы `nether/temperature` и `nether/vegetation` (`firstOctave = −7`, амплитуды [1,1]); `RandomState` создаёт их из `LegacyRandomSource(seed + 0)` и `LegacyRandomSource(seed + 1)` (26.2: `RandomState.java:55-60`; 26.3: `RandomState.java:80-87`). ⇒ **Nether-биомы зависят только от младших 48 бит seed.**
* Инициализация последовательная из одного LCG (не `fromHashOf`): создаётся «нулевая» октава (`ImprovedNoise`), затем для i = zeroIdx−1…0: если `i < octaves` и амплитуда ≠ 0 — новая `ImprovedNoise`, иначе `consumeCount(262)` (`PerlinNoise.skipOctave`). Для [1,1] при `firstOctave = −7`: пропуск 5×262 вызовов, затем 2 октавы; то же для `second` (LCG продолжается). В 26.3 — `LegacyFbmInitializer` + `NoiseStack` (float).
* Климат: `x = bx·0.25`, `y = by·0.0`, `z = bz·0.25` (**2D**), depth = weirdness = continentalness = erosion = 0; 5 точек параметров (nether_wastes, soul_sand_valley, crimson_forest, warped_forest (offset .375), basalt_deltas (offset .175)).

## 4. End (48 бит seed)
* `erosion = end/islands` — `SimplexNoise` на `LegacyRandomSource(seed)` после `consumeCount(17292)`; 2D `getValue` **не добавляет** смещения `xo/yo` (в 26.3 — `SimplexNoise(random, true)`: смещения = 0, но 3 `nextDouble` всё равно потребляются). (26.2: `DensityFunctions.java:492`, 26.3: `EndIslandFunction.java`, `RandomState.java:95`.)
* `TheEndBiomeSource.getNoiseBiome`: центр (`chunkX²+chunkZ² ≤ 4096`) → `the_end`; иначе по `erosion` в точке `((chunk·2+1)·8, ·, (chunk·2+1)·8)`: `> 0.25` highlands, `≥ −0.0625` midlands, `< −0.21875` small_end_islands, иначе barrens (пороги — `TheEndBiomeSource.java`).
* `getHeightValue` (float): внешние острова — клетки чанков радиуса 12 вокруг, где `simplex(chunkX, chunkZ) < −0.9` и `chunk² > 4096`; размер `(|cx|·3439 + |cz|·147) % 13 + 9`; `doffs = max(…, clamp(100 − √(xd²+zd²)·size, −100, 80))`. 26.2: центральный остров внутри той же функции (`100 − √(sx²+sz²)·8`, секция = `block/8` с усечением к нулю); 26.3: центральный остров — отдельная JSON-функция `max(clamp(100 − dist, −100, 80) − 8)·2⁻⁷, end_outer_islands)` во float; сравнение шума `< −0.9f` (26.2: double против `(double)−0.9f`, 26.3: после приведения к float).
* Стоимость точки: 25×25 = 625 вычислений simplex ⇒ для поиска seed разумно предвычислять карту «центров островов».

## 5. R-дерево и ничьи
`mc_biomes.h` — порт `Climate.RTree.create/build/search`: сортировка по центрам (`(min+max)/2` с усечением к нулю), bucketize по `expected = 19^⌊log(n−0.01)/log 19⌋` (или 6), выбор измерения по минимальной суммарной стоимости, сортировка корзин по |центр|. Поиск — с `candidate = null` (первый поиск в потоке Java). Дамп реального дерева из игры: `oracle/run.sh <V> rtree` (`data/rtree_<V>_*.json`).

## 6. Различия версий (что меняется при том же seed)
| | 26.1 → 26.2 | 26.2 → 26.3 |
|---|---|---|
| Таблица климата Overworld | +1 запись (`sulfur_caves`, подземный биом, depth 0.2–0.9) → 7595 | 92 записи `plains` → `dappled_forest` (T∈[−0.45,−0.15], H≤−0.35, высокая weirdness-вариация) |
| Шум | double | **float** (NoiseStack, parity-параметры) |
| R-дерево | 6 потомков | **19** потомков (влияет только на ничьи) |
| Nether-таблица | = | = |
| End | double | float, центральный остров вынесен в JSON |
| Структуры (RandomSpread, слайм, пиллары) | без изменений (векторы совпали побайтово) | без изменений; +`abandoned_camp` |

## 7. Верификация (воспроизводимо)
| Проверка | Как | Результат |
|---|---|---|
| NormalNoise, режим DOUBLE | `tests/unit/test_noise_old.c` против `NoiseRef262.java` (реальные классы 26.2): 9 seed × 6 шумов × 40 точек, координаты до 2·10⁷ | 2160 значений, **0 расхождений (бит-в-бит)** |
| NormalNoise, режим FLOAT | `tests/unit/test_noise_new.c` против `NoiseRef263.java` (26.3) | 2160, **0** |
| Климат (6 квантованных значений), одна точка | `oracle climate` vs `tools/mcquery --climate` | совпало |
| Биомы, сетки 48×48, 10 seed × (1–3 окна: у нуля, ±2·10⁵, ±3·10⁶ блоков) × несколько y | `python3 tests/diff_biomes.py --versions 26.1 26.2 26.3 --dims overworld nether end --seeds 10 --size 48` | **1 244 160 точек, 0 реальных расхождений**; 195 ничьих fitness (доля ≈ 0.03 %; Overworld 59/73/55, Nether 21/7/5, End 0) — недетерминированы и в игре |
| Пресеты `large_biomes`, `amplified` (26.1 и 26.3; 26.2 использует те же данные, что 26.1) | `tests/diff_biomes.py --versions 26.1 26.3 --dims overworld --seeds 6 --size 40 --preset …` | по 76 800 точек на версию и пресет: **0 реальных расхождений** (ничьих 24–40) |

Скорость эталонного CPU-движка (1 поток, `-O2`, без оптимизаций): Overworld ≈ 1.5·10⁵ точек/с (включая поиск по R-дереву), Nether ≈ 10⁶ точек/с.

## 8. Расширяемость: 26.4-snapshot-2 (проверка на ещё не вышедшей версии)
Цель — показать, что конвейер «jar → декомпиляция → данные → дифф с oracle» переносится на следующую версию за минуты. Шаги (все сделаны и повторяемы): скачать jar из манифеста; `unzip META-INF`/`data`; `tools/extract_climate.py 26.4-snapshot-2`; дамп таблицы параметров (`DumpParams`); `oracle/build.sh 26.4-snapshot-2` (слой `oracle/src/v264`: `BiomeResolver`→`NoiseBiomeResolver`, height-команды отключены); `tests/diff_biomes.py --versions 26.4-snapshot-2`.

Что изменилось относительно 26.3 (по `diff -r` декомпилированных исходников и данных):
* **Данные worldgen** (noise, density_function, structure_set, biome) — без изменений; таблица параметров: 2 подземные записи (`dripstone_caves`, `sulfur_caves`: верхняя граница humidity 1.0 → 0.7).
* **`Climate.Parameter.distance`: `above = target − max + 1`** — верхняя граница интервала теперь **исключающая**; добавлена валидация `ParameterList` («записи не перекрываются по всем шумовым параметрам») — ничьи fitness почти исчезают (Overworld 1/76 800, Nether 0). Эквивалентно старой формуле с `hi' = hi − 1` для всех узлов R-дерева (включая измерение offset) — реализовано в `mc_load_biome_tree` для `MC_26_4`.
* Шум/синтез (`levelgen/synth`) — идентичен 26.3; `densityfunction/*`, `BiomeSource/BiomeResolver` — рефакторинг API (`NoiseBiomeResolver`, `CachedChunkBiomeResolver`, `NoiseBiomeChunk`), не влияет на значения.
* Результат дифф-теста: **172 800 точек (Overworld/Nether/End), 0 реальных расхождений** (до учёта исключающей границы было 7 «расхождений» — все на границах интервалов; именно такие случаи ловит дифф-тест).
Вывод: поддержка новой версии = 4 мелких правки, и каждое изменение семантики ловится автоматически.

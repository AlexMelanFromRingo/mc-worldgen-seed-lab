# 00. Путеводитель по генерации мира Minecraft (26.1 / 26.2 / 26.3): от одного числа до готового мира

Это сквозное руководство «от и до»: как из **одного 64-битного числа (seed)** получается весь мир — шумы, климат, биомы, рельеф, пещеры, поверхность, структуры, растительность. Каждый шаг привязан к реальному коду (`src/dec/<V>/net/minecraft/...`, декомпилированные исходники Mojang, которые с 26.1 не обфусцированы) и, где возможно, к проверке запуском.

Обозначения: **[В]** — проверено запуском (наш C-движок `engine/` совпадает с настоящим кодом игры, см. `docs/05`; позиции структур, хэши и пиллары сверены с `oracle/`), **[К]** — прочитано в исходниках, но отдельно не запускалось. Версии: если не сказано иное, пути — для 26.3; различия 26.1/26.2/26.3 отмечены.
Дальше по деталям: RNG — `docs/01`, структуры — `docs/02`, «где seed влияет» — `docs/03`, математика шума/климата — `docs/05`, GPU — `docs/13`.

---

## 0. Общая картина в одной схеме

```
                       ┌──────────────────────────────────────────────────────────────┐
  (1) ОТКУДА seed      │ randomSeed(): LegacyRandomSource(uniquifier·k ^ nanoTime()).nextLong() │
                       │   или введённый вручную (число / строка → String.hashCode)   │
                       └───────────────────────────────┬──────────────────────────────┘
                                                       │   long seed (64 бита) — хранится в level.dat
        ┌──────────────────────────────┬───────────────┼───────────────┬───────────────────────────┐
        ▼                              ▼               ▼               ▼                           ▼
 (2) RandomState                (3) Legacy-LCG    (4) SHA-256      (5) Xoroshiro на          (6) прочие «мелкие»
 Xoroshiro128++ ← seed          48 бит seed      обфускация seed    декорации/фичи            seed-пользователи:
 positional-фабрика             ┌────────────┐   → клиенту (hashed  WorldgenRandom             слайм-чанки, шипы End,
   │                            │ структуры  │   seed), «зум» биомов (setDecorationSeed,       драконий бой, геода
   │ md5("minecraft:<имя>")     │ (RandomSpread, │ 4×4×4 → 1×1×1    setFeatureSeed)            (LegacyRandomSource(seed))
   ▼                            │ кольца     │                          │
 ШУМЫ (NormalNoise = 2×Перлин   │ стронгхолдов)│                          ▼
 × октавы)                      │ карверы    │                  ДЕКОРАЦИИ: деревья, руды,
   │                            │ Nether/End │                  озёра, цветы, подземные биомы-фичи
   ▼                            │ биомы      │
 КЛИМАТ (6 параметров на       │ слайм-чанки│
 квартах 4×4 блока)            └────────────┘
   │
   ├──► БИОМ (R-дерево: ближайшая точка параметров)  ──► список фич/карверов/поверхность/спавн
   ├──► РЕЛЬЕФ (depth, offset, factor, jaggedness → sloped_cheese → final_density > 0 ⇒ камень)
   └──► ПОВЕРХНОСТЬ (трава/песок/снег… по правилам по биому, глубине и y)
```

Ключевая идея, которой не хватает в наивной картине «одно число → один хэш → всё»: **seed — это не «семя одного генератора», а ключ, из которого независимо выводятся десятки отдельных потоков случайности.** Они не зависят друг от друга: Перлин-шум температуры и позиция деревни получаются из одного seed совершенно разными, непересекающимися преобразованиями. Связь между ними — только через общий исходный seed (и через *данные*: биом решает, какая структура/фича вообще допустима).

---

## 1. Откуда берётся seed

### 1.1. Автоматический seed — «время»
* `WorldOptions.randomSeed()` = `RandomSource.create().nextLong()` (`world/level/levelgen/WorldOptions.java`).
* `RandomSource.create()` = `create(RandomSupport.generateUniqueSeed())`, а `create(long)` — это **`LegacyRandomSource`** (`util/RandomSource.java:14-25`).
* `RandomSupport.generateUniqueSeed()` = `SEED_UNIQUIFIER.updateAndGet(c -> c * 1181783497276652981L) ^ System.nanoTime()`; `SEED_UNIQUIFIER` стартует с `8682522807148012L` (`levelgen/RandomSupport.java`).
  Так что интуиция «берётся текущее время» верна по сути: используется **`System.nanoTime()`** (монотонный счётчик наносекунд JVM — не календарная дата), перемешанный с умножающимся «уникализатором», чтобы два вызова подряд не дали одно значение.
* Результат (48-битное состояние LCG) превращается в `long` двумя вызовами `next(32)`. Отсюда важное следствие **[В]**: у автоматически сгенерированных seed'ов лишь ≤ 2⁴⁸ вариантов (в наших проверках 2000/2000 сгенерированных значений лежат в образе 48-битного LCG; см. `docs/03`).

### 1.2. Ручной seed
`WorldOptions.parseSeed`: пустая строка — случайный; иначе `Long.parseLong(s)`, а если не число — **`s.hashCode()`** (32 бита, Java String.hashCode). Именно поэтому «слово» как seed всегда даёт то же число **[К]**.

### 1.3. Куда seed попадает дальше
* Сохраняется в `level.dat` (`WorldOptions.CODEC`, поле `seed`).
* `MinecraftServer.createLevels()` (`server/MinecraftServer.java:410-440`): `seed = worldOptions.seed()`, `biomeZoomSeed = BiomeManager.obfuscateSeed(seed)`; **один и тот же seed** получают все измерения (Overworld, Nether, End).
* В каждом измерении `ChunkMap` (`server/level/ChunkMap.java:180-184`) создаёт `RandomState.create(noises, levelSeed, noiseGeneratorSettings)`, а `ChunkGeneratorStructureState` — состояние структур с тем же `levelSeed`.

---

## 2. Карта «потребителей» seed: какое преобразование, сколько бит

| # | Потребитель | Преобразование seed | Бит seed | Что определяет |
|---|---|---|---|---|
| A | `RandomState.random` (Overworld/амплифайд/large_biomes) | `XoroshiroRandomSource(seed)` → `forkPositional()` | **64** | все именованные шумы: климат, рельеф, пещеры, жилы, aquifer, поверхность |
| B | Nether/End/«caves» (в `noise_settings`: `legacy_random_source: true`) | `LegacyRandomSource(seed)` → positional-фабрика | **48** | Nether-биомы (`seed`, `seed+1`), бедрок Nether, End-острова (`LegacyRandomSource(seed)` + `consumeCount(17292)`) |
| C | Структуры (`RandomSpreadStructurePlacement`, reducers) | `WorldgenRandom(Legacy).setLargeFeatureWithSalt(seed, gx, gz, salt)` | **48** | позиция «потенциального» чанка структуры в регионе |
| D | Стронгхолды (`ConcentricRingsStructurePlacement`) | `LegacyRandomSource.setSeed(seed)`: угол/радиус колец | 48 (+ биомы) | геометрия колец, затем сдвиг в допустимый биом (зависит от климата) |
| E | Карверы (пещеры/каньоны) | `setLargeFeatureSeed(seed + индекс_карвера, chunkX, chunkZ)` | 48 | «стартовые» чанки и форма пещер-коридоров |
| F | Декорации (фичи) | `setDecorationSeed(seed, x, z)` + `setFeatureSeed(decSeed, индекс, шаг)` на Xoroshiro | **64** | деревья, руды, озёра, цветы, геоды, данжи… |
| G | `BiomeManager.obfuscateSeed(seed)` | SHA-256 от 8 байт seed, первые 8 байт | 64 (необратимо) | «зум» биомов 4×4×4 → блоки; **клиент получает этот хэш** (пакет входа/респавна) |
| H | Слайм-чанки | `seedSlimeChunk(x, z, seed, 987234911L)` (Legacy) | 48 | какие чанки «слаймовые» |
| I | Шипы End (обсидиановые башни) | `createThreadLocalInstance(seed).nextLong() & 0xFFFF` → ключ → перестановка | 16 из 48 | высоты/радиусы башен |
| J | `BlendedNoise` («terrain», базовый 3D-шум рельефа) | `random.fromHashOf("terrain")` (Overworld); Legacy `seed` (если legacy) | 64 / 48 | объёмный «шумок» твёрдости |
| K | Биомные «температурные» шумы для снега/льда (`Biome.TEMPERATURE_NOISE`) | **константы 1234 / 3456 / 2345** — **не зависят от seed** | 0 | линия снега по высоте, льдины |

Вывод: самый «широкий» по числу бит — A/F/G (64 бита); остальные «геометрические» подсистемы (структуры, карверы, слайм, Nether, End) видят только **младшие 48 бит**. Это и есть основа атак на seed (см. `docs/13`).

---

## 3. Строительные кирпичи: генераторы случайных чисел

### 3.1. `LegacyRandomSource` — 48-битный LCG (`java.util.Random`)
```
setSeed(s):  state = (s ^ 0x5DEECE66D) & (2^48 − 1)            // старшие 16 бит s отбрасываются!
next(bits):  state = (state · 0x5DEECE66D + 0xB) & (2^48 − 1);  return (int)(state >> (48 − bits))
nextInt(n):  если n — степень двойки: (n · next(31)) >> 31
             иначе: цикл  v = next(31); m = v % n;  принять, если v − m + (n−1) не переполняет int
nextLong():  (next(32) << 32) + next(32)
nextDouble(): ((next(26) << 27) + next(27)) · 2⁻⁵³
```
**[В]** код: `levelgen/LegacyRandomSource.java`, `BitRandomSource.java`; реализация — `engine/mc_rng.h`.

### 3.2. `XoroshiroRandomSource` — Xoroshiro128++ и «апгрейд» 64 → 128 бит
```
upgrade(seed):  lo = seed ^ 0x6A09E667F3BCC909;  hi = lo + 0x9E3779B97F4A7C15
                (lo, hi) = (mixStafford13(lo), mixStafford13(hi))        // биекция 64→64 по каждой половине
nextLong():     result = rotl(lo + hi, 17) + lo;  hi ^= lo;  lo = rotl(lo, 49) ^ hi ^ (hi << 21);  hi = rotl(hi, 28)
nextDouble():   (nextLong() >>> 11) · 2⁻⁵³        nextFloat(): (nextLong() >>> 40) · 2⁻²⁴
nextInt(n):     умножение 32 бит на n с отбраковкой (метод Лемира)
```
**[В]** `levelgen/Xoroshiro128PlusPlus.java`, `XoroshiroRandomSource.java`, `RandomSupport.java`.

### 3.3. Позиционные фабрики и имена: как «один seed» даёт независимые потоки
`forkPositional()` берёт **два** `nextLong()` — получается «фабрика» `(seedLo, seedHi)`. Из неё:
* `fromHashOf("minecraft:temperature")` = `Xoroshiro(md5lo(name) ^ seedLo, md5hi(name) ^ seedHi)` — **MD5 от имени** (UTF-8) делится на два big-endian `long`;
* `at(x, y, z)` = `Xoroshiro(Mth.getSeed(x,y,z) ^ seedLo, seedHi)` — случайность «для блока»;
* для Legacy: `fromHashOf(name)` = `LegacyRandomSource(name.hashCode() ^ seed)`, `at(x,y,z)` = `LegacyRandomSource(Mth.getSeed(x,y,z) ^ seed)`.

Так и получается независимость: **имя шума → MD5 → XOR с состоянием фабрики → свой отдельный генератор**. Шумы `temperature`, `vegetation`, `continentalness`… никогда не делят состояние, хотя «выросли» из одного seed. (Именно это отвечает на вопрос «один и тот же хэш идёт и в карту биомов, и в шум Перлина?» — нет: в каждый шум попадает *свой* хэш, а общее у них только исходный seed.)

`Mth.getSeed(x, y, z)`: `s = x·3129871 ^ z·116129781L ^ y;  s = s·s·42317861L + s·11L;  return s >> 16`.

### 3.4. `WorldgenRandom` — «сидеры» для регионов и чанков (`levelgen/WorldgenRandom.java`)
```
setLargeFeatureWithSalt(seed, x, z, salt):  setSeed(x·341873128712 + z·132897987541 + seed + salt)     // структуры
setLargeFeatureSeed(seed, cx, cz):          setSeed(seed); a = nextLong(); b = nextLong(); setSeed(cx·a ^ cz·b ^ seed)   // карверы
setDecorationSeed(seed, x, z):              setSeed(seed); a = nextLong()|1; b = nextLong()|1; r = x·a + z·b ^ seed; setSeed(r)  // базовый seed декораций чанка
setFeatureSeed(decSeed, index, step):       setSeed(decSeed + index + 10000·step)                       // конкретная фича на шаге
seedSlimeChunk(x, z, seed, salt):           Random(seed + x·x·4987142 + x·5947611 + z·z·4392871L + z·389711 ^ salt)
```

### 3.5. Сквозной числовой пример: seed = 12345 [В]
```
LegacyRandom setSeed(12345):          state = 0x0005deecd654                (только 48 бит)
Xoroshiro upgrade:                    lo = 0x0a2c34e6ca54dd9e   hi = 0xcf828dadc78bbeeb
forkPositional (2 nextLong):          lo = 0x8f5558a8036890fb   hi = 0x725fe51ad193097e      ← «фабрика мира»
md5("minecraft:temperature"):         lo = 0x5c7e6b29735f0d7f   hi = 0xf7d86f1bbc734988
 → RNG шума temperature:              lo = 0xd32b338170379d84   hi = 0x85878a016de040f6      (md5 ^ фабрика)
   → forkPositional (первый Перлин):  lo = 0x4e30efb12d2c4ee9   hi = 0xa3b0992de5fffc25
   → md5("octave_-10") ^ фабрика:     lo = 0x78e3c95ff922b05b   hi = 0xf8595735c7c39f4f      ← RNG самой низкочастотной октавы
     → ImprovedNoise: смещения xo=186.059741 yo=186.245626 zo=12.250021; перестановка perm[0..7] = 184 252 245 112 233 149 104 205
hashed seed (то, что получает клиент): 0x04139124513554e1 = 293737985876514017
Климат в блоке (0, y=64, 0):          temperature=−215  humidity=6721  continentalness=−3991  erosion=−7294  depth=−1237  weirdness=1569
 → ближайшая точка таблицы параметров: minecraft:ocean
Деревни: регион (0,0): seed' = 0·341873128712 + 0·132897987541 + 12345 + 10387312 = 10399657
 → LCG state = 0x6892c16fd01e;  limit = 34 − 8 = 26;  nextInt(26) → 21, nextInt(26) → 5   ⇒ потенциальный чанк деревни (21, 5)
Шипы End: ключ = 45873; первая башня: центр (42, 0), радиус 4, высота 97
```
Климат и позиция деревни (21, 5) совпали с настоящим кодом игры (`oracle`: `climate`, `structs`), хэш — с `BiomeManager.obfuscateSeed`; остальное посчитано нашим движком (`engine/`), чьи промежуточные состояния проверены побитно.

---

## 4. Шум Перлина: из чего он состоит и как «вычисляется в точке»

В коде это буквально алгоритмы Кена Перлина: `ImprovedNoise` («Improved Noise», 2002) и `SimplexNoise` (2001). **[В]** `synth/ImprovedNoise.java` (26.1/26.2), `synth/GradientNoise.java` + `PerlinNoise.java` (26.3).

### 4.1. Один октавный шум (`ImprovedNoise` / `GradientNoise`)
Создание из генератора `rnd` (зависит только от него, то есть от seed + имени):
```
xo = rnd.nextDouble()·256;  yo = rnd.nextDouble()·256;  zo = rnd.nextDouble()·256      // случайный сдвиг решётки
p[0..255] = 0..255, затем для i = 0..255:  j = i + rnd.nextInt(256 − i);  swap(p[i], p[j])   // перестановка Фишера–Йейтса
```
Значение в точке (x, y, z):
```
x' = x + xo, y' = y + yo, z' = z + zo;     xi = floor(x'), xr = x' − xi   (аналогично y, z)
для 8 углов ячейки (xi..xi+1, yi..yi+1, zi..zi+1):
    h = p[(p[(p[xi & 255] + yi) & 255] + zi) & 255]           // «хэш угла» через таблицу перестановок
    g = GRADIENT[h & 15]                                       // 12 направлений «к серединам рёбер куба» (+4 повтора) — градиенты Перлина
    d = g · (смещение от угла к точке)                         // скалярное произведение
fade(t) = t³(6t² − 15t + 10)                                    // кривая сглаживания Перлина (6t⁵ − 15t⁴ + 10t³)
результат = трилинейная интерполяция восьми d с весами fade(xr), fade(yr), fade(zr)   // диапазон ≈ [−1; 1]
```
* **26.1/26.2** считают всё в `double`; **26.3** — в `float` (`(float)(x − floor)`, `gradDot`, `lerp`, `smoothstep` — float). Поэтому значения 26.3 отличаются от 26.2 на ~10⁻⁷ — этого достаточно, чтобы редкие точки на границах биомов поменялись.
* Идея «один и тот же шум, но неповторимый»: перестановка `p` и сдвиги `xo,yo,zo` случайны и определяются seed'ом. Больше в алгоритме от seed ничего не зависит.

### 4.2. Октавы (`PerlinNoise`) и «двойной» шум (`NormalNoise`)
Одна октава слишком гладкая, поэтому их складывают с растущей частотой и убывающей амплитудой (fBm):
```
value = Σ_i  amplitude[i] · noise_i( x·f_i, y·f_i, z·f_i ),     f_i = 2^(firstOctave + i)
```
`firstOctave` — «основной масштаб»: октава −10 — это 1 цикл на 2¹⁰ = 1024 единицы входа. Для климата вход — **кварты** (1 кварт = 4 блока), поэтому октава −10 ≈ 4096 блоков на период, −9 ≈ 2048, −7 ≈ 512, −3 ≈ 32.
* Каждая октава — отдельный `ImprovedNoise` со **своим** генератором: `positional.fromHashOf("octave_" + (firstOctave + i))`. Нулевая амплитуда — октава пропускается (не создаётся).
* `NormalNoise` берёт **два** таких стека (first, second) из одного потока генератора и складывает: `(first(x,y,z) + second(x·1.0181268882175227, y·…, z·…)) · valueFactor`. Множитель 1.018… разрывает совпадение решёток двух стеков (отсюда «нормальное», более гауссово распределение), а `valueFactor` (в 26.1/26.2 `(1/6)/(0.1·(1 + 1/(span+1)))`) нормирует разброс.
* Параметры шумов — данные: `worldgen/noise/<имя>.json`, например `temperature`: `firstOctave −10, amplitudes [1.5, 0, 1, 0, 0, 0]`; `continentalness`: `−9, [1,1,2,2,2,1,1,1,1]`; `ridge`: `−7, [1,2,1,0,0,0]`; `offset`: `−3, [1,1,1,0]`; для large_biomes те же амплитуды, но `firstOctave` меньше на 2 (масштаб ×4). В 26.3 JSON записан в «parity»-виде (`base_amplitude`, `octave_count`, `amplitude_modifiers`), результат тот же (с точностью float).

Итог: **любой «шум X в точке» = функция от (seed, имя X, координаты)**, без побочных зависимостей. Реализация и побитовая проверка: `engine/mc_noise.h`, `tests/unit/*` ([В]: 4320 значений, 0 расхождений).

---

## 5. От шума к климату Overworld: 6 параметров

Для каждой точки сетки биомов (кварт 4×4×4 блока; `blockX = 4·quartX` …) считаются шесть чисел. **[В]** `densityfunction/`/`DensityFunctions` + `data/.../density_function/overworld/*.json`; `Climate.Sampler.sample`.

1. **Искажение координат («shift»):** `shift_x = 4·offset(x/4, 0, z/4)`, `shift_z = 4·offset(z/4, x/4, 0)`; дальше все климатические шумы берутся в точке `(x/4 + shift_x, 0, z/4 + shift_z)`. Это «варпинг» — он делает границы биомов извилистыми, а не повторяющими решётку шума. Вход — только x и z: **климат двумерный** (y_scale = 0) — на разных высотах колонки значения те же, кроме `depth`.
2. **temperature** (T), **vegetation** (в коде «humidity», H), **continentalness** (C), **erosion** (E), **ridges / weirdness** (W) — просто значения соответствующих `NormalNoise` в этой точке (диапазон примерно [−1; 1]).
3. **ridges_folded** (PV, «peaks and valleys»): `PV = −3·( | |W| − 2/3 | − 1/3 )`. Это складка: `W ≈ 0` → PV = −1 (долина), `|W| ≈ 2/3` → PV = +1 (пик), `|W| → 1` → PV ≈ 0.
4. **offset** (рельефная «база»): `offset = −0.50375 + spline(C, E, W, PV)` — кусочно-кубический **сплайн** (`CubicSpline`, дерево: C → E → PV/W). Большая континентальность — выше, высокая эрозия — ниже и т. д. Это «карта высот в нормированных единицах».
5. **depth** (D): `D = gradient_y(y) + offset`, где `gradient_y`: на y = −64 равно 1.5, на y = 320 равно −1.5, между — линейно (за пределами — константа). Смысл: `D ≈ 0` — «на уровне поверхности», `D > 0` — под поверхностью (чем глубже, тем больше), `D < 0` — над ней.
6. **Квантование:** каждый параметр → `(long)(значение · 10000)`. Так что «температура 0.55» — это 5500.

Итого одна точка → вектор `(T, H, C, E, D, W)`. **[В]** Для seed 12345 в блоке (0, 64, 0): `(−215, 6721, −3991, −7294, −1237, 1569)`.

---

## 6. От климата к биому: таблица параметров и «ближайший сосед»

### 6.1. Как выбирается биом
Игра хранит **таблицу из 7594–7595 «коробок»** в 7-мерном пространстве `(T, H, C, E, D, W, offset)`, у каждой — свой биом (`MultiNoiseBiomeSource` → `Climate.ParameterList`). Биом точки — тот, чья коробка **ближе всего** (в сумме квадратов расстояний до ближайшего края по каждой оси, 0 если внутри; в 26.4-snapshot верхний край исключающий). Для скорости используется R-дерево (`Climate.RTree`, 6 потомков в 26.1/26.2, 19 в 26.3) — результат совпадает с полным перебором, кроме **точных ничьих** (тогда решает история поиска — в самой игре это недетерминировано, ≈0.03 % точек). **[В]**

### 6.2. Откуда берётся сама таблица (правила, закодированные в `OverworldBiomeBuilder`)
Таблицу строит код, а не JSON. Уровни по осям:

| Ось | Деления |
|---|---|
| Temperature (индексы 0–4) | [−1; −0.45] «замёрзшее», [−0.45; −0.15] «холодное», [−0.15; 0.2] «умеренное», [0.2; 0.55] «тёплое», [0.55; 1] «жаркое» |
| Humidity (0–4) | [−1; −0.35], [−0.35; −0.1], [−0.1; 0.1], [0.1; 0.3], [0.3; 1] — от сухого к влажному |
| Continentalness | грибное [−1.2; −1.05] · глубокий океан [−1.05; −0.455] · океан [−0.455; −0.19] · побережье [−0.19; −0.11] · «около суши» [−0.11; 0.03] · «средняя суша» [0.03; 0.3] · «далёкая суша» [0.3; 1] |
| Erosion (0–6) | [−1; −0.78], [−0.78; −0.375], [−0.375; −0.2225], [−0.2225; 0.05], [0.05; 0.45], [0.45; 0.55], [0.55; 1] — от гористого (0) к плоскому/болотистому (6) |
| Weirdness (срезы «формы рельефа») | долина [−0.05; 0.05] · низ [±0.05…±0.267] · середина [±0.267…±0.4] · верх [±0.4…±0.567] · **пик** [±0.567…±0.767] · верх [±0.767…±0.933] · середина [±0.933…±1] (знак «−» — отрицательная половина, «+» — положительная) |
| Depth | `0` и `1` — **поверхностные** записи (`addSurfaceBiome` создаёт обе); `[0.2; 0.9]` — **подземные** биомы; `1.1` — «самое дно» (глубокая тьма) |

Главные правила (псевдокод от `addOffCoastBiomes/addInlandBiomes/addMidSlice/...`):
```
если C ∈ грибное:                       mushroom_fields                         (любые T, H, E, W)
если C ∈ глубокий океан:                OCEANS[0][T_idx] = deep_frozen/deep_cold/deep/deep_lukewarm/warm_ocean
если C ∈ океан:                         OCEANS[1][T_idx] = frozen/cold/ocean/lukewarm/warm
иначе (суша) — по срезу W:  mid / high / peaks / high / mid / low / VALLEYS / low / mid / high / peaks / high / mid
   внутри среза выбор по (E, C, T_idx, H_idx):
     C = побережье:   E 0–2 → stony_shore;  E 3–4 → «средний» биом;  E 5 → shattered coast;  E 6 → beach (если W<0) / «средний»
     E 0 (самая гористая): slope-биом (T холодный: snowy_slopes / grove; тёплый: плато) — в «пиках» ещё jagged/frozen/stony peaks или badlands
     E 1: плато или «средний/бэдленд-если-жарко»;  E 2: «средний»/плато;  E 3: «средний»;  E 4: «средний» (+beach)
     E 5: shattered (windswept hills/forest/gravelly) или windswept savanna;  E 6 (плоский): swamp (T 1–2), mangrove_swamp (T 3–4), beach…
   «средний» биом = MIDDLE_BIOMES[T_idx][H_idx] (при W<0) или MIDDLE_BIOMES_VARIANT[T_idx][H_idx] (если она задана, при W≥0):
        T\H   сух      …       …       …       влажно
        0     snowy_plains  snowy_plains  snowy_plains  snowy_taiga  taiga
        1     plains        plains        forest        taiga        old_growth_spruce_taiga
        2     flower_forest plains        forest        birch_forest dark_forest
        3     savanna       savanna       forest        jungle       jungle
        4     desert        desert        desert        desert       desert
      (variant при W≥0: ice_spikes…, в 26.3 для T=1,H=0: dappled_forest вместо plains; sunflower_plains; old_growth_birch_forest; bamboo_jungle…)
   «жарко» (T=4) в склонах/пиках → badlands / eroded_badlands / wooded_badlands по H и знаку W
   реки: узкий срез W≈0 (долина) пересекает сушу → river / frozen_river
подземные (D ∈ [0.2; 0.9]):  dripstone_caves  (C ∈ [0.8; 1]),  lush_caves (H ∈ [0.7; 1]),  sulfur_caves (26.2+; C от побережья до суши, E 5–6, W ∈ [−1.1; −0.85]);
дно (D = 1.1):               deep_dark (E ∈ [−1; −0.375])
```
Полную таблицу можно посмотреть: `data/params/<V>/overworld.tsv` (колонки — границы ×10⁴ и биом). Разница версий: 26.2 добавил `sulfur_caves` (+1 запись), 26.3 заменил 92 записи `plains` на `dappled_forest` (холодно-сухо, W ≥ 0), 26.4-snapshot — исключающая верхняя граница.

Практический смысл «если … то»: **рельеф и биом связаны через одни и те же шумы** — гористость задаётся эрозией и PV, а биом выбирается по тем же C/E/W; поэтому горные пики совпадают с биомами «peaks/slopes», а плоские равнины — с болотами и пляжами.

---

## 7. Nether и End (другие правила, тот же принцип)

### 7.1. Nether — 48 бит seed
* Климатические шумы `nether/temperature` и `nether/vegetation` строятся не из Xoroshiro-фабрики, а из **`LegacyRandomSource(seed)`** (температура) и **`LegacyRandomSource(seed + 1)`** (влажность) — последовательно: «нулевая» октава, затем пропуски `consumeCount(262)` и две рабочие октавы (`firstOctave −7`, амплитуды [1, 1]). **[В]**
* Климат двумерный (y_scale 0): вход `(x/4, 0, z/4)` без варпинга; остальные параметры (C, E, D, W) = 0.
* Таблица — всего **5 точек** (`MultiNoiseBiomeSourceParameterList.Preset.NETHER`): `(T, H)` = (0, 0) nether_wastes · (0, −0.5) soul_sand_valley · (0.4, 0) crimson_forest · (0, 0.5; offset 0.375) warped_forest · (−0.5, 0; offset 0.175) basalt_deltas. Выбор — по тому же «ближайшему соседу».

### 7.2. End — 48 бит seed
* Нет климата: биом определяет функция **высоты островов** (`erosion = end/islands`). Источник — `SimplexNoise` из `LegacyRandomSource(seed)` после `consumeCount(17292)` (2D, без смещений).
* Правило биома (`TheEndBiomeSource`): если `chunkX² + chunkZ² ≤ 4096` (радиус 64 чанка = 1024 блока) → **the_end** (главный остров). Иначе берётся значение функции в центре «секции» `((chunk·2+1)·8)` и: `> 0.25` → end_highlands; `≥ −0.0625` → end_midlands; `< −0.21875` → small_end_islands; иначе end_barrens.
* Сама функция: «острова» существуют там, где `simplex(chunkX, chunkZ) < −0.9` (и чанк дальше 64 от центра); размер острова `(|cx|·3439 + |cz|·147) mod 13 + 9`; высота падает с расстоянием `100 − √(xd² + zd²)·размер`, берётся максимум по соседним (±12 чанков) островам. **[В]** на 69 120 точках × 3 версии.
* Шипы-башни и дракон — отдельные потребители seed (таблица §2, строка I).

---

## 8. «Зум» биомов: от кварта к блоку (`BiomeManager`)

Климатическая сетка — 4×4×4 блока, но биом нужен для каждого блока. `BiomeManager.getBiome(x,y,z)` (**[К]** `biome/BiomeManager.java`):
```
(abs) = (x−2, y−2, z−2);  parent = abs >> 2;  fract = (abs & 3)/4
для 8 соседних ячеек кварта: dist = |fract − угол|² + «встряска» (fiddle)
fiddle ячейки = LCG от (biomeZoomSeed, координаты ячейки): ((rval >> 24) mod 1024 /1024 − 0.5)·0.9 по каждой оси
выбирается ячейка с минимальной «встряхнутой» дистанцией → её биом
```
`biomeZoomSeed = SHA-256(seed)` — тот самый **hashed seed**, который клиент знает. Поэтому границы биомов на уровне блоков слегка «рваные» и зависят от всех 64 бит через хэш, даже в Nether/End; в центрах ячеек (блоки ≈ 2–3 внутри кварта) совпадают с «чистым» климатическим биомом.

---

## 9. Порядок генерации чанка (что за чем и от какого seed)

`ChunkStatus` (26.3: `world/level/chunk/status/ChunkStatus.java`): `empty → structure_starts → structure_references → biomes → terrain → features → initialize_light → light → spawn → full`. В 26.1/26.2 этапы `noise → surface → carvers` — отдельные статусы (проверено), в 26.3 они слиты в `terrain` (внутри `NoiseBasedChunkGenerator.buildTerrain`: **doFill → buildSurface → generateCarvers**).

| Этап | Что делает | Какие потоки случайности |
|---|---|---|
| `structure_starts` | для каждого `structure_set`: «потенциальный чанк» ← `setLargeFeatureWithSalt` (48 бит); проверка биома; расстановка блоков-«кусков» | Legacy LCG от seed (структуры) + `WorldgenRandom` на старт (`setLargeFeatureSeed`) |
| `structure_references` | каждый чанк запоминает, какие старты рядом (радиус 8 чанков) | — |
| `biomes` | заполняет 4×4×4-ячейки биомами по климату | климат: Xoroshiro-шумы (64 бита) |
| `terrain` → `doFill` | по `final_density` ставит камень/воздух/жидкость (§10–11) | шумы (64 бита); aquifer: `positional "aquifer"` |
| `terrain` → `buildSurface` | трава/песок/глина… по правилам (§12) | шумы `surface`, `clay_bands`… |
| `terrain` → `generateCarvers` | классические пещеры/каньоны поверх (§13) | `setLargeFeatureSeed(seed+index, cx, cz)` (48 бит) |
| `features` | структуры-«куски» + фичи по шагам (§15) | `setDecorationSeed` / `setFeatureSeed` (64 бита) |
| `spawn` | начальные мобы | — |

**Важно:** этапы идут по порядку, и каждый следующий **читает то, что оставили предыдущие** (блоки, высоты, биомы). Именно здесь возникают «взаимосвязи» (§16).

---

## 10. Рельеф: из климата — в «плотность»

Для блока (x, y, z) считается число `density`; **`density > 0` ⇒ твёрдый (камень), `≤ 0` ⇒ воздух/жидкость**. **[К]** `noise_settings/overworld.json → noise_router.final_density` (JSON 26.3, 26.1/26.2 — тот же смысл в `NoiseRouterData`).

```
final_density = min( squeeze( 0.64 · blend_density( LERP_Y ) ), noodle ) + beardifier
squeeze(c)    = c/2 − c³/24, c = clamp(·, −1, 1)                              // сглаживание
LERP_Y        = lerp( gradient_y(−64 → 0, −40 → 1),  0.1171875,               // у дна мира (y ≤ −64) → 0.117 (твёрдо)
                      lerp( gradient_y(240 → 1, 256 → 0),  −0.078125,         // выше y = 256 → −0.078 (воздух)
                            CAVE_FIELD ))
CAVE_FIELD    = range_choice( sloped_cheese ∈ (−∞, 1.5625) ?
                   min(sloped_cheese, 5·entrances)                               // вблизи поверхности
                 : max( min( min( 4·cave_layer² + clamp(cave_cheese + 0.27, −1, 1) + clamp(1.5 − 0.64·sloped_cheese, 0, 0.5),
                                  entrances ),
                             spaghetti_2d + spaghetti_roughness ),
                        pillars (отсекая значения < 0.03) ) )                    // глубоко: пещеры
sloped_cheese = 4 · quarter_negative( (depth + jaggedness · half_negative(jagged_noise)) · factor ) + base_3d_noise
quarter_negative(x) = x ≥ 0 ? x : 0.25·x;   half_negative(x) = x ≥ 0 ? x : 0.5·x      // «дырявый ReLU»
```
Смысл частей:
* **depth** (из климата, §5): `depth = (128 − y)/128 + offset`. Поэтому **высота поверхности ≈ `128 + 128·offset`** (там, где depth = 0). Для `offset = −0.50375 + spline(...)`: сплайн ≈ 0 даёт y ≈ 63.5 (уровень моря), сплайн −0.22 (океан) — y ≈ 35, сплайн > 0 (суша/горы) — выше. Это и есть «если значение больше — то выше».
* **factor** (круче/площе, сплайн от C, E, PV): масштабирует `depth` — большой factor делает склоны резкими.
* **jaggedness · jagged_noise** (`noise jagged`, масштаб ×1500 по xz): добавляет «зубцы» только там, где сплайн включает jaggedness (пики).
* **base_3d_noise** (`old_blended_noise`, из `random.fromHashOf("terrain")`): три стека октав «min limit», «max limit», «main»; `lerp(clamp(main/… + 0.5, 0, 1), minLimit, maxLimit)` — объёмный шум 3D, даёт нависающие скалы и неровности.
* **Асимметрия** `quarter_negative`: под поверхностью (положительный аргумент) плотность растёт быстро, над поверхностью (отрицательный) — в 4 раза медленнее ⇒ мягкие склоны.
* **Интерполяция:** `interpolated(cell_size_xz = 4, cell_size_y = 8)` — плотность считается **только в углах ячеек 4×8×4 блока** и интерполируется трилинейно. Отсюда «ячеистость» рельефа и то, почему шумы пещер меньше 4–8 блоков не видны.
* **beardifier:** прибавка плотности вокруг структур с `terrain_adaptation` (деревни «приглаживают» землю, города/муниципальные структуры «закапываются»).

## 11. Заполнение блоков: жидкости, бедрок, глубинный сланец, жилы

`doFill` для каждого блока (сверху вниз) берёт `density` и спрашивает **aquifer** (`Aquifer.computeSubstance`): `density > 0` → камень; иначе → воздух, вода или лава.

### 11.1. Aquifer (подземные воды и лава) **[К]** `levelgen/Aquifer.java`
* Глобально: `y < min(−54, sea_level)` → **лава**, иначе вода до `sea_level = 63`; выше `skipSamplingAboveY` всё просто «глобальная жидкость».
* Подземные «карманы»: сетка ячеек **16 × 12 × 16**; в каждой ячейке по `positional("aquifer").at(gx, gy, gz)` выбирается случайная точка (смещения `nextInt(10)`, `nextInt(9)`, `nextInt(10)`). Для блока берутся 4 ближайшие точки; уровни жидкости соседних ячеек сравниваются, а шум `aquifer_barrier` решает, ставить ли **стенку** (камень) между разными уровнями.
* Уровень жидкости ячейки: если `exclusion` > 0 (зона глубокой тьмы: `min(−0.225 − erosion, max(depth − 0.9, 0))`) — «сухо»; иначе по `aquifer_fluid_level_floodedness`: порог «полностью затоплено» — от −0.3 у самой поверхности до +0.8 на глубине ≥ 64 блоков под ней (чем глубже, тем труднее затопить; поправка действует, только если поверхность в центре ячейки ниже глобального уровня жидкости, т. е. в океанических районах) — либо полный затоп до уровня моря, либо «частично» — случайный уровень в ячейке 16×40×16: `средина ячейки + 10·noise(fluid_level_spread)` (квантуется по 3), ограничен самой низкой поверхностью.
* **Лава в aquifer:** если уровень жидкости ячейки `≤ −10` и модуль `aquifer_lava` (ячейки 64×40×64) `> 0.3` → ячейка заполняется **лавой**, иначе водой.

### 11.2. Правила материала (`material_rule/overworld.json`, 26.3; в 26.1/26.2 — `SurfaceRules` + `OreVeinifier`) **[К]**
Порядок проверки для каждого блока: **(1)** `bedrock_floor`: `vertical_gradient` от `above_bottom 0` до `5` с `random_name "bedrock_floor"` — на y = −64 бедрок всегда, к y = −59 вероятность падает до 0 (`random.at(x,y,z).nextFloat() < p`); **(2)** жилы меди и железа (`ore_vein`); **(3)** над предварительной поверхностью — правила поверхности (§12); **(4)** под поверхностью: `deepslate` по `vertical_gradient(true ≤ y 0, false ≥ y 8, random "deepslate")` (т. е. граница камень/глубинный сланец размыта на высотах 0…8), а в `sulfur_caves` ещё полосы серы.

### 11.3. Жилы руды (big ore veins) **[К]** `density_function/overworld/ore_vein/*`
* Шум `ore_veininess` задаёт «переключатель» (`toggle`), активный на y ∈ [−64, 57). **Знак** переключателя выбирает тип: `toggle > 0` и y ∈ [0, 50) — **медная** жила (granite-«заполнитель», `raw_copper_block`), `toggle < 0` и y ∈ [−60, −8) — **железная** (tuff, `raw_iron_block`, `deepslate_iron_ore`).
* Жила существует, только если `|toggle| ≥ 0.4` (иначе `mask = −1`), и «густота» растёт с `|toggle|` (richness 0.1 → 0.3). Шум `ore_gap` прорезает в жиле «пустоты» (заполнитель вместо руды), а `raw_ore_chance = 0.02` — шанс сырого блока.

---

## 12. Поверхность: «если биом X и на поверхности — то блок Y»

После `doFill` для каждой колонки (x, z) запускаются **правила поверхности** (26.3 `MaterialSystem`/`material_rule`, 26.1/26.2 `SurfaceSystem`/`SurfaceRules`). **[К]**
* **Глубина поверхностного слоя:** `surface_depth = (int)( surface_noise(x, 0, z)·2.75 + 3.0 + random.at(x, 0, z).nextDouble()·0.25 )` — то есть 3 ± ~3 блока (шум `surface`).
* Условия правил: `on_floor` (верхний блок колонки), `under_floor` (в пределах `surface_depth` под ним), `stone_depth`, `y_above/y_below (anchor, surface_depth_multiplier)`, `water` (над уровнем воды/под водой), `biome in […]`, `noise_threshold (noise, min, max)`, `vertical_gradient (random_name)`, `steep` (крутизна), `hole`, `above_preliminary_surface`.
* Корень `overworld/surface` (порядок = приоритет): (a) «зелёные» биомы у пиков/склонов — свои подправила (`frozen_peaks`, `snowy_slopes`, `jagged_peaks`, `grove`, `stony_peaks`, `stony_shore`, `windswept_*`); (b) пляжи/пустыни (`warm_ocean, beach, snowy_beach, desert`) — **песок, а под ним песчаник** (`sand_or_sandstone_if_ceiling`); (c) пещерные биомы (`dripstone_caves`, `sulfur_caves`), `mangrove_swamp` (грязь), `ice_spikes`, `mushroom_fields` (мицелий), `old_growth_*_taiga` (подзол), `dappled_forest` (26.3) …; (d) по умолчанию `default` (**проверено по JSON**): `not_underwater` → **трава**, иначе → **земля**; под верхним блоком (в пределах `surface_depth`) — **земля**. Песок, глина и гравий на дне рек и океанов в эту цепочку **не входят** — они приходят позже как *фичи* `disk_sand` / `disk_clay` / `disk_gravel` (шаг `underground_ores`, §15), поэтому у реки «глинистое дно» — результат отдельной фичи, а не правил поверхности.
* **Бэдленды (terracotta):** 192 «полосы» `clay_bands`, заполняются терракотой и цветными слоями случайно (`positional "clay_bands"`), смещаются шумом `clay_bands_offset`; слой выбирается по `y + offset`. Поэтому бэдленды выглядят слоистыми.
* **Бедрок-пол/потолок и глубинный сланец** — тоже правила материала (§11.2).
* **Биом в правилах — зумленный** (§8): `buildSurface` получает `BiomeManager`, поэтому границы песка/травы повторяют «рваные» блоковые границы биомов, а не квартовые ячейки.

---

## 13. Пещеры: две независимые системы

### 13.1. «Шумовые» пещеры (внутри `final_density`, 64 бита) — этап `doFill`
Это **непрерывные поля**, а не «червяки»; блок становится воздухом там, где суммарная плотность ≤ 0 (формула в §10).
* **cheese** (огромные залы): `4·cave_layer² + clamp(cave_cheese + 0.27, −1, 1) + …`; `cave_layer` (y_scale 8) даёт «этажи» по высоте; пустота там, где шум `cave_cheese` достаточно отрицателен.
* **spaghetti_2d** (горизонтальные тоннели): `max(|n|·… + 0.083·толщина, (|elevation·8 + gradient_y| + толщина)³)` — тоннель вдоль линии пересечения «поверхности нулевого уровня» шума и «высотного коридора».
* **spaghetti_3d** (в `entrances`): `max(|n₁|, |n₂|) + порог(толщина)` — **пересечение двух нулевых поверхностей двух независимых шумов**. Пересечение двух поверхностей в 3D — это **кривая**, у которой нет концов: она либо замкнута (петля), либо уходит в бесконечность; тоннели «обрываются» только там, где модулятор толщины делает радиус нулевым. Именно из-за этого такие пещеры выглядят «закольцованными» червями.
* **noodle** («лапша»): тонкие тоннели `noodle` + два ridge-шума на y ∈ [−60, 321).
* **entrances** (`cave_entrance` шум): прорезают вход с поверхности; применяются только там, где `sloped_cheese < 1.5625` (около поверхности).
* **pillars** (колонны в залах): `(pillar·2 − pillar_rareness − 1) · cube(0.55·pillar_thickness + 0.55)`; значения `< 0.03` заменяются на −10⁶ (колонны нет).
* Эти поля — функции Xoroshiro-шумов, поэтому зависят от **64 бит** seed.

### 13.2. Классические «карверы» (Legacy, 48 бит) — этап `generateCarvers`
Конфиги `carver/*.json` перечислены в списках биомов (`"carvers": ["cave", "cave_extra_underground", "canyon"]`):

| Карвер | Вероятность старта в чанке | Высоты старта | Прочее |
|---|---|---|---|
| `cave` | 0.15 | y ∈ [bottom+8, 180] | число стартовых систем в чанке `very_biased_to_bottom` 0…14, радиусы 0.7–1.4 (гориз.) / 0.8–1.3 (верт.), `floor_level` −1…−0.4 |
| `cave_extra_underground` | 0.07 | y ≤ 47 | то же |
| `canyon` (разлом/«каньон») | 0.01 | y ∈ [10, 67] | толщина трапеция 0–6, `y_scale 3` — глубокие вертикальные щели |
| `nether_cave` (Nether) | 0.2 | y ∈ [0, top−1] | `floor_level −0.7` |

Алгоритм (`NoiseBasedChunkGenerator.generateCarvers`): для **каждого** чанка и каждого **исходного** чанка в радиусе ±8 (17×17) берутся карверы биома исходного чанка; для каждого `index`: `random.setLargeFeatureSeed(seed + index, sourceX, sourceZ)`; если `carver.isStartChunk(random)` (`nextFloat() ≤ probability`) — случайное блуждание «червяка» в пределах радиуса 8 чанков, вырезание эллипсоидов в `CarvingMask`. В конце маска применяется: блок → воздух/вода/лава через `aquifer.computeSubstance(…, 0.0)`; **лава:** в 26.1/26.2 у `cave`-карверов есть `lava_level` (`above_bottom 8` ⇒ y ≤ −56 вырезанное заливается лавой); **в 26.3 это поле и `replaceable` из конфигов карверов убраны** — жидкость решает только aquifer/глобальный уровень (лава при `y < −54`); **под вырезанной травой** земля заменяется обратно «верхним материалом» поверхности (чтобы не оставалось голой земли).
* Червяки — случайные блуждания с двумя концами (в отличие от кривых-пересечений §13.1): они ветвятся и **могут заканчиваться тупиком**.
* `UNCARVABLE` (тег блоков) не вырезается (например бедрок, структурные блоки).

---

## 14. Структуры: от региона до готовых «кусков»

### 14.1. Где структура появится (48 бит, этап `structure_starts`) **[В]**
Для `structure_set` типа `random_spread` (`spacing`, `separation`, `salt`, `spread_type`): мир разбит на **регионы** `spacing × spacing` чанков; в регионе `(gx, gz)` один «потенциальный» чанк:
```
seed' = gx·341873128712 + gz·132897987541 + seed + salt;   LCG.setSeed(seed')
limit = spacing − separation
linear:      dx = nextInt(limit);  dz = nextInt(limit)
triangular:  dx = (nextInt(limit) + nextInt(limit)) / 2;   dz = (… + …) / 2
потенциальный чанк = (gx·spacing + dx,  gz·spacing + dz)
```
Пример (деревни, spacing 34, separation 8, salt 10387312, seed 12345): регион (0,0) → чанк **(21, 5)** [В, совпало с игрой]. Дополнительные фильтры: `frequency` (редкость: `legacy_type_1/2/3` — отдельные формулы для аванпостов, кладов, шахт; `exclusion_zone` — например, аванпост не ближе 10 чанков к «потенциальной» деревне), стронгхолды — **кольца** (`concentric_rings`): угол/радиус от `LegacyRandomSource(seed)`, затем позиция сдвигается в ближайший допустимый биом (поэтому зависят ещё и от климата). Полные формулы и таблицы по всем наборам — `docs/02`.

### 14.2. Станет ли реально структурой (биом, высота) **[К]** `ChunkGenerator.createStructures`, `Structure.generate`
1. Если в наборе несколько структур (например `villages`: плains/desert/savanna/snowy/taiga) — выбор по весам: `WorldgenRandom.setLargeFeatureSeed(seed, cx, cz)`, `nextInt(Σвес)`.
2. `Structure.findValidGenerationPoint`: структура считает точку старта (часто — центр чанка + высота `getFirstOccupiedHeight(heightmap)` **по шуму рельефа, без учёта пещер/карверов**), затем `isValidBiome`: **биом в точке старта должен входить в тег `biomes` структуры** (биом берётся из климата на (x>>2, y>>2, z>>2)). Нет подходящего биома — старт отменяется (и позиция «пропадает»).
3. Для «jigsaw»-структур (деревни, аванпосты, бастионы, трайал-чемберы, древние города…) куски выбираются из `template_pool` случайным обходом (`WorldgenRandom` на чанк), с ограничениями `max_distance_from_center`, `project_start_to_heightmap`, `terrain_adaptation`.
4. Результат — `StructureStart` (список кусков с bounding box) записывается в чанк; `structure_references` раздаёт ссылки соседям (радиус 8).

### 14.3. Когда блоки ставятся и как структура влияет на рельеф
* **До рельефа:** `terrain_adaptation` (`none / beard_thin / beard_box / bury / encapsulate`) через **beardifier** добавляет плотность вокруг структуры — земля «подстраивается». По данным 26.3: `ancient_city` — `beard_box`; `trial_chambers` — `encapsulate`; `stronghold`, `trail_ruins` — `bury`; `nether_fossil` и поверхностные (в т. ч. 24 варианта `abandoned_camp`) — `beard_thin`; шахты, пирамиды, End City и др. — `none`.
* **Во время `features`:** структуры ставятся **внутри цикла шагов декорации** — в своём шаге (шаг задаёт `Structure.step()`; по JSON 26.3: `surface_structures` — деревни, пирамиды, иглу, особняки, End City, бастион, abandoned_camp, **стронгхолд**; `underground_structures` — шахты, клады, trial chambers, trail ruins; `underground_decoration` — древний город, крепость Nether, nether fossil) **перед фичами того же шага** (`applyBiomeDecoration`, см. §15). Поэтому, например, шахты (шаг 3) ставятся **до** руд (шаг 6), а деревни (шаг 4) — **до** растительности (шаг 9): деревья и цветы подстраиваются под уже стоящие дома.

---

## 15. Декорации (фичи): деревья, руды, озёра и т. д. (64 бита)

### 15.1. Цикл (`ChunkGenerator.applyBiomeDecoration`, этап `features`) **[К]**
```
decSeed = setDecorationSeed(seed, minBlockX, minBlockZ)         // один на чанк (координаты угла чанка)
possibleBiomes = биомы в окне 3×3 чанка вокруг (радиус 1), ∩ биомы источника
для step = 0 .. 10:   RAW_GENERATION, LAKES, LOCAL_MODIFICATIONS, UNDERGROUND_STRUCTURES, SURFACE_STRUCTURES,
                      STRONGHOLDS, UNDERGROUND_ORES, UNDERGROUND_DECORATION, FLUID_SPRINGS, VEGETAL_DECORATION, TOP_LAYER_MODIFICATION
    1) для каждой структуры этого шага:  setFeatureSeed(decSeed, index, step);  start.placeInChunk(...)
    2) features_этого_шага = объединение списков фич всех possibleBiomes (отсортировано по ГЛОБАЛЬНОМУ индексу)
       для каждой:  setFeatureSeed(decSeed, globalIndex, step);   placeWithBiomeCheck(feature, random, origin)
```
`setFeatureSeed(decSeed, index, step)` = `setSeed(decSeed + index + 10000·step)` — **у каждой фичи свой независимый поток**, определяемый (seed, чанк, номер фичи, шаг). Поэтому две фичи (например глина и алмазы) **не коррелируют по случайным числам**.

### 15.2. Глобальный индекс и «хрупкость» декораций к версии (`FeatureSorter`) **[В на дампе реального кода]**
Порядок фич в шаге — результат топологической сортировки **списков всех биомов измерения сразу**. Поэтому добавление одного биома/фичи может сдвинуть индекс (а с ним и поток случайности) у *несвязанных* фич. Реальные числа (Overworld): 26.1 → 26.2: 164 → 168 фич (+4 сульфурные), индекс изменился у **3**; 26.2 → 26.3: 168 → 171 (+`trees_dappled_forest`, `brown_mushroom_dappled_forest`, `patch_red_shrub`), индекс изменился у **101 из 168**. Для Nether и End индексы в 26.1–26.3 не менялись. Вывод: **в 26.3 расположение деревьев/руд/цветов при том же seed отличается от 26.2 почти везде**, хотя рельеф и биомы почти те же.

### 15.3. Конвейер одной фичи (`PlacedFeature`: цепочка модификаторов, потом сама фича)
Модификаторы по очереди превращают «исходную позицию» (угол чанка) в поток позиций:
`count(n)` (n копий; n бывает случайным: `uniform/weighted_list/very_biased_to_bottom`) → `rarity_filter(1/chance)` → `in_square` (случайные x, z внутри чанка: `nextInt(16)`) → `height_range` (y: `uniform` / `trapezoid` / `biased_to_bottom`) **или** `heightmap` (y = высота поверхности: `WORLD_SURFACE_WG`, `OCEAN_FLOOR_WG`, …) → `environment_scan` (идти вверх/вниз до условия) → `block_predicate_filter` (`would_survive`, `matching_blocks`, `replaceable`, `solid` — **читает уже стоящие блоки!**) → `surface_water_depth_filter` / `surface_relative_threshold_filter` → `biome` (биом в точке должен содержать эту фичу). Затем сама фича (`ore`, `tree`, `lake`, `geode`, `disk`, `random_patch`, `root_system`, …) пишет блоки.

Примеры из данных (26.3):
* **Железо верхнее** `ore_iron_upper`: `count 90` → `in_square` → `height_range trapezoid [y=80 … y=384]` → `biome`; сама руда — `size 9`. То есть «чем выше 80, тем…» — только там и растёт вероятность (трапеция: максимум в середине 80…384).
* **Деревья равнин** `trees_plains`: `count` (0 с весом 19 или 1 с весом 1) → `in_square` → `surface_water_depth_filter(0)` → `heightmap OCEAN_FLOOR` → **`would_survive(oak_sapling)`** (проверка, что саженец выживет: под ногами земля/трава, место свободно) → `biome`.
* **Лавовое озеро поверхностное** `lake_lava_surface`: `rarity 1/200` → `in_square` → `heightmap WORLD_SURFACE_WG` → `biome`. **Подземное** `lake_lava_underground`: `rarity 1/9` → `in_square` → `height_range uniform [0 … top]` → `environment_scan(вниз, 32 шага, цель: не воздух и внутри мира)` → `surface_relative_threshold_filter(OCEAN_FLOOR_WG ≤ −5)` (озеро должно быть не менее чем на 5 ниже поверхности) → `biome`.
* **Корневая азалия** `rooted_azalea_tree` (биом `lush_caves`, шаг 9): `count 1–2` → `in_square` → `height_range [bottom … 256]` → `environment_scan(вверх до 12 шагов; путь — воздух; цель — твёрдый блок)` → `offset(y −1)` → `biome`.

### 15.4. Руды Overworld — таблица «если высота …» (из `placed_feature/ore_*`, 26.3)
`size` — число блоков в жиле; `воздух` — `discard_chance_on_air_exposure` (вероятность **отбросить** блок руды, если он касается воздуха; 1.0 — «никогда не видна у пещеры»).

| Фича | Сколько | Высоты | size | воздух |
|---|---|---|---|---|
| `ore_coal_upper` | ×30 | равномерно y 136…верх | 17 | 0 |
| `ore_coal_lower` | ×20 | трапеция y 0…192 | 17 | 0.5 |
| `ore_iron_upper` | ×90 | трапеция y 80…384 | 9 | 0 |
| `ore_iron_middle` | ×10 | трапеция y −24…56 | 9 | 0 |
| `ore_iron_small` | ×10 | равномерно низ…72 | 4 | 0 |
| `ore_copper` / `_large` | ×16 | трапеция y −16…112 | 10 / 20 | 0 |
| `ore_gold` | ×4 | трапеция y −64…32 | 9 | 0.5 |
| `ore_gold_lower` | 0–1 | равномерно y −64…−48 | 9 | 0.5 |
| `ore_gold_extra` (бэдленды) | ×50 | равномерно y 32…256 | 9 | 0 |
| `ore_redstone` / `_lower` | ×4 / ×8 | низ…15 / трапеция низ−32…низ+32 | 8 | 0 |
| `ore_lapis` / `_buried` | ×2 / ×4 | трапеция −32…32 / низ…64 | 7 | 0 / 1.0 |
| `ore_emerald` (горы) | ×100 | трапеция y −16…480 | 3 | 0 |
| **`ore_diamond`** | ×7 | трапеция «низ−80…низ+80» (≈ y −64…16, пик у дна) | 4 | **0.5** |
| `ore_diamond_medium` | ×2 | равномерно y −64…−4 | 8 | 0.5 |
| `ore_diamond_large` | 1/9 чанков | трапеция «низ−80…низ+80» | 12 | **0.7** |
| `ore_diamond_buried` | ×4 | трапеция «низ−80…низ+80» | 8 | **1.0** |
| `ore_clay` | ×46 | равномерно низ…256 | 33 | 0 |
| `ore_dirt`, `ore_gravel`, гранит/диорит/андезит (верх/низ), туф, `ore_infested` | см. файлы | | 33–64 | 0 |

Механика «воздух»: в `AbstractOreFeature` блок руды ставится, если `random.nextFloat() ≥ discard` **или** он **не граничит с воздухом** (проверяются только блоки-`air`, вода/лава не считаются). **Это реальная связь пещер и руд:** карверы и шумовые пещеры вырезаются *до* фич, значит руда, вскрытая пещерой, отбрасывается с вероятностью 0.5/0.7 (алмаз), 1.0 (`buried`, `lapis_buried`) — поэтому алмазы у стен пещер реже, чем «внутри» массива. **[В на настоящем сервере, 6561 чанк]:** алмазы касаются воздуха в 0.97 % случаев против 2.94 % у обычных твёрдых блоков того же слоя (отношение 0.33).

### 15.5. Высота, температура и уровни: правила «если … то» (не только руды)

> **Полный автоматический перечень** всех правил «если y / температура / вода / карта высот» по **всем 273 размещаемым фичам**, развёрнутым правилам поверхности (`material_rule`), y-узлам density-функций, карверам, структурам и **всем биомам** (с высотой снежной линии) — `docs/08-height-temperature-rules.md` (генерируется `tools/audit_height_rules.py`, данные `data/height-rules-26.3.json`). Ниже — отобранные и объяснённые механики.

**Уровни жидкостей (константы из `noise_settings`) [В на настоящих чанках]**

| Измерение | `sea_level` | Чем заполнено ниже | Проверка |
|---|---|---|---|
| Overworld | **63** (вода занимает блоки y ≤ 62, поверхность на 63) | вода; глубже `y < −54` — **лава** (`createFluidPicker`: `y < min(−54, sea_level)`) | верхний блок воды в 97.9 % колонок с водой — y = 62 (остальные — пещерные/aquifer-карманы) |
| Nether | **32** (лава y ≤ 31) | **лава** (`default_fluid = lava`) | верхний уровень лавы в колонке: y = 31 в 51 % колонок, 29.7 % колонок без лавы (рельеф выше 32) |
| End | 0 (жидкости нет) | — | — |

**Температура → биом (шкала `OverworldBiomeBuilder`; T — значение климата, не градусы)**

| T | Класс | Средний биом при низкой/высокой влажности (`MIDDLE_BIOMES[T][H]`) |
|---|---|---|
| −1 … −0.45 | снежный | snowy_plains → snowy_taiga → taiga |
| −0.45 … −0.15 | холодный | plains / forest → taiga → old_growth_spruce_taiga |
| −0.15 … 0.2 | умеренный | flower_forest / plains → forest → birch_forest → dark_forest |
| 0.2 … 0.55 | тёплый | savanna → forest → jungle |
| **0.55 … 1** | **жаркий** | **desert** (при любой влажности); в склонах и пиках — badlands / eroded / wooded badlands |

Правило «если жарко — пустыня» верно именно для **средних** (равнинных) биомов, то есть при среднем рельефе (эрозия 2–4). Рядом с побережьем та же жара даёт `desert` вместо пляжа (`pickBeachBiome`: T = 4 → desert), а на плато/пиках — бэдленды.

**Снежная линия — два разных механизма**

1. **Снег «по биому» (без учёта высоты).** В правилах поверхности `frozen_peaks`, `snowy_slopes`, `jagged_peaks`, `grove` нет условия по y: блок ставится везде, где колонка не под водой — `snow_block` (в `snowy_slopes`/`grove` ещё островки `powder_snow` по шуму `powder_snow ∈ [0.35; 0.6]`; в `frozen_peaks` — `packed_ice` на крутых местах и по шуму, `ice` по шуму). Поэтому снег начинается там, где начинается **биом**: границу задают климат (эрозия 0–1, пик/склон, холодная T), а не абсолютная высота. Эмпирика (6561 чанк): снег на верхнем блоке в `jagged_peaks` 96–100 %, `grove` 90–98 %, `snowy_taiga` 82–94 % **на всех высотах биома начиная с самого низа** (y ≈ 56–80), в `frozen_peaks` 41–78 % (остальное — лёд).
2. **Высотная снежная линия для остальных биомов** (фича `freeze_top_layer`, шаг 10, `SnowAndFreezeFeature` + `Biome.shouldSnow/shouldFreeze`). Температура на высоте y (`Biome.getHeightAdjustedTemperature`): при `y > sea_level + 17 = 80`:
   `T_adj = T_base − (v + y − 80) · 0.05 / 40`, `v = 8 · SimplexNoise(x/8, z/8)` (шум с **константным** seed 1234 — не зависит от мира; v ≈ ±8).
   Идёт снег (осадки — снег), если `T_adj < 0.15`. Отсюда **порог высоты** `y_снег ≈ 80 + 800 · (T_base − 0.15) − v`; при `T_base < 0.15` снег **на любой высоте**; если порог выше 320 — снега нет вообще. Для биомов 26.3:
   * `T_base < 0.15` (снег везде): frozen_peaks −0.7, jagged_peaks −0.7, snowy_taiga −0.5, snowy_slopes −0.3, grove −0.2, frozen_ocean/frozen_river/ice_spikes/snowy_plains 0.0, snowy_beach 0.05;
   * windswept_hills / windswept_forest / windswept_gravelly_hills / stony_shore (0.2): **≈ 120 ± 8**; taiga / old_growth_spruce_taiga (0.25): **≈ 160**; old_growth_pine_taiga (0.3): **≈ 200**; cherry_grove, meadow, ocean, river, lush_caves (0.5): ≈ 360 (т. е. не бывает); birch (0.6): 440; forest/dark_forest/pale_garden (0.7): 520; plains/swamp/beach (0.8): 600.
   * Эмпирика: `stony_shore` (0.2) — снег на верхнем блоке начинается с y ≈ 112 (2 % на 104, 19 % на 112) при предсказанных 120 ✔; в `old_growth_pine_taiga` (0.3) предсказано 200, но снег 1–12 % встречается уже с y ≈ 112–136 — это **утечка из соседнего снежного биома через зум биомов** (`SnowAndFreezeFeature` берёт блоковый биом `BiomeManager`, т. е. с «рваными» границами, §8).
   * `temperature_modifier: frozen` (только `frozen_ocean`, `deep_frozen_ocean`) — свой шум льда (`FROZEN_TEMPERATURE_NOISE`, seeds 3456/2345), поэтому лёд на океане «пятнами».
   * Лёд на воде: `shouldFreeze` — вода на поверхности в холодном по `T_adj` месте замерзает (`ice`), если рядом нет воды/освещённого блока.

**Высотные правила не-рудных фич** (из `placed_feature`, 26.3; «низ/верх» — `above_bottom` / `below_top`)

| Фича | Частота | Высота / условие |
|---|---|---|
| `amethyst_geode` | 1/24 чанков | равномерно от низ+6 до y=30 |
| `monster_room` / `_deep` | ×10 / ×4 | y ∈ [0; верх] / [низ+6; −1] |
| `fossil_upper` / `_lower` | 1/64 | y ∈ [0; верх] / [низ; −8] |
| `lake_lava_underground` | 1/9 | y ∈ [0; верх]; вниз ≤ 32 блоков до непустого; поверхность на ≥ 5 выше |
| `lake_lava_surface` | 1/200 | на карте `WORLD_SURFACE_WG` |
| `blue_ice` | — | y ∈ [30; 61] (в океанах/лёд) |
| `end_island_decorated` | — | y ∈ [55; 70] |
| `cave_vines`, `lush_caves_*`, `dripstone_*`, `large_dripstone`, `pointed_dripstone`, `sculk_*`, `glow_lichen` | много | y ∈ [низ; 256], привязка `environment_scan` вверх/вниз ≤ 12 блоков (к потолку/полу пещеры) |
| `glow_lichen` | — | дополнительно ≥ 13 блоков ниже поверхности (`surface_relative_threshold_filter ≤ −13`) |
| трава/цветы/деревья/грибы/морская трава/келп | — | по карте высот `MOTION_BLOCKING` / `OCEAN_FLOOR` / `WORLD_SURFACE_WG` (то есть **на поверхности**, а не по абсолютному y); у морской травы и келпа — `OCEAN_FLOOR` под водой |
| `desert_well` | 1/1000 | `MOTION_BLOCKING` (на поверхности) |
| `ice_spike`, `ice_patch`, `forest_rock` | ×3/×2/×2 | `MOTION_BLOCKING` |
| `disk_sand` ×3 / `disk_gravel` / `disk_clay` / `disk_grass` | — | `OCEAN_FLOOR_WG`, только где над блоком вода (песок/глина/гравий на дне) |

Общий принцип: **поверхностные** фичи привязаны к *карте высот* (где земля на этой колонке), **подземные** — к *абсолютному y* и/или к «потолку/полу» через `environment_scan`. Поэтому в горах деревья растут выше (карта высот выше), а, например, жеоды и данжи всегда в диапазоне y около 0…30.

**Nether (особенности высоты)**
* Рельеф: `final_density` Nether = `lerp(gradient(−8 → 0, 24 → 1), 2.5, lerp(gradient(104 → 1, 128 → 0), 0.9375, nether/base_3d_noise))`: у дна (y < −8) и под потолком (y > 128) плотность задаёт «пол/крышу», между ними объёмный шум `old_blended_noise`; плотность `> 0` — нижняя часть скал → лава ниже y = 32.
* Пещеры: карвер `nether_cave` (вероятность 0.2, y ∈ [0; верх−1]). Тоннель начинается эллипсоидом, у которого **вертикальный радиус в 5 раз больше горизонтального** (26.2: `NetherWorldCarver.getYScale() = 5.0`; 26.3: параметр `start_vertical_radius_multiplier = 5.0`), до «точки разделения» (`split`), после чего две ветви идут с множителем 1.0 (обычные горизонтальные тоннели). Горизонтальный радиус `1.5 + sin(π·шаг/длина)·толщина`, толщина — трапеция 0…6 (плато 2): отсюда у начала тоннелей **вертикальные «пустые столбы» высотой до десятков блоков** (при радиусе 6 — до ~60). Комнаты (`room_vertical_radius_multiplier 0.5`) — сплюснутые.
* Лава: помимо «моря» на 32 — фичи `spring_open` (×8, y ∈ [низ+4; верх−4]), `spring_closed` (×16, y ∈ [низ+10; верх−10]), `spring_lava` (шаг 9) и «дельта» (`delta` — лавовые лужи 3…7 блоков с рамкой из магмы, только в `basalt_deltas`, `count_on_every_layer 40`). Эмпирика (1681 чанк Nether): лава выше y = 33 найдена в warped_forest 5602, basalt_deltas 3267, nether_wastes 2523, crimson_forest 2045 блоков; верхнее «море» заполнено до y = 31.

---

## 16. Взаимосвязи между подсистемами (что подтверждено по коду)

Здесь важно различать **три вида связи**: (1) *общие шумы/функции* (одна и та же функция читается разными потребителями), (2) *порядок «чтение-после-записи»* (позже идущая стадия читает блоки/высоты/биомы, оставленные ранней), (3) *зависимость от общего seed без связи по значениям* (потоки независимы). Случайные числа разных подсистем **не коррелируют** — любая видимая «связь» объясняется (1) или (2).

| Связь | Тип | Что именно происходит (подтверждено) |
|---|---|---|
| Климат → рельеф → поверхность → фичи → структуры | (1)+(2) | один и тот же `continents/erosion/ridges` определяет биом (§6) и форму земли (`offset`, `factor`, §10); поэтому «горный» биом всегда на возвышенности |
| `preliminary_surface_level` | (1) | одна функция высоты читается aquifer'ом (уровень воды, `skipSamplingAboveY`), правилами материала (`above_preliminary_surface`) |
| Пещеры ↔ вода/лава | (2) | `final_density` создаёт полость; `aquifer.computeSubstance` решает, чем её заполнить: воздух / вода / лава (§11.1); карверы заполняются тем же aquifer'ом |
| Пещеры ↔ руды | (2) | вскрытие воздухом → `discard_chance_on_air_exposure` (§15.4) |
| Карверы ↔ трава | (2) | `applyCarvingMask`: если вырезали траву, землю под ней восстанавливают «верхним материалом» (чтобы не было голой земли) |
| Структуры ↔ рельеф | (2) | beardifier меняет плотность вокруг уже запланированных структур (§14.3) |
| Структуры ↔ фичи | (2) | структуры ставятся в своём шаге перед фичами шага; фичи (`would_survive`, `matching_blocks`) видят их блоки |
| Озёра лавы ↔ поверхность | (2) | подземное озеро требует ≥ 5 блоков до поверхности (`surface_relative_threshold_filter`); на поверхности — 1/200 |
| Азалия ↔ лаш-пещера | (2)+биом | `rooted_azalea_tree` — фича биома `lush_caves` (подземный биом с H ≥ 0.7, depth 0.2–0.9). Старт — потолок пещеры (поиск вверх ≤ 12 блоков), корни (`rooted_dirt`, до 100 блоков) поднимаются вверх, дерево — наверху. Поэтому под азалией с корнями **всегда** есть пещера — она и есть источник. Цветущие листья **не обязательны** (`azalea_leaves : flowering_azalea_leaves = 3 : 1`). Эмпирика: 28/28 корневых систем имеют пещеру или лаш-признаки под нижним слоем; обратное неверно — многие системы остаются без дерева |
| Версия ↔ декорации | (3) | индексы фич в `FeatureSorter` (§15.2): 26.3 сдвигает индекс у 101 из 168 фич |
| Нижний/верхний уровень ↔ seed | (3) | таблица §2: Nether/End/структуры/карверы — 48 бит; климат/фичи — 64 бита |

### Систематический аудит (чтобы ничего не упустить)
Список выше — только то, что я проверил руками. Полный и воспроизводимый перебор связей — `docs/06-interactions.md` (скрипты `tools/audit_interactions.py`, `tools/audit_code_reads.py`, `tools/audit_report.py`): для каждой фичи из данных извлекаются читаемые/записываемые блоки и карты высот, по глобальному порядку строятся «ранняя пишет → поздняя читает», собираются общие шумы и density-функции, структуры (шаг/адаптация рельефа), карверы, а Java-код фич просканирован на чтения окружения/записи/использование seed. Из него следуют, в частности: 47 фич ставятся по «живой» карте высот `MOTION_BLOCKING` и 37 по `OCEAN_FLOOR` (то есть видят результат более ранних фич), 215 из 279 потенциальных связей — перезапись жил камня/руд друг другом, цепочка лаш-пещер «глина/мох → растительность → азалия», сульфурная цепочка (26.2+).

### Реки (проверено на карте и сверено с chunkbase)
**Реки действительно «закольцованы»: у них нет истоков.** Причина — в правилах биомов (`OverworldBiomeBuilder.addValleys`): `river` / `frozen_river` — это биом **узкой полосы `weirdness ∈ [−0.05; 0.05]`** (срез «долина»), то есть полоса вдоль **линии нулевого уровня 2D-шума `ridge`**. Линия уровня непрерывной функции на плоскости не имеет концов: она либо замыкается в петлю, либо уходит в бесконечность. Реально «обрывается» река лишь там, где другой биом перекрывает полосу — **океан** (континентальность ниже −0.19), **болото/мангры** (эрозия 6 в глубине суши) или каменный берег. Поэтому реки образуют сеть, впадающую в океаны, и кольца вокруг возвышенностей.
Проверка на seed 12345 (26.3, наш движок, окно 6144×6144 блоков): 192 речных компонента; в 81 крупном (≥ 200 клеток) найдено **120 замкнутых петель** (дыр в маске реки), 14 компонентов не касаются ни океана, ни болота, ни края карты (то есть замкнуты сами в себя); остальные упираются в океан (54) или болото (11) либо уходят за край окна (17). Картинка и сравнение с chunkbase для того же сида: `docs/img/rivers-compare-12345.png`; скрипт — `tools/l3_rivers.py`.

### Что я проверил из того, что вы вспомнили, и что — нет
* **«Пещеры-петли».** По коду: шумовые тоннели (`spaghetti_3d`) — пересечение двух нулевых поверхностей ⇒ кривые без концов (петли/бесконечные) — §13.1; классические карверы — блуждания с концами — §13.2. *Историческое утверждение «раньше все были замкнуты» я не проверял.*
* **«Гигантские пещеры/разломы».** Это cheese-залы (шумовые, §13.1) и `canyon`-карвер (p = 0.01, y 10–67, §13.2); они живут в 26.x. Какая версия их добавила — по этим исходникам не определить.
* **«Вертикальные пустые столбы» (Незер).** Объяснение найдено в коде: тоннели `nether_cave` стартуют эллипсоидом с вертикальным радиусом ×5 (§15.5, раздел «Nether»), то есть оставляют высокие вертикальные полости; затем тоннель разветвляется на обычные горизонтальные. Это старая особенность (26.2: `NetherWorldCarver.getYScale() = 5.0`), в 26.3 вынесена в данные (`start_vertical_radius_multiplier`). Прямую статистику «вертикальной вытянутости» по 1681 чанкам Nether я снял, но автокорреляция твёрдости не показала вытянутости по y (длина корреляции y — 6 блоков против 12 по x/z) — столбы занимают малую долю объёма (полностью пустых колонок y 34…120 всего 0.46 %); если вы имели в виду другое явление, опишите, как оно выглядит.
* **Лавовые озёра и «озёра».** Overworld: в списках биомов (plains, river, lush_caves) в шаге `lakes` только `lake_lava_underground/surface` (водяных озёр нет, §15.3). Nether: «озёра» лавы — это **лавовое море** на y ≤ 31 (`sea_level 32`), плюс лавовые источники/водопады (`spring_*`) и лужи дельты (§15.5).
* **«Глина у реки → алмаз на фиксированном смещении» (Reddit «Diamonds and clay linked generation?», видео Zakviel).** В 26.x связи **нет**: на настоящем сервере (6561 чанк, seed 12345, 26.3) пар «глина → алмаз» 177 799 против 184 651 ± 4 769 в контроле (`docs/06` §10). Но в старых версиях она была **реальной и проверена на 14 настоящих серверах 1.7.10 … 1.21.11** (`docs/07`): в 1.7.10–1.12.2 связи нет; в 1.13.2 и 1.14.4 есть только по x; в **1.15.2, 1.16.5, 1.17.1 — по обеим осям** (z жилы = z центра диска + Δz, где Δz зависит от биома и от `seed & 15`); с 1.18 и в 26.x — нет. Механизм (воспроизведён ГСЧ, центры дисков угаданы 116/116, 142/142, 144/144): в 1.13–1.17.1 у каждой фичи свой `java.util.Random` с seed `decSeed + index + 10000·step`; seed'ы глины и алмаза различаются на малую константу, а LCG у близких seed'ов даёт почти равные старшие биты — x совпадает (97,7 %), z сдвинут на −11,695·δ' (mod 16). С 1.18 — Xoroshiro128++ (`WorldgenRandom` поверх `XoroshiroRandomSource`, `ChunkGenerator.applyBiomeDecoration`), корреляции нет. Подробности и таблицы смещений — `docs/09` и `docs/07`.
* **Заквиэль (Zakviel) и «глина → алмазы».** Вы описали трюк так: найти месторождение глины, отойти на N блоков, копать вниз — и находить алмазы. Для 26.3 это проверено (выше) — связи нет. Если трюк работал в более старых версиях, это проверяется тем же методом на настоящих серверах 1.7–1.21: задача запущена, результат будет в `docs/07-clay-diamond-by-version.md`. Само видео я не смотрел.

---

## 17. Итоговая «карта независимости»: что от чего зависит

```
seed(64) ─┬─ Xoroshiro ─ positional ─┬─ md5("minecraft:temperature") ─ Перлин×октавы ─┐
          │                          ├─ md5("…vegetation")        ─ …                 ├─► климат(6) ─┬► биом ─┬► фичи биома, карверы биома
          │                          ├─ md5("…continentalness/erosion/ridge/offset") ─┘              │        └► правила поверхности
          │                          ├─ md5("terrain") ─ base_3d_noise ────────────────────────────────┤
          │                          ├─ md5("aquifer"), md5("ore"), md5("clay_bands"), cave_*, spaghetti_*, noodle, pillar … ─► рельеф/пещеры
          │                          └─ (всё остальное из noise/*.json)                               └► density → камень/воздух
          ├─ seed(48) LCG ─┬─ структуры (регион → чанк) ─► старты ─► beardifier (рельеф) и этап features
          │                ├─ карверы  (seed+index, cx, cz)
          │                ├─ Nether-биомы (seed, seed+1), бедрок Nether, End-острова (consumeCount 17292)
          │                ├─ слайм-чанки, кольца стронгхолдов, шипы End (16 бит ключа)
          └─ SHA-256 ─► hashed seed (клиенту) ─► зум биомов 4×4×4→блок
decSeed(64) = f(seed, угол чанка) ─ + index + 10000·step ─► независимый поток КАЖДОЙ фичи (деревья, руды, озёра…)
```

---

## 18. Историческая справка (с оговорками)
* Алгоритмы шума в коде — **Кена Перлина**: *Perlin noise* (SIGGRAPH 1985, «An Image Synthesizer»; Оскар за технические достижения 1997), *Improved Noise* (2002; 12 градиентов «по серединам рёбер куба» и кривая `6t⁵−15t⁴+10t³` — ровно те, что в `ImprovedNoise`) и *simplex noise* (2001; используется в End и в «температурном» шуме снега). В Minecraft вместо фиксированной таблицы перестановок она **перемешивается генератором** — поэтому каждый шум «свой».
* Подход «плотность + сплайны + мульти-шумовые биомы» — переработка рельефа 1.18 (Caves & Cliffs, часть II). Кто автор и какие статьи писал — я точно не знаю; публичные разборы: блог Нотча о 3D-рельефе (2011) и видео Хенрика Книберга про генерацию 1.18 — **по памяти, источники не проверял**.

## 19. Где что лежит в коде (26.3)
| Тема | Файл |
|---|---|
| Происхождение seed | `world/level/levelgen/WorldOptions.java`, `RandomSupport.java`, `util/RandomSource.java` |
| Раздача seed по измерениям | `server/MinecraftServer.java:createLevels`, `server/level/ChunkMap.java:180` |
| RNG | `levelgen/{LegacyRandomSource,XoroshiroRandomSource,Xoroshiro128PlusPlus,WorldgenRandom,PositionalRandomFactory}.java` |
| Шум | `levelgen/synth/{GradientNoise,PerlinNoise,NormalNoise,NoiseStack,SimplexNoise,BlendedNoise}.java` |
| Климат/биомы | `biome/{Climate,OverworldBiomeBuilder,MultiNoiseBiomeSource,TheEndBiomeSource,BiomeManager}.java` |
| Рельеф | `data/.../noise_settings/*.json`, `density_function/**`, `levelgen/{NoiseBasedChunkGenerator,Aquifer,NoiseChunk}.java` |
| Поверхность/материалы | `levelgen/material/*`, `data/.../material_rule/**` (26.1/26.2: `SurfaceRules.java`, `SurfaceSystem.java`) |
| Карверы | `levelgen/carver/*`, `data/.../carver/*.json` |
| Структуры | `levelgen/structure/**`, `chunk/ChunkGenerator.java:501+`, `structure/placement/*` |
| Фичи | `chunk/ChunkGenerator.java:351 (applyBiomeDecoration)`, `levelgen/placement/*`, `levelgen/feature/*`, `biome/FeatureSorter.java` |

## 20. Как это проверялось
`python3 tests/diff_biomes.py` (биомы всех измерений, 1.24 млн точек, 0 расхождений); `tests/unit/test_noise_*.c` (шум бит-в-бит); `oracle/run.sh <V> climate|noise|structs|pillars|hashed` (настоящий код игры); `tools/extract_*.py` (данные из jar). Разделы §10–15 о рельефе/пещерах/фичах опираются на **чтение кода и данных** (**[К]**) и числа, посчитанные скриптами по реальным JSON; прямую проверку «собрать мир и сравнить» для них ещё предстоит сделать (эмпирический слой аудита, `docs/06`).

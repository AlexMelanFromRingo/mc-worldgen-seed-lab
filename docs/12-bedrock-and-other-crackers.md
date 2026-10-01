# 12. Nether_Bedrock_Cracker и другие инструменты

## 1. Nether_Bedrock_Cracker (19MisterX98; Rust; `refs/Nether_Bedrock_Cracker`, последний коммит 2025-03-11)

### 1.1 Что именно вычисляется в игре (проверено в исходниках 26.1/26.2/26.3)
* Nether (и `caves`, `end`, `floating_islands`) имеют `legacy_random_source: true` (`src/data-26.{1,2,3}/.../noise_settings/*.json`; в 26.3 строка смещена: `nether.json:74`). Overworld/amplified/large_biomes — `false` (Xoroshiro).
* `RandomState` (26.1: `RandomState.java:36`; 26.3: `:64-66`): `random = (LEGACY ? LegacyRandomSource : Xoroshiro).newInstance(seed).forkPositional()`; `getOrCreateRandomFactory(name) = random.fromHashOf(name).forkPositional()` (26.1 `:116-117`, 26.3 `:124-125`).
* Бедрок: 26.1/26.2 — `SurfaceRules.verticalGradient("minecraft:bedrock_floor"/"bedrock_roof", ...)` (`SurfaceRules.java:735-776`); **26.3 — новая material system**: `data/minecraft/worldgen/material_rule/bedrock_floor.json` (`vertical_gradient`, `above_bottom 0..5`) и `bedrock_roof.json` (`not(vertical_gradient below_top 5..0)`), код `material/condition/VerticalGradientCondition.java` — **та же логика**: `probability = Mth.map(y, trueAtAndBelow, falseAtAndAbove, 1, 0); random = factory.at(x,y,z); return random.nextFloat() < probability`. `MaterialRuleContext.getOrCreateRandomFactory` (`:193`) делегирует в тот же `RandomState`.
* Legacy-фабрика (`LegacyRandomSource.java`): `forkPositional() = LegacyPositionalRandomFactory(nextLong())`, `fromHashOf(name) = LegacyRandomSource(name.hashCode() ^ seed)`, `at(x,y,z) = LegacyRandomSource(Mth.getSeed(x,y,z) ^ seed)`, `Mth.getSeed`: `i = x*3129871 ^ z*116129781L ^ y; i = i*i*42317861L + i*11L; return i >> 16`.
* Итого для y из слоя: `F_name = nextLong(Random(hash(name) ^ R))`, где `R = nextLong(Random(W))`; `state0(x,y,z) = ((Mth.getSeed ^ F_name) ^ 0x5DEECE66D) & (2^48-1)`; бедрок, если `nextFloat() < p(y)` (пол: y=0 всегда, 1..4 -> p = 0.8,0.6,0.4,0.2; потолок инверсия). Хэши: `"minecraft:bedrock_roof".hashCode() = 343340730`, `"minecraft:bedrock_floor" = 2042456806` (проверено Python и `GameRef`).
* **Проверка на игре**: `crack/experiments/vectors/gameref-26.{1,2,3}.txt`: 84 значения `nextFloat` бедрока (Float bits) по реальным классам `LegacyRandomSource`/`PositionalRandomFactory` — байт-в-байт одинаковы для 26.1, 26.2, 26.3 и совпадают с моей C-реализацией (`mcrand.h`: `bedrock_float`). Значит *арифметика* бедрока Nether одинакова; сама цепочка surface-rule -> factory сверена чтением кода (полный запуск генератора — задача oracle).

### 1.2 Алгоритм крекера
Файлы: `bedrock_cracker/src/{lib.rs,layer.rs,block_data.rs}`.
1. Наблюдение «блок (x,y,z) — бедрок/не бедрок» = интервал `[L,U)` на `state1 = (v*MUL+11) mod 2^48`, где `v = F ^ posHash`, `posHash = Mth.getSeed ^ 0x5DEECE66D` (`block_data.rs:hashcode`, `bounds`). `nextFloat < p` <=> `state1 < p*2^48` (с точностью до 24 бит).
2. Неизвестное — 48-битное `F` (для крыши и пола независимо: `roof_seed`, `floor_seed`; `lib.rs:12-13`). Пол и потолок связаны общим `R`: `F_floor = nextLong(Random(FLOOR_HASH ^ R))`, `F_roof = nextLong(Random(ROOF_HASH ^ R))` (`layer.rs: CrossComparison::run`). Поэтому берут **более селективную** поверхность как «primary» (`create_filter_tree`: `floor_resulting_seeds < roof_resulting_seeds`), вторую проверяют после восстановления `R`.
3. **Поиск сверху вниз по старшим битам**: старшие 36 бит F фиксируются, младшие k (<=12) неизвестны; `(F_hi ^ h)*MUL` для всех k младших бит лежит в интервале ширины `(2^k-1)*MUL` (без переполнения при k<=13, т.к. MUL≈2^34.5) => `CheckObject.check` (`block_data.rs`) отбрасывает префикс, если интервал не пересекает `[L,U)`. Слои `bits = 12..0` (`Layer.split = 2^(bits-1)`), каждый слой раскрывает по одному биту. Всего 2^36 префиксов первого слоя (`lib.rs:search_bedrock_pattern`: `start_bits = thread*2^36/threads`, `<<12`).
4. Для прошедших: `reverse_next_long(F)` (2D-решётка, `next_long_reverser::get_next_long`: по 48 младшим битам `nextLong` находит состояния LCG — 1–2 шт) -> `R`; проверка второй поверхности; далее `reverse_next_long` ещё раз -> **structure seed (48 бит)**; режим `WorldSeed` дополнительно считает `nextLong` от прообраза, т.е. предполагает, что world seed сгенерирован случайно (`Random().nextLong()`) — для ручных seed'ов неверно (README «User specified seeds»).
5. Вывод: из бедрока Nether получаем **48-битный structure seed** (полностью, единственный/парный), а не 64-битный world seed; верхние 16 бит — отдельным источником (hashed seed, декораторы, биомы).

### 1.3 Производительность (моё измерение, Rust --release, эта машина: Ryzen 5 5500, 12 потоков)
Пример из репозитория `examples/seed 765906787396911863.txt` (36 блоков: 32 на крыше y=123 и 4 на полу y=4), `bench.rs` в scratchpad поверх библиотеки:
* полный проход 2^48: **151.6 с** (wall), **623 с** CPU-времени (user); верный seed найден на 22.6 с (порядок перебора);
* на префикс (2^36 штук): ≈ 9 нс CPU.
README утверждает, что это «Gui-приложение»; GPU-версии нет (подтверждено: только Rust/`std::thread`).

### 1.4 Перенос на GPU (проектное предложение; числа — оценка)
* Ядро — цикл по 2^36 префиксов с дешёвым слоем-12: на каждый 1–3 проверки `((h^pre)*MUL + off) & MASK < cond` (≈12 целочисленных инструкций). Оценка: 6.9e10 префиксов * ~30 инстр / (~1.2e13 инстр/с RTX 4080 SUPER) ≈ 0.2–0.5 с; ветвящиеся слои ниже 12 — редкие (оценка доли прошедших: порядка 1e-3..1e-2). Итого **ожидаем 1–3 с против 152 с на CPU (оценка, не измерено)**. Реализация: поток = диапазон префиксов; внутри — стек-less DFS по 12 битам, выходы через `atomicAdd` в буфер; `reverse_next_long` — на хосте (единицы кандидатов).
* Данные пользователя: список `x y z bedrock|other` (тот же формат, что в README/examples), крыша y=123..126 и пол y=1..4; y=4 и y=123 самые информативные (p=0.2). Информация одного блока: бедрок при p=0.2 — 2.3 бит; не бедрок при p=0.2 — 0.32 бит; на y=127/0 информации нет. Для 48 бит нужно ≈ 25–30 «бедрок при p=0.2» наблюдений либо больше смешанных (подтверждается тем, что пример даёт 1 кандидат при 36 блоках).
* **Overworld бедрок (y=-64..-60) этим методом НЕ берётся**: `legacy_random_source=false` -> `XoroshiroRandomSource.forkPositional` (md5 от имени, 128-битное состояние, Stafford-mix от 64-битного seed) — нет аналога «умножения по модулю 2^48». Полный перебор 2^64 на GPU: ≈ 60–100 инструкций/кандидат -> оценка >2 лет на одной карте. Не реализуем.
* Paper < 1.19.2-213: режим `Paper1_18` (y заменяется на 0/122) — учитывать флагом.

## 2. Другие открытые инструменты
Часть 4–5 задания (chunkbase и сторонние GPU-крекеры) исследуется отдельным агентом в фоне; на момент записи файла результаты ещё не получены. Из уже проверенного в этом репозитории:

| инструмент | что делает | алгоритм | скорость (источник) |
|---|---|---|---|
| `refs/seedfinding/LattiCG` (mjtb49; 2025-11) | обращение Java `Random` по неравенствам на вызовы (`nextInt/Float/Double/Long`) | LLL + перебор точек решётки (Fincke–Pohst-подобный `Enumerate`) | в репозитории оценок нет |
| `SeedFinding/mc_reversal_java` (2023) | population/carver seed -> world seed; 2D-решётка region seed; MultiChunkHelper (2 chunk seed) | Hensel по 16 бит; Lagrange-Gauss | нет оценок |
| SeedcrackerX (док. 10) | структуры/пиллары/lifting/hashed seed | см. док. 10 | мои измерения — док. 10 |
| Nether_Bedrock_Cracker | бедрок Nether | слои по старшим битам | мои измерения выше |
| `refs/cubiomes` | биомы/структуры до 1.21.3 | док. 11 | док. 11 |

Если отчёт фонового агента появится, он будет положен в scratchpad (`web-research.md`) и может быть слит в этот раздел отдельно.

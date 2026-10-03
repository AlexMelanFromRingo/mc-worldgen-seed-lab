# Эталоны (ground truth): настоящие чанки ванильного сервера с изоляцией стадий

Поток W6. Всё измерено на серверах Mojang (`jars/server-<V>.jar`, Java 25); основная версия — 26.3.
Инструменты — `tools/gt/`, данные — `run/gt/` (вне git), результаты ворот — `docs/blender/accuracy.md`.

## 0. Быстрый старт

```bash
ls run/gt/26.3/                                         # варианты: raw veins surface carvers carve_raw features full feature_* structure_*
ls run/gt/26.3/raw/                                     # миры: <dim>-s<seed>-c<cx>_<cz>-r<R>/world (+ manifest.json)
python3 tools/gt/diff.py --ref run/gt/26.3/raw/overworld-s12345-c0_0-r10 --mcr region.mcr --mask-ext --biomes --heightmaps --list 20
python3 tools/gt/run_gate.py --gate G2|G2v|G3|G4|G4c|G5|G6|all [--profile core|land|all] [--seeds 12345] [--dims overworld] [--version 26.3]
python3 tools/gt/gen_world.py --version 26.3 --variant surface --dim overworld --seed 12345 --cx 0 --cz 0 --radius 10     # новый эталон (flock)
python3 tools/gt/gen_queue.py --version 26.3 --variants feature --plan features --group ores     # очередь «одна фича» по плану
python3 tools/gt/gen_queue.py --version 26.3 --variants structure,full --plan structures          # очередь «одна структура» + витрина full
python3 tools/gt/selftest.py                                                                       # самопроверка (20 проверок, без libmcgen)
```

`run_gate.py` вызывает `libmcgen/build/mcgen-cli` (или `--lib` — ctypes). Для G2/G4c просит `--tweak ore_veins=0`; растекание жидкостей — `--pp-margin margin-1`
и строгое сравнение (флаг `--mask-flow` включает маску вместо этого). Коды возврата: 0 — все PASS, 1 — есть FAIL, 2 — нет эталона/ошибка, 3 — нет библиотеки.

## 1. Механизм изоляции датапаком (измерено)

Датапак `<мир>/datapacks/<имя>/` с переопределениями `data/minecraft/worldgen/<реестр>/<id>.json` действует на встроенные измерения без `dimension`/`world_preset`.

* Новый мир: пак, положенный ДО первого запуска, подхватывается автоматически (`Found new data pack file/gt, loading it automatically`; `level.dat` →
  `DataPacks.Enabled=[vanilla, file/gt]`). Явное `initial-enabled-packs=vanilla,file/gt` работает так же (им пользуется `gen_world.py`).
* **Существующий мир** (опыт `run/gt/_exp/m1b`: копия ванильного мира + пак, положенный между запусками): при старте тот же автоподхват «новых» паков; чанки,
  созданные после этого, генерируются уже «сырыми» (бедрок/сланец/трава = 0), старые чанки остаются прежними. `/datapack enable` для уже включённого — «already enabled».
* `pack.mcmeta`: `{"pack": {"description": "...", "min_format": N, "max_format": N}}`, N = `data_major` из `run/pack-<V>/version.json` (26.1 — 101, 26.2 — 107, 26.3 — 121,
  26.4-snapshot-2 — 122).
* Запуск: `java -DbundlerRepoDir=<общий каталог> -jar jars/server-<V>.jar nogui` (свойство именно `bundlerRepoDir`), cwd = каталог запуска мира; `libraries/`, `versions/`
  распаковываются один раз в `run/gt/_srv/<V>/`. После `Done (`: `gamerule random_tick_speed 0` (+ `mob_griefing`, `spawn_mobs`, `advance_time`, `advance_weather` = false) и `tick freeze`:
  `/forceload` продолжает генерировать чанки, но случайных тиков и свободных потоков жидкости нет. `/forceload add` — БЛОЧНЫЕ координаты, ≤ 256 чанков на команду (окна 16x16).
* Готовность: `Status == full` во всех чанках области (чтение `.mca` после `save-all flush`); сторожок «нет прогресса 300 с» (сервер держит глобальный flock).

## 2. Стадии в данных версий (из `src/dec/<V>`)

| версия | цепочка статусов | правила |
|---|---|---|
| 26.3 | `… biomes → terrain → features → initialize_light → light → spawn → full` | `noise_settings.material_rule` (бедрок, жилы, поверхность, сланец) одним `buildTerrain`: `doFill` → правило материала → карверы; `default_block` в noise_settings |
| 26.1 / 26.2 | `… noise → surface → carvers …` | `surface_rule` в noise_settings, `ore_veins_enabled` |
| 26.4-snapshot-2 | `… noise_biomes → terrain …`; в NBT ключ `status` строчными | `material_rule`; **нет `default_block`**: заполняющий блок кладёт последний элемент правила (`block stone/netherrack/end_stone`); биомы секции хранятся ПОБЛОЧНО (4096 значений) |

Отсюда варианты (`datapack.py`): `raw` = пустое правило (26.4: единственный элемент `block <заполнитель>`), в 26.1/26.2 — пустое `surface_rule` и `ore_veins_enabled=false`;
`veins` — только `ore_vein`-правила; `surface`/`carvers`/`features`/`full` — настоящие правила + постепенное включение карверов/фич/построек.
**Жилы руды в 26.3 — часть правила материала**, а не заполнения (в `mcgen.h` они приписаны TERRAIN): поэтому G2 просит `ore_veins=0`, G2v — с жилами.

### Что в «чистом» эталоне делает не данные, а код игры

1. **Расширения биомов** `erodedBadlandsExtension` (столбы `eroded_badlands`) и `frozenOceanExtension` (`packed_ice`/`snow_block` айсбергов `frozen_ocean`, `deep_frozen_ocean`)
   выполняются при ЛЮБОМ правиле материала (проверено: 1929 `packed_ice` в raw seed −7048155917072976836 (−61,−208)). В raw/veins/carve_raw: `diff.py --mask-ext`
   (столбцы, у которых в 3x3 клетках вокруг эталон имеет эти биомы; ~2.7 % столбцов такой области). В surface и выше расширения — часть стадии.
2. **Кольца чанков.** Вокруг forceload-области r игра генерирует: r+1, r+2 — `full`; r+3 — `initialize_light`; r+4 — `terrain`; r+5 — `biomes`; дальше `structure_starts`.
   Чанки r+3, r+4 («чистые», статус ниже full) не проходили PostProcessing; `run_gate` для G2–G4 берёт дамп r+4 и сравнивает все чанки со статусом ≥ terrain
   (чистые — особенно надёжны). Для G5/G6 — только full (r+2).
3. **Растекание жидкостей.** При переходе чанка в `full` игра один раз растекает жидкость из позиций, помеченных aquifer (`PostProcessing`, даже при `tick freeze`) и каскадно
   превращает соседние клетки в источники; в чанках, ещё не начавших тикать (не все 8 соседей full), этого нет. В raw ~26–70 текущих клеток на 43 млн. libmcgen
   воспроизводит это (`--pp-margin K`), `run_gate` сравнивает строго. `diff.py` умеет и маскировать (`--mask-flow` по умолчанию: клетки `water/lava[level=1..15]` и цепочки
   расхождений «жидкость ↔ не жидкость», связанные с ними; `--flow-halo N`; `--no-mask-flow` — строго).
4. **Порядок шагов features недетерминирован.** Шаг FEATURES чанка требует соседей только на `terrain` (`blockStateWriteRadius 1`), порядок соседей зависит от планировщика.
   Два прогона одного и того же мира: raw/veins/surface/carvers — идентичны (0 расхождений блоков/биомов/карт высот, Overworld seed 12345 r=10; raw ещё Nether, End);
   **features Overworld — 107 351 из 43,35 млн блоков (0,25 %), full — 118 536 (0,27 %), features Nether — 38 682**, End — 0. Расхождения прижаты к границам чанков
   (по расстоянию до границы 0…7: 2858, 2394, 1907, 1475, 1037, 641, 350, 108), на y −64…32: мох/глина/сколк/лианы (clay↔deepslate, moss_block↔deepslate, cave_vines). С
   `-Dmax.bg.threads=1` — всё равно 0,09 % (10 770 из 11,9 млн, r=5). Вывод: **100 % по декорациям недостижимы и у самой ванили**; порог G5 99,9 % сопоставим с шумом эталона.
   Инструмент: `diff.py --stable-with <повторный мир>` (повтор.) не сравнивает клетки, где эталон не совпал с повтором; `run_gate` подхватывает `<мир>_rep1`, `<мир>_rep2`
   (`gen_queue.py --tag rep1`). `determinism.py` измеряет шум; `tick-effect`: те же `full` с обычными тиками против `tick freeze` — 148 697 (0,34 %): отличить от шума порядка нельзя.

## 3. Варианты (`tools/gt/variants/<имя>/variant.json`)

Каталоги — декларативные (`variant.json`); датапак строит `tools/gt/datapack.py` из `run/pack-<V>/` при каждой генерации (ресурсы Mojang в репозиторий не попадают), хэш пака — в `manifest.json`.

| вариант | состав | G |
|---|---|---|
| `raw` | только `doFill` (aquifer): камень/вода/лава/воздух; нет бедрока, сланца, жил, травы; карверов/фич/построек нет | G2 |
| `veins` | raw + `copper_ore_vein`, `iron_ore_vein` (только Overworld) | G2v |
| `surface` | настоящие правила материала (бедрок, жилы, поверхность, сланец, лёд, бэдленды) | G3 |
| `carvers` | surface + карверы | G4 |
| `carve_raw` | raw + карверы (карверы отдельно от поверхности; `--tweak ore_veins=0`, маска ext) | G4c |
| `features` | carvers + все placed_feature (без построек) | G5 |
| `full` | чистая ваниль (без пака), структуры включены | G6 |
| `feature:<id>` | surface + одна placed_feature на своём шаге у биомов, где она есть; карверов нет | G5 по одной |
| `structure:<set>` | surface + один набор построек (остальные `structures=[]`), фич/карверов нет | G6 по одной |

`gen_queue.py --plan features` — по `tools/gt/features_plan.json` (`feature_plan.py`: для каждой placed_feature область матрицы или extras с максимумом клеток её биомов; 209 фич,
2 без биомов; группы: `ores` — руды/диски/источники/блобы/геология, `veg` — растительность; r=5). `--plan structures` — по `structures_plan.json` (`structure_plan.py`, oracle
`structstart`: ближайший валидный старт набора для каждого seed; r=8): найдены все наборы кроме `strongholds` (кольца — отдельно: oracle `stronghold <seed>`), а также ruined_portals,
nether_fossils и (не на всех seed) buried_treasures, desert_pyramids, igloos, swamp_huts, woodland_mansions.

## 4. Матрица, раскладка

`run/gt/<V>/<вариант>/<dim>-s<seed>-c<cx>_<cz>-r<R>/`: `world/` (Overworld `world/dimensions/minecraft/overworld/region/*.mca`; `the_nether`, `the_end`), `world/datapacks/gt/`, `server.properties`,
`server.log`, `manifest.json`, `world/anvilcache/` (npz). Seeds 12345, 8675309, −7048155917072976836. `tools/gt/matrix.json` (`pick_regions.py`, `tools/mcquery`, y=68, окна 21x21 чанков):
для каждого seed — Overworld spawn (0,0) и области ocean / mountains / desert / jungle (до ±3300 блоков от начала), Nether (0,0) и (60,−60), End (0,0) и внешние острова (90,0), (−40,−90);
`extras` (r=5) — 14 областей с редкими биомами для фич (old_growth_taiga, pale_garden, dappled_forest, flower_forest, mangrove_swamp, ice_spikes, warm_ocean, cold_ocean,
mushroom_fields, wooded_badlands, windswept_*, soul_sand_valley). Профили очереди: `core` (4 области/seed), `land` (8), `all` (10), `extras`.

Готово (26.3): raw, surface, carvers, carve_raw — 30 миров (`all`); veins — 15 (Overworld); features, full — core (12); structure_*/full — витрина по 45 областям построек;
feature_* — по плану (см. статус ниже). Для 26.1, 26.2, 26.4-snapshot-2: raw — `land` (24 мира каждая версия, 3 seed × 3 измерения + области), surface — core.

## 5. Читатель, дамп, сравнение

* `anvil.py` — блоки `u16[y][z][x]` (id по `reports/blocks.json`), биомы `u8[qy][qz][qx]`, статус, карты высот; npz-кэш. Находки формата 26.x: палитра блоков — `{"": "minecraft:stone"}`
  (состояние по умолчанию; NBT не смешивает строки и компаунды) либо `{id, properties}` (26.4: строки `minecraft:x[a=b]`); «световые» секции вне диапазона блоков (`Y=-1`, `SkyLight`)
  пропускаются, высоту берём из `dimension_type` (Overworld −64/384, Nether, End 0/256); 26.4-snapshot-2: биомы поблочно (`Chunk.biomes_block`, клетки — приближение; diff по клеткам
  биомов для 26.4 пропускается, сверка — `mcgen_biome_at` по блокам); имя ключа статуса `Status`/`status`.
* `mcr.py` — MCR1 (чтение memmap, запись, `from-world`). `diff.py` — сравнение по именам состояний, метрики и маски в `--help`; `--list N` — координаты первых расхождений.
* Кэш больших областей: до 6 region-ов в памяти (64x64 чанков — до 9 region-ов по ~200 МБ).

## 6. Результаты ворот (libmcgen W1, 26.3; подробные таблицы — `accuracy.md`)

G2 30/30 (100 %, Nether/End/Overworld), G2v 15/15, G3 30/30, G4 29/30 (1 блок `water[level=0] → water[level=1]` у растекания), G4c 30/30; G5/G6 — по core-областям
(Overworld/Nether расходятся: поток декораций/построек ещё не реализован). Биомы raw 99,9–100 % (клетки на границах), карты высот — по сути 0 расхождений.

## 7. Бюджет времени и объёма (измерено, `tools/gt/budget.py --md`)

Время — от запуска JVM; «генерация» — от `forceload` до готовности всех чанков (шаг опроса ~3 с, поэтому для быстрых вариантов это верхняя граница); на мир ≈ старт + генерация + ~8 с остановки.
Миры по 441 чанк: raw ≈ 35 с на мир, features ≈ 36 с (Overworld генерация ≈ 12–16 с), full ≈ 30–60 с; region-файлы ≈ 9–13 МБ на мир (2200–2600 чанков с кольцами).
Матрица `all` (30 миров) одного варианта ≈ 17–25 мин; одна фича r=5 ≈ 45–60 с на мир.

<!-- budget begin -->
| вариант | измерение | миров | чанков в области | старт сервера, с | генерация, с (шаг 3 с) | всего на мир, с | region-файлы, МБ | чанков в мире (с кольцами) |
|---|---|---|---|---|---|---|---|---|
| carve_raw r=10 | overworld | 15 | 441 | 18.7 | 7.2 | 34.4 | 10.8 | 2615 |
| carve_raw r=10 | the_end | 9 | 441 | 18.3 | 3.3 | 29.7 | 9.1 | 2209 |
| carve_raw r=10 | the_nether | 6 | 441 | 19.0 | 5.0 | 32.4 | 9.1 | 2209 |
| carvers r=10 | overworld | 15 | 441 | 14.3 | 8.0 | 30.3 | 10.9 | 2615 |
| carvers r=10 | the_end | 9 | 441 | 15.6 | 4.6 | 27.8 | 9.1 | 2209 |
| carvers r=10 | the_nether | 6 | 441 | 16.2 | 6.4 | 39.3 | 9.1 | 2209 |
| feature:* r=5 | overworld | 1 | 121 | 26.0 | 6.0 | 40.2 | 8.0 | 1898 |
| feature:* r=5 | the_nether | 1 | 121 | 20.0 | 6.0 | 33.2 | 5.6 | 1369 |
| features r=10 | overworld | 3 | 441 | 16.0 | 12.1 | 36.1 | 12.9 | 2209 |
| features r=10 | the_end | 6 | 441 | 19.3 | 5.5 | 32.9 | 9.1 | 2209 |
| features r=10 | the_nether | 3 | 441 | 16.0 | 7.0 | 30.6 | 10.3 | 2209 |
| full r=8 | overworld | 39 | 289 | 13.1 | 9.4 | 30.3 | 12.0 | 2195 |
| full r=10 | overworld | 3 | 441 | 28.4 | 25.3 | 63.1 | 13.2 | 2209 |
| full r=8 | the_end | 3 | 289 | 12.3 | 3.0 | 22.7 | 7.6 | 1849 |
| full r=10 | the_end | 6 | 441 | 25.0 | 6.5 | 40.1 | 9.1 | 2209 |
| full r=8 | the_nether | 3 | 289 | 15.0 | 7.0 | 29.7 | 8.6 | 1849 |
| full r=10 | the_nether | 3 | 441 | 27.4 | 12.1 | 47.6 | 10.3 | 2209 |
| raw r=2 | overworld | 1 | 25 | 20.2 | 6.0 | 32.0 | 4.2 | 1007 |
| raw r=5 | overworld | 13 | 121 | 10.3 | 3.0 | 20.5 | 7.8 | 1894 |
| raw r=10 | overworld | 15 | 441 | 16.5 | 6.5 | 40.4 | 10.8 | 2615 |
| raw r=10 | the_end | 9 | 441 | 13.8 | 4.0 | 85.0 | 9.1 | 2209 |
| raw r=5 | the_nether | 1 | 121 | 10.0 | 3.0 | 20.1 | 5.6 | 1369 |
| raw r=10 | the_nether | 6 | 441 | 17.8 | 4.7 | 38.0 | 9.1 | 2209 |
| structure:* r=8 | overworld | 39 | 289 | 12.1 | 4.5 | 24.3 | 9.1 | 2195 |
| structure:* r=10 | overworld | 1 | 441 | 12.0 | 9.1 | 29.0 | 9.4 | 2255 |
| structure:* r=8 | the_end | 3 | 289 | 10.3 | 4.0 | 21.7 | 7.6 | 1849 |
| structure:* r=8 | the_nether | 3 | 289 | 13.3 | 4.0 | 24.8 | 7.6 | 1849 |
| surface r=10 | overworld | 15 | 441 | 17.9 | 7.0 | 33.0 | 10.8 | 2615 |
| surface r=10 | the_end | 9 | 441 | 18.5 | 3.9 | 29.9 | 9.1 | 2209 |
| surface r=10 | the_nether | 6 | 441 | 15.7 | 4.9 | 28.1 | 9.1 | 2209 |
| veins r=10 | overworld | 15 | 441 | 17.1 | 6.6 | 32.5 | 10.8 | 2615 |
<!-- budget end -->

## 8. Что осталось

* G5/G6 (W8/W-структуры): сравнивать с `--stable-with` (rep1/rep2); оценка шума ванили — `determinism.py`.
* `strongholds`, `ruined_portals`, `nether_fossils` — отдельный поиск позиций; 26.1/26.2/26.4 для features/structures (только по необходимости).

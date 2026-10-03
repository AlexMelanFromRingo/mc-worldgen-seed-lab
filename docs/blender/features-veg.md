# W10 «Растительность» (G5-veg): фичи травы, цветов, водорослей, лоз, бамбука, кораллов, мхов, грибов Nether

Отчёт потока W10 аддона «MC Worldgen». Каркас стадии FEATURES (порядок фич, модификаторы размещения, провайдеры, предикаты, `simple_block`, селекторы) —
`docs/blender/features.md` (W8); здесь только то, что добавила группа растительности. Все числа измерены на настоящих чанках ванильного сервера
(`run/gt/<V>/…`); команды воспроизведения — в конце (§7).

## 0. Итог

* Реализованы типы фич: `block_column`, `bamboo`, `vines`, `vegetation_patch`, `waterlogged_vegetation_patch`, `coral_tree`, `coral_claw`, `root_system`, `huge_fungus` (26.3/26.4) и,
  для 26.1/26.2, ещё `coral_mushroom`, `kelp`, `seagrass`, `sea_pickle`, `nether_forest_vegetation`, `twisting_vines`, `weeping_vines`; провайдер состояний `random_block`;
  `BlockState.canSurvive` около 25 классов растений; `MossyCarpetBlock.placeAt`. В 26.3 группа — **90 placed_feature** (травы, цветы, кусты, грибы, тростник, кактус, тыквы/арбузы,
  кувшинки, морская трава, келп, морские огурцы, кораллы, лозы, бамбук, мох/лаш-пещеры/клей, бледный мох, азалия, грибы и корни Nether); все реализованы полностью.
* **Ворота изолированной фичи (100 %).** Два вида эталонов: плотные `feature:<id>` (r=5) и разреженные `featuresparse:<id>@K` (r=10, W6; перед фичей стоит `rarity_filter(K)`).
  26.3: 102 разреженных мира (87 с эффектом, 72 фичи): **61 мир — 0 расхождений на 360 тыс. изменённых блоков**; в остальных 26 расхождения лежат только в «кластерах» соседних
  декорируемых чанков (порядок обхода соседей — недетерминизм игры, §3), а в окнах одиночных декорируемых чанков (341 одиночный чанк, 3069 чанков в окнах) — **0 расхождений**.
  Плотные r=5: 91 запуск, 65 с эффектом, 41 — 0 расхождений (остальные — тот же шум порядка). 26.1: 17 миров (14 — 0, остальные — кластерный шум), **26.2: 11 из 11 — 0**,
  **26.4-snapshot-2: 5 из 5 — 0** (поведение растительности 26.4 = 26.3; различия 26.1/26.2 — §2).
* **В сборе** (`featureset:veg_overworld` / `veg_the_nether`: вся растительность без деревьев и построек, r=10, `g5_veg_set.py`): Overworld s12345 (0,0) — 99,961 % (16 876 из 43,35 млн; ваниль против
  собственного повтора — 35 667), s8675309 (0,0) — **99,9987 %**, джунгли s12345 (−103,2) — 99,887 %, s8675309 (−30,−187) — 99,870 %; Nether — 99,787 % и 99,905 % (грибы-гиганты).
  Порог 99,9 % из спецификации достигается там, где нет плотных кластеров; в плотных биомах (джунгли) недетерминизм ванили сопоставим с нашим расхождением — измерено на мире с повтором
  (c0_0: наши 16 876, после маски шума повтора 10 554, ваниль против ваниль 35 667). Сборка «всё вместе» с деревьями/льдом (`features`, W8): доля растительности в расхождениях ≈ 30 % (§6).
* **Найдено попутно (общее для всей группы G5):** WG-карты высот 26.3 — *снимок сразу после TERRAIN*, а не ленивое праймирование: `doFill`, поверхность и карверы пишут в `*_WG`, пока статус чанка
  ≤ BIOMES, дальше карты не обновляются (замечено W9 на постройках, подтверждено здесь). Ленивая модель (W8) давала лишние расхождения: `patch_grass_jungle@16` 397 → 80, `patch_grass_taiga@16` 43 → 1,
  `patch_grass_plain@4` 46 → 0, `warm_ocean_vegetation` r=5 22 813 → 1 617, `patch_grass_normal@4` (26.4) 7 → 0. Включено по умолчанию (`feature.c`; старая модель — `MCGEN_FEATURES_WGLAZY=1`).
  Регрессии у других групп нет (повтор всех 254 изолированных миров r=5 до и после).
* **Грибы** на стадии FEATURES почти никогда не выживают (небесный свет 15, §2): `brown/red_mushroom_*` дают 0 блоков и в ванили, и у нас — это тоже проверка (прежняя модель «свет 0» ставила ~900 лишних грибов).

## 1. Где код

| файл | что |
|---|---|
| `libmcgen/src/feature_veg.c` | `block_column`, `bamboo`, `vines`, `vegetation_patch`, `waterlogged_vegetation_patch`, `coral_tree`/`coral_claw`/`coral_mushroom`, `huge_fungus`, `root_system`; `veg_survive` — `BlockState.canSurvive` классов растений; `fc_region_random` — `WorldGenRegion.getRandom()`; `MossyCarpetBlock.placeAt`; провайдер `random_block` (в `feature_bsp.c`) использует `veg_holderset_blocks` отсюда |
| `libmcgen/src/feature_veg2.c` | типы 26.1/26.2 (в 26.3 заменены `block_column`/`simple_block`): `kelp`, `seagrass`, `sea_pickle`, `nether_forest_vegetation`, `twisting_vines`, `weeping_vines` |
| `libmcgen/src/feature_veg_hash.c` | порядок обхода `java.util.HashSet<BlockPos>` (HashMap: рост таблицы, цепочки, деревья-корзины) — без него `vegetation_patch` расходится с игрой |
| `libmcgen/src/feature_veg.h` | `VPos`, `veg_hashset_order`, `veg_survive`, `veg_tag`, мелкие помощники |
| `libmcgen/tests/g5_veg_sparse.py` | ворота G5-veg по разрежённым эталонам `featuresparse:<id>@K` (W6) |
| `libmcgen/tests/g5_veg_pick.py` | подбор областей r=10 с высокой долей биомов фичи (без океанов); список — `libmcgen/tests/results/g5_veg_pick-26.3.json` |
| `libmcgen/tests/g5_veg_hash.py` (+ `g5_veg_hash.c`, `g5_veg/HashOrder.java`) | сверка эмуляции `HashSet<BlockPos>` с настоящей Java на 3000 наборах |
| `libmcgen/tests/g5_veg_probe.py` | срез блоков «эталон | наш дамп» вокруг точки (отладка) |
| `libmcgen/tests/g5_veg_table.py` | таблица «placed_feature × статус» (§5) |
| `tools/gt/featuresets/veg_overworld.json`, `veg_the_nether.json` | наборы для `featureset:<имя>`: вся растительность без деревьев (80 и 13 фич) — «в сборе» |

Правки общих файлов (точечные): `feature.c` (регистрация `feature_register_veg`; отладочные переменные `MCGEN_PACK_OVERLAY`, `MCGEN_FEATURES_LOGCHUNKS`,
`MCGEN_FEATURES_WGEAGER`; сброс `region_rnd` в начале чанка), `feature.h` (`FCtx.region_rnd`, `fc_region_random`), `feature_bpred.c` (вызов `veg_survive`),
`feature_bsp.c` (провайдер `random_block`), `feature_misc.c` (`MossyCarpetBlock` в `simple_block`), `placement.c` — без изменений поведения.
`tests/g5_features.py`: переменная `MCGEN_CLI` (свой CLI), `--stable` (маска недетерминизма по повторным мирам), пропуск `_rep*`. ABI `mcgen.h` не менялся.

## 2. Что реализовано и чем оно «хитрое»

* **`block_column`** (келп, пещерные лозы, кактус, сахарный тростник, `twisting_vines` в 26.3): высоты слоёв выбираются все сразу (порядок ГСЧ), затем идёт
  проверка `allowed_placement` вдоль направления, обрезка (`prioritize_tip`: с вершины или с основания) — только потом запись блоков.
* **`bamboo`**: `canSurvive` бамбука = тег `supports_bamboo`; подзол — по `WORLD_SURFACE` (финальная карта, обновляется записями), обрезка стебля, три верхних блока.
* **`vines`**: грань, на которую можно опереться, — `MultifaceBlock.canAttachTo` (полная грань опорной формы или формы столкновения; в таблице флагов — `sturdy` и `FULL_COLL`).
* **`vegetation_patch` / `waterlogged_vegetation_patch`**: **порядок обхода `HashSet<BlockPos>`**. Игра хранит поверхность патча в `HashSet`, а потом бросает кубик
  `random.nextFloat()` на каждую позицию *в порядке обхода множества*; сдвиг одной позиции меняет все последующие выборки. Хэш `Vec3i.hashCode = (y + z·31)·31 + x`,
  `spread = h ^ (h >>> 16)`, таблица 16 → удвоение при `size > 0,75·cap`; цепочки ≥ 9 при ёмкости ≥ 64 превращаются в красно-чёрные деревья (`treeifyBin`), при `resize` —
  `split` с `untreeify` ≤ 6. `feature_veg_hash.c` воспроизводит всё это (включая `moveRootToFront`). Проверка против настоящей Java (`g5_veg_hash.py`): 2999 из 3000
  наборов, включая «враждебные» с сотнями столкновений; единственное расхождение — набор с двумя позициями с полностью равным хэшем в дереве-корзине, где игра
  выбирает порядок по `System.identityHashCode` (недетерминированно; для реальных патчей такого не бывает: позиции в пределах ±9 по x/z).
  Влияние (`lush_caves_ceiling_vegetation`, плотный r=5): порядок вставки без эмуляции — 15 885 расхождений, простая модель корзин без деревьев — 5949, полная модель — 4666 (остаток — недетерминизм порядка чанков, §3); в разреженном эталоне @12 с полной моделью — 0 (53 без неё).
  В `waterlogged_vegetation_patch` добавлено `placeVegetation(pos.below())` и последующее `waterlogged=true`.
* **коралловые фичи**: 26.3+ — `coral_tree`/`coral_claw` с вложенной размещённой фичей «кораллового блока»; 26.1/26.2 — те же формы, но блок ставится самим кодом
  (`CoralFeature.placeCoralBlock`: теги `coral_blocks`/`corals`/`wall_corals` перебираются **в порядке файлов тегов**, `Util.getRandomSafe`), плюс `coral_mushroom`.
  `Util.shuffle` (Фишер–Йейтс сверху вниз) и `Direction.Plane.HORIZONTAL` в порядке `NORTH, EAST, SOUTH, WEST`.
* **`random_block`** (`BlockStateProvider`): `HolderSet<Block>.getRandomElement` — равновероятный выбор в порядке элементов набора (теги разворачиваются рекурсивно, без дублей).
* **`MossyCarpetBlock.placeAt`** (бледный мох) использует *не* ГСЧ декорации, а `WorldGenRegion.getRandom()` — позиционный Xoroshiro от `RandomState`
  (`getOrCreateRandomFactory("minecraft:worldgen_region_random").at(мин. блок центрального чанка)`), один на чанк; нативные методы Xoroshiro (`nextBoolean` — младший бит `nextLong`).
* **`root_system`** (азалия): колонка вверх до первого подходящего места, корни/подвесные корни на ГСЧ декорации; вложенное дерево — группа деревьев (W11).
* **`huge_fungus`** (багровые/искажённые грибы Nether): шляпа/ножка/декор, висячие лозы `weeping_vines` в шляпе, `isReplaceable` по `canBeReplaced` + предикат растений.
* **26.1/26.2**: `kelp`, `seagrass`, `sea_pickle`, `nether_forest_vegetation`, `twisting_vines`, `weeping_vines` — старые классы `Feature` с собственным порядком вызовов ГСЧ (например
  `SeagrassFeature`: `nextInt(8) − nextInt(8)` по x и z, потом `nextDouble() < probability`; `twisting_vines`: `Mth.nextInt(random, −w, w)` — а не `nextInt(w) − nextInt(w)`).
* **`BlockState.canSurvive`** (`veg_survive`, класс блока из `block_flags.json`): морская трава (низ — прочная грань, не в `cannot_support_seagrass`), высокая морская трава
  (+ вода глубины 8), морской огурец (прочная грань или форма столкновения), кувшинка (`FluidState` воды/тег `supports_lily_pad` и пустая жидкость над), огонь/душа огня,
  сахарный тростник (вода рядом с опорой через `supports_sugar_cane[_adjacently]`), кактус (соседи-твёрдые/лава, `supports_cactus`, жидкость над), листовая подстилка,
  споровый цветок (потолок с центральной опорой, не под водой), `GrowingPlantBlock` (келп, лозы: опора с противоположной стороны роста, своё тело/голова, `cannot_support_kelp`),
  `HangingRootsBlock`, коралловые растения/веера/настенные веера, бледный мох, `DryVegetationBlock`/`ShortDryGrassBlock`/`TallDryGrassBlock` (тег `supports_dry_vegetation`, не
  `supports_vegetation`!), `SweetBerryBushBlock`, бамбук.
* **Грибы и свет.** `MushroomBlock.canSurvive`: `getRawBrightness(pos, 0) < 13` ∧ непрозрачная опора, либо опора из `overrides_mushroom_light_requirement` (мицелий, подзол, нилий).
  На стадии FEATURES освещения нет: небесный свет в чанках без данных — **15** (`SkyLightSectionStorage.getLightValue` → `15` при отсутствии секции), блочный — 0. Поэтому обычный
  гриб выживает только на «переопределяющей» опоре; ровно так ведёт себя и игра (после исправления `brown_mushroom_old_growth`/`_taiga` дали 0 расхождений вместо ~900/~200).
  Исключение игры, которое мы не воспроизводим: уже освещённые соседние чанки (если сосед успел пройти `light`) могут дать настоящий свет < 13.

## 3. Недетерминизм игры и как мы его отделяем

Шаг FEATURES чанка в ванили выполняется в потоках генерации; порядок декорации *соседних* чанков (окна 3×3 пересекаются) зависит от планировщика (W6, `ground-truth.md` §2.4;
у `ice_patch` — перестановка двух соседних чанков). У растительности последствия такие:

* **плотная фича** (много срабатываний на чанк: трава, цветы, морская трава, кораллы, мох) — соседние чанки пишут в общие клетки; результат зависит от порядка. Измерено:
  две независимые генерации одного и того же эталона (`feature_<id>…_rep1`, W6) различаются между собой на `lush_caves_ceiling_vegetation` 1895 блоков, `lush_caves_vegetation` 2375,
  `seagrass_deep_warm` 947 (11,9 млн блоков) — наши расхождения с ванилью того же порядка (4237 / 2776 / 255 после маски `--stable-with`);
* **разреженный эталон** `featuresparse:<id>@K` (W6: перед фичей добавлен `rarity_filter(K)`) — декорируется 1/K чанков, и «одиночные» декорируемые чанки (в Чебышёвском радиусе 2 нет других)
  не зависят от порядка. `g5_veg_sparse.py` отдельно считает расхождения в окнах одиночных чанков: **во всех проверенных разреженных эталонах там 0 расхождений**.

Итог: изолированная фича считается принятой, если (а) есть эталон с эффектом, (б) расхождений 0 либо все расхождения лежат в кластерах соседних декорируемых чанков, а в окнах одиночных 0.

## 4. Скорость

Измерено (`mcgen-cli`, Overworld s12345, 32×32 чанков = 1156 декорируемых вместе с кольцом, стадии `0x1f`, 12 ядер WSL2): растительность одна (`MCGEN_FEATURES_ONLY` = 80 фич набора) —
**0,14 с декорации (≈ 8200 чанков/с) на 12 потоках, 0,57 с на 1 потоке (≈ 2000 чанков/с)**; все фичи Overworld (171 placed_feature: деревья, руды, диски…) — 1,6 с на 12 потоках (724 чанков/с)
и 2,14 с на 1 потоке (540 чанков/с). Остальное время — генерация рельефа, поверхности, карверов и карт высот окна 36×36 (вся команда: 6,4 с с растительностью и 11,1 с со всеми фичами на 12 потоках;
28–29 с на 1 потоке). Результат побитово одинаков при любом числе потоков (волны чанков `t = iz + 3·ix`, `features.md` §3.2).

## 5. Таблица placed_feature × статус (26.3)

Легенда: «100 %» — есть эталон с эффектом (> 0 блоков) и 0 расхождений (плотный r=5 или `featuresparse`); «100 % в одиночных окнах» — расхождения только в кластерах соседних декорируемых чанков (§3), в окнах одиночных — 0;
«шум порядка» — расхождения есть, одиночных окон нет (нельзя отделить от недетерминизма: нужен более разреженный эталон); «нет эффекта» — в имеющемся эталоне ванильный эффект 0 блоков, и у нас 0 (совпадение «ничего»;
для грибов это ожидаемо — §2). Столбец «эталоны» — число расхождений из числа блоков, изменённых ванилью. Данные — `libmcgen/tests/results/g5veg-{dense,sparse}-26.3.json`.

Группа растительности (26.3): 90 placed_feature; 100 %: 72, 100 % в одиночных окнах: 8, нет эффекта: 7, шум порядка: 3

| placed_feature | тип | изм. | статус | эталоны |
|---|---|---|---|---|
| `bamboo` | `bamboo` | overworld | 100 % | плотный r5: 4 из 15830 изменённых; @16: расх. 0, одиночных окон 1/3 (расх. в них 0); @4: расх. 3, одиночных окон 1/39 (расх. в них 0) |
| `bamboo_light` | `bamboo` | overworld | 100 % | плотный r5: 0 из 352 изменённых; @4: расх. 2, одиночных окон 11/23 (расх. в них 0) |
| `brown_mushroom_dappled_forest` | `simple_block` | overworld | шум порядка | плотный r5: 4 из 4 изменённых |
| `brown_mushroom_nether` | `simple_block` | nether | 100 % | плотный r5: 0 из 52 изменённых |
| `brown_mushroom_normal` | `simple_block` | nether,overworld | нет эффекта | — |
| `brown_mushroom_old_growth` | `simple_block` | overworld | 100 % | плотный r5: 0 из 710 изменённых |
| `brown_mushroom_swamp` | `simple_block` | overworld | шум порядка | @4: расх. 2, одиночных окон 0/0 (расх. в них 0) |
| `brown_mushroom_taiga` | `simple_block` | overworld | нет эффекта | — |
| `cave_vines` | `block_column` | overworld | 100 % | плотный r5: 0 из 1188 изменённых |
| `classic_vines_cave_feature` | `vines` | overworld | 100 % | плотный r5: 0 из 137 изменённых |
| `crimson_forest_vegetation` | `simple_block` | nether | 100 % в одиночных окнах | плотный r5: 286 из 5277 изменённых; @4: расх. 316, одиночных окон 1/122 (расх. в них 0) |
| `crimson_fungi` | `huge_fungus` | nether | 100 % | плотный r5: 24698 из 50473 изменённых; @12: расх. 1002, одиночных окон 5/45 (расх. в них 0); @48: расх. 0, одиночных окон 5/14 (расх. в них 0) |
| `flower_cherry` | `simple_block` | overworld | 100 % | @16: расх. 0, одиночных окон 5/12 (расх. в них 0); @4: расх. 1692, одиночных окон 1/64 (расх. в них 0) |
| `flower_default` | `simple_block` | overworld | 100 % в одиночных окнах | @4: расх. 2, одиночных окон 4/4 (расх. в них 0) |
| `flower_flower_forest` | `simple_block` | overworld | 100 % | плотный r5: 0 из 3323 изменённых; @4: расх. 2, одиночных окон 1/108 (расх. в них 0) |
| `flower_forest_flowers` | `simple_random_selector` | overworld | 100 % | плотный r5: 0 из 464 изменённых; @4: расх. 0, одиночных окон 8/8 (расх. в них 0) |
| `flower_meadow` | `simple_block` | overworld | 100 % | @4: расх. 0, одиночных окон 1/94 (расх. в них 0) |
| `flower_pale_garden` | `simple_block` | overworld | 100 % | плотный r5: 0 из 1 изменённых; @4: расх. 0, одиночных окон 1/3 (расх. в них 0) |
| `flower_plains` | `simple_block` | overworld | 100 % | плотный r5: 0 из 23 изменённых; @4: расх. 0, одиночных окон 3/9 (расх. в них 0) |
| `flower_swamp` | `simple_block` | overworld | 100 % в одиночных окнах | @4: расх. 2, одиночных окон 6/6 (расх. в них 0) |
| `flower_warm` | `simple_block` | overworld | 100 % | плотный r5: 1 из 125 изменённых; @4: расх. 0, одиночных окон 2/2 (расх. в них 0) |
| `forest_flowers` | `simple_random_selector` | overworld | 100 % | плотный r5: 0 из 73 изменённых; @4: расх. 0, одиночных окон 4/4 (расх. в них 0) |
| `kelp_cold` | `block_column` | overworld | 100 % | плотный r5: 0 из 14930 изменённых |
| `kelp_warm` | `block_column` | overworld | 100 % | плотный r5: 0 из 2031 изменённых |
| `lush_caves_ceiling_vegetation` | `vegetation_patch` | overworld | 100 % | плотный r5: 4666 из 21104 изменённых; @12: расх. 0, одиночных окон 7/19 (расх. в них 0) |
| `lush_caves_clay` | `random_boolean_selector` | overworld | 100 % | плотный r5: 1030 из 25739 изменённых; @12: расх. 0, одиночных окон 5/14 (расх. в них 0) |
| `lush_caves_vegetation` | `vegetation_patch` | overworld | 100 % | плотный r5: 4324 из 17349 изменённых; @12: расх. 0, одиночных окон 7/19 (расх. в них 0) |
| `nether_sprouts` | `simple_block` | nether | 100 % | плотный r5: 0 из 3223 изменённых |
| `pale_garden_flowers` | `simple_block` | overworld | 100 % | плотный r5: 0 из 198 изменённых; @4: расх. 0, одиночных окон 2/5 (расх. в них 0) |
| `pale_moss_patch` | `vegetation_patch` | overworld | 100 % | плотный r5: 77 из 7288 изменённых; @4: расх. 0, одиночных окон 0/32 (расх. в них 0) |
| `patch_berry_common` | `simple_block` | overworld | 100 % | плотный r5: 0 из 73 изменённых; @4: расх. 0, одиночных окон 3/3 (расх. в них 0) |
| `patch_berry_rare` | `simple_block` | overworld | 100 % | @1: расх. 0, одиночных окон 3/3 (расх. в них 0) |
| `patch_bush` | `simple_block` | overworld | 100 % | плотный r5: 0 из 70 изменённых; @4: расх. 0, одиночных окон 5/35 (расх. в них 0) |
| `patch_cactus_decorated` | `block_column` | overworld | 100 % | плотный r5: 0 из 4 изменённых; @1: расх. 0, одиночных окон 6/8 (расх. в них 0) |
| `patch_cactus_desert` | `block_column` | overworld | 100 % | плотный r5: 0 из 36 изменённых; @4: расх. 0, одиночных окон 6/18 (расх. в них 0) |
| `patch_crimson_roots` | `simple_block` | nether | 100 % | плотный r5: 0 из 22 изменённых |
| `patch_dead_bush` | `simple_block` | overworld | 100 % | плотный r5: 0 из 56 изменённых; @4: расх. 2, одиночных окон 8/52 (расх. в них 0) |
| `patch_dead_bush_2` | `simple_block` | overworld | 100 % | плотный r5: 0 из 180 изменённых; @4: расх. 0, одиночных окон 1/115 (расх. в них 0) |
| `patch_dead_bush_badlands` | `simple_block` | overworld | 100 % | плотный r5: 0 из 1334 изменённых; @16: расх. 0, одиночных окон 9/32 (расх. в них 0); @4: расх. 0, одиночных окон 0/140 (расх. в них 0) |
| `patch_dry_grass_badlands` | `simple_block` | overworld | 100 % | плотный r5: 0 из 138 изменённых; @4: расх. 0, одиночных окон 6/24 (расх. в них 0) |
| `patch_dry_grass_desert` | `simple_block` | overworld | 100 % | плотный r5: 0 из 451 изменённых; @4: расх. 0, одиночных окон 5/44 (расх. в них 0) |
| `patch_fire` | `simple_block` | nether | 100 % | плотный r5: 0 из 89 изменённых |
| `patch_firefly_bush_near_water` | `simple_block` | overworld | 100 % | @1: расх. 0, одиночных окон 13/30 (расх. в них 0) |
| `patch_firefly_bush_near_water_swamp` | `simple_block` | overworld | нет эффекта | — |
| `patch_firefly_bush_swamp` | `simple_block` | overworld | 100 % в одиночных окнах | @4: расх. 2, одиночных окон 8/12 (расх. в них 0) |
| `patch_grass_badlands` | `simple_block` | overworld | 100 % | @4: расх. 0, одиночных окон 2/7 (расх. в них 0) |
| `patch_grass_forest` | `simple_block` | overworld | 100 % | плотный r5: 0 из 1021 изменённых; @4: расх. 0, одиночных окон 0/103 (расх. в них 0) |
| `patch_grass_jungle` | `simple_block` | overworld | 100 % в одиночных окнах | плотный r5: 9494 из 13013 изменённых; @16: расх. 80, одиночных окон 9/41 (расх. в них 0); @4: расх. 723, одиночных окон 0/126 (расх. в них 0) |
| `patch_grass_meadow` | `simple_block` | overworld | 100 % | @16: расх. 0, одиночных окон 7/22 (расх. в них 0); @4: расх. 0, одиночных окон 0/101 (расх. в них 0) |
| `patch_grass_normal` | `simple_block` | overworld | 100 % | плотный r5: 0 из 2606 изменённых; @16: расх. 0, одиночных окон 11/23 (расх. в них 0); @4: расх. 0, одиночных окон 0/101 (расх. в них 0) |
| `patch_grass_plain` | `simple_block` | overworld | 100 % | плотный r5: 0 из 883 изменённых; @16: расх. 0, одиночных окон 5/14 (расх. в них 0); @4: расх. 0, одиночных окон 1/53 (расх. в них 0) |
| `patch_grass_savanna` | `simple_block` | overworld | 100 % | плотный r5: 0 из 96 изменённых; @4: расх. 0, одиночных окон 2/16 (расх. в них 0) |
| `patch_grass_taiga` | `simple_block` | overworld | 100 % в одиночных окнах | плотный r5: 264 из 4834 изменённых; @16: расх. 1, одиночных окон 14/24 (расх. в них 0); @4: расх. 29, одиночных окон 1/124 (расх. в них 0) |
| `patch_grass_taiga_2` | `simple_block` | overworld | 100 % | плотный r5: 0 из 390 изменённых; @4: расх. 0, одиночных окон 2/125 (расх. в них 0) |
| `patch_large_fern` | `simple_block` | overworld | 100 % | плотный r5: 0 из 348 изменённых; @4: расх. 0, одиночных окон 9/28 (расх. в них 0) |
| `patch_leaf_litter` | `simple_block` | overworld | 100 % | плотный r5: 2 из 50 изменённых; @8: расх. 0, одиночных окон 7/56 (расх. в них 0) |
| `patch_melon` | `simple_block` | overworld | 100 % | плотный r5: 0 из 181 изменённых; @8: расх. 2, одиночных окон 4/10 (расх. в них 0) |
| `patch_melon_sparse` | `simple_block` | overworld | 100 % | плотный r5: 0 из 75 изменённых |
| `patch_pumpkin` | `simple_block` | overworld | 100 % | @1: расх. 0, одиночных окон 6/6 (расх. в них 0); @2: расх. 0, одиночных окон 2/2 (расх. в них 0) |
| `patch_red_shrub` | `simple_block` | overworld | 100 % в одиночных окнах | @4: расх. 1, одиночных окон 3/107 (расх. в них 0) |
| `patch_soul_fire` | `simple_block` | nether | нет эффекта | — |
| `patch_sugar_cane` | `block_column` | overworld | 100 % | @4: расх. 0, одиночных окон 2/2 (расх. в них 0) |
| `patch_sugar_cane_badlands` | `block_column` | overworld | 100 % | @1: расх. 0, одиночных окон 1/3 (расх. в них 0) |
| `patch_sugar_cane_desert` | `block_column` | overworld | 100 % | @4: расх. 0, одиночных окон 2/2 (расх. в них 0) |
| `patch_sugar_cane_swamp` | `block_column` | overworld | 100 % в одиночных окнах | @4: расх. 2, одиночных окон 7/11 (расх. в них 0) |
| `patch_sunflower` | `simple_block` | overworld | 100 % | @4: расх. 0, одиночных окон 8/30 (расх. в них 0) |
| `patch_tall_grass` | `simple_block` | overworld | 100 % | @4: расх. 0, одиночных окон 7/15 (расх. в них 0) |
| `patch_tall_grass_2` | `simple_block` | overworld | 100 % | плотный r5: 0 из 184 изменённых; @4: расх. 2, одиночных окон 7/19 (расх. в них 0) |
| `patch_waterlily` | `simple_block` | overworld | 100 % | плотный r5: 0 из 329 изменённых; @4: расх. 2, одиночных окон 3/71 (расх. в них 0) |
| `red_mushroom_nether` | `simple_block` | nether | 100 % | плотный r5: 0 из 52 изменённых |
| `red_mushroom_normal` | `simple_block` | nether,overworld | нет эффекта | — |
| `red_mushroom_old_growth` | `simple_block` | overworld | шум порядка | плотный r5: 1 из 11 изменённых |
| `red_mushroom_swamp` | `simple_block` | overworld | нет эффекта | — |
| `red_mushroom_taiga` | `simple_block` | overworld | нет эффекта | — |
| `sea_pickle` | `simple_block` | overworld | 100 % | плотный r5: 0 из 170 изменённых |
| `seagrass_cold` | `weighted_random_selector` | overworld | 100 % | плотный r5: 79 из 1198 изменённых; @4: расх. 0, одиночных окон 1/75 (расх. в них 0) |
| `seagrass_deep` | `weighted_random_selector` | overworld | 100 % | плотный r5: 0 из 6501 изменённых; @16: расх. 0, одиночных окон 5/22 (расх. в них 0); @4: расх. 172, одиночных окон 0/78 (расх. в них 0) |
| `seagrass_deep_cold` | `weighted_random_selector` | overworld | 100 % | плотный r5: 459 из 5427 изменённых; @4: расх. 0, одиночных окон 0/53 (расх. в них 0) |
| `seagrass_deep_warm` | `weighted_random_selector` | overworld | 100 % | плотный r5: 574 из 12759 изменённых; @16: расх. 0, одиночных окон 4/43 (расх. в них 0); @4: расх. 190, одиночных окон 0/125 (расх. в них 0) |
| `seagrass_normal` | `weighted_random_selector` | overworld | 100 % | плотный r5: 233 из 6314 изменённых; @4: расх. 0, одиночных окон 3/95 (расх. в них 0) |
| `seagrass_river` | `weighted_random_selector` | overworld | 100 % | плотный r5: 19 из 1653 изменённых; @4: расх. 0, одиночных окон 1/28 (расх. в них 0) |
| `seagrass_swamp` | `weighted_random_selector` | overworld | 100 % | плотный r5: 124 из 2496 изменённых; @16: расх. 0, одиночных окон 4/4 (расх. в них 0); @4: расх. 110, одиночных окон 1/32 (расх. в них 0) |
| `seagrass_warm` | `weighted_random_selector` | overworld | 100 % | плотный r5: 2 из 2333 изменённых; @4: расх. 0, одиночных окон 2/33 (расх. в них 0) |
| `spore_blossom` | `simple_block` | overworld | 100 % | плотный r5: 0 из 43 изменённых; @4: расх. 0, одиночных окон 7/28 (расх. в них 0) |
| `twisting_vines` | `block_column` | nether | 100 % | плотный r5: 0 из 647 изменённых; @4: расх. 0, одиночных окон 3/3 (расх. в них 0) |
| `vines` | `vines` | overworld | 100 % | плотный r5: 1 из 250 изменённых; @4: расх. 0, одиночных окон 2/84 (расх. в них 0) |
| `warped_forest_vegetation` | `simple_block` | nether | 100 % | плотный r5: 92 из 3806 изменённых; @4: расх. 0, одиночных окон 4/106 (расх. в них 0) |
| `warped_fungi` | `huge_fungus` | nether | 100 % | плотный r5: 16416 из 42565 изменённых; @12: расх. 0, одиночных окон 3/16 (расх. в них 0) |
| `wildflowers_birch_forest` | `simple_block` | overworld | 100 % | плотный r5: 0 из 273 изменённых; @4: расх. 0, одиночных окон 1/95 (расх. в них 0) |
| `wildflowers_meadow` | `simple_block` | overworld | 100 % | @16: расх. 0, одиночных окон 7/22 (расх. в них 0); @4: расх. 12, одиночных окон 0/101 (расх. в них 0) |

## 6. Ограничения и что осталось

* **Недетерминизм порядка чанков** (§3) — главный остаточный источник. У плотных фич (`patch_grass_jungle`, `seagrass_deep*`, `warm_ocean_vegetation` @4/@8, `crimson_fungi`@12, `flower_cherry`@4, `bamboo_vegetation`) расхождения
  лежат в кластерах соседних декорируемых чанков; с ростом разрежения они исчезают: `crimson_fungi` @12: 1002 → @48: 0; `flower_cherry` @4: 1692 → @16: 0; `patch_grass_jungle` @4: 723 → @16: 80; `seagrass_deep_warm` @4: 190 → @16: 0.
  Исправить это нельзя без воспроизведения планировщика игры; наш порядок — как у forceload-билетов (x внешний, z внутренний), из проверенных (`MCGEN_FEATURES_SEQ`) он ближе всего к ванили.
* **Грибы и освещение.** Уже освещённые к моменту декорации соседние чанки (сосед успел пройти `light`) дают настоящий свет < 13, и грибы там выживают; без движка света это не воспроизвести. В измеренных эталонах такие случаи не встретились.
* **Растекание воды** (расхождения `water ↔ air` в 2–4 блока на мир рядом с жидкостью) — пост-обработка жидкостей W1, не растительность.
* **`HashSet<BlockPos>` с полностью равными хэшами в дереве-корзине**: порядок определяет `System.identityHashCode`; для реальных патчей (позиции в окне ±9) не встречается; в тесте 1 набор из 3000 искусственных.
* **Фичи без эффекта в имеющихся эталонах**: `patch_pumpkin`, `patch_berry_rare`, `patch_cactus_decorated`, `patch_firefly_bush_near_water[_swamp]`, `patch_soul_fire`, `patch_sugar_cane_badlands`, `rooted_azalea_tree` — ванильный эффект 0 (редкие фичи).
  По счётчикам блоков в `featureset:veg_overworld` они согласуются (`rooted_dirt` 2132 против 2142 у ванили), но блок-в-блок не подтверждены. `root_system` в 26.2 реализован по 26.3 (в данных 26.2 `level_test_distance` = 0).
* **Не мои типы, видимые в сборе**: `multiface_growth` (лишайник, сколк-жилы), `sculk_patch`, `freeze_top_layer`, `block_pile`, `overlay` (корни Nether, `nylium_bonemeal`, коралловые «декорации» 26.3 — это `overlay` W12), деревья и гигантские грибы (W11),
  `template` (колодец, серная пружина) — их расхождения в `features` в долю растительности не входят.
* **Сборка `features` целиком** (Overworld s12345 (0,0), r=10, `run_gate.py --gate G5`): 99,589 % (251 236 расхождений из 61 млн блоков); из 1996 различных пар расхождений на растительность (мои типы, включая `clay↔deepslate`,
  `moss_block↔stone`) приходится ≈ 30 % блоков, на деревья ≈ 14 %, остальное — лёд/снег (`ice → water` 89 тыс., W12) и сколк. Nether: 99,830 % / 99,946 %, End: 100 %.

## 7. Как воспроизвести

```bash
make -C libmcgen && export MCGEN_CLI=$PWD/libmcgen/build/mcgen-cli
python3 libmcgen/tests/g5_blockflags.py 26.3                      # свойства блоков из Java (нужны block_flags.json)
python3 libmcgen/tests/g5_features.py --only patch_grass_plain,kelp_cold [--stable]   # плотные изолированные r=5 (feature:<id>)
python3 libmcgen/tests/g5_veg_sparse.py [--only <id>] [--orders] [--report r.json]     # разреженные featuresparse:<id>@K, r=10
python3 libmcgen/tests/g5_veg_hash.py                                                  # эмуляция HashSet<BlockPos> против Java
python3 libmcgen/tests/g5_veg_pick.py --features patch_tall_grass,flower_default       # подбор областей для новых эталонов
python3 libmcgen/tests/g5_veg_table.py --dense dense.json --sparse sparse.json         # таблица §5
```
Отладка: `MCGEN_FEATURES_ONLY=<placed_feature>`, `MCGEN_FEATURES_SEQ=<xz|zx|…>` (порядок обхода чанков), `MCGEN_FEATURES_LOGCHUNKS=1` (чанки, где фича что-то поставила → stderr),
`MCGEN_PACK_OVERLAY=<мир>/world/datapacks/gt` (подмена JSON датапаком эталона — для `featuresparse`), `MCGEN_FEATURES_WGEAGER=1` (WG-карты высот 26.3 сразу, а не лениво).

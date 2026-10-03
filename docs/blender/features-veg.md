# W10 «Растительность» (G5-veg): фичи травы, цветов, водорослей, лоз, бамбука, кораллов, мхов, грибов Nether

Отчёт потока W10 аддона «MC Worldgen». Каркас стадии FEATURES (порядок фич, модификаторы размещения, провайдеры, предикаты, `simple_block`, селекторы) —
`docs/blender/features.md` (W8); здесь только то, что добавила группа растительности. Все числа измерены на настоящих чанках ванильного сервера
(`run/gt/<V>/…`); команды воспроизведения — в конце (§7).

@@ИТОГ@@

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

@@СКОРОСТЬ@@

## 5. Таблица placed_feature × статус (26.3)

@@ТАБЛИЦА@@

## 6. Ограничения и что осталось

@@ОГРАНИЧЕНИЯ@@

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

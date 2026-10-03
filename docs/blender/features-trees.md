# W11 «Декорации: деревья, грибы-деревья, фунги» (ворота G5, группа G5-trees)

Отчёт потока W11 аддона «MC Worldgen». Каркас стадии FEATURES — `docs/blender/features.md` (W8); здесь только то, что добавлено для деревьев. Все числа измерены на настоящем
сервере (эталоны `run/gt`, делает W6); команды воспроизведения — в §8. Версия по умолчанию — 26.3.

## 1. Итог

* Реализованы типы фич `minecraft:tree` (все 10 стволов, 12 листвы, 11 украшателей, корни мангров), `minecraft:fallen_tree`, `minecraft:huge_red_mushroom`,
  `minecraft:huge_brown_mushroom`; `minecraft:huge_fungus` и `minecraft:root_system` (азалия) написаны мной и независимо W10 (`feature_veg.c`) — результаты **побитово совпали**
  (одинаковые числа расхождений на crimson_fungi/warped_fungi/rooted_azalea_tree), активна версия W10 (регистрируется раньше), моя — запасная.
* **Одиночные деревья** (`custom:<имя>`, одна настроенная фича с цепочкой `rarity_filter 2 → in_square → surface_water_depth_filter → heightmap OCEAN_FLOOR → would_survive`):
  **45 из 45** настроенных фич 26.3 — 100,000000 % при margin 1 (внутри области, без края); при margin 0 38 из 45 — 100,000000 %, 7 (все poplar-варианты) — 4 блока
  подземной воды `air → water` на краю области (y=13, чанк на границе дампа; не дерево, подтверждено сравнением со стадией без фич).
* **Изолированные placed_feature из биомов** (`feature:<id>`, 27 штук, §5): 13 из 27 — ровно 100,000000 % при margin 0 (ещё три — ≥ 99,9985 %: `trees_birch` 1 блок, `trees_sparse_jungle` 4, `trees_badlands` 175); остальные ограничены
  **недетерминизмом самой игры** (порядок чанков у плотных лесов): при margin 2 с маской `--stable-with` (повторные миры игры) — 100,000000 % у `trees_old_growth_pine_taiga`,
  `trees_flower_forest`, `dark_forest_vegetation`, `trees_grove`, `mushroom_island_vegetation`, а в повторных областях — `trees_swamp`, `trees_savanna`, `trees_snowy` (margin 0 и 2 без масок);
  `trees_cherry` (72 блока из 4,8 млн при margin 2 с маской) и `trees_old_growth_spruce_taiga` (274); остальное — на уровне шума самой игры (§6).
* **В сборе** (`features`, все декорации 0x1f, Overworld r=10, margin 2, маска rep1/rep2): деревья/грибы-деревья дают 0,042 % блоков расхождений (seed 12345) и 0,112 % (seed 8675309,
  густой лес), Nether (seed 12345) — 0,079 %; в общем балле G5 (99,59–99,63 %) доминируют не деревья, а лёд/снег/мох (W12).
* Скорость: дерево — около 2700 деревьев/с на ядро (изолированный `trees_jungle`, 733 попытки: 0,27 с на 1156 чанков одним потоком); `features` целиком 25×25 чанков с рельефом — 1,9 с.
* Версии (одиночные деревья `custom:`, поднабор из 21–23 файлов на версию, margin 0 / margin 1): **26.3** — 45 из 45 (7 poplar по 4 блока воды / 45 из 45);
  **26.4-snapshot-2** — 23 из 23 по margin 1 (по margin 0 — poplar по 4 блока воды); **26.1** — 19 из 21 ровно 100,000000 % (`oak_leaf_litter` 23 блока, `birch_bees_0002_leaf_litter` 13 блоков — подстилка двух
  соседних деревьев, §6.6); **26.2** — 17 из 21 (те же две подстилки 23 блока, `mega_spruce` 9 блоков расстояния листвы, `tall_mangrove` 235 блоков лоз — пересечение крон соседей, §6.6). Фичи из биомов
  на 26.2 (без повторов игры): `mushroom_island_vegetation` 100,000000 %, `dark_forest_vegetation` margin 2 — 0, `trees_old_growth_pine_taiga` margin 0 — 702, margin 2 — 6 блоков; `trees_jungle` 99,33 % / margin 2 99,23 %
  и `crimson_fungi` 99,68 % (шума игры нет в наличии). Различия кода между версиями учтены: `radiusOffset`→`radiusOffsetXZ` + `foliageHeightOffset` = 0 везде, `root_system` — проверки `level_test_distance`
  и «высота WORLD_SURFACE» только с 26.2, **26.4-snapshot-2: `StraightTrunkPlacer` получил `trunk_width` (ствол шире 1, угол «северо-запад», `placeBelowTrunkBlock` убран)** — без этого 26.4 расходился на
  36…3583 блока на дерево (`oak`, `birch`, `jungle_tree`, `pine`, `swamp_oak`), после — 0. Реализовано по `src/dec/26.4-snapshot-2`.

## 2. Где код

| файл | что |
|---|---|
| `libmcgen/src/feature_tree.h` | внутренний интерфейс группы: `JSet` (java.util.HashSet<BlockPos>), конфигурации (`TreeCfg`, `TrunkCfg`, `FoliageCfg`, `RootCfg`, `TreeDeco`), `TreeRun`, `TreeCtx` |
| `libmcgen/src/feature_tree.c` | `TreeFeature.place` (двухфазность), `updateLeaves` (BFS расстояния), разбор `tree`, `FeatureSize`, `fallen_tree`, регистрация `feature_register_trees()` |
| `libmcgen/src/feature_tree_placers.c` | `TrunkPlacer` (straight, forking, giant, mega_jungle, dark_oak, fancy, bending, upwards_branching, cherry, poplar), `FoliagePlacer` (blob, bush, fancy, spruce, pine, acacia, jungle/mega_jungle, mega_pine, dark_oak, random_spread, cherry, poplar), `RootPlacer` (mangrove) |
| `libmcgen/src/feature_tree_deco.c` | `TreeDecorator` (beehive, cocoa, leave_vine, trunk_vine, alter_ground, attached_to_leaves, attached_to_logs, place_on_ground, pale_moss, creaking_heart, shelf_mushroom), `StructureTemplate.updateShapeAtEdge` |
| `libmcgen/src/feature_mushroom.c` | `huge_red_mushroom`, `huge_brown_mushroom`; запасные `huge_fungus`, `root_system` (регистрируются, только если W10 не зарегистрировал); `feature_register_mushroom()` |
| `libmcgen/tests/g5_tree_*.{py,sh}`, `libmcgen/tests/g5_jset/` | тесты и диагностика (§8), проверка `JSet` против JDK |
| `tools/gt/custom/*.json` | описания одиночных деревьев для вариантов эталона `custom:<имя>` (45 файлов; генератор `libmcgen/tests/g5_tree_custom_gen.py`) |

Правки общих файлов (точечные): `feature.c` — две строки регистрации групп и переменная `MCGEN_FEATURES_RING`; `placement.c` — трассировка попыток `MCGEN_TRACE_ATT` (отладка).
ABI `mcgen.h` не менялся.

## 3. Как устроено (что неочевидно)

### 3.1. `TreeFeature.place`: фазы и множества
Фаза 1: стволы/корни/листва ставятся `setBlock(…, 19)` прямо в окно 3×3 чанков, позиции копятся в четырёх `HashSet<BlockPos>` (корни, стволы, листва, украшения); фаза 2 (если есть
стволы или листва): украшатели (`TreeDecorator.Context`: списки `logs/leaves/roots` — `ObjectArrayList(set)` и **стабильная сортировка по Y**), затем `updateLeaves` и
`StructureTemplate.updateShapeAtEdge(level, 3, shape, bounds.min)` — всё в рамке `BoundingBox.encapsulatingPositions(корни ∪ стволы ∪ листва ∪ украшения)`.

**Порядок обхода `HashSet`** определяет и порядок вызовов ГСЧ в украшателях (`leave_vine` — по листьям, `attached_to_leaves` — перемешивание копии), и порядок обработки в BFS, поэтому
`JSet` воспроизводит `java.util.HashMap` буквально (сверка с настоящим `java.util.HashSet<BlockPos>` на 20 случайных последовательностях add/pop-first до 20 000 операций — `libmcgen/tests/g5_jset/run.sh`, 20 из 20 побитно): хэш `BlockPos.hashCode() = (y + z·31)·31 + x`, `h ^ (h >>> 16)`, таблица 16 → удвоение при 0,75, разделение корзин `(hash & oldCap)`,
порядок в корзине — порядок вставки, итератор — по возрастанию корзин. «Деревянные корзины» (≥ 8 в цепочке при таблице ≥ 64) не воспроизведены; счётчик `treeified` на всех тестовых мирах
равен 0.

### 3.2. `updateLeaves` и «дубликаты» в BFS
Игра ведёт 7 множеств `toCheck[0..6]`; позиция может попасть в `toCheck[d+1]`, не будучи ещё извлечённой из `toCheck[d]`, и при повторном извлечении **перезаписывается большим
расстоянием** (так в игре; тики листвы потом исправляют это, но эталон снимается при `tick freeze`). Алгоритм перенесён без «исправлений»: извлечение — `iterator().next()` по `HashSet`,
значение `distance` — `Math.min(текущее distance соседа, d + 1)` с учётом `prevents_nearby_leaf_decay` (=0) и свойства `distance` (нет свойства — блок не листва).

### 3.3. `updateShapeAtEdge`
Обход граней формы по трём осям (`DiscreteVoxelShape.forAllFaces`, порядок: Z, затем Y, затем X), для обеих сторон грани — `updateShape` только для классов, которые реально меняются
у деревьев: `vine` (пересчёт граней по `canAttachTo`), `cocoa` (`supports_cocoa`), `shelf_mushroom` (опорная грань); листва лишь планирует тики (состояние не меняется).

### 3.4. Прочее
* `TrunkPlacer.isFree` — виртуальный: у `upwards_branching` (мангры) добавляется `can_grow_through`.
* Мангровые корни: `simulateRoots` рекурсивно собирает позиции, провал любого направления отменяет дерево целиком (после того как ГСЧ уже израсходован — как в игре);
  `placeRoot` сначала проверяет «илистые» блоки (`muddy_roots_in`), затем `canPlaceRoot` (`replaceable_by_trees` или `can_grow_through`) и «верхний» корень.
* `huge_*_mushroom`: `getTreeRadiusForHeight(−1, −1, r, y)` у красного гриба всегда 0 (в `isValidPosition` игра зовёт его с `treeHeight = −1`) — проверка места только по столбику.
* `fallen_tree`: пень `placeLogBlock` + `markAboveForPostProcessing`, затем бревно; `HashSet` порядка для украшателей (грибы-полки, лозы).
* Бревно упавшего дерева и `shelf_mushroom` различают «лежащее» по равенству Y первого и последнего элемента отсортированного списка.

## 4. Реестр типов (26.3; в скобках — число настроенных фич, использующих тип)

| группа | типы |
|---|---|
| стволы | straight (19), fancy (6), poplar (6), dark_oak (5), giant (2), upwards_branching (2), cherry (2), mega_jungle (1), bending (1), forking (1) |
| листва | blob (16), fancy (6), poplar (6), dark_oak (5), random_spread (3), mega_pine (2), cherry (2), jungle (1), acacia (1), pine (1), spruce (1), bush (1) |
| украшатели | place_on_ground (20), beehive (16), shelf_mushroom (6), leave_vine (5), pale_moss (2), trunk_vine (2), alter_ground (2), attached_to_leaves (2), cocoa (1), creaking_heart (1), attached_to_logs (в fallen_tree) |
| корни | mangrove_root_placer (2) |
| размеры | two_layers_feature_size (40), three_layers_feature_size (5), `min_clipped_height` |
| фичи | tree (45), fallen_tree (6), huge_red_mushroom (1), huge_brown_mushroom (1), huge_fungus (4), root_system (2: азалия, `rooted_sulfur_spring` — W12) |

Не реализовано: `PaleMossDecorator` вызывает `pale_moss_patch` (`vegetation_patch`, W10) — если тип отсутствует, патч молча пропускается (в 26.3 W10 реализовал; `pale_oak` 100 %).

## 5. Таблица «placed_feature × статус»

«Статус» = «изоляция `feature:<id>` (стадии 0x17, фильтр `MCGEN_FEATURES_ONLY`)»; margin N — N крайних чанков области дампа не сравниваются (там действует каскад порядка чанков игры, §6);
«маска rep» — клетки, где эталон не совпал с повторным прогоном игры (`_rep1`, `_rep2`), не считаются; «шум игры» — число блоков, которыми игра расходится сама с собой (ref↔rep1, margin 0).

| placed_feature | изм. | seed | блоков | расх. margin 0 | % margin 0 | расх. margin 2 | расх. margin 2 + маска rep | шум игры ref↔rep1 |
|---|---|---|---:|---:|---:|---:|---:|---:|
| `birch_tall` | overworld | 8675309 | 11 894 754 | 0 | 100,000000 | 0 | — | — |
| `trees_birch` | overworld | 8675309 | 11 894 754 | 1 | 99,999992 | 0 | — | — |
| `trees_badlands` | overworld | 8675309 | 11 894 741 | 175 | 99,9985 | 0 | — | — |
| `trees_taiga` | overworld | -7048155917072976836 | 11 894 778 | 2 100 | 99,9823 | 1 205 | — | нет rep |
| `trees_old_growth_pine_taiga` | overworld | -7048155917072976836 | 11 894 761 | 6 704 | 99,9436 | 2 311 | **0** | 6 811 |
| `trees_old_growth_spruce_taiga` | overworld | -7048155917072976836 | 11 894 761 | 0 | 100,000000 | 0 | — | — |
| `trees_jungle` | overworld | 12345 | 11 894 783 | 51 999 | 99,5628 | 15 439 | 1 405 | 103 676 |
| `trees_sparse_jungle` | overworld | 8675309 | 11 894 777 | 4 | 99,99997 | 0 | — | — |
| `trees_savanna` | overworld | 8675309 | 11 894 778 | 0 | 100,000000 | 0 | — | — |
| `trees_swamp` | overworld | 12345 | 11 894 784 | 0 | 100,000000 | 0 | — | — |
| `trees_mangrove` | overworld | -7048155917072976836 | 11 894 767 | 273 203 | 97,7032 | 107 392 | 61 280 | 171 668 |
| `trees_cherry` | overworld | 8675309 | 11 894 778 | 0 | 100,000000 | 0 | — | — |
| `trees_flower_forest` | overworld | 12345 | 11 894 781 | 1 444 | 99,9879 | 1 197 | **0** | 2 215 |
| `trees_birch_and_oak_leaf_litter` | overworld | 8675309 | 11 894 754 | 32 625 | 99,7257 | 14 475 | 2 311 | 30 836 |
| `dark_forest_vegetation` | overworld | 12345 | 11 894 767 | 6 259 | 99,9474 | **0** | **0** | 124 |
| `trees_snowy` | overworld | -7048155917072976836 | 11 894 781 | 0 | 100,000000 | 0 | — | — |
| `trees_windswept_hills` | overworld | 12345 | 11 894 784 | 0 | 100,000000 | 0 | — | — |
| `trees_grove` | overworld | -7048155917072976836 | 11 894 775 | 2 009 | 99,9831 | 350 | **0** | 2 752 |
| `trees_meadow` | overworld | 8675309 | 43 352 024 | 0 | 100,000000 | 0 | — | — |
| `trees_dappled_forest` | overworld | 12345 | 11 894 775 | 19 567 | 99,8355 | 13 070 | 5 686 | 10 203 |
| `trees_water` | overworld | -7048155917072976836 | 11 894 770 | 0 | 100,000000 | 0 | — | — |
| `trees_plains` | overworld | 8675309 | 11 894 778 | 0 | 100,000000 | 0 | — | — |
| `trees_windswept_savanna` | overworld | -7048155917072976836 | 11 894 784 | 0 | 100,000000 | 0 | — | — |
| `mushroom_island_vegetation` | overworld | 8675309 | 11 894 774 | 0 | 100,000000 | 0 | 0 | 0 |
| `rooted_azalea_tree` | overworld | 12345 | 11 894 767 | 0 | 100,000000 | 0 | — | — |
| `crimson_fungi` | nether | 8675309 | 7 929 856 | 24 698 | 99,6885 | 9 154 | 8 435 | 14 343 |
| `warped_fungi` | nether | 12345 | 7 929 856 | 16 416 | 99,7930 | 4 424 | — | нет rep |

Повторные области (`tools/gt/features_plan_trees2.json`): области плана для семи селекторов оказались «вакуумными» (в эталоне нет ни одного ствола/листа: `trees_swamp`, `trees_savanna`,
`trees_cherry`, `trees_snowy`, `trees_old_growth_spruce_taiga`, `trees_water`, `rooted_azalea_tree` (0,0) — результат 100 % ничего не доказывает). Для пяти подобраны области целиком в нужном
биоме (`libmcgen/tests/g5_tree_areas.py`), W6 снял эталоны и повторы:

| placed_feature (область 2) | изм. | seed (центр) | блоков | расх. margin 0 | расх. margin 2 | расх. margin 2 + маска rep | шум игры ref↔rep1 | деревьев-блоков (листва+бревно) эталон/наше |
|---|---|---|---:|---:|---:|---:|---:|---|
| `trees_swamp` | overworld | 8675309 (−64,238) | 11 894 779 | 101 | **0** | **0** | 101 | 17 257 / 17 257 |
| `trees_savanna` | overworld | 8675309 (47,133) | 11 894 782 | **0** | **0** | **0** | 0 | 6 048 / 6 048 |
| `trees_snowy` | overworld | 12345 (192,155) | 11 894 784 | **0** | **0** | **0** | 0 | 521 / 521 |
| `trees_cherry` | overworld | −7048155917072976836 (25,61) | 11 894 784 | 3 978 | 732 | 72 | 15 663 | 60 171 / 60 146 |
| `trees_old_growth_spruce_taiga` | overworld | 8675309 (−202,−231) | 11 894 780 | 4 426 | 2 803 | 274 | 7 917 | 53 426 / 53 844 |

Остаётся вакуумным `trees_water` (деревья в океанских биомах не растут).

Дополнительно:
* `rooted_azalea_tree` с карверами (`featurecarve:`, стадии 0x1f): seed 12345 (24,3) r=5 — **100,000000 %**, при этом 127 `azalea_leaves` + 51 `flowering_azalea_leaves` + 13 `hanging_roots` + 2033 `rooted_dirt`
  в эталоне и у нас поровну (область (0,0) пуста и в эталоне — тест вакуумный; (13,−33) — 100 %, деревьев нет). Корни/листва азалии на 100 % — в обоих.
* Не присутствуют в биомах как отдельные placed_feature, но проверены как одиночные деревья (`custom:`): `oak`, `fancy_oak`, `birch`, `spruce`, `pine`, `acacia`, `dark_oak`, `jungle_tree`, `jungle_bush`,
  `mega_jungle_tree`, `mega_pine`, `mega_spruce`, `cherry`, `mangrove`, `tall_mangrove`, `swamp_oak`, `pale_oak`, `pale_oak_creaking`, `*_bees_*`, `*_leaf_litter`, `orange/red/yellow_poplar*`,
  `fallen_*` — всё 100,000000 % (margin 1), 7 poplar-вариантов при margin 0 — те же 4 блока воды (подземная вода на краю области, не относится к деревьям).
* Не реализовано/не моё: `rooted_sulfur_spring` (`root_system` + `sulfur_spring`, W12), `desert_well` (W12), `bamboo`/`vines` (W10).

## 6. Недетерминизм игры и ограничения (измерено)

1. **Порядок чанков у плотных лесов.** Фичи соседних чанков пересекаются (кроны, стволы, подстилка), и результат зависит от порядка декорации чанков. Одна неудача/удача дерева меняет
   число вызовов ГСЧ **всего остатка потока чанка** (ГСЧ — один на фичу и чанк): после первого расхождения все следующие попытки в чанке расходятся (последовательности «HHH T TTT» по чанкам;
   `libmcgen/tests/g5_tree_assembly.py`). Пример: первая попытка чанка (−7,−7) в `features` seed 8675309 совпала по (x,z) с эталоном, но высота `OCEAN_FLOOR` у нас вышла 75 (кроной соседа,
   декорированного раньше), а у игры — 71.
2. **Сама игра не воспроизводима:** в `features` seed 8675309 из 68 017 клеток «ствол/листва» области 17×17 чанков два прогона игры различаются в 27 877 (41 %) — наша ошибка
   в этой метрике 58 081 (пересечение с эталоном 54 % против 79 % между прогонами игры). Порядки обхода чанков, которые мы пробовали (метрика — число клеток «ствол/листва», где мы
   не совпадаем с эталоном; чем меньше, тем лучше): x внешний/z внутренний по возрастанию (по умолчанию) 58 081; z внешний/x внутренний 61 437; смешанные направления 69–72 тыс.;
   обратные 74–76 тыс.; от центра к краю 65 835, от края к центру 64 667, порядок окон forceload 57 591, порядок обхода хэш-таблицы fastutil (по ключу `ChunkPos.asLong`, 10 вариантов)
   64–70 тыс., кольцо декорации 3 (59 128). Лучший — «по возрастанию x, затем z» (по умолчанию); остальные не лучше. Попарно игра то опережает, то отстаёт: в трёх разобранных случаях
   (`trees_birch_and_oak_leaf_litter` чанки (−7,−7)/(−8,−7), (−7,−5)/(−7,−4) и `trees_cherry` (27,59)/(27,60)) кроны соседа с большим x или z уже стояли, когда чанк с меньшими
   координатами делал первую попытку: высота `OCEAN_FLOOR` на кроне даёт `would_survive` = false и попытка отсеивается *без* вызовов ГСЧ дерева, поток сдвигается на 2·k вызовов
   (подтверждено подбором сдвига: пропуск одной отсеянной попытки возвращает следующие две позиции в точности на стволы эталона). Глобального порядка, воспроизводящего эти пары, нет —
   он определяется планировщиком потоков игры.
3. Эталон — один прогон. Там, где игра детерминирована (`dark_forest_vegetation`: ref↔rep1 124 блока), мы совпадаем **полностью внутри области** (margin 2: 0 расхождений из 4,8 млн);
   расхождения margin 0/1 — каскад от краевых чанков, которые игра декорирует вместе с кольцом r+1…r+3, а мы — с кольцом 1 (`MCGEN_FEATURES_RING=N` увеличивает: dark_forest margin 0
   6 180 → 4 843 при N=3, стоимость — (n+2N)² чанков).
4. `trees_mangrove`, `crimson_fungi`, `trees_dappled_forest`: наша ошибка в 1,0–1,6 раза от шума игры; `trees_jungle`: меньше шума (52 000 против 104 000).
5. Блоки BlockEntity (улей: число пчёл `2 + nextInt(2)`, затем `nextInt(599)` на пчелу) в данные чанка не пишутся — расходуется только ГСЧ (число улья совпало: 8 из 8 в `trees_flower_forest`).
6. Порядок декорации соседних чанков в **26.1/26.2** (другой планировщик, чем в 26.3: шаги `noise → surface → carvers → features` вместо `terrain → features`): при разреженных деревьях у пары
   соседних чанков порядок «z по убыванию» даёт 0 расхождений там, где «z по возрастанию» — 9…235 блоков (`mega_spruce` 9→0, `tall_mangrove` 235→0, подстилка 23→6…8); но на плотных лесах (`trees_old_growth_pine_taiga`
   26.2: 702 против 26 373 блоков, `trees_jungle` 79 тыс. против 175 тыс.) лучше обычный «по возрастанию». Единого порядка нет — это тот же недетерминизм планировщика (§6.2). Остатки одиночных деревьев на 26.1/26.2 — только
   такие пересечения соседних крон и подстилки.
7. `PaleMossDecorator`, зависящий от `pale_moss_patch`, работает только при реализованном `vegetation_patch` (W10).

## 7. Скорость

Изолированный `trees_jungle` (1156 чанков, 733 попытки; 12 ядер): декорация 0,27 с при одном потоке (4233 чанка/с) и 1,1 с при 12 потоках (накладные расходы волн каркаса W8 на лёгкой фиче);
`features` Overworld 25×25 чанков (все фичи 26.3, рельеф, поверхность, 12 ядер): 1,9 с. Временные `JSet`/списки — пул на поток (`_Thread_local`), аллокаций в горячем пути нет.

## 8. Как воспроизвести

```bash
make -C libmcgen
python3 libmcgen/tests/g5_features.py --only trees_birch,birch_tall                  # ворота G5i (margin 0), как у остальных групп
libmcgen/tests/g5_tree_all.sh 0 2                                                      # сводка по всем мирам деревьев (STABLE=1 — с маской rep; V=26.2 — версия; FEATS=… — список)
python3 libmcgen/tests/g5_tree_custom.py [имя …] [--version 26.3] [--margin 1]         # одиночные деревья custom:<имя>
python3 libmcgen/tests/g5_tree_table.py --out libmcgen/tests/results/g5-trees-26.3.json   # таблица §5 (~15 минут)
python3 libmcgen/tests/g5_tree_noise.py trees_mangrove --margin 0                      # шум игры против нашей ошибки (ours↔ref, ours↔rep1, ref↔rep1)
python3 libmcgen/tests/g5_tree_assembly.py --seed 8675309 --stable                     # деревья в сборе (features 0x1f)
python3 libmcgen/tests/g5_tree_chunk.py <мир> [cx cz] --margin 1                       # расхождения по чанкам / блоки чанка
python3 libmcgen/tests/g5_tree_probe.py <мир> <cx> <cz>                                # трасса деревьев чанка и столбцы эталона
```
Отладочные переменные: `MCGEN_FEATURES_ONLY`, `MCGEN_TREE_TRACE=1` (строки `TREE chunk(..) at (x,y,z) ok res trunks foliage decor why`), `MCGEN_TRACE_ATT=1` (каждая попытка placed_feature,
`ATT chunk(..) id (x,y,z) -> 0/1`), `MCGEN_FEATURES_RING=N`, `MCGEN_FEATURES_SEQ=xz|zx|…`.
Эталоны: `tools/gt/custom/*.json` читает `gen_queue.py --variants custom --plan custom`; `g5_tree_custom.py` строит «оверлей-пак» `run/gt/_tree_overlay/<V>` (биомы с одним шагом 9 = все `gt_custom_*`).

## 9. Что осталось

* Вакуумные эталоны (§5) заменены областями `features_plan_trees2.json`; осталась `trees_water`.
* Для 26.1 и 26.4-snapshot-2 нет `feature:`-миров плотных лесов (только одиночные `custom:`), для 26.2 — пять (`trees_jungle`, `trees_old_growth_pine_taiga`, `dark_forest_vegetation`, `crimson_fungi`,
  `mushroom_island_vegetation`) без повторов игры; запрос к W6: `gen_queue.py --version <V> --variants feature --plan-file tools/gt/features_plan_trees2.json` и `--tag rep1`.
* Порядок декорации чанков игры (§6) — единственный крупный источник расхождений деревьев; без модели планировщика игры выше этих цифр не поднять.
* Мир `rooted_sulfur_spring` (26.2+) — после `sulfur_spring` (W12).

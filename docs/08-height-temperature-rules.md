# 08. Каталог правил, зависящих от высоты, температуры и уровней (26.3) — генерируется автоматически

Источник — **все** данные датапака и Java-код версии; скрипт `tools/audit_height_rules.py`, сырые данные `data/height-rules-26.3.json`. Это полный перечень «если y … / если температура … / если вода …» из `placed_feature`, `material_rule`, `density_function`, `noise_settings`, `carver`, `structure`, `biome` и скан Java-классов. Пояснения к механикам — `docs/00-worldgen-guide.md` §10–15.

Обозначения высот: `y=N` — абсолютная; `низ±N` — от нижней границы мира (`above_bottom`); `верх∓N` — от верхней (`below_top`). Overworld: min_y −64, высота 384 (низ = −64, верх = 320); Nether/End: min_y 0, высота 128 (верх = 128).

## 1. Базовые уровни по измерениям (`noise_settings`)

| noise_settings | sea_level | min_y | высота | основной блок | основная жидкость | legacy RNG |
|---|---|---|---|---|---|---|
| `amplified` | 63 | -64 | 384 | stone | water | False |
| `caves` | 32 | -64 | 192 | stone | water | True |
| `end` | 0 | 0 | 128 | end_stone | air | True |
| `floating_islands` | -64 | 0 | 256 | stone | water | True |
| `large_biomes` | 63 | -64 | 384 | stone | water | False |
| `nether` | 32 | 0 | 128 | netherrack | lava | True |
| `overworld` | 63 | -64 | 384 | stone | water | False |

Уровни жидкости в коде: Overworld — глобально `y < min(−54, sea_level)` → лава, иначе вода до sea_level (aquifer; `NoiseBasedChunkGenerator.createFluidPicker`); Nether — лава ниже y = 32 (`default_fluid = lava`, sea_level 32).

## 2. y-зависимые узлы функций рельефа/пещер/aquifer

Узлы density-функций, в которых явно участвует координата y (градиенты, `range_choice` по y, срезы). Включая `noise_router` и `aquifers` каждого `noise_settings`.

### 2.1. По `noise_settings`

* **`caves`**
  * градиент по y: y=-72→0.0, y=-40→1.0 (вне — константа)
  * градиент по y: y=104→1.0, y=128→0.0 (вне — константа)
* **`end`**
  * градиент по y: y=4→0.0, y=32→1.0 (вне — константа)
  * градиент по y: y=56→1.0, y=312→0.0 (вне — константа)
* **`floating_islands`**
  * градиент по y: y=184→1.0, y=440→0.0 (вне — константа)
  * градиент по y: y=4→0.0, y=32→1.0 (вне — константа)
* **`nether`**
  * градиент по y: y=-8→0.0, y=24→1.0 (вне — константа)
  * градиент по y: y=104→1.0, y=128→0.0 (вне — константа)

### 2.2. По отдельным функциям (`density_function/*`)

| Функция | y-узлы |
|---|---|
| `end/islands` | срез по y=0 |
| `overworld/caves/entrances` | градиент по y: y=-10→0.3, y=30→0.0 (вне — константа) |
| `overworld/caves/noodle` | если y ∈ [-60.0; 321.0) → вариант А, иначе вариант Б |
| `overworld/caves/spaghetti_2d` | градиент по y: y=-64→8.0, y=320→-40.0 (вне — константа) |
| `overworld/depth` | градиент по y: y=-64→1.5, y=320→-1.5 (вне — константа) |
| `overworld/final_density` | градиент по y: y=-64→0.0, y=-40→1.0 (вне — константа); градиент по y: y=240→1.0, y=256→0.0 (вне — константа) |
| `overworld/ore_vein/copper_density` | если y ∈ [0.0; 50.0) → вариант А, иначе вариант Б |
| `overworld/ore_vein/iron_density` | если y ∈ [-60.0; -8.0) → вариант А, иначе вариант Б |
| `overworld/ore_vein/mask` | если y ∈ [-64.0; 57.0) → вариант А, иначе вариант Б |
| `overworld/ore_vein/toggle` | если y ∈ [-64.0; 57.0) → вариант А, иначе вариант Б |
| `overworld/preliminary_surface_level` | градиент по y: y=-64→0.0, y=-40→1.0 (вне — константа); градиент по y: y=-64→1.5, y=320→-1.5 (вне — константа); градиент по y: y=240→1.0, y=256→0.0 (вне — константа); поиск верха поверхности: шаг 8, нижняя граница -64 |
| `overworld_amplified/depth` | градиент по y: y=-64→1.5, y=320→-1.5 (вне — константа) |
| `overworld_amplified/final_density` | градиент по y: y=-64→0.0, y=-40→1.0 (вне — константа); градиент по y: y=304→1.0, y=320→0.0 (вне — константа) |
| `overworld_amplified/preliminary_surface_level` | градиент по y: y=-64→0.0, y=-40→1.0 (вне — константа); градиент по y: y=-64→1.5, y=320→-1.5 (вне — константа); градиент по y: y=304→1.0, y=320→0.0 (вне — константа); поиск верха поверхности: шаг 8, нижняя граница -64 |
| `overworld_large_biomes/depth` | градиент по y: y=-64→1.5, y=320→-1.5 (вне — константа) |
| `overworld_large_biomes/final_density` | градиент по y: y=-64→0.0, y=-40→1.0 (вне — константа); градиент по y: y=240→1.0, y=256→0.0 (вне — константа) |
| `overworld_large_biomes/preliminary_surface_level` | градиент по y: y=-64→0.0, y=-40→1.0 (вне — константа); градиент по y: y=-64→1.5, y=320→-1.5 (вне — константа); градиент по y: y=240→1.0, y=256→0.0 (вне — константа); поиск верха поверхности: шаг 8, нижняя граница -64 |
| `y` | градиент по y: y=-4064→-4064.0, y=4062→4062.0 (вне — константа) |

## 3. Правила поверхности и материала (развёрнутые «условия → результат»)

Полный разбор `material_rule/*` с раскрытием всех ссылок (26.3; в 26.1/26.2 то же в `SurfaceRules`). Порядок строк = приоритет (первое сработавшее правило выигрывает внутри своей `sequence`). Условия по высоте: `y ≥/< якорь`, `градиент y`, `вода(offset)` (под/над водой), `stone_depth` (глубина от поверхности), `steep` (крутой склон), `биом∈`, `шум∈` (пятна по шуму), `above_preliminary_surface`.

### 3.1. `overworld` (114 правил)

```
если градиент y низ+0→низ+5 [bedrock_floor]
    → bedrock
→ ore_vein {"density": "minecraft:overworld/ore_vein/copper_density", "filler_block": "minecraft:gran
→ ore_vein {"density": "minecraft:overworld/ore_vein/iron_density", "filler_block": "minecraft:tuff",
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[-0.909; -0.5454]
    → coarse_dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[-0.1818; 0.1818]
    → coarse_dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[0.5454; 0.909]
    → coarse_dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
    → dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈swamp
  И y ≥ y=62
  И НЕ(y ≥ y=63)
  И шум surface_swamp∈[0.0; 1.7976931348623157e+308]
    → water
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈mangrove_swamp
  И y ≥ y=60
  И НЕ(y ≥ y=63)
  И шум surface_swamp∈[0.0; 1.7976931348623157e+308]
    → water
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=256
    → orange_terracotta
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[-0.909; -0.5454]
    → terracotta
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[-0.1818; 0.1818]
    → terracotta
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[0.5454; 0.909]
    → terracotta
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
    → bandlands {}
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → red_sandstone
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
    → red_sand
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И НЕ(hole)
    → orange_terracotta
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_under_deep_water{вода(offset=-6,×-1)}
    → white_terracotta
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
    → gravel
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И y ≥ y=63 (+-1×глубина слоя) [+stone_depth]
  И y ≥ y=63
  И НЕ(y ≥ y=74 (+1×глубина слоя) [+stone_depth])
    → orange_terracotta
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И y ≥ y=63 (+-1×глубина слоя) [+stone_depth]
    → bandlands {}
если above_preliminary_surface
  И биом∈badlands,eroded_badlands,wooded_badlands
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И not_under_deep_water{вода(offset=-6,×-1)}
    → white_terracotta
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_ocean,deep_frozen_ocean
  И hole
    → air
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И steep
    → packed_ice
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И шум packed_ice∈[0.0; 0.2]
    → packed_ice
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И шум ice∈[0.0; 0.025]
    → ice
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И steep
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И шум powder_snow∈[0.35; 0.6]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈jagged_peaks
  И steep
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈jagged_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈grove
  И шум powder_snow∈[0.35; 0.6]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈grove
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_peaks
  И шум calcite∈[-0.0125; 0.0125]
    → calcite
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_peaks
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
    → gravel
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈warm_ocean,beach,snowy_beach
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈warm_ocean,beach,snowy_beach
    → sand
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈desert
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈desert
    → sand
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈dripstone_caves
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈mangrove_swamp
    → mud
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_savanna
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_savanna
  И шум surface∈[-0.06060606060606061; 1.7976931348623157e+308]
    → coarse_dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
    → gravel
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
    → dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
    → gravel
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈old_growth_pine_taiga,old_growth_spruce_taiga
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → coarse_dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈old_growth_pine_taiga,old_growth_spruce_taiga
  И шум surface∈[-0.11515151515151514; 1.7976931348623157e+308]
    → podzol
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈ice_spikes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈mushroom_fields
    → mycelium
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈dappled_forest
  И шум small_patch∈[1.2000000476837158; 1.7976931348623157e+308]
    → coarse_dirt
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
    → dirt
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_ocean,deep_frozen_ocean
  И hole
    → water
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И steep
    → packed_ice
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И шум packed_ice∈[-0.5; 0.2]
    → packed_ice
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И шум ice∈[-0.0625; 0.025]
    → ice
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И steep
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И шум powder_snow∈[0.45; 0.58]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈jagged_peaks
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈grove
  И шум powder_snow∈[0.45; 0.58]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈grove
    → dirt
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_peaks
  И шум calcite∈[-0.0125; 0.0125]
    → calcite
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_peaks
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
    → gravel
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,beach,snowy_beach
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,beach,snowy_beach
    → sand
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈desert
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈desert
    → sand
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈dripstone_caves
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈mangrove_swamp
    → mud
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_savanna
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
    → gravel
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
    → dirt
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
    → gravel
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
    → dirt
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И биом∈warm_ocean,beach,snowy_beach
  И deep_under_floor{stone_depth(floor,offset=0,range=6)}
    → sandstone
если above_preliminary_surface
  И not_under_deep_water{вода(offset=-6,×-1)}
  И биом∈desert
  И very_deep_under_floor{stone_depth(floor,offset=0,range=30)}
    → sandstone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks,jagged_peaks
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,lukewarm_ocean,deep_lukewarm_ocean
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,lukewarm_ocean,deep_lukewarm_ocean
    → sand
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если above_preliminary_surface
  И on_floor{stone_depth(floor,offset=0,range=0)}
    → gravel
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если градиент y y=0→y=8 [deepslate]
    → deepslate
```

### 3.2. `overworld_caves` (115 правил)

```
если НЕ(градиент y верх-5→верх+0 [bedrock_roof])
    → bedrock
если градиент y низ+0→низ+5 [bedrock_floor]
    → bedrock
→ ore_vein {"density": "minecraft:overworld/ore_vein/copper_density", "filler_block": "minecraft:gran
→ ore_vein {"density": "minecraft:overworld/ore_vein/iron_density", "filler_block": "minecraft:tuff",
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[-0.909; -0.5454]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[-0.1818; 0.1818]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[0.5454; 0.909]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
    → dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈swamp
  И y ≥ y=62
  И НЕ(y ≥ y=63)
  И шум surface_swamp∈[0.0; 1.7976931348623157e+308]
    → water
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈mangrove_swamp
  И y ≥ y=60
  И НЕ(y ≥ y=63)
  И шум surface_swamp∈[0.0; 1.7976931348623157e+308]
    → water
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=256
    → orange_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[-0.909; -0.5454]
    → terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[-0.1818; 0.1818]
    → terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[0.5454; 0.909]
    → terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
    → bandlands {}
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → red_sandstone
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
    → red_sand
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И НЕ(hole)
    → orange_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_under_deep_water{вода(offset=-6,×-1)}
    → white_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
    → gravel
если биом∈badlands,eroded_badlands,wooded_badlands
  И y ≥ y=63 (+-1×глубина слоя) [+stone_depth]
  И y ≥ y=63
  И НЕ(y ≥ y=74 (+1×глубина слоя) [+stone_depth])
    → orange_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И y ≥ y=63 (+-1×глубина слоя) [+stone_depth]
    → bandlands {}
если биом∈badlands,eroded_badlands,wooded_badlands
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И not_under_deep_water{вода(offset=-6,×-1)}
    → white_terracotta
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_ocean,deep_frozen_ocean
  И hole
    → air
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И steep
    → packed_ice
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И шум packed_ice∈[0.0; 0.2]
    → packed_ice
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И шум ice∈[0.0; 0.025]
    → ice
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И steep
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И шум powder_snow∈[0.35; 0.6]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈jagged_peaks
  И steep
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈jagged_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈grove
  И шум powder_snow∈[0.35; 0.6]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈grove
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_peaks
  И шум calcite∈[-0.0125; 0.0125]
    → calcite
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_peaks
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
    → gravel
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈warm_ocean,beach,snowy_beach
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈warm_ocean,beach,snowy_beach
    → sand
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈desert
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈desert
    → sand
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈dripstone_caves
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈mangrove_swamp
    → mud
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_savanna
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_savanna
  И шум surface∈[-0.06060606060606061; 1.7976931348623157e+308]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
    → gravel
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
    → dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
    → gravel
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈old_growth_pine_taiga,old_growth_spruce_taiga
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈old_growth_pine_taiga,old_growth_spruce_taiga
  И шум surface∈[-0.11515151515151514; 1.7976931348623157e+308]
    → podzol
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈ice_spikes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈mushroom_fields
    → mycelium
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈dappled_forest
  И шум small_patch∈[1.2000000476837158; 1.7976931348623157e+308]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_ocean,deep_frozen_ocean
  И hole
    → water
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И steep
    → packed_ice
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И шум packed_ice∈[-0.5; 0.2]
    → packed_ice
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И шум ice∈[-0.0625; 0.025]
    → ice
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И steep
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И шум powder_snow∈[0.45; 0.58]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈jagged_peaks
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈grove
  И шум powder_snow∈[0.45; 0.58]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈grove
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_peaks
  И шум calcite∈[-0.0125; 0.0125]
    → calcite
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_peaks
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
    → gravel
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,beach,snowy_beach
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,beach,snowy_beach
    → sand
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈desert
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈desert
    → sand
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈dripstone_caves
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈mangrove_swamp
    → mud
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_savanna
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
    → gravel
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
    → gravel
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И биом∈warm_ocean,beach,snowy_beach
  И deep_under_floor{stone_depth(floor,offset=0,range=6)}
    → sandstone
если not_under_deep_water{вода(offset=-6,×-1)}
  И биом∈desert
  И very_deep_under_floor{stone_depth(floor,offset=0,range=30)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks,jagged_peaks
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,lukewarm_ocean,deep_lukewarm_ocean
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,lukewarm_ocean,deep_lukewarm_ocean
    → sand
если on_floor{stone_depth(floor,offset=0,range=0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
    → gravel
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если градиент y y=0→y=8 [deepslate]
    → deepslate
```

### 3.3. `overworld_floating_islands` (113 правил)

```
→ ore_vein {"density": "minecraft:overworld/ore_vein/copper_density", "filler_block": "minecraft:gran
→ ore_vein {"density": "minecraft:overworld/ore_vein/iron_density", "filler_block": "minecraft:tuff",
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[-0.909; -0.5454]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[-0.1818; 0.1818]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И шум surface∈[0.5454; 0.909]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈wooded_badlands
  И y ≥ y=97 (+2×глубина слоя)
    → dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈swamp
  И y ≥ y=62
  И НЕ(y ≥ y=63)
  И шум surface_swamp∈[0.0; 1.7976931348623157e+308]
    → water
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈mangrove_swamp
  И y ≥ y=60
  И НЕ(y ≥ y=63)
  И шум surface_swamp∈[0.0; 1.7976931348623157e+308]
    → water
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=256
    → orange_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[-0.909; -0.5454]
    → terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[-0.1818; 0.1818]
    → terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
  И шум surface∈[0.5454; 0.909]
    → terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=74 (+1×глубина слоя) [+stone_depth]
    → bandlands {}
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → red_sandstone
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
    → red_sand
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И НЕ(hole)
    → orange_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И not_under_deep_water{вода(offset=-6,×-1)}
    → white_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если биом∈badlands,eroded_badlands,wooded_badlands
  И on_floor{stone_depth(floor,offset=0,range=0)}
    → gravel
если биом∈badlands,eroded_badlands,wooded_badlands
  И y ≥ y=63 (+-1×глубина слоя) [+stone_depth]
  И y ≥ y=63
  И НЕ(y ≥ y=74 (+1×глубина слоя) [+stone_depth])
    → orange_terracotta
если биом∈badlands,eroded_badlands,wooded_badlands
  И y ≥ y=63 (+-1×глубина слоя) [+stone_depth]
    → bandlands {}
если биом∈badlands,eroded_badlands,wooded_badlands
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И not_under_deep_water{вода(offset=-6,×-1)}
    → white_terracotta
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_ocean,deep_frozen_ocean
  И hole
    → air
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И steep
    → packed_ice
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И шум packed_ice∈[0.0; 0.2]
    → packed_ice
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И шум ice∈[0.0; 0.025]
    → ice
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈frozen_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И steep
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И шум powder_snow∈[0.35; 0.6]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈snowy_slopes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈jagged_peaks
  И steep
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈jagged_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈grove
  И шум powder_snow∈[0.35; 0.6]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈grove
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_peaks
  И шум calcite∈[-0.0125; 0.0125]
    → calcite
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_peaks
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
    → gravel
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈stony_shore
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈warm_ocean,beach,snowy_beach
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈warm_ocean,beach,snowy_beach
    → sand
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈desert
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈desert
    → sand
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈dripstone_caves
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈sulfur_caves
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈mangrove_swamp
    → mud
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_savanna
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_savanna
  И шум surface∈[-0.06060606060606061; 1.7976931348623157e+308]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
    → gravel
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
    → dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈windswept_gravelly_hills
    → gravel
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈old_growth_pine_taiga,old_growth_spruce_taiga
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈old_growth_pine_taiga,old_growth_spruce_taiga
  И шум surface∈[-0.11515151515151514; 1.7976931348623157e+308]
    → podzol
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈ice_spikes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈mushroom_fields
    → mycelium
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И биом∈dappled_forest
  И шум small_patch∈[1.2000000476837158; 1.7976931348623157e+308]
    → coarse_dirt
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
  И not_underwater{вода(offset=-1,×0)}
    → grass_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И not_underwater{вода(offset=-1,×0)}
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_ocean,deep_frozen_ocean
  И hole
    → water
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И steep
    → packed_ice
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И шум packed_ice∈[-0.5; 0.2]
    → packed_ice
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И шум ice∈[-0.0625; 0.025]
    → ice
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И steep
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И шум powder_snow∈[0.45; 0.58]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈snowy_slopes
  И not_underwater{вода(offset=-1,×0)}
    → snow_block
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈jagged_peaks
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈grove
  И шум powder_snow∈[0.45; 0.58]
  И not_underwater{вода(offset=-1,×0)}
    → powder_snow
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈grove
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_peaks
  И шум calcite∈[-0.0125; 0.0125]
    → calcite
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_peaks
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
  И шум gravel∈[-0.05; 0.05]
    → gravel
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈stony_shore
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,beach,snowy_beach
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,beach,snowy_beach
    → sand
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈desert
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈desert
    → sand
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈dripstone_caves
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈sulfur_caves
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈mangrove_swamp
    → mud
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_savanna
  И шум surface∈[0.21212121212121213; 1.7976931348623157e+308]
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.24242424242424243; 1.7976931348623157e+308]
    → gravel
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[0.12121212121212122; 1.7976931348623157e+308]
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И шум surface∈[-0.12121212121212122; 1.7976931348623157e+308]
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈windswept_gravelly_hills
    → gravel
если not_under_deep_water{вода(offset=-6,×-1)}
  И under_floor{stone_depth(floor,offset=0,range=0)}
    → dirt
если not_under_deep_water{вода(offset=-6,×-1)}
  И биом∈warm_ocean,beach,snowy_beach
  И deep_under_floor{stone_depth(floor,offset=0,range=6)}
    → sandstone
если not_under_deep_water{вода(offset=-6,×-1)}
  И биом∈desert
  И very_deep_under_floor{stone_depth(floor,offset=0,range=30)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈frozen_peaks,jagged_peaks
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,lukewarm_ocean,deep_lukewarm_ocean
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → sandstone
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warm_ocean,lukewarm_ocean,deep_lukewarm_ocean
    → sand
если on_floor{stone_depth(floor,offset=0,range=0)}
  И on_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → stone
если on_floor{stone_depth(floor,offset=0,range=0)}
    → gravel
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[-0.4000000059604645; -0.10000000149011612]
    → cinnabar
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.0; 0.4000000059604645]
    → sulfur
если биом∈sulfur_caves
  И шум sulfur_cave_gradient∈[0.4000000059604645; 1.7976931348623157e+308]
    → cinnabar
если градиент y y=0→y=8 [deepslate]
    → deepslate
```

### 3.4. `nether` (22 правил)

```
если градиент y низ+0→низ+5 [bedrock_floor]
    → bedrock
если НЕ(градиент y верх-5→верх+0 [bedrock_roof])
    → bedrock
если y ≥ верх-5
    → netherrack
если биом∈basalt_deltas
  И under_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → basalt
если биом∈basalt_deltas
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И шум patch∈[-0.012; 1.7976931348623157e+308]
  И y ≥ y=30 [+stone_depth]
  И НЕ(y ≥ y=35 [+stone_depth])
    → gravel
если биом∈basalt_deltas
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И шум nether_state_selector∈[0.0; 1.7976931348623157e+308]
    → basalt
если биом∈basalt_deltas
  И under_floor{stone_depth(floor,offset=0,range=0)}
    → blackstone
если биом∈soul_sand_valley
  И under_ceiling{stone_depth(ceiling,offset=0,range=0)}
  И шум nether_state_selector∈[0.0; 1.7976931348623157e+308]
    → soul_sand
если биом∈soul_sand_valley
  И under_ceiling{stone_depth(ceiling,offset=0,range=0)}
    → soul_soil
если биом∈soul_sand_valley
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И шум patch∈[-0.012; 1.7976931348623157e+308]
  И y ≥ y=30 [+stone_depth]
  И НЕ(y ≥ y=35 [+stone_depth])
    → gravel
если биом∈soul_sand_valley
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И шум nether_state_selector∈[0.0; 1.7976931348623157e+308]
    → soul_sand
если биом∈soul_sand_valley
  И under_floor{stone_depth(floor,offset=0,range=0)}
    → soul_soil
если on_floor{stone_depth(floor,offset=0,range=0)}
  И НЕ(y ≥ y=32)
  И hole
    → lava
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warped_forest
  И НЕ(шум netherrack∈[0.54; 1.7976931348623157e+308])
  И y ≥ y=31
  И шум nether_wart∈[1.17; 1.7976931348623157e+308]
    → warped_wart_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈warped_forest
  И НЕ(шум netherrack∈[0.54; 1.7976931348623157e+308])
  И y ≥ y=31
    → warped_nylium
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈crimson_forest
  И НЕ(шум netherrack∈[0.54; 1.7976931348623157e+308])
  И y ≥ y=31
  И шум nether_wart∈[1.17; 1.7976931348623157e+308]
    → nether_wart_block
если on_floor{stone_depth(floor,offset=0,range=0)}
  И биом∈crimson_forest
  И НЕ(шум netherrack∈[0.54; 1.7976931348623157e+308])
  И y ≥ y=31
    → crimson_nylium
если биом∈nether_wastes
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И шум soul_sand_layer∈[-0.012; 1.7976931348623157e+308]
  И НЕ(hole)
  И y ≥ y=30 [+stone_depth]
  И НЕ(y ≥ y=35 [+stone_depth])
    → soul_sand
если биом∈nether_wastes
  И under_floor{stone_depth(floor,offset=0,range=0)}
  И шум soul_sand_layer∈[-0.012; 1.7976931348623157e+308]
    → netherrack
если биом∈nether_wastes
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=31
  И НЕ(y ≥ y=35 [+stone_depth])
  И шум gravel_layer∈[-0.012; 1.7976931348623157e+308]
  И y ≥ y=32
    → gravel
если биом∈nether_wastes
  И on_floor{stone_depth(floor,offset=0,range=0)}
  И y ≥ y=31
  И НЕ(y ≥ y=35 [+stone_depth])
  И шум gravel_layer∈[-0.012; 1.7976931348623157e+308]
  И НЕ(hole)
    → gravel
→ netherrack
```

### 3.5. `end` (1 правил)

```
→ end_stone
```

### 3.6. `bedrock_floor` (1 правил)

```
если градиент y низ+0→низ+5 [bedrock_floor]
    → bedrock
```

### 3.7. `bedrock_roof` (1 правил)

```
если НЕ(градиент y верх-5→верх+0 [bedrock_roof])
    → bedrock
```

## 4. Размещаемые фичи: высота, карты высот, температура-зависимые фильтры (все 273)

«Правила» — модификаторы размещения по порядку (`биом` и `в чанке` опущены: они есть у каждой). Категория — эвристика по имени/типу. «Шаги» — шаги декорации биомов, где фича подключена; «Биомов» — сколько биомов её используют.

### 4.1. Незер (12)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `basalt_blobs` | netherrack_replace_blobs | underground_decoration | 1 | ×75; y: равномерно низ+0…верх+0 |
| `basalt_pillar` | overlay | local_modifications | 1 | ×10; y: равномерно низ+0…верх+0; фильтр блока: И(тег#air; НЕ(тег#air)) |
| `blackstone_blobs` | netherrack_replace_blobs | underground_decoration | 1 | ×25; y: равномерно низ+0…верх+0 |
| `brown_mushroom_nether` | simple_block | underground_decoration | 2 | 1/2; y: равномерно низ+0…верх+0; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `crimson_forest_vegetation` | simple_block | vegetal_decoration | 1 | ×6 на каждом слое; фильтр блока: тег#nylium; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `delta` | delta_feature | surface_structures | 1 | ×40 на каждом слое |
| `large_basalt_columns` | weighted_random_selector | surface_structures | 1 | ×2 на каждом слое |
| `nether_sprouts` | simple_block | vegetal_decoration | 1 | ×4 на каждом слое; фильтр блока: тег#nylium; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `red_mushroom_nether` | simple_block | underground_decoration | 2 | 1/2; y: равномерно низ+0…верх+0; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `small_basalt_columns` | weighted_random_selector | surface_structures | 1 | ×4 на каждом слое |
| `underwater_magma` | underwater_magma | underground_ores | 56 | ×U[44..52]; y: равномерно низ+0…y=256; относит. поверхности −∞…-2 (OCEAN_FLOOR_WG) |
| `warped_forest_vegetation` | simple_block | vegetal_decoration | 1 | ×5 на каждом слое; фильтр блока: тег#nylium; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |

### 4.2. Энд (5)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `chorus_plant` | chorus_plant | vegetal_decoration | 1 | ×U[0..4]; карта высот MOTION_BLOCKING |
| `end_gateway_return` | end_gateway | surface_structures | 1 | 1/700; карта высот MOTION_BLOCKING; сдвиг(0,U[3..9],0) |
| `end_island_decorated` | end_island | raw_generation | 1 | 1/14; ×взвеш.(1:3, 2:1); y: равномерно y=55…y=70 |
| `end_platform` | end_platform | top_layer_modification | 1 | fixed_placement {"positions": [[100, 49, 0]]} |
| `end_spike` | end_spike | surface_structures | 1 | — |

### 4.3. вода и океан (15)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `blue_ice` | blue_ice | surface_structures | 2 | ×U[0..19]; y: равномерно y=30…y=61 |
| `iceberg_blue` | iceberg | local_modifications | 2 | 1/200 |
| `iceberg_packed` | iceberg | local_modifications | 2 | 1/16 |
| `kelp_cold` | block_column | vegetal_decoration | 4 | noise_based_count {"noise_factor": 80.0, "noise_to_count_ratio": 120}; карта высот OCEAN_FLOOR; фильтр блока: И(блок∈water; блок∈water; прочная_грань up; НЕ(тег#cannot_support_kelp)) |
| `kelp_warm` | block_column | vegetal_decoration | 2 | noise_based_count {"noise_factor": 80.0, "noise_to_count_ratio": 80}; карта высот OCEAN_FLOOR; фильтр блока: И(блок∈water; блок∈water; прочная_грань up; НЕ(тег#cannot_support_kelp)) |
| `sea_pickle` | simple_block | vegetal_decoration | 1 | 1/16; ×20; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_cold` | weighted_random_selector | vegetal_decoration | 1 | ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_deep` | weighted_random_selector | vegetal_decoration | 1 | ×48; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_deep_cold` | weighted_random_selector | vegetal_decoration | 1 | ×40; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_deep_warm` | weighted_random_selector | vegetal_decoration | 1 | ×80; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_normal` | weighted_random_selector | vegetal_decoration | 1 | ×48; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_river` | weighted_random_selector | vegetal_decoration | 1 | ×48; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_swamp` | weighted_random_selector | vegetal_decoration | 2 | ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `seagrass_warm` | weighted_random_selector | vegetal_decoration | 2 | ×80; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); карта высот OCEAN_FLOOR; фильтр блока: блок∈water |
| `warm_ocean_vegetation` | simple_random_selector | vegetal_decoration | 1 | noise_based_count {"noise_factor": 400.0, "noise_to_count_ratio": 20}; карта высот OCEAN_FLOOR_WG |

### 4.4. деревья и грибы-деревья (75)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `acacia` | tree | — | 0 | фильтр блока: выживет(acacia_sapling) |
| `acacia_checked` | tree | — | 0 | фильтр блока: выживет(acacia_sapling) |
| `birch_bees_0002` | tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `birch_bees_0002_leaf_litter` | tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `birch_bees_002` | tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `birch_checked` | tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `birch_leaf_litter` | tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `cherry_bees_005` | tree | — | 0 | фильтр блока: выживет(cherry_sapling) |
| `cherry_checked` | tree | — | 0 | фильтр блока: выживет(cherry_sapling) |
| `crimson_fungi` | huge_fungus | vegetal_decoration | 1 | ×8 на каждом слое |
| `dark_oak_checked` | tree | — | 0 | фильтр блока: выживет(dark_oak_sapling) |
| `dark_oak_leaf_litter` | tree | — | 0 | фильтр блока: выживет(dark_oak_sapling) |
| `fallen_birch_tree` | fallen_tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `fallen_jungle_tree` | fallen_tree | — | 0 | фильтр блока: выживет(jungle_sapling) |
| `fallen_oak_tree` | fallen_tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `fallen_poplar_tree` | fallen_tree | — | 0 | фильтр блока: выживет(poplar_sapling) |
| `fallen_spruce_tree` | fallen_tree | — | 0 | фильтр блока: выживет(spruce_sapling) |
| `fallen_super_birch_tree` | fallen_tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `fancy_oak_bees` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `fancy_oak_bees_0002_leaf_litter` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `fancy_oak_bees_002` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `fancy_oak_checked` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `fancy_oak_leaf_litter` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `jungle_bush` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `jungle_tree` | tree | — | 0 | фильтр блока: выживет(jungle_sapling) |
| `mangrove_checked` | tree | — | 0 | фильтр блока: выживет(mangrove_propagule) |
| `mega_jungle_tree_checked` | tree | — | 0 | фильтр блока: выживет(jungle_sapling) |
| `mega_pine_checked` | tree | — | 0 | фильтр блока: выживет(spruce_sapling) |
| `mega_spruce_checked` | tree | — | 0 | фильтр блока: выживет(spruce_sapling) |
| `oak` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `oak_bees_0002_leaf_litter` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `oak_bees_002` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `oak_checked` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `oak_leaf_litter` | tree | — | 0 | фильтр блока: выживет(oak_sapling) |
| `orange_poplar` | tree | — | 0 | фильтр блока: выживет(poplar_sapling) |
| `orange_poplar_leaf_litter` | tree | — | 0 | фильтр блока: выживет(poplar_sapling) |
| `pale_oak_checked` | tree | — | 0 | фильтр блока: выживет(pale_oak_sapling) |
| `pale_oak_creaking_checked` | tree | — | 0 | фильтр блока: выживет(pale_oak_sapling) |
| `pine` | tree | — | 0 | фильтр блока: выживет(spruce_sapling) |
| `pine_checked` | tree | — | 0 | фильтр блока: выживет(spruce_sapling) |
| `pine_on_snow` | tree | — | 0 | поиск up ≤8 до: НЕ(блок∈powder_snow); фильтр блока: блок∈snow_block,powder_snow |
| `red_poplar` | tree | — | 0 | фильтр блока: выживет(poplar_sapling) |
| `red_poplar_leaf_litter` | tree | — | 0 | фильтр блока: выживет(poplar_sapling) |
| `rooted_azalea_tree` | root_system | vegetal_decoration | 1 | ×U[1..2]; y: равномерно низ+0…y=256; поиск up ≤12 (путь: тег#air) до: solid; сдвиг(0,-1,0) |
| `spruce` | tree | — | 0 | фильтр блока: выживет(spruce_sapling) |
| `spruce_checked` | tree | — | 0 | фильтр блока: выживет(spruce_sapling) |
| `spruce_on_snow` | tree | — | 0 | поиск up ≤8 до: НЕ(блок∈powder_snow); фильтр блока: блок∈snow_block,powder_snow |
| `super_birch_bees` | tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `super_birch_bees_0002` | tree | — | 0 | фильтр блока: выживет(birch_sapling) |
| `tall_mangrove_checked` | tree | — | 0 | фильтр блока: выживет(mangrove_propagule) |
| `trees_badlands` | random_selector | vegetal_decoration | 1 | ×взвеш.(5:9, 6:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR; фильтр блока: выживет(oak_sapling) |
| `trees_birch` | random_selector | vegetal_decoration | 1 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR; фильтр блока: выживет(birch_sapling) |
| `trees_birch_and_oak_leaf_litter` | random_selector | vegetal_decoration | 1 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_cherry` | tree | vegetal_decoration | 1 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR; фильтр блока: выживет(cherry_sapling) |
| `trees_dappled_forest` | weighted_random_selector | vegetal_decoration | 1 | ×6; глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_flower_forest` | random_selector | vegetal_decoration | 1 | ×взвеш.(6:9, 7:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_grove` | random_selector | vegetal_decoration | 1 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_jungle` | random_selector | vegetal_decoration | 1 | ×взвеш.(50:9, 51:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_mangrove` | random_selector | vegetal_decoration | 1 | ×25; глубина воды ≤ 5; карта высот OCEAN_FLOOR |
| `trees_meadow` | random_selector | vegetal_decoration | 1 | 1/100; глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_old_growth_pine_taiga` | random_selector | vegetal_decoration | 1 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_old_growth_spruce_taiga` | random_selector | vegetal_decoration | 1 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_plains` | random_selector | vegetal_decoration | 4 | ×взвеш.(0:19, 1:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR; фильтр блока: выживет(oak_sapling) |
| `trees_savanna` | random_selector | vegetal_decoration | 2 | ×взвеш.(1:9, 2:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_snowy` | random_selector | vegetal_decoration | 2 | ×взвеш.(0:9, 1:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR; фильтр блока: выживет(spruce_sapling) |
| `trees_sparse_jungle` | random_selector | vegetal_decoration | 1 | ×взвеш.(2:9, 3:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_swamp` | tree | vegetal_decoration | 1 | ×взвеш.(2:9, 3:1); глубина воды ≤ 2; карта высот OCEAN_FLOOR; фильтр блока: выживет(oak_sapling) |
| `trees_taiga` | random_selector | vegetal_decoration | 2 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_water` | random_selector | vegetal_decoration | 11 | ×взвеш.(0:9, 1:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_windswept_forest` | random_selector | vegetal_decoration | 1 | ×взвеш.(3:9, 4:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_windswept_hills` | random_selector | vegetal_decoration | 2 | ×взвеш.(0:9, 1:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `trees_windswept_savanna` | random_selector | vegetal_decoration | 1 | ×взвеш.(2:9, 3:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `warped_fungi` | huge_fungus | vegetal_decoration | 1 | ×8 на каждом слое |
| `yellow_poplar` | tree | — | 0 | фильтр блока: выживет(poplar_sapling) |
| `yellow_poplar_leaf_litter` | tree | — | 0 | фильтр блока: выживет(poplar_sapling) |

### 4.5. пещеры, подземелья, жидкости (37)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `amethyst_geode` | geode | local_modifications | 56 | 1/24; y: равномерно низ+6…y=30 |
| `cave_vines` | block_column | vegetal_decoration | 1 | ×188; y: равномерно низ+0…y=256; поиск up ≤12 (путь: тег#air) до: прочная_грань down; сдвиг(0,-1,0) |
| `classic_vines_cave_feature` | vines | vegetal_decoration | 1 | ×256; y: равномерно низ+0…y=256 |
| `dripstone_cluster` | speleothem_cluster | underground_decoration | 1 | ×U[48..96]; y: равномерно низ+0…y=256 |
| `fossil_lower` | fossil | underground_structures | 3 | 1/64; y: равномерно низ+0…y=-8 |
| `fossil_upper` | fossil | underground_structures | 3 | 1/64; y: равномерно y=0…верх+0 |
| `glow_lichen` | multiface_growth | vegetal_decoration | 56 | ×U[104..157]; y: равномерно низ+0…y=256; относит. поверхности −∞…-13 (OCEAN_FLOOR_WG) |
| `glowstone` | random_neighbor_spread | underground_decoration | 5 | ×10; y: равномерно низ+0…верх+0; фильтр блока: И(тег#air; блок∈netherrack,basalt,blackstone) |
| `glowstone_extra` | random_neighbor_spread | underground_decoration | 5 | ×к_низу[0..9]; y: равномерно низ+4…верх-4; фильтр блока: И(тег#air; блок∈netherrack,basalt,blackstone) |
| `lake_lava_surface` | lake | lakes | 55 | 1/200; карта высот WORLD_SURFACE_WG |
| `lake_lava_underground` | lake | lakes | 55 | 1/9; y: равномерно y=0…верх+0; поиск down ≤32 до: И(НЕ(тег#air); внутри_мира[0, -5, 0]); относит. поверхности −∞…-5 (OCEAN_FLOOR_WG) |
| `large_dripstone` | large_dripstone | local_modifications | 1 | ×U[10..48]; y: равномерно низ+0…y=256 |
| `lush_caves_ceiling_vegetation` | vegetation_patch | vegetal_decoration | 1 | ×125; y: равномерно низ+0…y=256; поиск up ≤12 (путь: тег#air) до: solid; сдвиг(0,-1,0) |
| `lush_caves_clay` | random_boolean_selector | vegetal_decoration | 1 | ×62; y: равномерно низ+0…y=256; поиск down ≤12 (путь: тег#air) до: solid; сдвиг(0,1,0) |
| `lush_caves_vegetation` | vegetation_patch | vegetal_decoration | 1 | ×125; y: равномерно низ+0…y=256; поиск down ≤12 (путь: тег#air) до: solid; сдвиг(0,1,0) |
| `monster_room` | monster_room | underground_structures | 56 | ×10; y: равномерно y=0…верх+0 |
| `monster_room_deep` | monster_room | underground_structures | 56 | ×4; y: равномерно низ+6…y=-1 |
| `pale_moss_patch` | vegetation_patch | vegetal_decoration | 1 | ×1; карта высот MOTION_BLOCKING_NO_LEAVES |
| `pointed_dripstone` | simple_random_selector | underground_decoration | 1 | ×U[192..256]; y: равномерно низ+0…y=256; ×U[1..5]; сдвиг(норм(μ=0.0,σ=3.0,[-10;10]),норм(μ=0.0,σ=0.6,[-2;2]),норм(μ=0.0,σ=3.0,[-10;10])) |
| `rooted_sulfur_spring` | root_system | lakes | 1 | ×U[1..2]; y: равномерно низ+0…y=256; поиск up ≤12 (путь: тег#air) до: solid; сдвиг(0,-1,0) |
| `sculk_patch_ancient_city` | sequence | — | 0 | — |
| `sculk_patch_deep_dark` | sequence | underground_decoration | 1 | ×256; y: равномерно низ+0…y=256 |
| `sculk_vein` | multiface_growth | underground_decoration | 1 | ×U[204..250]; y: равномерно низ+0…y=256 |
| `spore_blossom` | simple_block | vegetal_decoration | 1 | ×25; y: равномерно низ+0…y=256; поиск up ≤12 (путь: тег#air) до: solid; сдвиг(0,-1,0) |
| `spring_closed` | spring_feature | underground_decoration | 4 | ×16; y: равномерно низ+10…верх-10 |
| `spring_closed_double` | spring_feature | underground_decoration | 1 | ×32; y: равномерно низ+10…верх-10 |
| `spring_delta` | spring_feature | underground_decoration | 1 | ×16; y: равномерно низ+4…верх-4 |
| `spring_lava` | spring_feature | fluid_springs,vegetal_decoration | 59 | ×20; y: сильно к низу низ+0…верх-8 |
| `spring_lava_frozen` | spring_feature | fluid_springs | 4 | ×20; y: сильно к низу низ+0…верх-8 |
| `spring_open` | spring_feature | underground_decoration | 4 | ×8; y: равномерно низ+4…верх-4 |
| `spring_water` | spring_feature | fluid_springs | 55 | ×25; y: равномерно низ+0…y=192 |
| `sulfur_pool` | sequence | lakes | 1 | ×256; y: равномерно низ+0…y=256; фильтр блока: solid; поиск up ≤32 до: тег#air; сдвиг(0,-1,0); фильтр блока: блок∈sulfur |
| `sulfur_spike` | simple_random_selector | underground_decoration | 1 | ×U[192..256]; y: равномерно низ+0…y=256; ×U[1..5]; сдвиг(норм(μ=0.0,σ=3.0,[-10;10]),норм(μ=0.0,σ=0.6,[-2;2]),норм(μ=0.0,σ=3.0,[-10;10])) |
| `sulfur_spike_cluster` | speleothem_cluster | underground_decoration | 1 | ×U[48..96]; y: равномерно низ+0…y=256 |
| `twisting_vines` | block_column | vegetal_decoration | 1 | ×10; y: равномерно низ+0…верх+0; фильтр блока: И(тег#air; блок∈netherrack,warped_nylium,warped_wart_block); ×64; сдвиг(U[-8..8],U[-4..4],U[-8..8]); сдвиг(0,-1,0); поиск down ≤32 до: НЕ(тег#air); сдвиг(0,1,0); фильтр блока: И(тег#air; блок∈netherrack,warped_nylium,warped_wart_block) |
| `vines` | vines | vegetal_decoration | 3 | ×127; y: равномерно y=64…y=100 |
| `weeping_vines` | overlay | vegetal_decoration | 1 | ×10; y: равномерно низ+0…верх+0; фильтр блока: И(тег#air; блок∈netherrack,nether_wart_block) |

### 4.6. прочее (32)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `birch_tall` | random_selector | vegetal_decoration | 1 | ×взвеш.(10:9, 11:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `brown_mushroom_dappled_forest` | simple_block | vegetal_decoration | 1 | 1/2; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `brown_mushroom_normal` | simple_block | vegetal_decoration | 44 | 1/256; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `brown_mushroom_old_growth` | simple_block | vegetal_decoration | 2 | ×3; 1/4; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `brown_mushroom_swamp` | simple_block | vegetal_decoration | 1 | ×2; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `brown_mushroom_taiga` | simple_block | vegetal_decoration | 3 | 1/4; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `dark_forest_vegetation` | random_selector | vegetal_decoration | 1 | ×16; глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `desert_well` | overlay | surface_structures | 1 | 1/1000; карта высот MOTION_BLOCKING; сдвиг(0,-1,0); фильтр блока: И(блок∈sand; volume_match {"match": {"type": "minecraft:not", "predicate": {"type": "m) |
| `disk_clay` | disk | underground_ores | 56 | карта высот OCEAN_FLOOR_WG; фильтр блока: жидкость∈water |
| `disk_grass` | disk | underground_ores | 1 | ×1; карта высот OCEAN_FLOOR_WG; сдвиг(0,-1,0); фильтр блока: блок∈mud |
| `disk_gravel` | disk | underground_ores | 54 | карта высот OCEAN_FLOOR_WG; фильтр блока: жидкость∈water |
| `disk_sand` | disk | underground_ores | 54 | ×3; карта высот OCEAN_FLOOR_WG; фильтр блока: жидкость∈water |
| `forest_rock` | block_blob | local_modifications | 2 | ×2; карта высот MOTION_BLOCKING |
| `freeze_top_layer` | freeze_top_layer | top_layer_modification | 56 | — |
| `grass_bonemeal` | simple_block | — | 0 | фильтр блока: тег#air |
| `ice_patch` | disk | surface_structures | 1 | ×2; карта высот MOTION_BLOCKING; сдвиг(0,-1,0); фильтр блока: блок∈snow_block |
| `ice_spike` | spike | surface_structures | 1 | ×3; карта высот MOTION_BLOCKING |
| `mushroom_island_vegetation` | random_boolean_selector | vegetal_decoration | 1 | карта высот MOTION_BLOCKING |
| `pale_garden_flowers` | simple_block | vegetal_decoration | 1 | 1/8; карта высот MOTION_BLOCKING_NO_LEAVES; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `pale_garden_vegetation` | random_selector | vegetal_decoration | 1 | ×16; глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `pile_hay` | block_pile | — | 0 | — |
| `pile_ice` | block_pile | — | 0 | — |
| `pile_melon` | block_pile | — | 0 | — |
| `pile_pumpkin` | block_pile | — | 0 | — |
| `pile_snow` | block_pile | — | 0 | — |
| `red_mushroom_normal` | simple_block | vegetal_decoration | 44 | 1/512; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `red_mushroom_old_growth` | simple_block | vegetal_decoration | 2 | 1/171; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `red_mushroom_swamp` | simple_block | vegetal_decoration | 1 | 1/64; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `red_mushroom_taiga` | simple_block | vegetal_decoration | 3 | 1/256; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `void_start_platform` | void_start_platform | top_layer_modification | 1 | — |
| `wildflowers_birch_forest` | simple_block | vegetal_decoration | 2 | ×3; 1/2; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |
| `wildflowers_meadow` | simple_block | vegetal_decoration | 1 | noise_threshold_count {"above_noise": 10, "below_noise": 5, "noise_level": -0.8}; карта высот MOTION_BLOCKING; ×8; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |

### 4.7. руды и жилы камня (40)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `ore_ancient_debris_large` | scattered_ore | underground_decoration | 5 | y: трапеция y=8…y=24 |
| `ore_andesite_lower` | ore | underground_ores | 56 | ×2; y: равномерно y=0…y=60 |
| `ore_andesite_upper` | ore | underground_ores | 56 | 1/6; y: равномерно y=64…y=128 |
| `ore_blackstone` | ore | underground_decoration | 4 | ×2; y: равномерно y=5…y=31 |
| `ore_clay` | ore | underground_ores | 1 | ×46; y: равномерно низ+0…y=256 |
| `ore_coal_lower` | ore | underground_ores | 56 | ×20; y: трапеция y=0…y=192 |
| `ore_coal_upper` | ore | underground_ores | 56 | ×30; y: равномерно y=136…верх+0 |
| `ore_copper` | ore | underground_ores | 55 | ×16; y: трапеция y=-16…y=112 |
| `ore_copper_large` | ore | underground_ores | 1 | ×16; y: трапеция y=-16…y=112 |
| `ore_debris_small` | scattered_ore | underground_decoration | 5 | y: равномерно низ+8…верх-8 |
| `ore_diamond` | ore | underground_ores | 56 | ×7; y: трапеция низ-80…низ+80 |
| `ore_diamond_buried` | ore | underground_ores | 56 | ×4; y: трапеция низ-80…низ+80 |
| `ore_diamond_large` | ore | underground_ores | 56 | 1/9; y: трапеция низ-80…низ+80 |
| `ore_diamond_medium` | ore | underground_ores | 56 | ×2; y: равномерно y=-64…y=-4 |
| `ore_diorite_lower` | ore | underground_ores | 56 | ×2; y: равномерно y=0…y=60 |
| `ore_diorite_upper` | ore | underground_ores | 56 | 1/6; y: равномерно y=64…y=128 |
| `ore_dirt` | ore | underground_ores | 56 | ×7; y: равномерно y=0…y=160 |
| `ore_emerald` | ore | underground_ores | 10 | ×100; y: трапеция y=-16…y=480 |
| `ore_gold` | ore | underground_ores | 56 | ×4; y: трапеция y=-64…y=32 |
| `ore_gold_deltas` | ore | underground_decoration | 1 | ×20; y: равномерно низ+10…верх-10 |
| `ore_gold_extra` | ore | underground_ores | 3 | ×50; y: равномерно y=32…y=256 |
| `ore_gold_lower` | ore | underground_ores | 56 | ×U[0..1]; y: равномерно y=-64…y=-48 |
| `ore_gold_nether` | ore | underground_decoration | 4 | ×10; y: равномерно низ+10…верх-10 |
| `ore_granite_lower` | ore | underground_ores | 56 | ×2; y: равномерно y=0…y=60 |
| `ore_granite_upper` | ore | underground_ores | 56 | 1/6; y: равномерно y=64…y=128 |
| `ore_gravel` | ore | underground_ores | 56 | ×14; y: равномерно низ+0…верх+0 |
| `ore_gravel_nether` | ore | underground_decoration | 4 | ×2; y: равномерно y=5…y=41 |
| `ore_infested` | ore | underground_decoration | 10 | ×14; y: равномерно низ+0…y=63 |
| `ore_iron_middle` | ore | underground_ores | 56 | ×10; y: трапеция y=-24…y=56 |
| `ore_iron_small` | ore | underground_ores | 56 | ×10; y: равномерно низ+0…y=72 |
| `ore_iron_upper` | ore | underground_ores | 56 | ×90; y: трапеция y=80…y=384 |
| `ore_lapis` | ore | underground_ores | 56 | ×2; y: трапеция y=-32…y=32 |
| `ore_lapis_buried` | ore | underground_ores | 56 | ×4; y: равномерно низ+0…y=64 |
| `ore_magma` | ore | underground_decoration | 5 | ×4; y: равномерно y=27…y=36 |
| `ore_quartz_deltas` | ore | underground_decoration | 1 | ×32; y: равномерно низ+10…верх-10 |
| `ore_quartz_nether` | ore | underground_decoration | 4 | ×16; y: равномерно низ+10…верх-10 |
| `ore_redstone` | ore | underground_ores | 56 | ×4; y: равномерно низ+0…y=15 |
| `ore_redstone_lower` | ore | underground_ores | 56 | ×8; y: трапеция низ-32…низ+32 |
| `ore_soul_sand` | ore | underground_decoration | 1 | ×12; y: равномерно низ+0…y=31 |
| `ore_tuff` | ore | underground_ores | 56 | ×2; y: равномерно низ+0…y=0 |

### 4.8. трава, цветы, кусты (57)

| Фича | Тип | Шаг | Биомов | Правила (размещение / высота / условия) |
|---|---|---|---|---|
| `bamboo` | bamboo | vegetal_decoration | 1 | noise_based_count {"noise_factor": 80.0, "noise_offset": 0.3, "noise_to_count_ratio": 160}; карта высот WORLD_SURFACE_WG |
| `bamboo_in_structure` | bamboo | — | 0 | фильтр блока: тег#air |
| `bamboo_light` | bamboo | vegetal_decoration | 1 | 1/4; карта высот MOTION_BLOCKING |
| `bamboo_vegetation` | random_selector | vegetal_decoration | 1 | ×взвеш.(30:9, 31:1); глубина воды ≤ 0; карта высот OCEAN_FLOOR |
| `flower_cherry` | simple_block | vegetal_decoration | 1 | noise_threshold_count {"above_noise": 10, "below_noise": 5, "noise_level": -0.8}; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |
| `flower_default` | simple_block | vegetal_decoration | 29 | 1/32; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `flower_flower_forest` | simple_block | vegetal_decoration | 1 | ×3; 1/2; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |
| `flower_forest_flowers` | simple_random_selector | vegetal_decoration | 1 | 1/7; карта высот MOTION_BLOCKING; ×clamped {"max_inclusive": 3, "min_inclusive": 0, "source": {"type":  |
| `flower_meadow` | simple_block | vegetal_decoration | 1 | карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |
| `flower_pale_garden` | simple_block | vegetal_decoration | 1 | 1/32; карта высот MOTION_BLOCKING |
| `flower_plain` | simple_block | — | 0 | ×64; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |
| `flower_plains` | simple_block | vegetal_decoration | 4 | noise_threshold_count {"above_noise": 4, "below_noise": 15, "noise_level": -0.8}; 1/32; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |
| `flower_swamp` | simple_block | vegetal_decoration | 1 | 1/32; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 6, "min": -6, "plateau": 0},trapezoid {"max": 2, "min": -2, "plateau": 0},trapezoid {"max": 6, "min": -6, "plateau": 0}); фильтр блока: тег#air |
| `flower_warm` | simple_block | vegetal_decoration | 5 | 1/16; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `forest_flowers` | simple_random_selector | vegetal_decoration | 4 | 1/7; карта высот MOTION_BLOCKING; ×clamped {"max_inclusive": 1, "min_inclusive": 0, "source": {"type":  |
| `patch_berry_bush` | simple_block | — | 0 | ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; блок∈grass_block) |
| `patch_berry_common` | simple_block | vegetal_decoration | 3 | 1/32; карта высот WORLD_SURFACE_WG; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; блок∈grass_block) |
| `patch_berry_rare` | simple_block | vegetal_decoration | 1 | 1/384; карта высот WORLD_SURFACE_WG; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; блок∈grass_block) |
| `patch_bush` | simple_block | vegetal_decoration | 9 | 1/4; карта высот MOTION_BLOCKING; ×24; сдвиг(trapezoid {"max": 5, "min": -5, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 5, "min": -5, "plateau": 0}); фильтр блока: тег#air |
| `patch_cactus` | block_column | — | 0 | ×10; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; выживет(cactus)) |
| `patch_cactus_decorated` | block_column | vegetal_decoration | 3 | 1/13; карта высот MOTION_BLOCKING; ×10; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; выживет(cactus)) |
| `patch_cactus_desert` | block_column | vegetal_decoration | 1 | 1/6; карта высот MOTION_BLOCKING; ×10; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; выживет(cactus)) |
| `patch_crimson_roots` | simple_block | underground_decoration | 1 | y: равномерно низ+0…верх+0; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_dead_bush` | simple_block | vegetal_decoration | 4 | карта высот WORLD_SURFACE_WG; ×4; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_dead_bush_2` | simple_block | vegetal_decoration | 1 | ×2; карта высот WORLD_SURFACE_WG; ×4; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_dead_bush_badlands` | simple_block | vegetal_decoration | 3 | ×20; карта высот WORLD_SURFACE_WG; ×4; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_dry_grass_badlands` | simple_block | vegetal_decoration | 3 | 1/6; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_dry_grass_desert` | simple_block | vegetal_decoration | 1 | 1/3; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_fire` | simple_block | underground_decoration | 5 | ×U[0..5]; y: равномерно низ+4…верх-4; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; блок∈netherrack) |
| `patch_firefly_bush_near_water` | simple_block | vegetal_decoration | 42 | ×2; карта высот MOTION_BLOCKING_NO_LEAVES; фильтр блока: И(тег#air; выживет(firefly_bush); ИЛИ(жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water)); ×20; сдвиг(trapezoid {"max": 4, "min": -4, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 4, "min": -4, "plateau": 0}); фильтр блока: тег#air |
| `patch_firefly_bush_near_water_swamp` | simple_block | vegetal_decoration | 1 | ×3; карта высот MOTION_BLOCKING; фильтр блока: И(тег#air; выживет(firefly_bush); ИЛИ(жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water)); ×20; сдвиг(trapezoid {"max": 4, "min": -4, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 4, "min": -4, "plateau": 0}); фильтр блока: тег#air |
| `patch_firefly_bush_swamp` | simple_block | vegetal_decoration | 1 | 1/8; карта высот MOTION_BLOCKING; ×20; сдвиг(trapezoid {"max": 4, "min": -4, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 4, "min": -4, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_badlands` | simple_block | vegetal_decoration | 24 | карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_forest` | simple_block | vegetal_decoration | 6 | ×2; карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_jungle` | simple_block | vegetal_decoration | 3 | ×25; карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; НЕ(блок∈podzol)) |
| `patch_grass_meadow` | simple_block | vegetal_decoration | 1 | noise_threshold_count {"above_noise": 10, "below_noise": 5, "noise_level": -0.8}; карта высот WORLD_SURFACE_WG; ×16; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_normal` | simple_block | vegetal_decoration | 3 | ×5; карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_plain` | simple_block | vegetal_decoration | 5 | noise_threshold_count {"above_noise": 10, "below_noise": 5, "noise_level": -0.8}; карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_savanna` | simple_block | vegetal_decoration | 2 | ×20; карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_taiga` | simple_block | vegetal_decoration | 2 | ×7; карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_grass_taiga_2` | simple_block | vegetal_decoration | 2 | карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_large_fern` | simple_block | vegetal_decoration | 4 | 1/5; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_leaf_litter` | simple_block | vegetal_decoration | 1 | ×2; карта высот WORLD_SURFACE_WG; ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; блок∈grass_block) |
| `patch_melon` | simple_block | vegetal_decoration | 2 | 1/6; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(replaceable; жидкость∈empty; блок∈grass_block) |
| `patch_melon_sparse` | simple_block | vegetal_decoration | 1 | 1/64; карта высот MOTION_BLOCKING; ×64; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(replaceable; жидкость∈empty; блок∈grass_block) |
| `patch_pumpkin` | simple_block | vegetal_decoration | 46 | 1/300; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; блок∈grass_block) |
| `patch_red_shrub` | simple_block | vegetal_decoration | 1 | карта высот WORLD_SURFACE_WG; ×8; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_soul_fire` | simple_block | underground_decoration | 4 | ×U[0..5]; y: равномерно низ+4…верх-4; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: И(тег#air; блок∈soul_soil) |
| `patch_sugar_cane` | block_column | vegetal_decoration | 38 | 1/6; карта высот MOTION_BLOCKING; ×20; сдвиг(trapezoid {"max": 4, "min": -4, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 4, "min": -4, "plateau": 0}); фильтр блока: И(тег#air; выживет(sugar_cane); ИЛИ(жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water)) |
| `patch_sugar_cane_badlands` | block_column | vegetal_decoration | 3 | 1/5; карта высот MOTION_BLOCKING; ×20; сдвиг(trapezoid {"max": 4, "min": -4, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 4, "min": -4, "plateau": 0}); фильтр блока: И(тег#air; выживет(sugar_cane); ИЛИ(жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water)) |
| `patch_sugar_cane_desert` | block_column | vegetal_decoration | 1 | карта высот MOTION_BLOCKING; ×20; сдвиг(trapezoid {"max": 4, "min": -4, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 4, "min": -4, "plateau": 0}); фильтр блока: И(тег#air; выживет(sugar_cane); ИЛИ(жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water)) |
| `patch_sugar_cane_swamp` | block_column | vegetal_decoration | 1 | 1/3; карта высот MOTION_BLOCKING; ×20; сдвиг(trapezoid {"max": 4, "min": -4, "plateau": 0},trapezoid {"max": 0, "min": 0, "plateau": 0},trapezoid {"max": 4, "min": -4, "plateau": 0}); фильтр блока: И(тег#air; выживет(sugar_cane); ИЛИ(жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water; жидкость∈water,flowing_water)) |
| `patch_sunflower` | simple_block | vegetal_decoration | 1 | 1/3; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_taiga_grass` | simple_block | — | 0 | ×32; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_tall_grass` | simple_block | vegetal_decoration | 2 | 1/5; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_tall_grass_2` | simple_block | vegetal_decoration | 8 | noise_threshold_count {"above_noise": 7, "below_noise": 0, "noise_level": -0.8}; 1/32; карта высот MOTION_BLOCKING; ×96; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |
| `patch_waterlily` | simple_block | vegetal_decoration | 2 | ×4; карта высот WORLD_SURFACE_WG; ×10; сдвиг(trapezoid {"max": 7, "min": -7, "plateau": 0},trapezoid {"max": 3, "min": -3, "plateau": 0},trapezoid {"max": 7, "min": -7, "plateau": 0}); фильтр блока: тег#air |

## 5. Карверы (пещеры, разломы)

| Карвер | Тип | Вероятность старта в чанке | y старта | lava_level (26.1/26.2) | Параметры | Биомов |
|---|---|---|---|---|---|---|
| `canyon` | canyon | 0.01 | равномерно y=10…y=67 | — | vertical_rotation=U[-0.125..0.125), distance_factor=U[0.75..1.0), horizontal_radius_factor=U[0.75..1.0), thickness=trapezoid {"max": 6.0, "min": 0.0, "plateau": 2.0}, vertical_radius_center_factor=0.0, vertical_radius_default_factor=1.0, width_smoothness=3, y_scale=3.0 | 56 |
| `cave` | cave | 0.15 | равномерно низ+8…y=180 | — | count=очень_к_низу[0..14], floor_level=U[-1.0..-0.4), horizontal_radius_multiplier=U[0.7..1.4), room_vertical_radius_multiplier=U[0.1..0.9), thickness=trapezoid {"max": 3.0, "min": 0.0, "plateau": 1.0}, vertical_radius_multiplier=U[0.8..1.3), weird_thickness_bias=True | 56 |
| `cave_extra_underground` | cave | 0.07 | равномерно низ+8…y=47 | — | count=очень_к_низу[0..14], floor_level=U[-1.0..-0.4), horizontal_radius_multiplier=U[0.7..1.4), room_vertical_radius_multiplier=U[0.1..0.9), thickness=trapezoid {"max": 3.0, "min": 0.0, "plateau": 1.0}, vertical_radius_multiplier=U[0.8..1.3), weird_thickness_bias=True | 56 |
| `nether_cave` | cave | 0.2 | равномерно y=0…верх-1 | — | count=очень_к_низу[0..9], floor_level=-0.7, horizontal_radius_multiplier=1.0, room_vertical_radius_multiplier=0.5, start_vertical_radius_multiplier=5.0, thickness=trapezoid {"max": 6.0, "min": 0.0, "plateau": 2.0}, vertical_radius_multiplier=1.0 | 5 |

## 6. Структуры: шаг, адаптация рельефа, высотные настройки (данные)

Высотные ограничения, зашитые в **Java-код** структур (а не в JSON), перечислены в §8 (использование карт высот/уровня моря по классам).

| Структура | Тип | Шаг | `terrain_adaptation` | Высотные/прочие настройки |
|---|---|---|---|---|
| `abandoned_camp_bamboo_jungle` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_birch_forest` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_cherry_grove` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_dappled_forest` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_flower_forest` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_forest` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_meadow` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_old_growth_birch_forest` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_old_growth_pine_taiga` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_old_growth_spruce_taiga` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_pale_garden` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_savanna` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_snowy_taiga` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_sparse_jungle` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_swamp` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_taiga` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_windswept_forest` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `abandoned_camp_wooded_badlands` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 2, "use_expansion_hack": true} |
| `ancient_city` | jigsaw | underground_decoration | beard_box | {"start_height": {"absolute": -27}, "max_distance_from_center": 116, "size": 7, "use_expansion_hack": false, "start_jigsaw_name": "minecraft:city_anchor"} |
| `bastion_remnant` | jigsaw | surface_structures | none | {"start_height": {"absolute": 33}, "max_distance_from_center": 80, "size": 6, "use_expansion_hack": false} |
| `buried_treasure` | buried_treasure | underground_structures | none | — |
| `desert_pyramid` | desert_pyramid | surface_structures | none | — |
| `end_city` | end_city | surface_structures | none | — |
| `fortress` | fortress | underground_decoration | none | — |
| `igloo` | igloo | surface_structures | none | — |
| `jungle_pyramid` | jungle_temple | surface_structures | none | — |
| `mansion` | woodland_mansion | surface_structures | none | — |
| `mineshaft` | mineshaft | underground_structures | none | {"mineshaft_type": "normal"} |
| `mineshaft_mesa` | mineshaft | underground_structures | none | {"mineshaft_type": "mesa"} |
| `monument` | ocean_monument | surface_structures | none | — |
| `nether_fossil` | nether_fossil | underground_decoration | beard_thin | — |
| `ocean_ruin_cold` | ocean_ruin | surface_structures | none | — |
| `ocean_ruin_warm` | ocean_ruin | surface_structures | none | — |
| `pillager_outpost` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 7, "use_expansion_hack": true} |
| `ruined_portal` | ruined_portal | surface_structures | none | {"setups": [{"placement": "underground", "air_pocket_probability": 1.0, "mossiness": 0.2, "cold": true, "overgrown": false, "vines": false, "replace_with_blackstone": false, "weight": 0.5}, {"placement": "on_land_surface", "air_pocket_probability": 0.0, "mossi |
| `ruined_portal_desert` | ruined_portal | surface_structures | none | {"setups": [{"placement": "partly_buried", "air_pocket_probability": 0.0, "mossiness": 0.0, "cold": false, "overgrown": false, "vines": false, "replace_with_blackstone": false, "weight": 1.0}]} |
| `ruined_portal_jungle` | ruined_portal | surface_structures | none | {"setups": [{"placement": "on_land_surface", "air_pocket_probability": 0.0, "mossiness": 0.8, "cold": false, "overgrown": true, "vines": true, "replace_with_blackstone": false, "weight": 1.0}]} |
| `ruined_portal_mountain` | ruined_portal | surface_structures | none | {"setups": [{"placement": "in_mountain", "air_pocket_probability": 1.0, "mossiness": 0.2, "cold": true, "overgrown": false, "vines": false, "replace_with_blackstone": false, "weight": 0.5}, {"placement": "on_land_surface", "air_pocket_probability": 0.0, "mossi |
| `ruined_portal_nether` | ruined_portal | surface_structures | none | {"setups": [{"placement": "in_nether", "air_pocket_probability": 0.0, "mossiness": 0.0, "cold": false, "overgrown": false, "vines": false, "replace_with_blackstone": true, "weight": 1.0}]} |
| `ruined_portal_ocean` | ruined_portal | surface_structures | none | {"setups": [{"placement": "on_ocean_floor", "air_pocket_probability": 0.0, "mossiness": 0.8, "cold": true, "overgrown": false, "vines": false, "replace_with_blackstone": false, "weight": 1.0}]} |
| `ruined_portal_swamp` | ruined_portal | surface_structures | none | {"setups": [{"placement": "on_ocean_floor", "air_pocket_probability": 0.0, "mossiness": 0.5, "cold": false, "overgrown": false, "vines": true, "replace_with_blackstone": false, "weight": 1.0}]} |
| `shipwreck` | shipwreck | surface_structures | none | {"is_beached": false} |
| `shipwreck_beached` | shipwreck | surface_structures | none | {"is_beached": true} |
| `stronghold` | stronghold | surface_structures | bury | — |
| `swamp_hut` | swamp_hut | surface_structures | none | — |
| `trail_ruins` | jigsaw | underground_structures | bury | {"start_height": {"absolute": -15}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 7, "use_expansion_hack": false} |
| `trial_chambers` | jigsaw | underground_structures | encapsulate | {"start_height": {"type": "minecraft:uniform", "max_inclusive": {"absolute": -20}, "min_inclusive": {"absolute": -40}}, "max_distance_from_center": 116, "size": 20, "use_expansion_hack": false, "liquid_settings": "ignore_waterlogging", "dimension_padding": 10, |
| `village_desert` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 6, "use_expansion_hack": true} |
| `village_plains` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 6, "use_expansion_hack": true} |
| `village_savanna` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 6, "use_expansion_hack": true} |
| `village_snowy` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 6, "use_expansion_hack": true} |
| `village_taiga` | jigsaw | surface_structures | beard_thin | {"start_height": {"absolute": 0}, "project_start_to_heightmap": "WORLD_SURFACE_WG", "max_distance_from_center": 80, "size": 6, "use_expansion_hack": true} |

## 7. Температура, осадки, снег по биомам (все биомы)

`T_base` — температура биома; снег/осадки по `Biome.getHeightAdjustedTemperature`: выше y = `sea_level + 17 = 80` температура падает: `T_adj = T_base − (v + y − 80)·0.05/40`, `v = 8·Simplex(x/8, z/8)` (константный шум 1234, ±8); снег при `T_adj < 0.15`. Порог высоты: `y ≈ 80 + 800·(T_base − 0.15) − v`.

| T_base | Биом | downfall | Осадки | Модификатор | Снег |
|---|---|---|---|---|---|
| -0.7 | `frozen_peaks` | 0.9 | да | — | снег на любой высоте |
| -0.7 | `jagged_peaks` | 0.9 | да | — | снег на любой высоте |
| -0.5 | `snowy_taiga` | 0.4 | да | — | снег на любой высоте |
| -0.3 | `snowy_slopes` | 0.9 | да | — | снег на любой высоте |
| -0.2 | `grove` | 0.8 | да | — | снег на любой высоте |
| 0.0 | `frozen_ocean` | 0.5 | да | frozen | снег на любой высоте |
| 0.0 | `frozen_river` | 0.5 | да | — | снег на любой высоте |
| 0.0 | `ice_spikes` | 0.5 | да | — | снег на любой высоте |
| 0.0 | `snowy_plains` | 0.5 | да | — | снег на любой высоте |
| 0.05 | `snowy_beach` | 0.3 | да | — | снег на любой высоте |
| 0.2 | `stony_shore` | 0.3 | да | — | снег выше y ≈ 120 ± 8 |
| 0.2 | `windswept_forest` | 0.3 | да | — | снег выше y ≈ 120 ± 8 |
| 0.2 | `windswept_gravelly_hills` | 0.3 | да | — | снег выше y ≈ 120 ± 8 |
| 0.2 | `windswept_hills` | 0.3 | да | — | снег выше y ≈ 120 ± 8 |
| 0.25 | `old_growth_spruce_taiga` | 0.8 | да | — | снег выше y ≈ 160 ± 8 |
| 0.25 | `taiga` | 0.8 | да | — | снег выше y ≈ 160 ± 8 |
| 0.3 | `old_growth_pine_taiga` | 0.8 | да | — | снег выше y ≈ 200 ± 8 |
| 0.5 | `cherry_grove` | 0.8 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `cold_ocean` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `deep_cold_ocean` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `deep_frozen_ocean` | 0.5 | да | frozen | снега нет (порог 360 > 320) |
| 0.5 | `deep_lukewarm_ocean` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `deep_ocean` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `end_barrens` | 0.5 | нет | — | осадков нет |
| 0.5 | `end_highlands` | 0.5 | нет | — | осадков нет |
| 0.5 | `end_midlands` | 0.5 | нет | — | осадков нет |
| 0.5 | `lukewarm_ocean` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `lush_caves` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `meadow` | 0.8 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `ocean` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `river` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.5 | `small_end_islands` | 0.5 | нет | — | осадков нет |
| 0.5 | `the_end` | 0.5 | нет | — | осадков нет |
| 0.5 | `the_void` | 0.5 | нет | — | осадков нет |
| 0.5 | `warm_ocean` | 0.5 | да | — | снега нет (порог 360 > 320) |
| 0.6 | `birch_forest` | 0.6 | да | — | снега нет (порог 440 > 320) |
| 0.6 | `dappled_forest` | 0.6 | да | — | снега нет (порог 440 > 320) |
| 0.6 | `old_growth_birch_forest` | 0.6 | да | — | снега нет (порог 440 > 320) |
| 0.7 | `dark_forest` | 0.8 | да | — | снега нет (порог 520 > 320) |
| 0.7 | `flower_forest` | 0.8 | да | — | снега нет (порог 520 > 320) |
| 0.7 | `forest` | 0.8 | да | — | снега нет (порог 520 > 320) |
| 0.7 | `pale_garden` | 0.8 | да | — | снега нет (порог 520 > 320) |
| 0.8 | `beach` | 0.4 | да | — | снега нет (порог 600 > 320) |
| 0.8 | `deep_dark` | 0.4 | да | — | снега нет (порог 600 > 320) |
| 0.8 | `dripstone_caves` | 0.4 | да | — | снега нет (порог 600 > 320) |
| 0.8 | `mangrove_swamp` | 0.9 | да | — | снега нет (порог 600 > 320) |
| 0.8 | `plains` | 0.4 | да | — | снега нет (порог 600 > 320) |
| 0.8 | `sulfur_caves` | 0.4 | да | — | снега нет (порог 600 > 320) |
| 0.8 | `sunflower_plains` | 0.4 | да | — | снега нет (порог 600 > 320) |
| 0.8 | `swamp` | 0.9 | да | — | снега нет (порог 600 > 320) |
| 0.9 | `mushroom_fields` | 1.0 | да | — | снега нет (порог 680 > 320) |
| 0.95 | `bamboo_jungle` | 0.9 | да | — | снега нет (порог 720 > 320) |
| 0.95 | `jungle` | 0.9 | да | — | снега нет (порог 720 > 320) |
| 0.95 | `sparse_jungle` | 0.8 | да | — | снега нет (порог 720 > 320) |
| 1.0 | `stony_peaks` | 0.3 | да | — | снега нет (порог 760 > 320) |
| 2.0 | `badlands` | 0.0 | нет | — | осадков нет |
| 2.0 | `basalt_deltas` | 0.0 | нет | — | осадков нет |
| 2.0 | `crimson_forest` | 0.0 | нет | — | осадков нет |
| 2.0 | `desert` | 0.0 | нет | — | осадков нет |
| 2.0 | `eroded_badlands` | 0.0 | нет | — | осадков нет |
| 2.0 | `nether_wastes` | 0.0 | нет | — | осадков нет |
| 2.0 | `savanna` | 0.0 | нет | — | осадков нет |
| 2.0 | `savanna_plateau` | 0.0 | нет | — | осадков нет |
| 2.0 | `soul_sand_valley` | 0.0 | нет | — | осадков нет |
| 2.0 | `warped_forest` | 0.0 | нет | — | осадков нет |
| 2.0 | `windswept_savanna` | 0.0 | нет | — | осадков нет |
| 2.0 | `wooded_badlands` | 0.0 | нет | — | осадков нет |

## 8. Java-классы с логикой уровня моря / границ высоты / карт высот / температуры

Скан исходников (`levelgen/feature`, `structure(s)`, `carver`, `placement`, `material`, `biome`, `levelgen`): сколько раз класс обращается к `getSeaLevel`, границам мира, `getHeight`/`Heightmap.Types.*`, функциям температуры/снега/льда. Это те места, где «высотная» логика живёт в коде, а не в JSON.

| Класс | Использование |
|---|---|
| `biome/Biome` | уровень моря: 14, температура/снег/лёд: 21 |
| `biome/BiomeSource` | границы мира minY/maxY: 2 |
| `biome/FixedBiomeSource` | границы мира minY/maxY: 2 |
| `biome/OverworldBiomeBuilder` | температура/снег/лёд: 1 |
| `levelgen/BelowZeroRetrogen` | границы мира minY/maxY: 3 |
| `levelgen/DebugLevelSource` | уровень моря: 1, границы мира minY/maxY: 2, getHeight/getBaseHeight: 1 |
| `levelgen/FlatLevelSource` | уровень моря: 1, границы мира minY/maxY: 8, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG', 'Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 4 |
| `levelgen/Heightmap` | границы мира minY/maxY: 5, getHeight/getBaseHeight: 1 |
| `levelgen/NoiseBasedChunkGenerator` | уровень моря: 7, границы мира minY/maxY: 7, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG', 'Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/NoiseGeneratorSettings` | уровень моря: 2 |
| `levelgen/NoiseRouterData` | температура/снег/лёд: 3 |
| `levelgen/NoiseSettings` | границы мира minY/maxY: 2 |
| `levelgen/Noises` | температура/снег/лёд: 3 |
| `levelgen/PatrolSpawner` | карты высот: ['Heightmap.Types.MOTION_BLOCKING_NO_LEAVES'] |
| `levelgen/PhantomSpawner` | уровень моря: 1 |
| `levelgen/RandomState` | уровень моря: 5, температура/снег/лёд: 1 |
| `levelgen/VerticalAnchor` | уровень моря: 2, границы мира minY/maxY: 3 |
| `levelgen/WorldGenerationContext` | уровень моря: 7, границы мира minY/maxY: 6, getHeight/getBaseHeight: 2 |
| `levelgen/carver/CanyonWorldCarver` | границы мира minY/maxY: 2 |
| `levelgen/feature/AbstractHugeMushroomFeature` | границы мира minY/maxY: 2 |
| `levelgen/feature/BambooFeature` | карты высот: ['Heightmap.Types.WORLD_SURFACE'], getHeight/getBaseHeight: 1 |
| `levelgen/feature/BlockBlobFeature` | границы мира minY/maxY: 2 |
| `levelgen/feature/BlockPileFeature` | границы мира minY/maxY: 1 |
| `levelgen/feature/BlueIceFeature` | уровень моря: 1 |
| `levelgen/feature/BonusChestFeature` | карты высот: ['Heightmap.Types.MOTION_BLOCKING_NO_LEAVES'] |
| `levelgen/feature/EndSpikeFeature` | границы мира minY/maxY: 1, getHeight/getBaseHeight: 4 |
| `levelgen/feature/FillLayerFeature` | границы мира minY/maxY: 1 |
| `levelgen/feature/FossilFeature` | границы мира minY/maxY: 3, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/feature/HugeFungusFeature` | границы мира minY/maxY: 1 |
| `levelgen/feature/IcebergFeature` | уровень моря: 1 |
| `levelgen/feature/LakeFeature` | границы мира minY/maxY: 1, температура/снег/лёд: 1 |
| `levelgen/feature/LargeDripstoneFeature` | карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 2 |
| `levelgen/feature/MonsterRoomFeature` | границы мира minY/maxY: 1 |
| `levelgen/feature/OreFeature` | карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/feature/ReplaceBlobsFeature` | границы мира minY/maxY: 3 |
| `levelgen/feature/RootSystemFeature` | карты высот: ['Heightmap.Types.WORLD_SURFACE'], getHeight/getBaseHeight: 1 |
| `levelgen/feature/SnowAndFreezeFeature` | карты высот: ['Heightmap.Types.MOTION_BLOCKING'], getHeight/getBaseHeight: 1, температура/снег/лёд: 2 |
| `levelgen/feature/SpeleothemClusterFeature` | getHeight/getBaseHeight: 2 |
| `levelgen/feature/SpikeFeature` | границы мира minY/maxY: 1 |
| `levelgen/feature/SteppedColumnClusterFeature` | уровень моря: 1, границы мира minY/maxY: 2 |
| `levelgen/feature/TreeFeature` | границы мира minY/maxY: 2 |
| `levelgen/material/MaterialRuleContext` | уровень моря: 2 |
| `levelgen/material/MaterialSystem` | уровень моря: 12, границы мира minY/maxY: 5, карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 6 |
| `levelgen/placement/CountOnEveryLayerPlacement` | границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.MOTION_BLOCKING'], getHeight/getBaseHeight: 1 |
| `levelgen/placement/HeightmapPlacement` | границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.CODEC'], getHeight/getBaseHeight: 1 |
| `levelgen/placement/PlacementContext` | границы мира minY/maxY: 2, getHeight/getBaseHeight: 1 |
| `levelgen/placement/SurfaceRelativeThresholdFilter` | карты высот: ['Heightmap.Types.CODEC'], getHeight/getBaseHeight: 1 |
| `levelgen/placement/SurfaceWaterDepthFilter` | карты высот: ['Heightmap.Types.OCEAN_FLOOR', 'Heightmap.Types.WORLD_SURFACE'], getHeight/getBaseHeight: 2 |
| `levelgen/structure/ScatteredFeaturePiece` | границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.MOTION_BLOCKING_NO_LEAVES'] |
| `levelgen/structure/SinglePieceStructure` | уровень моря: 1, карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'] |
| `levelgen/structure/Structure` | границы мира minY/maxY: 2, карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 5 |
| `levelgen/structure/StructurePiece` | границы мира minY/maxY: 2, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/structure/structures/BuriedTreasurePieces` | границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/structure/structures/BuriedTreasureStructure` | карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'] |
| `levelgen/structure/structures/EndCityPieces` | границы мира minY/maxY: 2 |
| `levelgen/structure/structures/IglooPieces` | карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/structure/structures/IglooStructure` | карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'] |
| `levelgen/structure/structures/JigsawStructure` | карты высот: ['Heightmap.Types.CODEC'] |
| `levelgen/structure/structures/MineshaftPieces` | границы мира minY/maxY: 7 |
| `levelgen/structure/structures/MineshaftStructure` | уровень моря: 6, границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/structure/structures/NetherFortressPieces` | границы мира minY/maxY: 12 |
| `levelgen/structure/structures/NetherFossilStructure` | уровень моря: 4 |
| `levelgen/structure/structures/OceanMonumentPieces` | уровень моря: 2 |
| `levelgen/structure/structures/OceanMonumentStructure` | уровень моря: 1, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'] |
| `levelgen/structure/structures/OceanRuinPieces` | уровень моря: 1, границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'], getHeight/getBaseHeight: 2 |
| `levelgen/structure/structures/OceanRuinStructure` | карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG'] |
| `levelgen/structure/structures/RuinedPortalPiece` | карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG', 'Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 1 |
| `levelgen/structure/structures/RuinedPortalStructure` | уровень моря: 3, границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG', 'Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 1, температура/снег/лёд: 1 |
| `levelgen/structure/structures/ShipwreckPieces` | границы мира minY/maxY: 1, карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG', 'Heightmap.Types.WORLD_SURFACE_WG'], getHeight/getBaseHeight: 2 |
| `levelgen/structure/structures/ShipwreckStructure` | карты высот: ['Heightmap.Types.OCEAN_FLOOR_WG', 'Heightmap.Types.WORLD_SURFACE_WG'] |
| `levelgen/structure/structures/StrongholdPieces` | границы мира minY/maxY: 12 |
| `levelgen/structure/structures/StrongholdStructure` | уровень моря: 1, границы мира minY/maxY: 1 |
| `levelgen/structure/structures/SwampHutStructure` | карты высот: ['Heightmap.Types.WORLD_SURFACE_WG'] |
| `levelgen/structure/structures/WoodlandMansionStructure` | границы мира minY/maxY: 1 |


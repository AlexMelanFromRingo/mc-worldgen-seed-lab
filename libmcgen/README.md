# libmcgen — генератор мира Minecraft 26.x (C11)

Публичный контракт — `include/mcgen.h`. Проект и ворота приёмки — `docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md`.

## Правила
* C11, без внешних зависимостей (свой JSON-парсер, inflate, потоки через тонкий слой pthreads/Win32). Кросс-сборка — `zig cc` (win-x64, linux-x64, macos-arm64, macos-x64); локально — gcc.
* Код пишется заново по математике и поведению алгоритмов; **не копировать построчно декомпилят Mojang**. Справка — `src/dec/<V>/` (локально), документы `docs/00–23`.
* Данные не зашиваются: всё, что в игре лежит в датапаке, читается из pack-каталога (`tools/make_pack.py`, `run/pack-<V>/`). В коде — только то, что в игре закодировано в Java.
* Две численные ветки: 26.1/26.2 (`double`) и 26.3+ (`float`, NoiseStack) — выбираются по версии (`engine/mc_common.h: McVersion`). Переиспользуйте и расширяйте проверенный `engine/`.
* Бит-точность: `-ffp-contract=off`, без `-ffast-math`, без FMA-слияний.
* Потокобезопасность: `McGen`/`McWorld` после создания только читаются; временные буферы — в локальных структурах потока.
* Тесты — `libmcgen/tests/`; эталон — oracle (`oracle/run.sh <V> serve`, команда `df`, `height`, …) и настоящие чанки (`tools/gt/`).

## Формат дампа MCR1 (little-endian)
```
char[4] "MCR1"; i32 abi; i32 cx0, cz0, nx, nz, min_y, height; u32 stages;
по чанкам в порядке cz-major (cz = cz0..; cx = cx0..):
    u16 blocks[height*256]            // [y][z][x]
    u8  biomes[(height/4)*16]         // [qy][qz][qx]
    i16 heightmaps[4][256]            // WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES
таблицы: u32 n_states; затем n_states строк (u16 длина + UTF-8) — имена состояний блоков; u32 n_biomes; строки имён биомов.
```
Читатель/писатель на Python — `tools/gt/mcr.py`.

## CLI (`libmcgen/cli/mcgen-cli.c`)
```
mcgen-cli --pack run/pack-26.3 --version 26.3 --dim minecraft:overworld --preset normal --seed 12345 \
          [--seeds climate,terrain,structures,features] [--tweak id=value …] \
          --cx0 0 --cz0 0 --nx 8 --nz 8 --stages 0x3f --threads 0 --out region.mcr
```
Дополнительно: `--pp-margin K` — растекание жидкостей только в чанках не ближе K к краю региона (сверка с областью,
загруженной сервером); подкоманды `info`, `df`, `biome`, `qbiome`, `climate`, `chunkbiomes`, `biometie`, `fillraw`, `bench`
(описание — в начале `cli/mcgen-cli.c`). Устройство, тесты и измеренные результаты — `docs/blender/terrain.md`.

## Ускорение на видеокарте (необязательно)
`gpu/` — библиотека `libmcgen_cuda` (CUDA, только NVIDIA; `gpu/build.sh`, под Windows `gpu/build.bat`): сетка биомов (`mcgen_biome_grid`, все версии/измерения/пресеты) и,
по явному выбору, плотность/жилы рельефа 26.3+ — бит-в-бит как CPU. `src/gpu_bridge.c` подгружает её динамически (dlopen/LoadLibrary): без библиотеки, без
устройства и при любой ошибке всё считается на CPU. Управление — `mcgen_gpu_*` в `include/mcgen.h` (режимы Auto/CPU/GPU, самопроверка GPU = CPU); переменные окружения
`MCGEN_COMPUTE=cpu|gpu|auto`, `MCGEN_CUDA_LIB=<путь>`, `MCGEN_GPU_DEBUG=1`. Тесты ворот G9 — `tests/g9_*` (`make g9`). Описание, цифры и ограничения — `docs/blender/gpu.md`.

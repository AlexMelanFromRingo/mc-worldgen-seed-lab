# WorldGen — восстановление seed Minecraft (26.1 / 26.2 / 26.3), все измерения

Цель: изучить исходники генерации мира (Mojang с 26.1 не обфусцирует код) и построить
инструменты для ускоренного перебора / восстановления seed (в т.ч. на GPU, CUDA),
поддерживающие версии 26.1, 26.2, 26.3 (и расширяемые на следующие) и измерения
Overworld, Nether, End. Референсы: cubiomes (C, до 1.21.3), SeedcrackerX (мод, уже на 26.3),
Nether_Bedrock_Cracker, chunkbase (порт cubiomes-подобного кода).

Все тексты, отчёты и комментарии — по-русски. Идентификаторы кода — как в оригинале.

## Окружение (проверено)
- Linux/WSL2, RTX 4080 (16 GB, sm_89), 12 ядер, 27 GB RAM, nvcc, gcc/clang, cmake, Java 25 (OpenJDK), Maven, Python 3, Rust, Go.
- Нужен доступ в сеть (piston-meta.mojang.com, github, maven central) для `make setup`.
- Все пути ниже — от корня репозитория; бинарники ищут данные в `./data` или в `$MCGEN_ROOT`.

## Раскладка
Каталоги `jars/`, `src/`, `run/` и `refs/` **не входят в репозиторий** (материалы Mojang и чужие клоны): `jars/` и `src/` создаёт `make setup` (`tools/fetch_game.py`), `refs/` — это справочные клоны cubiomes, SeedcrackerX и др. (ссылки — в README).

```
jars/game-26.{1,2,3}.jar        внутренний jar сервера (классы + data/ датапак)
jars/server-<V>.jar               официальный bundler-jar Mojang (скачивает tools/fetch_game.py)
src/bundle-<V>/META-INF/libraries/**.jar   библиотеки версии (нужны в classpath)
src/dec/<V>/                    ДЕКОМПИЛИРОВАННЫЕ исходники (Vineflower, 0 ошибок): src/dec/26.3/net/minecraft/...
src/data-<V>/data/minecraft/    распакованный датапак: worldgen/{biome,noise,noise_settings,density_function,structure_set,...}
tools/classpath.sh <V>          печатает classpath: CP=$(tools/classpath.sh 26.3)
refs/                           cubiomes, SeedcrackerX, SeedCracker, Nether_Bedrock_Cracker, LangExperiments
oracle/                         Java-эталон на реальном коде Mojang (ground truth для тестов)
engine/                         C/CUDA библиотека генерации (host+device), проверяется против oracle
crack/                          CUDA/CPU инструменты восстановления seed
data/                           извлечённые из jar/oracle таблицы (JSON/бинарные)
tests/vectors/                  тест-векторы от oracle
docs/                           отчёты и исследование (по-русски)
```

## Известные факты о версиях (из jar'ов)
- 26.1 (world_version 4786), 26.2 (4903), 26.3 (5023). Java 25.
- 26.1→26.2: новый биом `sulfur_caves` (подземный, добавлен в OverworldBiomeBuilder.addUndergroundBiome), новый шум `sulfur_cave_gradient`, пресет `flat_all_dimensions`.
- 26.2→26.3: КРУПНЫЙ рефакторинг: пакет `levelgen/densityfunction` (DensityFunctionCompiler, DensitySampler, SamplerContext, op/*), `synth/{NoiseStack,GradientNoise,SmearedPerlinNoise,LegacyFbmInitializer}`;
  NormalNoise теперь строится из `NoiseStack` с `float`-накоплением (`Noise.get` возвращает float), «parity»-нормализация;
  новый биом `dappled_forest` (в MIDDLE_BIOMES_VARIANT[1][0]), новый structure_set `abandoned_camp`, шум `small_patch`;
  density-функции вынесены в JSON (final_density, ore_vein/*, temperature, vegetation, chunk_surface_level ...);
  `spawnTarget` теперь на density-функциях.
  => ЧИСЛЕННО 26.3 может отличаться от 26.2 (float vs double), нужна проверка на реальном коде.

## Соглашения
- Эталон правды — реальный код игры (oracle), а не cubiomes/память. Любое утверждение в docs подкрепляй ссылкой `файл:строка` из src/dec/<V>/ или результатом запуска.
- Пишешь код — запусти и проверь; в docs указывай реальные цифры (скорость, число совпадений).
- Общие данные между инструментами — только через `data/` и `tests/vectors/`.
- Временные файлы — вне репозитория (`$TMPDIR`), бинарники — в `crack/bin/` (в git не попадают).

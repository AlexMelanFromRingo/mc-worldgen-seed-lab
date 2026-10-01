#!/usr/bin/env bash
# Перерисовывает бейджи README, сделанные blazon (docs/badges/*.svg; остальные бейджи в README — shields.io)
# генератор blazon — https://github.com/AlexMelanFromRingo/blazon
# Использование:  BLAZON="node /путь/к/blazon/bin/blazon.js" tools/make_badges.sh     (или `blazon` из npm, если установлен)
set -euo pipefail
cd "$(dirname "$0")/.."
B=${BLAZON:-blazon}; O=docs/badges; ST=flat-square; mkdir -p "$O"
# бейдж Minecraft — blazon, со значком-кубиком (собственный рисунок, docs/badges/logo-block.svg); остальные «стандартные» бейджи README — shields.io
LOGO="data:image/svg+xml;base64,$(base64 -w0 $O/logo-block.svg)"
$B --label Minecraft  --message '26.1 – 26.3'             --color '#62b47a' --style $ST --logo "$LOGO" --out $O/minecraft.svg
$B --label dimensions --message 'Overworld · Nether · End' --color '#8957e5' --style $ST --out $O/dimensions.svg
$B --label bedrock    --message '2⁴⁸ in 0.16 s'           --color '#da3633' --style $ST --out $O/bedrock.svg
# анимированный бейдж с результатами проверок (числа — из docs/21, 22, 23 и docs/oracle.md)
$B --label check --message '1.24M pts vs the game · 0 diff' --color '#3fb950' --style $ST --delay 2200 \
   --frames 'GPU ↔ CPU|15.7M pts · 0 diff|#3fb950;known answer|300 / 300 seeds|#3fb950;crackers|713 checks · 0 failures|#3fb950;nether bedrock|2⁴⁸ sweep in 0.16 s|#da3633' \
   --out $O/results.svg
# матрица поддержки версий (таблица blazon grid)
$B grid 'version|26.1~#2f81f7|26.2~#2f81f7|26.3~#2f81f7|26.4-snapshot-2~#8957e5;noise math|double~#d29922|double~#d29922|float~#da3633|float~#da3633;engine + oracle|ok~#3fb950|ok~#3fb950|ok~#3fb950|ok~#3fb950;GPU crackers|ok~#3fb950|ok~#3fb950|ok~#3fb950|n/a~#9f9f9f' \
   --style $ST --out $O/matrix.svg

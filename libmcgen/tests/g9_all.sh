#!/usr/bin/env bash
# g9_all.sh — полный прогон ворот G9 (GPU ≡ CPU): биомы (≥10^7 точек), рельеф (≥256 чанков × ≥3 seed × 3 измерения), регионы MCR1.
# Использование: libmcgen/tests/g9_all.sh [каталог сборки (B)=build] ; логи — libmcgen/build/g9/*.log. Нужны libmcgen/build/gpu/libmcgen_cuda.so и run/pack-*.
set -u
cd "$(dirname "$0")/../.."
B="${1:-libmcgen/build}"
LIB="$PWD/libmcgen/build/gpu/libmcgen_cuda.so"
LOG=libmcgen/build/g9; mkdir -p "$LOG"
G() { flock /tmp/gpu.lock "$@"; }
echo "== биомы"      ; G $B/tests/g9_biomes --run run --lib "$LIB" --points 500000 --seeds 4 --grids 6 > $LOG/g9_biomes_full.log 2>&1; tail -1 $LOG/g9_biomes_full.log
echo "== рельеф"     ; G $B/tests/g9_terrain --run run --lib "$LIB" --versions 26.3,26.4-snapshot-2 --chunks 256 --seeds 3 --presets > $LOG/g9_terrain_presets.log 2>&1; tail -1 $LOG/g9_terrain_presets.log
echo "== рельеф+настройки"; G $B/tests/g9_terrain --run run --lib "$LIB" --versions 26.3,26.4-snapshot-2 --chunks 256 --seeds 3 --presets --tweaks > $LOG/g9_terrain_tweaks.log 2>&1; tail -1 $LOG/g9_terrain_tweaks.log
echo "== рельеф+постройки"; G $B/tests/g9_terrain --run run --lib "$LIB" --versions 26.3,26.4-snapshot-2 --chunks 256 --seeds 3 --structures > $LOG/g9_terrain_structures.log 2>&1; tail -1 $LOG/g9_terrain_structures.log
echo "== регионы"    ; python3 libmcgen/tests/g9_region.py --cli $B/mcgen-cli --lib "$LIB" --versions 26.1,26.3,26.4-snapshot-2 --seeds 3 --size 16 --stages 0x3,0x7,0xf > $LOG/g9_region.log 2>&1; tail -1 $LOG/g9_region.log

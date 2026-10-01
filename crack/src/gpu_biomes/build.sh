#!/usr/bin/env bash
# Сборка GPU-биомов. Использование: crack/src/gpu_biomes/build.sh [crack-biomes|test|bench|all]
# ВАЖНО: --fmad=false (нет FMA-слияний => бит-точность с Java); без -use_fast_math; хост: -ffp-contract=off.
# Инкрементальная: объекты пересобираются, только если изменились исходники/заголовки.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${OUT_DIR:-$ROOT/crack/bin}"; OBJ="$ROOT/crack/src/gpu_biomes/obj"; mkdir -p "$OUT" "$OBJ"
ARCH="${CUDA_ARCH:-sm_89}"
NVFLAGS=(-O3 -std=c++17 -arch=$ARCH --fmad=false -Xptxas -O3 -lineinfo -Xcompiler -ffp-contract=off,-fopenmp,-O2)
HDRS=("$ROOT"/engine/*.h "$ROOT"/engine/cuda/*.h "$ROOT"/engine/cuda/*.cuh "$HERE"/gpu_biomes.h)
stale() { local o="$1"; shift; [ ! -f "$o" ] && return 0; for f in "$@"; do [ "$f" -nt "$o" ] && return 0; done; return 1; }
if stale "$OBJ/mcg_host.o" "$ROOT/engine/cuda/mcg_host.c" "${HDRS[@]}"; then
  gcc -O2 -ffp-contract=off -fopenmp -std=gnu11 -c "$ROOT/engine/cuda/mcg_host.c" -o "$OBJ/mcg_host.o"; fi
if stale "$OBJ/gpu_biomes.o" "$HERE/gpu_biomes.cu" "${HDRS[@]}"; then
  nvcc "${NVFLAGS[@]}" -c "$HERE/gpu_biomes.cu" -o "$OBJ/gpu_biomes.o"; fi
what="${1:-all}"
link() { if stale "$OUT/$2" "$HERE/$1.cu" "$OBJ/gpu_biomes.o" "$OBJ/mcg_host.o"; then
           nvcc "${NVFLAGS[@]}" "$HERE/$1.cu" "$OBJ/gpu_biomes.o" "$OBJ/mcg_host.o" -o "$OUT/$2" -lgomp; fi; echo "OK: $OUT/$2"; }
[ "$what" = all -o "$what" = crack-biomes ] && link crack_biomes crack-biomes
[ "$what" = all -o "$what" = test ] && link test_gpu_biomes gpu-biomes-test
[ "$what" = all -o "$what" = bench ] && link bench_gpu_biomes gpu-biomes-bench
exit 0

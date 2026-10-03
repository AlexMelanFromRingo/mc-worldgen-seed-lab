#!/usr/bin/env bash
# Сборка необязательной библиотеки libmcgen_cuda (Linux): nvcc, бит-точность — --fmad=false, без fast-math.
#   libmcgen/gpu/build.sh                    # sm_89 (RTX 40xx) + PTX compute_75 для совместимости вперёд -> libmcgen/build/gpu/libmcgen_cuda.so
#   CUDA_ARCH="75 80 86 89" libmcgen/gpu/build.sh   # несколько cubin-архитектур (релизная сборка)
#   OUT=/путь libmcgen/gpu/build.sh          # другой каталог результата
# Нужен только CUDA Toolkit при сборке; у пользователя — драйвер NVIDIA (libcuda.so); cudart и libstdc++ слинкованы статически.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/../build/gpu}"; mkdir -p "$OUT"
NVCC="${NVCC:-nvcc}"
ARCHS="${CUDA_ARCH:-89}"
PTX_ARCH="${CUDA_PTX_ARCH:-75}"
GEN=()
for a in $ARCHS; do a="${a#sm_}"; GEN+=(-gencode "arch=compute_${a},code=sm_${a}"); done
GEN+=(-gencode "arch=compute_${PTX_ARCH},code=compute_${PTX_ARCH}")
SRCS=("$HERE"/mcgpu_core.cu "$HERE"/mcgpu_biome.cu)
[ -f "$HERE/mcgpu_terrain.cu" ] && SRCS+=("$HERE/mcgpu_terrain.cu")
set -x
"$NVCC" -O3 -std=c++17 --shared -Xcompiler -fPIC,-ffp-contract=off,-fvisibility=hidden -Xlinker -z,noexecstack \
  "${GEN[@]}" --fmad=false -prec-div=true -prec-sqrt=true -ftz=false \
  -Xptxas -O3 -lineinfo -I"$HERE" ${EXTRA_NVFLAGS:-} \
  -Xcompiler -static-libstdc++,-static-libgcc \
  -o "$OUT/libmcgen_cuda.so" "${SRCS[@]}"
set +x
echo "OK: $OUT/libmcgen_cuda.so"

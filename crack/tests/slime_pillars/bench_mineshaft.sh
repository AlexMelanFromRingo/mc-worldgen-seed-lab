#!/usr/bin/env bash
# Замеры crack-mineshaft: скорость ядра на диапазонах 2^34..2^40 (лучшее из 3), CPU-вариант на 2^30, экстраполяция на 2^48.
# Полный 2^48: run_mineshaft_full.sh (30-60 мин).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"; BIN="$ROOT/crack/bin"
S=${1:-123456789012345}
mkdir -p "$HERE/results"; OUT="$HERE/results/bench-mineshaft.txt"; : > "$OUT"
echo "# загрузка GPU перед замером: $(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader)" | tee -a "$OUT"
for bits in 34 36 38 40 42; do
  BASE=$(python3 -c "print(($S & ((1<<48)-1)) & ~((1<<$bits)-1))")
  best=""
  for rep in 1 2 3; do
    [ $bits -ge 42 ] && [ $rep -gt 1 ] && break
    r=$("$BIN/crack-mineshaft" --gen $S,8 --range $BASE,$bits 2>&1 | grep "перебор" | grep -o "перебор [0-9.]* с (GPU; [0-9.e+]* W/с)")
    echo "  GPU 2^$bits rep$rep: $r" | tee -a "$OUT"
  done
done
for bits in 28 30; do
  BASE=$(python3 -c "print(($S & ((1<<48)-1)) & ~((1<<$bits)-1))")
  r=$("$BIN/crack-mineshaft-cpu" --gen $S,8 --range $BASE,$bits 2>&1 | grep "перебор" | grep -o "перебор [0-9.]* с (CPU; [0-9.e+]* W/с)")
  echo "  CPU(12 потоков) 2^$bits: $r" | tee -a "$OUT"
done
echo "# загрузка GPU после замера: $(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader)" | tee -a "$OUT"

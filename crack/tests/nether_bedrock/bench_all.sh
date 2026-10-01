#!/usr/bin/env bash
# Замеры скорости: варианты ядра GPU, число плотных проверок, K; CPU (моя версия, 1 поток) и оригинал (Rust, 1 поток; опционально полный проход).
# Машина общая (другие агенты грузят GPU/CPU), поэтому используется min по многим коротким запускам (--bench R); loadavg/GPU-загрузка пишутся в вывод.
# Использование: bench_all.sh [файл наблюдений] > results/bench.txt        Переменные: NB_BIN, NB_FULL_ORIG=1 (полный проход оригинала, ~2.5 мин), NB_FULL_CPU=1
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${NB_BIN:-$ROOT/crack/bin/crack-nether-bedrock}"
F="${1:-$HERE/data/example_seed_765906787396911863.txt}"
L="${NB_GPULOCK-flock /tmp/gpu.lock}"      # GPU делят несколько агентов: каждый GPU-прогон — под замком (NB_GPULOCK="" отключает)
echo "# bench_all: $(date '+%F %T'); файл $(basename "$F"); uptime: $(uptime | sed 's/.*load/load/')"
echo "# GPU: $(nvidia-smi --query-gpu=name,utilization.gpu,clocks.sm --format=csv,noheader 2>/dev/null)"
echo "## ядра GPU (K=12), --bench 200 (2^26 префиксов на запуск, оценка полного прохода по min)"
for k in 0 1 2 3; do echo -n "kernel $k: "; $L "$BIN" --quiet --kernel $k --bench 200 "$F" | sed 's/^bench: //'; done
for D in 2 4 6 8 10; do echo -n "kernel 4, dense $D: "; $L "$BIN" --quiet --kernel 4 --dense $D --bench 200 "$F" | sed 's/^bench: //'; done
echo "## верхний слой K (kernel 4, dense по умолчанию)"
for K in 9 10 11 12 13; do echo -n "K=$K: "; $L "$BIN" --quiet --top-bits $K --bench 100 "$F" | sed 's/^bench: //'; done
echo "## полный прогон «под ключ» (GPU), 7 запусков: wall и время скана"
for i in 1 2 3 4 5 6 7; do $L /usr/bin/time -f 'wall=%e c' "$BIN" "$F" 2>&1 >/dev/null | grep -E "скан|wall=" | sed 's/.*время: //' | tr '\n' ' '; echo; done
echo "## CPU, 1 поток (user-секунды на 2^29 префиксов)"
CPU="$ROOT/crack/bin/crack-nether-bedrock-cpu"
[ -x "$CPU" ] && for i in 1 2 3; do echo -n "моя CPU-версия: "; /usr/bin/time -f 'user=%U sys=%S wall=%e' "$CPU" --threads 1 --prefix-count 536870912 --force --quiet "$F" 2>&1 >/dev/null | tail -1; done
OB="$HERE/orig_bench/target/release/orig_bench"
[ -x "$OB" ] && for i in 1 2 3; do echo -n "оригинал (Rust): "; /usr/bin/time -f 'user=%U sys=%S wall=%e' "$OB" "$F" 1 --structure --max-events 16 2>&1 | tail -1; done
if [ "${NB_FULL_ORIG:-0}" = 1 ] && [ -x "$OB" ]; then echo "## полный проход оригинала, 12 потоков"; uptime | sed 's/.*load/load/'; /usr/bin/time -f 'user=%U sys=%S wall=%e' "$OB" "$F" 12 --structure 2>&1 | tail -4; fi
if [ "${NB_FULL_CPU:-0}" = 1 ] && [ -x "$CPU" ]; then echo "## полный проход моей CPU-версии, 12 потоков"; uptime | sed 's/.*load/load/'; /usr/bin/time -f 'user=%U sys=%S wall=%e' "$CPU" --threads 12 "$F" 2>&1 | tail -4; fi

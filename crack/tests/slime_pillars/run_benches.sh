#!/usr/bin/env bash
# Замеры скорости (несколько минут). GPU делят несколько агентов: каждый GPU-прогон берёт замок `flock /tmp/gpu.lock` (прогоны короткие),
# перед замером записывается загрузка GPU сторонними процессами (nvidia-smi). Полный перебор 2^48 шахт (~30 мин) здесь НЕ запускается — см. run_mineshaft_full.sh.
#   run_benches.sh [шаг...]   шаги: slime-gpu slime-cpu slime-brute-gpu slime-brute-cpu pillars mineshaft   (по умолчанию: все, кроме slime-brute-cpu)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"; mkdir -p results
export CRACK_TEST_TMP="${CRACK_TEST_TMP:-${TMPDIR:-/tmp}/slp_bench_tmp}"
STEPS=("$@"); [ ${#STEPS[@]} -eq 0 ] && STEPS=(slime-gpu slime-cpu pillars mineshaft slime-brute-gpu)
LOCK="flock /tmp/gpu.lock"
for s in "${STEPS[@]}"; do
  case "$s" in
    slime-gpu)        $LOCK python3 bench_slime.py gpu ABC analytic > results/bench-slime-gpu-analytic.txt 2>&1 ;;
    slime-cpu)        python3 bench_slime.py cpu ABC analytic > results/bench-slime-cpu-analytic.txt 2>&1 ;;
    slime-brute-gpu)  $LOCK python3 bench_slime.py gpu ABC brute > results/bench-slime-gpu-brute.txt 2>&1 ;;
    slime-brute-cpu)  python3 bench_slime.py cpu ABC brute > results/bench-slime-cpu-brute.txt 2>&1 ;;
    pillars)          $LOCK python3 bench_pillars.py > results/bench-pillars.txt 2>&1 ;;
    mineshaft)        $LOCK ./bench_mineshaft.sh > /dev/null 2>&1 ;;
  esac
  echo "$s done" >> results/bench-progress.log
done
echo done > results/bench-chain.done

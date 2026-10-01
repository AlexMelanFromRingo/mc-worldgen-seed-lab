#!/usr/bin/env bash
# Запускает gpu-biomes-bench, когда GPU свободен (другие процессы на этой же карте искажают замеры: GPU общая).
# Ждёт до MAXWAIT секунд, пока загрузка GPU < 5% подряд NQ секунд; после прогона печатает загрузку GPU по секундам
# (если во время прогона кто-то ещё грузил GPU — цифры занижены, см. выведенные строки "util").
# Использование: bench_quiet.sh [аргументы gpu-biomes-bench...]
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"
MAXWAIT="${MAXWAIT:-1800}"; NQ="${NQ:-4}"
quiet=0; waited=0
while [ $quiet -lt $NQ ]; do
  u=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits | head -1)
  if [ "$u" -lt 5 ]; then quiet=$((quiet+1)); else quiet=0; fi
  sleep 1; waited=$((waited+1))
  if [ $waited -ge $MAXWAIT ]; then echo "# GPU занята >$MAXWAIT с, запускаю как есть" >&2; break; fi
done
( while true; do nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits | head -1 >> /tmp/bench_util.$$; sleep 1; done ) &
mon=$!
MCGEN_ROOT="$ROOT" "$ROOT/crack/bin/gpu-biomes-bench" "$@"
kill $mon 2>/dev/null
echo "# util по секундам во время прогона (%): $(tr '\n' ' ' < /tmp/bench_util.$$ | cut -c1-600)"
rm -f /tmp/bench_util.$$

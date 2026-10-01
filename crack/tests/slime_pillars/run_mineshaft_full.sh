#!/usr/bin/env bash
# Полный перебор 2^48 для 8 стартовых чанков шахт (наблюдения из реального кода игры, если есть oracle-данные, иначе синтетика --gen).
# Запуск: flock /tmp/gpu.lock run_mineshaft_full.sh [seed]   (~30 мин на RTX 4080 SUPER при свободном GPU — занимает GPU надолго, запускать только по договорённости). Результат: results/mineshaft-full.txt
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"; BIN="$ROOT/crack/bin"
S=${1:-123456789012345}
mkdir -p "$HERE/results"; OUT="$HERE/results/mineshaft-full.txt"
TMPF="${TMPDIR:-/tmp}/ms_full_obs.txt"
CRACK_GEN_OUT="$TMPF" "$BIN/crack-mineshaft" --gen $S,8 --range 0,1 >/dev/null 2>&1 || true   # только чтобы сгенерировать файл наблюдений
{
  echo "# seed $S, наблюдения:"; cat "$TMPF"
  echo "# загрузка GPU перед запуском: $(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader)"
  date
  /usr/bin/time -v "$BIN/crack-mineshaft" --progress "$TMPF" 2>&1 | grep --line-buffered -E "прогресс|найдено|время|тест|Elapsed|Maximum resident|^[0-9]+ 0x"
  rc=${PIPESTATUS[0]}
  date
  echo "# код возврата crack-mineshaft: $rc"
  echo "# загрузка GPU после: $(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader)"
} | tee "$OUT"

#!/usr/bin/env bash
# Исследование (п.4): биомный поиск БЕЗ структур — реальные скорости полного перебора диапазонов seed'ов.
# Требует файлы наблюдений от oracle (make_obs.py) для seed 8675309. Результат — в stdout.
#   research_rates.sh OBSDIR [--full32]     (--full32: полный перебор всех 2^32 значений -2^31..2^31-1 для Overworld 26.3)
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; export MCGEN_ROOT="$ROOT"
D="${1:?каталог с obs_<V>_<dim>.txt}"; shift
BIN="$ROOT/crack/bin/crack-biomes"
echo "# диапазон 2^28 seed'ов (8 наблюдений, оценка скорости), GPU"
for v in 26.1 26.3; do for d in overworld nether end; do
  r=$("$BIN" --version $v --dim $d --range -134217728 268435456 --obs "$D/obs_${v}_${d}.txt" 2>&1 >/dev/null | grep "кандидатов")
  echo "$v $d: $r"
done; done
if [ "$1" = "--full32" ]; then
  echo "# ПОЛНЫЙ перебор 2^32 (все значения знакового int32) Overworld 26.3, 8 наблюдений, oracle-seed 8675309"
  ( time "$BIN" --version 26.3 --dim overworld --range -2147483648 4294967296 --obs "$D/obs_26.3_overworld.txt" -v ) 2>&1 | grep -v "прогресс" | tail -12
fi

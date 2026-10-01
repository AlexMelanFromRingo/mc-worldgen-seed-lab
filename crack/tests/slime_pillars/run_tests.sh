#!/usr/bin/env bash
# Полный набор тестов crack-slime / crack-pillars / crack-mineshaft против oracle для 26.1, 26.2, 26.3.
#   tests/slime_pillars/run_tests.sh [--quick] [версии...]
# Предусловие: make -f crack/Makefile.slp all cpu ; oracle собирается автоматически (oracle/run.sh).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="$ROOT/crack/bin"
QUICK=""; VERS=()
for a in "$@"; do if [ "$a" = "--quick" ]; then QUICK="--quick"; else VERS+=("$a"); fi; done
[ ${#VERS[@]} -eq 0 ] && VERS=(26.1 26.2 26.3)
mkdir -p "$HERE/results"
rc=0
echo "== самопроверки ядер =="
for t in slime pillars mineshaft; do
  "$BIN/crack-$t" --selftest || rc=1
done
echo "== slime: аналитический алгоритм == перебор 2^30 (случайные конфигурации, без oracle) =="
python3 "$HERE/test_slime_algos.py" ${QUICK:+12} > "$HERE/results/slime-algos.log"; r1=$?; tail -2 "$HERE/results/slime-algos.log"; [ $r1 -eq 0 ] || rc=1
for V in "${VERS[@]}"; do
  echo "== oracle-тесты $V =="
  python3 "$HERE/test_oracle.py" "$V" $QUICK | tee "$HERE/results/oracle-$V.log" | grep -E "^(FAIL|==)" || true
  [ ${PIPESTATUS[0]} -eq 0 ] || rc=1
  python3 "$HERE/test_extra.py" "$V" | tee "$HERE/results/extra-$V.log" | grep -E "^(FAIL|==)" || true
  [ ${PIPESTATUS[0]} -eq 0 ] || rc=1
done
python3 "$HERE/compare_versions.py" "${VERS[@]}" || rc=1
[ $rc -eq 0 ] && echo "ВСЕ ТЕСТЫ ПРОЙДЕНЫ" || echo "ЕСТЬ ПРОВАЛЫ"
exit $rc

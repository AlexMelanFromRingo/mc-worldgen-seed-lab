#!/usr/bin/env bash
# Сверка с оригиналом (Rust, 12 потоков, полный проход 2^48) на наших файлах: наборы найденных structure seed должны совпасть.
# Использование: crosscheck_orig.sh файл [--paper] ...     (каждый файл ≈ 2.5–4 минуты на загруженной машине)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${NB_BIN:-$ROOT/crack/bin/crack-nether-bedrock}"
OB="$HERE/orig_bench/target/release/orig_bench"
cp "$BIN" "${TMPDIR:-/tmp}/nb-cc-bin.$$" && BIN="${TMPDIR:-/tmp}/nb-cc-bin.$$"
for spec in "$@"; do
  f="${spec%%:*}"; opt=""; [ "$spec" != "$f" ] && opt="--paper"
  echo "== $(basename "$f") $opt   (load: $(uptime | sed 's/.*load average: //'))"
  mine=$("$BIN" --quiet ${opt/--paper/--paper1_18} "$f" 2>/dev/null | grep -Ex '[0-9]+' | sort -n | tr '\n' ' ')
  s=$(date +%s.%N)
  "$OB" "$f" 12 --structure $opt > "${TMPDIR:-/tmp}/orig_cc.$$" 2>&1
  e=$(date +%s.%N)
  cat "${TMPDIR:-/tmp}/orig_cc.$$"
  orig=$(grep '^seed ' "${TMPDIR:-/tmp}/orig_cc.$$" | awk '{print $2}' | sort -n | tr '\n' ' ')
  echo "мой:      $mine"; echo "оригинал: $orig   (wall $(echo "$e - $s" | bc) с)"
  [ "$mine" = "$orig" ] && echo "СОВПАДАЕТ" || echo "РАСХОЖДЕНИЕ"
done

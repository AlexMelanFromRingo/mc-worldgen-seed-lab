#!/usr/bin/env bash
# Использование: tools/verify/run_rngdump.sh 26.3 > out.txt   (запускает реальный код игры выбранной версии)
set -e
V="${1:?версия}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CP="$("$ROOT/tools/classpath.sh" "$V")"
OUT="${TMPDIR:-/tmp}/rngdump-$V"
mkdir -p "$OUT"
javac -nowarn -cp "$CP" -d "$OUT" "$ROOT/tools/verify/RngDump.java" 2>&1 | grep -v -E "^Note:" >&2 || true
java -cp "$OUT:$CP" RngDump

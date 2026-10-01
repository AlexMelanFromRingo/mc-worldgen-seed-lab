#!/usr/bin/env bash
# Компилирует и запускает GameRef против реального jar игры. Использование: run_gameref.sh 26.3 > vectors/gameref-26.3.txt
set -e
V="${1:-26.3}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CP=$("$ROOT/tools/classpath.sh" "$V")
OUT="${TMPDIR:-/tmp}/gameref-classes-$V"
mkdir -p "$OUT"
javac -nowarn -cp "$CP" -d "$OUT" "$ROOT/crack/experiments/java/GameRef.java" 2>&1 | grep -v "^Note:" >&2 || true
java -cp "$OUT:$CP" GameRef 2>/dev/null | sed -E "s/^\[[0-9:]+\] \[main\/INFO\]: \[STDOUT\]: //" | grep -E "^(struct|slime|pillar|pillarseed|bedrock|hash|stronghold|mineshaft) "

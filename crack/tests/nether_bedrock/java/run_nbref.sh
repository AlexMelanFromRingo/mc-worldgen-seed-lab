#!/usr/bin/env bash
# Запуск эталона NbRef на реальном коде игры:  run_nbref.sh <26.1|26.2|26.3>  < команды.txt
# Компилирует против jar игры и oracle/build/<V>/classes (oracle нужен только для Compat.bootstrap/loadRegistries/newRandomState).
set -euo pipefail
V="${1:?версия}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUTB="${NBREF_BUILD:-${TMPDIR:-/tmp}/nbref-classes-$V}"
[ -d "$ROOT/oracle/build/$V/classes" ] || "$ROOT/oracle/build.sh" "$V" >&2
CP="$("$ROOT/tools/classpath.sh" "$V"):$ROOT/oracle/build/$V/classes"
if [ ! -f "$OUTB/NbRef.class" ] || [ "$HERE/NbRef.java" -nt "$OUTB/NbRef.class" ]; then
  mkdir -p "$OUTB"; javac -nowarn -encoding UTF-8 -cp "$CP" -d "$OUTB" "$HERE/NbRef.java" 2>&1 | grep -v '^Note:' >&2 || true
fi
exec java -Xss8m -Xmx3g --sun-misc-unsafe-memory-access=allow \
  -Dlog4j2.configurationFile="$ROOT/oracle/log4j2.xml" -Dlog4j.configurationFile="$ROOT/oracle/log4j2.xml" \
  -cp "$OUTB:$CP" NbRef

#!/usr/bin/env bash
# Запуск эталона: run.sh <26.1|26.2|26.3> <команда> [аргументы...]   (или: run.sh <V> serve)
# Собирает автоматически, если классов ещё нет.
set -euo pipefail
V="${1:?использование: run.sh <26.1|26.2|26.3> <команда> ...}"; shift
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
[ -d "$HERE/build/$V/classes" ] || "$HERE/build.sh" "$V" >&2
CP="$("$ROOT/tools/classpath.sh" "$V")"
exec java -Xss8m ${ORACLE_JAVA_OPTS:--Xmx4g} \
  --sun-misc-unsafe-memory-access=allow \
  -Dlog4j2.configurationFile="$HERE/log4j2.xml" -Dlog4j.configurationFile="$HERE/log4j2.xml" \
  -Doracle.version="$V" \
  -cp "$HERE/build/$V/classes:$CP" oracle.Main "$@"

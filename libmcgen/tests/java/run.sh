#!/usr/bin/env bash
# Запуск эталона: run.sh <V>  (команды из stdin, см. Ref.java)
set -euo pipefail
V="${1:?run.sh <V>}"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"
[ -d "$ROOT/libmcgen/build/java/$V/mcgenref" ] || "$HERE/build.sh" "$V" >&2
CP="$ROOT/libmcgen/build/java/$V:$ROOT/oracle/build/$V/classes:$("$ROOT/tools/classpath.sh" "$V")"
exec java -Xss8m ${MCGENREF_JAVA_OPTS:--Xmx3g} --sun-misc-unsafe-memory-access=allow \
  -Dlog4j2.configurationFile="$ROOT/oracle/log4j2.xml" -Doracle.version="$V" -cp "$CP" mcgenref.Ref

#!/usr/bin/env bash
# Сборка эталона libmcgen (Java, настоящие классы игры + загрузка реестров из oracle): build.sh <V>  → libmcgen/build/java/<V>
set -euo pipefail
V="${1:?build.sh <26.1|26.2|26.3>}"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"
case "$V" in 26.1|26.2) SRC=old ;; *) SRC=new ;; esac
[ -d "$ROOT/oracle/build/$V/classes" ] || "$ROOT/oracle/build.sh" "$V" >&2
CP="$ROOT/oracle/build/$V/classes:$("$ROOT/tools/classpath.sh" "$V")"
OUT="$ROOT/libmcgen/build/java/$V"; mkdir -p "$OUT"
javac -nowarn -Xlint:none -encoding UTF-8 -d "$OUT" -cp "$CP" "$HERE/$SRC/Ref.java"
echo "OK: $OUT"

#!/usr/bin/env bash
# Сборка Java-эталона стадии CARVERS (настоящие классы игры, без сервера): build.sh <26.1|26.2|26.3|26.4-snapshot-2>
set -euo pipefail
V="${1:?build.sh <26.1|26.2|26.3|26.4-snapshot-2>}"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"
case "$V" in 26.1|26.2) SRC=old ;; 26.3) SRC=new ;; *) SRC=v264 ;; esac
[ -d "$ROOT/oracle/build/$V/classes" ] || "$ROOT/oracle/build.sh" "$V" >&2
CP="$ROOT/oracle/build/$V/classes:$("$ROOT/tools/classpath.sh" "$V")"
OUT="${G4JAVA_OUT:-$ROOT/libmcgen/build/g4java}/$V"; mkdir -p "$OUT"
javac -nowarn -Xlint:none -encoding UTF-8 -d "$OUT" -cp "$CP" "$HERE/$SRC/CarveRef.java"
echo "OK: $OUT"

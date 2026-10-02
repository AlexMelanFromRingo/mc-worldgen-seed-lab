#!/usr/bin/env bash
# Сборка эталона стадии SURFACE (Java, настоящие классы игры + реестры oracle): build.sh <V>  → libmcgen/build/g3java/<V>
set -euo pipefail
V="${1:?build.sh <26.1|26.2|26.3|26.4-snapshot-2>}"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"
case "$V" in 26.1|26.2) SRC=old ;; 26.3) SRC=new ;; *) SRC=v264 ;; esac
[ -d "$ROOT/oracle/build/$V/classes" ] || "$ROOT/oracle/build.sh" "$V" >&2
CP="$ROOT/oracle/build/$V/classes:$("$ROOT/tools/classpath.sh" "$V")"
OUT="${G3JAVA_OUT:-$ROOT/libmcgen/build/g3java}/$V"; mkdir -p "$OUT"
javac -nowarn -Xlint:none -encoding UTF-8 -d "$OUT" -cp "$CP" "$HERE/$SRC/SurfRef.java"
echo "OK: $OUT"

#!/usr/bin/env bash
# Сборка эталона для версии $1 (26.1 | 26.2 | 26.3). Результат: oracle/build/<V>/classes (обновляется атомарно).
set -euo pipefail
V="${1:?использование: build.sh <26.1|26.2|26.3>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
case "$V" in
  26.1|26.2) VL=v261_262 ;;
  26.3)      VL=v263 ;;
  26.4*)     VL=v264 ;;
  *) echo "неподдерживаемая версия: $V" >&2; exit 2 ;;
esac
CP="$("$ROOT/tools/classpath.sh" "$V")"
B="$HERE/build/$V"
mkdir -p "$B"
NEW="$B/classes.new.$$"
rm -rf "$NEW"; mkdir -p "$NEW"
if [ "$VL" = v264 ]; then   # 26.4: общий код с заглушкой height (API getBaseHeight изменился)
  PC="$B/common-patched"; rm -rf "$PC"; cp -r "$HERE/src/common" "$PC"
  sed -i 's/d\.gen\.getBaseHeight(/oracle.compat.Compat.baseHeightUnsupported(/g' "$PC"/oracle/*.java
  find "$PC" "$HERE/src/$VL" -name '*.java' | sort > "$B/sources.txt"
else
  find "$HERE/src/common" "$HERE/src/$VL" -name '*.java' | sort > "$B/sources.txt"
fi
javac -nowarn -Xlint:none -encoding UTF-8 -d "$NEW" -cp "$CP" @"$B/sources.txt"
rm -rf "$B/classes.old"
[ -d "$B/classes" ] && mv "$B/classes" "$B/classes.old"
mv "$NEW" "$B/classes"
rm -rf "$B/classes.old"
echo "OK: $B/classes"

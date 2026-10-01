#!/usr/bin/env bash
# Сборка cubiomes (копия refs/cubiomes -> $BUILD) и тестовых программ.
# Использование: ./build.sh [flags-name]   flags-name: release (по умолчанию, -O3) | native (-O3 -march=native)
# Результат: $BUILD/<flags-name>/{libcubiomes.a,bench,...}
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REF="$HERE/../../../refs/cubiomes"
BUILD="${BUILD:-${TMPDIR:-/tmp}/cb_build}"
MODE="${1:-release}"
OUT="$BUILD/$MODE"
mkdir -p "$OUT"
rsync -a --exclude='*.o' --exclude='*.a' "$REF/" "$OUT/src/" 2>/dev/null || { mkdir -p "$OUT/src"; cp -r "$REF"/. "$OUT/src/"; }
case "$MODE" in
  release) CF="-O3 -fwrapv" ;;
  native)  CF="-O3 -fwrapv -march=native" ;;
  fastmath) CF="-O3 -fwrapv -march=native -ffast-math" ;;   # как 'make native' в makefile cubiomes
  *) echo "unknown mode $MODE"; exit 1 ;;
esac
cd "$OUT/src"
for f in noise biomes layers biomenoise generator finders util quadbase; do
  gcc -c $CF -fPIC -Wall -Wextra $f.c -o $f.o
done
ar cr libcubiomes.a noise.o biomes.o layers.o biomenoise.o generator.o finders.o util.o quadbase.o
for prog in "$@"; do :; done
for c in "$HERE"/*.c; do
  n="$(basename "$c" .c)"
  gcc $CF -I. "$c" libcubiomes.a -lm -lpthread -o "$OUT/$n"
done
echo "built in $OUT"

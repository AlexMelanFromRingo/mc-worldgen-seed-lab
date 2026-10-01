#!/usr/bin/env bash
# Печатает classpath для запуска/компиляции против игры версии $1 (26.1 | 26.2 | 26.3).
# Использование: CP=$(tools/classpath.sh 26.3)
set -e
V="${1:?версия}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBS="$ROOT/src/bundle-$V/META-INF/libraries"
CP="$ROOT/jars/game-$V.jar"
while IFS= read -r j; do CP="$CP:$j"; done < <(find "$LIBS" -name '*.jar' | sort)
echo "$CP"

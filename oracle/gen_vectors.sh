#!/usr/bin/env bash
# Генерация тест-векторов в tests/vectors/<V>/ : gen_vectors.sh [версии...]  (по умолчанию 26.1 26.2 26.3, параллельно)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VS=("$@"); [ ${#VS[@]} -gt 0 ] || VS=(26.1 26.2 26.3)
for V in "${VS[@]}"; do "$HERE/build.sh" "$V" >/dev/null; done
for V in "${VS[@]}"; do python3 "$HERE/gen_vectors.py" "$V" & done
wait

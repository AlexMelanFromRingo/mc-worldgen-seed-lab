#!/usr/bin/env bash
# Статистика «сколько блоков нужно» и «сколько ложных кандидатов»: для каждого N генерируется M синтетических наборов из случайных
# structure seed (формула C, сверенная с реальной игрой — см. run_tests.sh [3]), запускается crack-nether-bedrock на полном пространстве 2^48.
# Использование: stats.sh <режим: bedrock|mixed> "<список N>" <M> <y-режим 0|1|2> <выход.tsv> [доля потолка]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${NB_BIN:-$ROOT/crack/bin/crack-nether-bedrock}"
MODE="${1:-bedrock}"; NS="${2:-20 24 28 32 40}"; M="${3:-10}"; YM="${4:-0}"; OUTF="${5:-/dev/stdout}"; ROOF="${6:-0.5}"
TMP="$(mktemp -d)"
echo -e "mode\tN\tseed\tinfo_bits\texpF\tcandF\tfound\tdistinctR\ttruth\tscan_s\ttotal_s" > "$OUTF"
for N in $NS; do
  for ((i=0;i<M;i++)); do
    S=$(python3 -c "import random; r=random.Random($N*1000+$i); print(r.getrandbits(48))")
    "$BIN" --gen "$S" "$N" "$MODE" --gen-y "$YM" --gen-roof "$ROOF" --gen-rng $((N*1000+i)) > "$TMP/o.txt"
    "$BIN" --force --max-cand 134217728 "$TMP/o.txt" > "$TMP/r.out" 2> "$TMP/r.err"; rc=$?
    info=$(grep -o 'всего [0-9.]* бит' "$TMP/r.err" | head -1 | grep -o '[0-9.]*')
    expF=$(grep -o 'кандидатов F основной поверхности ~[0-9.e+-]*' "$TMP/r.err" | grep -o '~.*' | tr -d '~')
    cand=$(grep -o 'кандидатов F: [0-9]*' "$TMP/r.err" | grep -o '[0-9]*$')
    found=$(grep -cE '^[0-9]+$' "$TMP/r.out")
    dR=$(grep -o 'различных R (наборов фабрик бедрока): [0-9]*' "$TMP/r.err" | grep -o '[0-9]*$')
    truth=0; grep -qx "$S" "$TMP/r.out" && truth=1
    scan=$(grep -o 'скан [0-9.]*' "$TMP/r.err" | grep -o '[0-9.]*$'); tot=$(grep -o 'всего [0-9.]* с' "$TMP/r.err" | tail -1 | grep -o '[0-9.]*')
    echo -e "$MODE\t$N\t$S\t$info\t$expF\t$cand\t$found\t$dR\t$truth\t$scan\t$tot" >> "$OUTF"
    [ $rc -ne 0 ] && echo "# rc=$rc N=$N seed=$S" >&2
  done
done
rm -rf "$TMP"

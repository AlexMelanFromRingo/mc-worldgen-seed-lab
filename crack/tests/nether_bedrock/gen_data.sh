#!/usr/bin/env bash
# Генерирует тестовые наблюдения РЕАЛЬНЫМ кодом игры (java/NbRef.java: настоящий RandomState + настоящее правило поверхности Незера)
# для 26.1, 26.2 и 26.3 и проверяет, что файлы побайтно совпадают между версиями. Результат: data/gen/*.txt (хранятся в репозитории).
# Использование: gen_data.sh            (около 1-2 минут: три запуска JVM)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/data/gen"
TMP="${TMPDIR:-/tmp}/nb-gen-$$"; mkdir -p "$OUT" "$TMP"
# метка  world_seed
SEEDS=(
  "s765906787396911863 765906787396911863"
  "sm8788534344520786540 -8788534344520786540"
  "s0 0"
  "s1234567890123456789 1234567890123456789"
  "sm1 -1"
  "s2p62 4611686018427387904"
  "smax 9223372036854775807"
)
# имя  режим  n  ymode  roofFrac  rng  area(радиус области по x и z; far — почти весь мир: проверка переполнения int в Mth.getSeed)
CFGS=(
  "b24 bedrock 24 0 0.5 1 40"
  "b30 bedrock 30 0 0.5 2 40"
  "b40 bedrock 40 0 0.5 3 40"
  "m120 mixed 120 0 0.5 4 40"
  "b100y bedrock 100 2 0.5 5 40"
  "far40 bedrock 40 0 0.5 6 29999000"
)
for V in 26.1 26.2 26.3; do
  mkdir -p "$TMP/$V"
  for s in "${SEEDS[@]}"; do
    read -r lab seed <<<"$s"
    for c in "${CFGS[@]}"; do
      read -r cn mode n ym rf rng area <<<"$c"
      echo "gen $seed $n $mode $area $ym $rf $rng $TMP/$V/${lab}_${cn}.txt"
    done
    echo "floats $seed 17 123 -5"
    echo "floats $seed -98 4 -469"
  done | "$HERE/java/run_nbref.sh" "$V" > "$TMP/$V/floats.txt" 2> "$TMP/$V/log.txt" || { cat "$TMP/$V/log.txt"; exit 1; }
  tail -1 "$TMP/$V/log.txt"
done
ok=1
for V in 26.1 26.2; do
  diff -r "$TMP/$V" "$TMP/26.3" -x log.txt > /dev/null && echo "версия $V: все файлы и значения nextFloat совпадают с 26.3 побайтно" || { echo "РАСХОЖДЕНИЕ $V vs 26.3"; diff -r "$TMP/$V" "$TMP/26.3" -x log.txt | head; ok=0; }
done
[ $ok = 1 ] || exit 1
rm -f "$OUT"/s*.txt
cp "$TMP/26.3/"s*.txt "$OUT/"
cp "$TMP/26.3/floats.txt" "$OUT/real_game_floats.txt"
rm -rf "$TMP"
ls "$OUT" | wc -l

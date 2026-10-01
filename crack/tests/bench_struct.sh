#!/usr/bin/env bash
# Замер скорости ядер crack-struct (полный перебор, окно 2^W): лучший из N прогонов на каждый тип драйвера.
# GPU может быть занят другими процессами — берём максимум. Использование: bench_struct.sh [W=44] [N=3] [версия=26.3]
# W — окно в «эквивалентных» значениях 2^W для быстрых драйверов; для triangular/generic окно на 6 бит меньше.
W=${1:-44}; N=${2:-3}; V=${3:-26.3}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; CS="$ROOT/crack/bin/crack-struct"; T=$(mktemp -d)
SEED=187654321098765
declare -A SETS=(
 [div-test:nether+ruined_portal]="nether_complexes,ruined_portals,nether_complexes,ruined_portals,nether_complexes,ruined_portals,nether_complexes,ruined_portals"
 [pow2-top-bits:ancient_city]="ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities,ancient_cities"
 [triangular:end_city]="end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities,end_cities"
 [triangular:monument+mansion]="ocean_monuments,woodland_mansions,ocean_monuments,woodland_mansions,ocean_monuments,woodland_mansions,ocean_monuments,ocean_monuments"
 [generic:mineshaft]="mineshafts,mineshafts,mineshafts,mineshafts,mineshafts,mineshafts,mineshafts,mineshafts"
)
for k in "${!SETS[@]}"; do
  "$CS" --version "$V" --gen-obs "${SETS[$k]}" --seed $SEED --gen-rng 11 > "$T/o.txt"
  best=0; WW=$W; case "$k" in triangular*|generic*) WW=$((W-6));; esac
  for i in $(seq 1 "$N"); do
    r=$(flock /tmp/gpu.lock "$CS" --version "$V" --mode full --assume-seed $SEED --window-bits "$WW" "$T/o.txt" 2>&1 >/dev/null | grep -oE "\([0-9.e+]+ значений" | tr -d '( значений')
    best=$(python3 -c "print(max($best, float('${r:-0}')))")
  done
  printf "%-32s лучший из %d: %.3e кандидатов/с  (2^48 за %.0f с)\n" "$k" "$N" "$best" "$(python3 -c "print(2**48/$best if $best else 0)")"
done
rm -rf "$T"

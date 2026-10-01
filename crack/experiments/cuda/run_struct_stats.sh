#!/usr/bin/env bash
# Статистика lifting по случайным seed: K структур -> число кандидатов и время (GPU)
sets=("desert_pyramid,igloo,swamp_hut,shipwreck"
      "desert_pyramid,igloo,swamp_hut,shipwreck,jungle_pyramid"
      "desert_pyramid,igloo,swamp_hut,shipwreck,trial_chambers,village"
      "desert_pyramid,igloo,swamp_hut,shipwreck,trial_chambers,village,trail_ruins,jungle_pyramid"
      "village,trial_chambers,trail_ruins,shipwreck,igloo"
      "village,trial_chambers,trail_ruins")
for set in "${sets[@]:0:5}"; do
  hits=0; tot=0; fp=0; tsum=0
  for i in $(seq 1 12); do
    seed=$(( (RANDOM<<33) ^ (RANDOM<<18) ^ (RANDOM<<3) ^ RANDOM ))
    out=$(./struct48 --mode lift --dev gpu --seed $seed --rng $((RANDOM)) --gen "$set" 2>&1)
    l=$(echo "$out" | grep -E "^\[lift\]" | sed -E 's/.*L=([0-9]+).*: ([0-9]+) кандидатов.*/\1 \2/')
    f=$(echo "$out" | grep -E "found [0-9]+$" | sed -E 's/.*found ([0-9]+)$/\1/')
    t=$(echo "$out" | grep -E "всего с lifting" | sed -E 's/.*всего с lifting ([0-9.]+) s.*/\1/')
    echo "$out" | grep -q "true seed recovered" && hits=$((hits+1))
    tot=$((tot+1)); fp=$((fp+f-1)); tsum=$(python3 -c "print($tsum+${t:-0})")
    last_l="$l"
  done
  info=$(echo "$out" | grep "info" | sed -E 's/.*~ ([0-9.]+) bits/\1/')
  echo "набор={$set} info=${info} бит | trials=$tot recovered=$hits | ложных кандидатов в сумме=$fp (среднее $(python3 -c "print(round($fp/$tot,2))")) | среднее время $(python3 -c "print(round($tsum/$tot,3))") s | L,lows(последний)=$last_l"
done

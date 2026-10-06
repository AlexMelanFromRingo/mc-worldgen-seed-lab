#!/bin/bash
# Сверка JSet (feature_tree.c) с java.util.HashSet: одинаковые последовательности add / pop-first, порядок извлечения и итерации совпадает побитно.
# Нужны JDK (java JSetRef.java) и собранная libmcgen/build/libmcgen.a.
set -e
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
cc -Iinclude -Isrc -Igen -I../engine -O1 -std=gnu11 -include stdlib.h -o $T/jset_test tests/g5_jset/jset_test.c build/libmcgen.a -lm -lpthread
ok=0; bad=0
# режимы: 0 — область дерева; 1–6 — плотные и враждебные наборы (много столкновений корзин: деревья-корзины, split/untreeify при resize, удаление из дерева)
for mode in 0 1 2 3 4 5 6; do for seed in 1 2 3 4 5; do for n in 50 400 3000 20000; do
  java tests/g5_jset/JSetRef.java $seed $n $mode > $T/j.txt; $T/jset_test $seed $n $mode > $T/c.txt 2>/dev/null
  if cmp -s $T/j.txt $T/c.txt; then ok=$((ok+1)); else bad=$((bad+1)); echo "РАСХОЖДЕНИЕ режим $mode seed $seed n $n"; fi
done; done; done
echo "JSet против java.util.HashSet: совпало $ok, расхождений $bad"
rm -rf $T
[ $bad -eq 0 ]

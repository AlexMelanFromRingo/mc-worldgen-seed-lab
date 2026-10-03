#!/bin/bash
# g5_tree_one.sh <placed_feature> [число пар] — прогон одной изолированной фичи деревьев и топ расхождений (первый найденный мир feature_<id>)
R=${MCGEN_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
f=$1; n=${2:-12}
w=$(ls -d $R/run/gt/26.3/feature_minecraft_$f/*/ | head -1)
python3 $R/libmcgen/tests/g5_tree_set.py --world $w --only minecraft:$f --list 6 --top $n $4 2>&1 | grep -v "^  y \|^   [-0-9]* [.a-eX~-]*$" | grep -v "^$" | head -${3:-60}

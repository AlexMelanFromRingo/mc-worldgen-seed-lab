#!/bin/bash
# g5_tree_all.sh [margin...] — сводка по всем изолированным мирам деревьев/грибов (имена из списка ниже); по умолчанию margin 0 и 1
R=${MCGEN_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
FEATS=${FEATS:-"birch_tall trees_birch trees_badlands trees_taiga trees_old_growth_pine_taiga trees_old_growth_spruce_taiga trees_jungle trees_sparse_jungle trees_savanna trees_swamp trees_mangrove trees_cherry trees_flower_forest trees_birch_and_oak_leaf_litter dark_forest_vegetation trees_snowy trees_windswept_hills trees_grove trees_meadow trees_dappled_forest trees_water trees_plains trees_windswept_savanna mushroom_island_vegetation rooted_azalea_tree crimson_fungi warped_fungi"}
MARGINS=${@:-0 1}
for f in $FEATS; do
  for w in $R/run/gt/${V:-26.3}/feature_minecraft_$f/*/; do
    [ -f $w/manifest.json ] || continue
    out="$f"
    for m in $MARGINS; do
      r=$(python3 $R/libmcgen/tests/g5_tree_set.py --world $w --only minecraft:$f --version ${V:-26.3} --margin $m --top 0 2>&1 | grep "^сравнено" | sed 's/сравнено //; s/  расхождений/ mism/')
      out="$out | m$m: $r"
    done
    echo "$out"
  done
done

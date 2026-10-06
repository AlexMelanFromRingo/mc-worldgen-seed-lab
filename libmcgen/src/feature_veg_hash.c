/* feature_veg_hash.c — порядок обхода java.util.HashSet<BlockPos> (HashMap) для растительности: набор строится в JSet (jset.c — таблица корзин по правилам
 * putVal/resize, деревья-корзины: treeifyBin при цепочке ≥ 9 и ёмкости ≥ 64, красно-чёрное дерево, moveRootToFront, split при resize, untreeify ≤ 6).
 *
 * Зачем: порядок обхода множества позиций (VegetationPatchFeature.surface и т. п.) определяет, в каком порядке тратится ГСЧ — отличие в одной позиции
 * сдвигает все последующие выборки. Хэш BlockPos = Vec3i.hashCode = (y + z·31)·31 + x; spread = h ^ (h >>> 16).
 * Допущения: ключи различны; при равных (после spread) хэшах в дереве порядок определяет System.identityHashCode (недетерминирован) — берём «влево». */
#include "feature_veg.h"
#include "feature_tree.h"
#include <stdlib.h>

void veg_hashset_order(VPos *a, int n) {
    if (n < 2) return;
    if (getenv("MCGEN_VEG_ORDER")) return;               /* отладка: порядок вставки */
    JSet s; jset_init(&s);
    for (int i = 0; i < n; i++) jset_add(&s, a[i].x, a[i].y, a[i].z);
    if (s.size == n) {                                   /* ключи различны (иначе HashSet сжал бы набор) */
        BList l = {0}; jset_to_list(&s, &l);
        for (int i = 0; i < n; i++) { a[i].x = l.a[i].x; a[i].y = l.a[i].y; a[i].z = l.a[i].z; }
        blist_free(&l);
    }
    jset_free(&s);
}

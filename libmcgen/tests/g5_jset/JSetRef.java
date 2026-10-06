import java.util.*;

/** Эталон порядка java.util.HashSet<BlockPos> для libmcgen/src/feature_tree.c (JSet): детерминированная последовательность операций add / pop-first (iterator.next()+remove()),
 *  печать порядка извлечения и итераций. Запуск: java JSetRef.java <seed> <число операций> [режим]
 *  режим 0 — позиции в области 40×30×40 (как у деревьев), 70 % add; 1 — плотный слой 12×4×12 (много столкновений корзин → деревья-корзины), 60 % add;
 *  2 — плотный кубоид 16×8×16, 55 % add; 3 — слой 24×1×24 (антидиагонали x + z = const делят корзину при ёмкости ≥ 64), 65 % add; 4 — два слоя 28×2×28, 60 % add;
 *  5, 6 — враждебные: x подобран так, что (x + 31y + z) mod 64 одинаков (одна корзина при ёмкости ≥ 64; в режиме 6 — одна из трёх), 60 % add:
 *  гарантируют деревья-корзины, их разрастание, split при resize, untreeify и удаление из дерева. */
public class JSetRef {
    record P(int x, int y, int z) {
        @Override public int hashCode() { return (y + z * 31) * 31 + x; }
    }
    static P hostile(Random r, int mode) {
        int c = mode == 5 ? 17 : 5 + 20 * r.nextInt(3);
        int y = 60, z = r.nextInt(8) + 200;     // y фиксирован: иначе (x, y, z) и (x − 31, y + 1, z) имели бы одинаковый полный хэш (порядок в дереве тогда задаёт identityHashCode)
        int base = ((c - 31 * y - z) % 64 + 64) % 64;
        return new P(base + 64 * r.nextInt(8), y, z);
    }
    public static void main(String[] a) {
        long seed = Long.parseLong(a[0]); int n = Integer.parseInt(a[1]); int mode = a.length > 2 ? Integer.parseInt(a[2]) : 0;
        Random r = new Random(seed);
        HashSet<P> s = new HashSet<>();
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            int op = r.nextInt(10);
            if (op < (mode == 0 ? 7 : mode == 1 ? 6 : mode == 3 ? (i % 20 < 13 ? 10 : 0) : mode >= 4 ? 6 : 5 + (i & 1)) || s.isEmpty()) {
                // позиции, похожие на деревья: небольшая область, отрицательные координаты тоже
                P p = mode == 0 ? new P(r.nextInt(40) - 20 + 300, r.nextInt(30) + 60, r.nextInt(40) - 20 - 500)
                    : mode == 1 ? new P(r.nextInt(12) - 3000, r.nextInt(4) + 70, r.nextInt(12) + 1000)
                    : mode == 2 ? new P(r.nextInt(16) + 17, r.nextInt(8) - 5, r.nextInt(16) - 3000)
                    : mode == 3 ? new P(r.nextInt(24) + 5000, 70, r.nextInt(24) - 100)
                    : mode == 4 ? new P(r.nextInt(28) - 777, r.nextInt(2) + 12, r.nextInt(28) + 40)
                    : hostile(r, mode);
                s.add(p);
            } else {
                Iterator<P> it = s.iterator(); P p = it.next(); it.remove();
                sb.append(p.x()).append(',').append(p.y()).append(',').append(p.z()).append('\n');
            }
        }
        // полный обход в конце
        sb.append("ITER\n");
        for (P p : s) sb.append(p.x()).append(',').append(p.y()).append(',').append(p.z()).append('\n');
        System.out.print(sb);
    }
}

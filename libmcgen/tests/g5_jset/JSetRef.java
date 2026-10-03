import java.util.*;

/** Эталон порядка java.util.HashSet<BlockPos> для libmcgen/src/feature_tree.c (JSet): детерминированная последовательность операций add / pop-first (iterator.next()+remove()),
 *  печать порядка извлечения и итераций. Запуск: java JSetRef.java <seed> <число операций> */
public class JSetRef {
    record P(int x, int y, int z) {
        @Override public int hashCode() { return (y + z * 31) * 31 + x; }
    }
    public static void main(String[] a) {
        long seed = Long.parseLong(a[0]); int n = Integer.parseInt(a[1]);
        Random r = new Random(seed);
        HashSet<P> s = new HashSet<>();
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            int op = r.nextInt(10);
            if (op < 7 || s.isEmpty()) {
                // позиции, похожие на деревья: небольшая область, отрицательные координаты тоже
                P p = new P(r.nextInt(40) - 20 + 300, r.nextInt(30) + 60, r.nextInt(40) - 20 - 500);
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

/* Эталон порядка обхода java.util.HashSet<BlockPos>: читает наборы «x y z» (первая строка набора — n), печатает порядок обхода (индексы вставки). */
import java.util.*;
import java.io.*;

public class HashOrder {
    static final class Pos {
        final int x, y, z, idx;
        Pos(int x, int y, int z, int idx) { this.x = x; this.y = y; this.z = z; this.idx = idx; }
        @Override public int hashCode() { return (y + z * 31) * 31 + x; }          // Vec3i.hashCode
        @Override public boolean equals(Object o) { return o instanceof Pos p && p.x == x && p.y == y && p.z == z; }
    }
    public static void main(String[] a) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        StringBuilder out = new StringBuilder();
        String line;
        while ((line = in.readLine()) != null) {
            int n = Integer.parseInt(line.trim());
            Set<Pos> set = new HashSet<>();
            for (int i = 0; i < n; i++) {
                StringTokenizer t = new StringTokenizer(in.readLine());
                set.add(new Pos(Integer.parseInt(t.nextToken()), Integer.parseInt(t.nextToken()), Integer.parseInt(t.nextToken()), i));
            }
            for (Pos p : set) out.append(p.idx).append(' ');
            out.append('\n');
        }
        System.out.print(out);
    }
}

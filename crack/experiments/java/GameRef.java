import java.util.*;
import java.util.stream.IntStream;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.util.RandomSource;
import net.minecraft.util.Util;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.structure.placement.RandomSpreadStructurePlacement;
import net.minecraft.world.level.levelgen.structure.placement.RandomSpreadType;

/**
 * Эталонные значения из РЕАЛЬНОГО кода игры (jar 26.x) для проверки наших C/CUDA реализаций.
 * Печатает строки "ключ значения..." в stdout. Запуск: crack/experiments/run_gameref.sh <версия>.
 */
public class GameRef {
    record Sp(String name, int spacing, int separation, RandomSpreadType type, int salt) {}

    static final Sp[] SETS = {
        new Sp("desert_pyramid", 32, 8, RandomSpreadType.LINEAR, 14357617),
        new Sp("igloo", 32, 8, RandomSpreadType.LINEAR, 14357618),
        new Sp("jungle_pyramid", 32, 8, RandomSpreadType.LINEAR, 14357619),
        new Sp("swamp_hut", 32, 8, RandomSpreadType.LINEAR, 14357620),
        new Sp("shipwreck", 24, 4, RandomSpreadType.LINEAR, 165745295),
        new Sp("village", 34, 8, RandomSpreadType.LINEAR, 10387312),
        new Sp("trial_chambers", 34, 12, RandomSpreadType.LINEAR, 94251327),
        new Sp("ancient_city", 24, 8, RandomSpreadType.LINEAR, 20083232),
        new Sp("monument", 32, 5, RandomSpreadType.TRIANGULAR, 10387313),
        new Sp("mansion", 80, 20, RandomSpreadType.TRIANGULAR, 10387319),
        new Sp("end_city", 20, 11, RandomSpreadType.TRIANGULAR, 10387313),
        new Sp("ruined_portal", 40, 15, RandomSpreadType.LINEAR, 34222645),
        new Sp("nether_complex", 27, 4, RandomSpreadType.LINEAR, 30084232),
    };

    public static void main(String[] a) {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        long[] seeds = {0L, 1L, -1L, 123456789012345678L, 765906787396911863L, -8788534344520786540L, 0x123456789ABCDEFL};
        // 1. позиции структур: seed, набор, регион (rx,rz) -> chunkX chunkZ
        for (Sp s : SETS) {
            RandomSpreadStructurePlacement p = new RandomSpreadStructurePlacement(s.spacing, s.separation, s.type, s.salt);
            for (long seed : seeds) {
                for (int[] r : new int[][]{{0,0},{1,2},{-1,-1},{-3,5},{40,-77}}) {
                    ChunkPos c = p.getPotentialStructureChunk(seed, r[0] * s.spacing, r[1] * s.spacing);
                    System.out.println("struct " + s.name + " " + seed + " " + r[0] + " " + r[1] + " " + c.x() + " " + c.z());
                }
            }
        }
        // 2. слайм-чанки: seed cx cz -> 0/1
        for (long seed : seeds) {
            for (int cx = -3; cx <= 3; cx++) for (int cz = -3; cz <= 3; cz++) {
                boolean slime = WorldgenRandom.seedSlimeChunk(cx, cz, seed, 987234911L).nextInt(10) == 0;
                System.out.println("slime " + seed + " " + cx + " " + cz + " " + (slime ? 1 : 0));
            }
            // и «далёкие» координаты (переполнение int в x*x*4987142)
            for (int[] c : new int[][]{{100000,-90000},{-1875000,1875000},{1875000,1875000},{46341,46341}}) {
                boolean slime = WorldgenRandom.seedSlimeChunk(c[0], c[1], seed, 987234911L).nextInt(10) == 0;
                System.out.println("slime " + seed + " " + c[0] + " " + c[1] + " " + (slime ? 1 : 0));
            }
        }
        // 3. End pillars: seed -> pillarSeed (nextLong & 65535) и порядок высот
        for (long seed : seeds) {
            long key = RandomSource.createThreadLocalInstance(seed).nextLong() & 65535L;
            IntArrayListWrap sizes = shuffled(key);
            System.out.println("pillar " + seed + " " + key + " " + sizes);
        }
        for (long ps : new long[]{0, 1, 2, 12345, 65535}) {
            System.out.println("pillarseed " + ps + " " + shuffled(ps));
        }
        // 4. бедрок Nether (legacy random): seed x y z -> nextFloat как в MaterialRuleContext.getOrCreateRandomFactory
        for (long seed : seeds) {
            PositionalRandomFactory base = new LegacyRandomSource(seed).forkPositional();
            for (String nm : new String[]{"minecraft:bedrock_roof", "minecraft:bedrock_floor"}) {
                PositionalRandomFactory f = base.fromHashOf(nm).forkPositional();
                for (int[] p : new int[][]{{0,4,0},{-98,4,-469},{17,123,-5},{1000,1,-2000},{-33,3,77},{5,126,6}}) {
                    float v = f.at(p[0], p[1], p[2]).nextFloat();
                    System.out.println("bedrock " + seed + " " + nm + " " + p[0] + " " + p[1] + " " + p[2] + " " + Float.floatToIntBits(v));
                }
            }
        }
        System.out.println("hash " + "minecraft:bedrock_roof".hashCode() + " " + "minecraft:bedrock_floor".hashCode());
        // 5. кольцо стронгхолдов, только сырая геометрия (без подстройки под биом): seed -> (x,z) для 3 позиций первого кольца
        for (long seed : seeds) {
            RandomSource random = RandomSource.create();
            random.setSeed(seed);
            double angle = random.nextDouble() * Math.PI * 2.0;
            StringBuilder sb = new StringBuilder("stronghold " + seed);
            int distance = 32, spread = 3;
            for (int i = 0; i < 3; i++) {
                double dist = 4 * distance + distance * 0 * 6 + (random.nextDouble() - 0.5) * (distance * 2.5);
                int ix = (int) Math.round(Math.cos(angle) * dist);
                int iz = (int) Math.round(Math.sin(angle) * dist);
                random.fork();
                sb.append(" ").append(ix).append(" ").append(iz);
                angle += (Math.PI * 2) / spread;
            }
            System.out.println(sb);
        }
        // 6. mineshaft legacy_type_3: seed cx cz -> passes?
        for (long seed : seeds) {
            for (int[] c : new int[][]{{0,0},{1,1},{-5,7},{100,-100},{12345,-6789}}) {
                WorldgenRandom r = new WorldgenRandom(new LegacyRandomSource(0L));
                r.setLargeFeatureSeed(seed, c[0], c[1]);
                double d = r.nextDouble();
                System.out.println("mineshaft " + seed + " " + c[0] + " " + c[1] + " " + Double.doubleToLongBits(d) + " " + (d < 0.004 ? 1 : 0));
            }
        }
    }

    static class IntArrayListWrap { final List<Integer> l; IntArrayListWrap(List<Integer> l){this.l=l;} public String toString(){ StringBuilder sb=new StringBuilder(); for(int i=0;i<l.size();i++){ if(i>0) sb.append(','); sb.append(l.get(i)); } return sb.toString(); } }
    static IntArrayListWrap shuffled(long seed) {
        var lst = Util.toShuffledList(IntStream.range(0, 10), RandomSource.createThreadLocalInstance(seed));
        List<Integer> out = new ArrayList<>();
        for (int i = 0; i < lst.size(); i++) out.add(lst.getInt(i));
        return new IntArrayListWrap(out);
    }
}

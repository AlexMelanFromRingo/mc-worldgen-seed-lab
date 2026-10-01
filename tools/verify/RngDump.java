import java.lang.reflect.Method;
import net.minecraft.util.Mth;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.levelgen.*;
import net.minecraft.world.level.levelgen.structure.placement.RandomSpreadStructurePlacement;
import net.minecraft.world.level.levelgen.structure.placement.RandomSpreadType;

/** Дамп значений реального RNG-кода игры (без bootstrap реестров: только чистые классы + placement). */
public class RngDump {
    static final long[] SEEDS = {0L, 1L, -1L, 42L, 123456789012345L, 0x7FFFFFFFFFFFFFFFL, Long.MIN_VALUE, 0x9E3779B97F4A7C15L, -4172144997902289642L, 8682522807148012L};
    static final int[][] XZ = {{0, 0}, {1, -1}, {-1, 1}, {5, -7}, {-123, 456}, {1000, -1000}, {-1875000, 1875000}, {1874999, 1874999}};
    static final int[][] XYZ = {{0, 0, 0}, {1, 2, 3}, {-1, -64, -1}, {100, 200, -300}, {29999999, 319, -29999999}, {-123456, 7, 654321}};

    static final java.io.PrintStream OUT = System.out; // Bootstrap перехватывает System.out
    static void p(String k, Object v) { OUT.println(k + " " + v); }
    static String h(long v) { return String.format("%016x", v); }

    static void basic(String tag, RandomSource r) {
        StringBuilder sb = new StringBuilder();
        sb.append(h(r.nextInt() & 0xffffffffL)).append(' ').append(h(r.nextInt() & 0xffffffffL));
        for (int b : new int[]{16, 16, 10, 1000, 1000, 7, 7, 1, 2, 3, 100, 65536, 1 << 30, 1_000_000_007, 34 - 8, 32 - 5, 1 << 20, 24, 96, 3})
            sb.append(' ').append(r.nextInt(b));
        sb.append(' ').append(h(r.nextLong()));
        sb.append(' ').append(Float.floatToRawIntBits(r.nextFloat()));
        sb.append(' ').append(h(Double.doubleToRawLongBits(r.nextDouble())));
        sb.append(' ').append(r.nextBoolean()).append(r.nextBoolean());
        sb.append(' ').append(r.nextInt(5, 17));
        r.consumeCount(17);
        sb.append(' ').append(h(r.nextLong()));
        p(tag, sb);
    }

    public static void main(String[] a) throws Exception {
        net.minecraft.SharedConstants.tryDetectVersion();
        net.minecraft.server.Bootstrap.bootStrap(); // нужен для инициализации StructurePlacement (BuiltInRegistries)
        for (long s : SEEDS) {
            p("seed", h(s));
            basic("L.basic", new LegacyRandomSource(s));
            basic("X.basic", new XoroshiroRandomSource(s));
            basic("WL.basic", new WorldgenRandom(new LegacyRandomSource(s)));
            basic("WX.basic", new WorldgenRandom(new XoroshiroRandomSource(s)));
            var up = RandomSupport.upgradeSeedTo128bit(s);
            var upu = RandomSupport.upgradeSeedTo128bitUnmixed(s);
            p("upgrade", h(up.seedLo()) + " " + h(up.seedHi()) + " unmixed " + h(upu.seedLo()) + " " + h(upu.seedHi()));
            p("stafford", h(RandomSupport.mixStafford13(s)));
            for (String name : new String[]{"minecraft:temperature", "minecraft:continentalness", "octave_-3", "minecraft:terrain", "bedrock_floor", "minecraft:aquifer", ""}) {
                var sh = RandomSupport.seedFromHashOf(name);
                // Xoroshiro-фабрика от seed: lo/hi = первые два nextLong
                RandomSource src = new XoroshiroRandomSource(s);
                PositionalRandomFactory f = src.forkPositional();
                RandomSource r = f.fromHashOf(name);
                p("X.fromHashOf[" + name + "]", h(sh.seedLo()) + " " + h(sh.seedHi()) + " -> " + h(r.nextLong()) + " " + h(r.nextLong()));
                RandomSource lsrc = new LegacyRandomSource(s);
                PositionalRandomFactory lf = lsrc.forkPositional();
                RandomSource lr = lf.fromHashOf(name);
                p("L.fromHashOf[" + name + "]", name.hashCode() + " -> " + h(lr.nextLong()));
            }
            for (int[] c : XYZ) {
                PositionalRandomFactory f = new XoroshiroRandomSource(s).forkPositional();
                p("X.at " + c[0] + "," + c[1] + "," + c[2], h(Mth.getSeed(c[0], c[1], c[2])) + " -> " + h(f.at(c[0], c[1], c[2]).nextLong()));
                PositionalRandomFactory lf = new LegacyRandomSource(s).forkPositional();
                p("L.at " + c[0] + "," + c[1] + "," + c[2], h(lf.at(c[0], c[1], c[2]).nextLong()));
            }
            {
                RandomSource f1 = new XoroshiroRandomSource(s).fork();
                p("X.fork", h(f1.nextLong()));
                RandomSource f2 = new LegacyRandomSource(s).fork();
                p("L.fork", h(f2.nextLong()));
                p("X.fromSeed", h(new XoroshiroRandomSource(7).forkPositional().fromSeed(s).nextLong()));
            }
            for (int[] c : XZ) {
                WorldgenRandom w = new WorldgenRandom(new XoroshiroRandomSource(1234567L));
                long dec = w.setDecorationSeed(s, c[0] * 16, c[1] * 16);
                StringBuilder sb = new StringBuilder(h(dec));
                for (int i = 0; i < 3; i++) sb.append(' ').append(w.nextInt(100));
                sb.append(' ').append(h(w.nextLong())).append(' ').append(Float.floatToRawIntBits(w.nextFloat())).append(' ').append(h(Double.doubleToRawLongBits(w.nextDouble())));
                p("WX.decoration " + c[0] + "," + c[1], sb);
                sb = new StringBuilder();
                for (int[] is : new int[][]{{0, 0}, {5, 3}, {123, 9}}) {
                    w.setFeatureSeed(dec, is[0], is[1]);
                    sb.append(' ').append(w.nextInt(1000)).append(' ').append(h(w.nextLong()));
                }
                p("WX.feature " + c[0] + "," + c[1], sb);
                // то же на Legacy-базе (в 26.x не используется для декораций, но полезно как контроль)
                WorldgenRandom wl = new WorldgenRandom(new LegacyRandomSource(999L));
                long decl = wl.setDecorationSeed(s, c[0] * 16, c[1] * 16);
                p("WL.decoration " + c[0] + "," + c[1], h(decl) + " " + wl.nextInt(100) + " " + h(wl.nextLong()));
                wl.setLargeFeatureSeed(s, c[0], c[1]);
                p("WL.largeFeature " + c[0] + "," + c[1], wl.nextInt(100) + " " + h(wl.nextLong()) + " " + Float.floatToRawIntBits(wl.nextFloat()) + " " + h(Double.doubleToRawLongBits(wl.nextDouble())));
                wl.setLargeFeatureWithSalt(s, c[0], c[1], 10387312);
                p("WL.largeFeatureSalt " + c[0] + "," + c[1], wl.nextInt(100) + " " + h(wl.nextLong()));
                wl.setLargeFeatureWithSalt(s, c[0], c[1], 0);
                p("WL.largeFeatureSalt0 " + c[0] + "," + c[1], wl.nextInt(100) + " " + h(wl.nextLong()));
                RandomSource sl = WorldgenRandom.seedSlimeChunk(c[0], c[1], s, 987234911L);
                p("slime " + c[0] + "," + c[1], sl.nextInt(10) == 0);
                p("slimeXZ " + c[0] + "," + c[1], (c[0] * c[0] * 4987142) + " " + (c[1] * c[1] * 4392871L));
                // структурные placement'ы
                RandomSpreadStructurePlacement vil = new RandomSpreadStructurePlacement(34, 8, RandomSpreadType.LINEAR, 10387312);
                RandomSpreadStructurePlacement mon = new RandomSpreadStructurePlacement(32, 5, RandomSpreadType.TRIANGULAR, 10387313);
                RandomSpreadStructurePlacement big = new RandomSpreadStructurePlacement(4096, 1, RandomSpreadType.LINEAR, 0);
                ChunkPos v = vil.getPotentialStructureChunk(s, c[0], c[1]);
                ChunkPos m = mon.getPotentialStructureChunk(s, c[0], c[1]);
                ChunkPos b = big.getPotentialStructureChunk(s, c[0], c[1]);
                p("spread.linear34/8/10387312 " + c[0] + "," + c[1], v.x() + " " + v.z());
                p("spread.tri32/5/10387313 " + c[0] + "," + c[1], m.x() + " " + m.z());
                p("spread.linear4096/1/0 " + c[0] + "," + c[1], b.x() + " " + b.z());
            }
            // frequency reducers
            Class<?> cls;
            try { cls = Class.forName("net.minecraft.world.level.levelgen.structure.placement.AbstractSpreadingStructurePlacement$FrequencyReductionMethod"); }
            catch (ClassNotFoundException e) { cls = Class.forName("net.minecraft.world.level.levelgen.structure.placement.StructurePlacement$FrequencyReductionMethod"); }
            Method sg = cls.getMethod("shouldGenerate", long.class, int.class, int.class, int.class, float.class);
            for (Object en : cls.getEnumConstants()) {
                StringBuilder sb = new StringBuilder();
                for (int[] c : XZ) for (float fr : new float[]{0.2f, 0.5f, 0.9f})
                    sb.append(((Boolean) sg.invoke(en, s, 10387313, c[0], c[1], fr)) ? '1' : '0');
                p("freq." + en, sb);
            }
        }
    }
}

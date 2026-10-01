import java.io.*;
import java.lang.reflect.*;
import java.util.*;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.biome.FixedBiomeSource;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.WorldGenerationContext;
import oracle.compat.Compat;

/**
 * Эталон бедрока Незера на РЕАЛЬНОМ коде игры (26.1 / 26.2 / 26.3), для crack-nether-bedrock.
 *
 * Для seed строится настоящий RandomState (реальные noise_settings the_nether из датапака, legacy_random_source=true), берётся
 * настоящая поверхностная система (SurfaceSystem в 26.1/26.2, MaterialSystem в 26.3) и настоящее правило поверхности Незера
 * (NoiseGeneratorSettings.surfaceRule() / materialRule() — содержащее bedrock_floor + bedrock_roof, в 26.3 из material_rule/*.json).
 * Блок (x,y,z) "считается бедроком", если правило вернуло minecraft:bedrock — т.е. выполняется весь реальный путь:
 * getOrCreateRandomFactory(bedrock_*) -> VerticalGradientCondition -> Mth.map -> PositionalRandomFactory.at -> nextFloat().
 * Доступ к protected/package-private классам — рефлексией. Режим batch читает команды из stdin:
 *   gen <seed> <n> <bedrock|mixed> <area> <ymode 0|1|2> <roofFrac> <rng> <outfile>
 *   classify <seed> <infile> <outfile>          (строки "x y z [тип]" -> "x y z Bedrock|Other")
 *   floats <seed> <x> <y> <z>                    (nextFloat через реальную фабрику — печатает биты Float)
 */
public class NbRef {
    static PrintStream OUT, ERR;   // Bootstrap переназначает System.out/err в лог — сохраняем настоящие потоки
    static HolderLookup.Provider reg;
    static Holder<NoiseGeneratorSettings> nsHolder;
    static NoiseGeneratorSettings settings;
    static Holder<Biome> biome;
    static WorldGenerationContext wgc;
    static boolean family263;
    static long evalErrors = 0;

    // ---- контекст для одного seed ----
    static class Ev {
        RandomState rs; Object ctx; Object rule; Method updXZ, updY, tryApply;
        boolean bedrock(int x, int y, int z) {
            try {
                if (family263) updXZ.invoke(ctx, x, z, 0, 0); else updXZ.invoke(ctx, x, z);
                if (updY.getParameterCount() == 6) updY.invoke(ctx, 1, 1, Integer.MIN_VALUE, x, y, z);
                else updY.invoke(ctx, 1, 1, Integer.MIN_VALUE, y);
                BlockState st = (BlockState) tryApply.invoke(rule, x, y, z);
                return st != null && st.is(Blocks.BEDROCK);
            } catch (Throwable t) {
                evalErrors++;
                if (evalErrors <= 3) { ERR.println("ОШИБКА вычисления правила в (" + x + "," + y + "," + z + "): " + t + " / " + t.getCause()); }
                return false;
            }
        }
    }

    static Method find(Class<?> c, String name, int pc) {
        for (Class<?> k = c; k != null; k = k.getSuperclass())
            for (Method m : k.getDeclaredMethods()) if (m.getName().equals(name) && m.getParameterCount() == pc) { m.setAccessible(true); return m; }
        for (Method m : c.getMethods()) if (m.getName().equals(name) && m.getParameterCount() == pc) { m.setAccessible(true); return m; }
        throw new RuntimeException("нет метода " + name + "/" + pc + " в " + c);
    }
    static Method findAny(Class<?> c, String name) {
        for (Class<?> k = c; k != null; k = k.getSuperclass())
            for (Method m : k.getDeclaredMethods()) if (m.getName().equals(name)) { m.setAccessible(true); return m; }
        for (Method m : c.getMethods()) if (m.getName().equals(name)) { m.setAccessible(true); return m; }
        throw new RuntimeException("нет метода " + name + " в " + c);
    }

    static Ev make(long seed) throws Exception {
        Ev e = new Ev();
        e.rs = Compat.newRandomState(reg, settings, seed);
        Object sys = RandomState.class.getMethod("surfaceSystem").invoke(e.rs);
        Function1 biomeGetter = new Function1();
        if (!family263) {
            Class<?> ctxCls = Class.forName("net.minecraft.world.level.levelgen.SurfaceRules$Context");
            Constructor<?> cons = ctxCls.getDeclaredConstructors()[0]; cons.setAccessible(true);
            Class<?>[] pt = cons.getParameterTypes(); Object[] args = new Object[pt.length];
            for (int i = 0; i < pt.length; i++) {
                String n = pt[i].getName();
                if (n.endsWith("SurfaceSystem")) args[i] = sys;
                else if (n.endsWith("RandomState")) args[i] = e.rs;
                else if (n.endsWith("WorldGenerationContext")) args[i] = wgc;
                else if (pt[i] == java.util.function.Function.class) args[i] = biomeGetter;
                else args[i] = null;   // ChunkAccess, NoiseChunk, Registry<Biome>, possibleBiomes — не используются правилами Незера до бедрока/после него
            }
            e.ctx = cons.newInstance(args);
            Object ruleSource = NoiseGeneratorSettings.class.getMethod("surfaceRule").invoke(settings);
            Method apply = findAny(ruleSource.getClass(), "apply");
            // RuleSource.apply(Context) -> SurfaceRule; apply есть и как bridge Object->Object: берём метод с параметром Context
            Method ap = null;
            for (Method m : ruleSource.getClass().getMethods()) if (m.getName().equals("apply") && m.getParameterCount() == 1 && m.getParameterTypes()[0] == ctxCls) ap = m;
            if (ap == null) ap = apply;
            ap.setAccessible(true);
            e.rule = ap.invoke(ruleSource, e.ctx);
            e.updXZ = find(ctxCls, "updateXZ", 2);
            e.updY = null;
            for (Method m : ctxCls.getDeclaredMethods()) if (m.getName().equals("updateY")) { m.setAccessible(true); e.updY = m; }
            e.tryApply = null;
            for (Class<?> ic : Class.forName("net.minecraft.world.level.levelgen.SurfaceRules$SurfaceRule").getInterfaces()) {}
            Class<?> ruleIface = Class.forName("net.minecraft.world.level.levelgen.SurfaceRules$SurfaceRule");
            e.tryApply = ruleIface.getDeclaredMethod("tryApply", int.class, int.class, int.class); e.tryApply.setAccessible(true);
        } else {
            Class<?> ctxCls = Class.forName("net.minecraft.world.level.levelgen.material.MaterialRuleContext");
            Constructor<?> cons = ctxCls.getDeclaredConstructors()[0]; cons.setAccessible(true);
            Class<?>[] pt = cons.getParameterTypes(); Object[] args = new Object[pt.length];
            Class<?> volCls = Class.forName("net.minecraft.world.level.levelgen.densityfunction.DensityVolume");
            for (int i = 0; i < pt.length; i++) {
                String n = pt[i].getName();
                if (n.endsWith("MaterialSystem")) args[i] = sys;
                else if (n.endsWith("RandomState")) args[i] = e.rs;
                else if (n.endsWith("WorldGenerationContext")) args[i] = wgc;
                else if (pt[i] == volCls) args[i] = volCls.getConstructor(int.class, int.class, int.class, int.class, int.class, int.class).newInstance(1, 1, 1, 0, 0, 0);
                else if (pt[i] == java.util.function.Function.class) args[i] = biomeGetter;
                else args[i] = null;   // DensitySamplerSet, possibleBiomes
            }
            e.ctx = cons.newInstance(args);
            Object ruleHolder = NoiseGeneratorSettings.class.getMethod("materialRule").invoke(settings);   // Holder<MaterialRule>
            Object matRule = ((Holder<?>) ruleHolder).value();
            Method comp = null;
            for (Method m : matRule.getClass().getMethods()) if (m.getName().equals("compile") && m.getParameterCount() == 1) comp = m;
            comp.setAccessible(true);
            e.rule = comp.invoke(matRule, e.ctx);
            e.updXZ = find(ctxCls, "updateXZ", 4);
            e.updY = find(ctxCls, "updateY", 4);
            Class<?> ev = Class.forName("net.minecraft.world.level.levelgen.material.rule.RuleEvaluator");
            e.tryApply = ev.getDeclaredMethod("tryApply", int.class, int.class, int.class); e.tryApply.setAccessible(true);
        }
        return e;
    }

    static class Function1 implements java.util.function.Function<net.minecraft.core.BlockPos, Holder<Biome>> {
        public Holder<Biome> apply(net.minecraft.core.BlockPos p) { return biome; }
    }

    static boolean validY(int y) { return (y >= 1 && y <= 4) || (y >= 123 && y <= 126); }

    public static void main(String[] a) throws Exception {
        try { main2(a); } catch (Throwable t) { if (ERR == null) ERR = new PrintStream(new FileOutputStream(FileDescriptor.err), true, "UTF-8"); t.printStackTrace(ERR); System.exit(1); }
    }

    static void main2(String[] a) throws Exception {
        OUT = new PrintStream(new FileOutputStream(FileDescriptor.out), true, "UTF-8");
        ERR = new PrintStream(new FileOutputStream(FileDescriptor.err), true, "UTF-8");
        Compat.bootstrap();
        reg = Compat.loadRegistries();
        nsHolder = reg.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(NoiseGeneratorSettings.NETHER);
        settings = nsHolder.value();
        biome = reg.lookupOrThrow(Registries.BIOME).getOrThrow(Biomes.NETHER_WASTES);
        ChunkGenerator gen = new NoiseBasedChunkGenerator(new FixedBiomeSource(biome), nsHolder);
        wgc = new WorldGenerationContext(gen, LevelHeightAccessor.create(0, 256));
        try { NoiseGeneratorSettings.class.getMethod("materialRule"); family263 = true; } catch (NoSuchMethodException ex) { family263 = false; }
        ERR.println("NbRef: семейство " + (family263 ? "26.3 (MaterialSystem)" : "26.1/26.2 (SurfaceSystem)") + ", nether legacy_random_source=" + settings.useLegacyRandomSource()
            + ", genDepth=" + wgc.getGenDepth() + ", minGenY=" + wgc.getMinGenY());
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        String line;
        Map<Long, Ev> cache = new HashMap<>();
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] t = line.split("\\s+");
            long seed = Long.parseLong(t[1]);
            Ev ev = cache.get(seed);
            if (ev == null) { ev = make(seed); if (cache.size() > 4) cache.clear(); cache.put(seed, ev); }
            switch (t[0]) {
                case "gen" -> {
                    int n = Integer.parseInt(t[2]); boolean bedrockOnly = t[3].equals("bedrock");
                    int area = Integer.parseInt(t[4]), ymode = Integer.parseInt(t[5]); double roofFrac = Double.parseDouble(t[6]);
                    long rng = Long.parseLong(t[7]);
                    SplittableRandom r = new SplittableRandom(rng * 0x9E3779B97F4A7C15L ^ seed);
                    LinkedHashMap<String, Boolean> used = new LinkedHashMap<>();
                    int guard = 0;
                    while (used.size() < n && guard++ < 50_000_000) {
                        boolean roof = r.nextDouble() < roofFrac;
                        int y = ymode == 0 ? (roof ? 123 : 4) : ymode == 1 ? (roof ? 123 + r.nextInt(4) : 1 + r.nextInt(4)) : (roof ? 123 + r.nextInt(2) * 3 : 1 + r.nextInt(2) * 3);
                        int x = r.nextInt(2 * area + 1) - area, z = r.nextInt(2 * area + 1) - area;
                        String key = x + " " + y + " " + z;
                        if (used.containsKey(key)) continue;
                        boolean b = ev.bedrock(x, y, z);
                        if (bedrockOnly && !b) continue;
                        used.put(key, b);
                    }
                    try (PrintWriter w = new PrintWriter(t[8])) {
                        w.println("# seed " + seed + " (structure seed " + (seed & 0xFFFFFFFFFFFFL) + ") n=" + used.size() + " mode=" + t[3] + " — сгенерировано реальным кодом игры (NbRef)");
                        for (var en : used.entrySet()) w.println(en.getKey() + " " + (en.getValue() ? "Bedrock" : "Other"));
                    }
                }
                case "classify" -> {
                    try (BufferedReader fr = new BufferedReader(new FileReader(t[2])); PrintWriter w = new PrintWriter(t[3])) {
                        String l;
                        while ((l = fr.readLine()) != null) {
                            String s = l.trim(); if (s.isEmpty() || s.startsWith("#")) continue;
                            String[] p = s.split("\\s+");
                            int x = Integer.parseInt(p[0]), y = Integer.parseInt(p[1]), z = Integer.parseInt(p[2]);
                            w.println(x + " " + y + " " + z + " " + (ev.bedrock(x, y, z) ? "Bedrock" : "Other"));
                        }
                    }
                }
                case "floats" -> {
                    // nextFloat через реальные фабрики RandomState (как VerticalGradientCondition): roof/floor
                    int x = Integer.parseInt(t[2]), y = Integer.parseInt(t[3]), z = Integer.parseInt(t[4]);
                    for (String nm : new String[]{"bedrock_roof", "bedrock_floor"}) {
                        var f = ev.rs.getOrCreateRandomFactory(net.minecraft.resources.Identifier.withDefaultNamespace(nm));
                        OUT.println("floats " + seed + " " + nm + " " + x + " " + y + " " + z + " " + Float.floatToIntBits(f.at(x, y, z).nextFloat()));
                    }
                }
                default -> ERR.println("неизвестная команда " + t[0]);
            }
        }
        ERR.println("NbRef: ошибок вычисления правила: " + evalErrors);
    }
}

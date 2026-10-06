import java.io.*;
import java.lang.reflect.*;
import java.util.*;

/** Запись «что видит игра» при каждой попытке PlaceOnGroundDecorator: центр региона, позиция, блок в клетке, блок над ней, значение MOTION_BLOCKING_NO_LEAVES. */
public class Dbg {
    static PrintWriter out;
    static final Map<Class<?>, Method> cache = new HashMap<>();
    static synchronized PrintWriter w() {
        if (out == null) {
            try {
                out = new PrintWriter(new BufferedWriter(new FileWriter(System.getProperty("dbg.out", "pog.log"))), false);
                Runtime.getRuntime().addShutdownHook(new Thread(() -> { synchronized (Dbg.class) { out.flush(); out.close(); } }));
            } catch (IOException e) { throw new RuntimeException(e); }
        }
        return out;
    }
    static Method m(Object o, String name, Class<?>... types) throws Exception {
        for (Method me : o.getClass().getMethods()) if (me.getName().equals(name) && me.getParameterCount() == types.length) { me.setAccessible(true); return me; }
        throw new NoSuchMethodException(o.getClass() + "." + name);
    }
    static Object call(Object o, String name, Object... args) throws Exception {
        for (Class<?> c = o.getClass(); c != null; c = c.getSuperclass()) {
            for (Method me : c.getDeclaredMethods()) if (me.getName().equals(name) && me.getParameterCount() == args.length) { me.setAccessible(true); return me.invoke(o, args); }
        }
        for (Class<?> i : o.getClass().getInterfaces()) for (Method me : i.getMethods()) if (me.getName().equals(name) && me.getParameterCount() == args.length) { me.setAccessible(true); return me.invoke(o, args); }
        throw new NoSuchMethodException(o.getClass() + "." + name);
    }
    static final Set<String> CLS = new HashSet<>(Arrays.asList(System.getProperty("dbg.cls", "MushroomBlock").split(",")));
    static final Set<String> WATCH = new HashSet<>(Arrays.asList(System.getProperty("dbg.watch", "").isEmpty() ? new String[0] : System.getProperty("dbg.watch").split(";")));
    static Field curGen;
    static final Set<String> seen = Collections.synchronizedSet(new HashSet<>());
    /** WorldGenRegion.setBlock: записываем записи малых грибов (класс MushroomBlock) с именем текущей фичи и центром региона */
    public static void set(Object region, Object pos, Object state, int flags) {
        try {
            Object block = call(state, "getBlock");
            if (TRACE.get() != null) synchronized (Dbg.class) { w().println("TW " + call(pos, "getX") + " " + call(pos, "getY") + " " + call(pos, "getZ") + " " + state + " f=" + flags); }
            if (!WATCH.isEmpty()) {     // -Ddbg.watch="x,y,z;x,y,z": любая запись в эти клетки (старое → новое состояние, флаги, фича)
                int wx = (Integer) call(pos, "getX"), wy = (Integer) call(pos, "getY"), wz = (Integer) call(pos, "getZ");
                if (WATCH.contains(wx + "," + wy + "," + wz)) {
                    if (curGen == null) { curGen = region.getClass().getDeclaredField("currentlyGenerating"); curGen.setAccessible(true); }
                    Object sup0 = curGen.get(region); String feat0 = sup0 == null ? "?" : String.valueOf(call(sup0, "get"));
                    Object c0 = call(region, "getCenter");
                    synchronized (Dbg.class) { w().println("W " + call(c0, "x") + " " + call(c0, "z") + " " + wx + " " + wy + " " + wz + " | " + call(region, "getBlockState", pos) + " -> " + state + " | flags=" + flags + " | " + feat0); }
                }
            }
            if (!CLS.contains(block.getClass().getSimpleName())) return;
            if (curGen == null) { curGen = region.getClass().getDeclaredField("currentlyGenerating"); curGen.setAccessible(true); }
            Object sup = curGen.get(region); String feat = "?";
            if (sup != null) feat = String.valueOf(call(sup, "get"));
            Object center = call(region, "getCenter");
            int cx = (Integer) call(center, "x"), cz = (Integer) call(center, "z");
            int x = (Integer) call(pos, "getX"), y = (Integer) call(pos, "getY"), z = (Integer) call(pos, "getZ");
            synchronized (Dbg.class) { w().println("S " + cx + " " + cz + " " + x + " " + y + " " + z + " | " + state + " | flags=" + flags + " | " + feat); }
        } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    /** TreeFeature.place: каждая попытка дерева — центр региона, начало, карты высот OCEAN_FLOOR / WORLD_SURFACE в клетке начала, блок под ней */
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void tree(Object ctx) {
        try { treeCore(call(ctx, "level"), call(ctx, "origin"), call(ctx, "random")); } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    public static void tree4(Object level, Object random, Object pos) {
        try { treeCore(level, pos, random); } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    static void treeCore(Object level, Object pos, Object random) {
        try {
            int x = (Integer) call(pos, "getX"), y = (Integer) call(pos, "getY"), z = (Integer) call(pos, "getZ");
            Class<?> ht = Class.forName("net.minecraft.world.level.levelgen.Heightmap$Types", true, level.getClass().getClassLoader());
            int of = (Integer) call(level, "getHeight", Enum.valueOf((Class) ht, "OCEAN_FLOOR"), x, z);
            int ws = (Integer) call(level, "getHeight", Enum.valueOf((Class) ht, "WORLD_SURFACE"), x, z);
            Object center = call(level, "getCenter");
            RND.set(random);
            TRACE.set(TREEAT.contains(x + "," + y + "," + z) ? Boolean.TRUE : null);
            if (TRACE.get() != null) synchronized (Dbg.class) { w().println("TS " + x + " " + y + " " + z + " rnd=" + rndLo(RND.get())); }
            String wgs = "";
            if (WGBOX && y >= 40 && y <= 110) {
                Object wgt = Enum.valueOf((Class) ht, "OCEAN_FLOOR_WG");
                long sum = 0; int mx = -9999;
                for (int dx = -9; dx <= 9; dx++) for (int dz = -9; dz <= 9; dz++) { int h = (Integer) call(level, "getHeight", wgt, x + dx, z + dz); sum += h; if (h > mx) mx = h; }
                wgs = " wg=" + sum + "/" + mx;
                if (WGAT.contains(x + "," + y + "," + z)) {
                    StringBuilder sb = new StringBuilder("WGBOX " + x + " " + y + " " + z + ":");
                    for (int dz = -9; dz <= 9; dz++) for (int dx = -9; dx <= 9; dx++) sb.append(' ').append((Integer) call(level, "getHeight", wgt, x + dx, z + dz));
                    synchronized (Dbg.class) { w().println(sb); }
                }
            }
            LAST.set("T " + call(center, "x") + " " + call(center, "z") + " " + x + " " + y + " " + z + " | of=" + of + " ws=" + ws + wgs + " | " + call(level, "getBlockState", call(pos, "below")));
        } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    static final Set<String> WGAT = new HashSet<>(Arrays.asList(System.getProperty("dbg.wgat", "").isEmpty() ? new String[0] : System.getProperty("dbg.wgat").split(";")));
    static final boolean WGBOX = System.getProperty("dbg.wgbox") != null;
    /** Heightmap.primeHeightmaps(chunk, types): кто и когда (пере)праймит карты высот чанка — для разбора WG-карт старых версий */
    public static void prime(Object chunk, Object types) {
        try {
            Object pos = call(chunk, "getPos");
            StackTraceElement[] st = Thread.currentThread().getStackTrace();
            StringBuilder sb = new StringBuilder();
            for (int i = 3; i < Math.min(st.length, 8); i++) sb.append(st[i].getClassName().replaceAll(".*\\.", "")).append('.').append(st[i].getMethodName()).append(' ');
            synchronized (Dbg.class) { w().println("P " + call(pos, "x") + " " + call(pos, "z") + " " + types + " " + call(chunk, "getPersistedStatus") + " <- " + sb); }
        } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    public static void secSet(Object section, int x, int y, int z, Object state) {
        if (TRACE.get() == null) return;
        synchronized (Dbg.class) { w().println("SW " + x + " " + y + " " + z + " " + state); }
    }
    static final ThreadLocal<String> LAST = new ThreadLocal<>();
    static final ThreadLocal<Object> RND = new ThreadLocal<>();
    static final ThreadLocal<Boolean> TRACE = new ThreadLocal<>();
    static final Set<String> TREEAT = new HashSet<>(Arrays.asList(System.getProperty("dbg.treeat", "").isEmpty() ? new String[0] : System.getProperty("dbg.treeat").split(";")));
    static String rndLo(Object rnd) {
        try {
            Object o = rnd;
            for (int i = 0; i < 4; i++) {
                Class<?> k = o.getClass(); Field f = null;
                for (Class<?> c = k; c != null && f == null; c = c.getSuperclass()) {
                    for (String n : new String[]{"randomSource", "randomNumberGenerator"}) { try { f = c.getDeclaredField(n); break; } catch (NoSuchFieldException e) { } }
                }
                if (f == null) break;
                f.setAccessible(true); o = f.get(o);
            }
            Field lo = o.getClass().getDeclaredField("seedLo"); lo.setAccessible(true);
            return Long.toHexString(lo.getLong(o));
        } catch (Throwable t) { return "?" + t; }
    }
    public static void treeRes(int res) { synchronized (Dbg.class) { w().println(LAST.get() + " | R=" + res + " rnd=" + rndLo(RND.get())); } TRACE.set(null); }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void pog(Object ctx, Object pos) {
        try {
            Object level = call(ctx, "level");
            int x = (Integer) call(pos, "getX"), y = (Integer) call(pos, "getY"), z = (Integer) call(pos, "getZ");
            Object above = call(pos, "above");
            Object sPos = call(level, "getBlockState", pos), sAbove = call(level, "getBlockState", above);
            Class<?> ht = Class.forName("net.minecraft.world.level.levelgen.Heightmap$Types", true, level.getClass().getClassLoader());
            Object type = Enum.valueOf((Class) ht, "MOTION_BLOCKING_NO_LEAVES");
            Object hp = call(level, "getHeightmapPos", type, pos);
            int hy = (Integer) call(hp, "getY");
            Object center = call(level, "getCenter");
            int cx = (Integer) call(center, "x"), cz = (Integer) call(center, "z");
            PrintWriter p = w();
            synchronized (Dbg.class) {
                p.println("G " + cx + " " + cz + " " + x + " " + y + " " + z + " | " + sPos + " | " + sAbove + " | hm=" + hy);
            }
        } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    /** MushroomBlock.canSurvive: яркость в клетке так, как её видит игра (сырая, небесная, блочная), центр региона, фича, поток */
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void cs(Object level, Object pos) {
        try {
            if (!level.getClass().getSimpleName().equals("WorldGenRegion")) return;
            int x = (Integer) call(pos, "getX"), y = (Integer) call(pos, "getY"), z = (Integer) call(pos, "getZ");
            Object le = call(level, "getLightEngine");
            Object raw = call(le, "getRawBrightness", pos, 0);
            Class<?> ll = Class.forName("net.minecraft.world.level.LightLayer", true, level.getClass().getClassLoader());
            Object sky = call(call(le, "getLayerListener", Enum.valueOf((Class) ll, "SKY")), "getLightValue", pos);
            Object blk = call(call(le, "getLayerListener", Enum.valueOf((Class) ll, "BLOCK")), "getLightValue", pos);
            Object below = call(level, "getBlockState", call(pos, "below"));
            if (curGen == null) { curGen = level.getClass().getDeclaredField("currentlyGenerating"); curGen.setAccessible(true); }
            Object sup = curGen.get(level); String feat = "?";
            if (sup != null) feat = String.valueOf(call(sup, "get"));
            Object center = call(level, "getCenter");
            int cx = (Integer) call(center, "x"), cz = (Integer) call(center, "z");
            if (seen.add(cx + "," + cz)) {
                Class<?> bp = Class.forName("net.minecraft.core.BlockPos", true, level.getClass().getClassLoader());
                Constructor<?> k = bp.getConstructor(int.class, int.class, int.class);
                Object lsky = call(le, "getLayerListener", Enum.valueOf((Class) ll, "SKY"));
                StringBuilder sb = new StringBuilder("L " + cx + " " + cz + " |");
                Object sl = call(level, "getLevel"); Object cs0 = call(sl, "getChunkSource");
                Object cmap = cs0.getClass().getField("chunkMap").get(cs0);
                Class<?> cp = Class.forName("net.minecraft.world.level.ChunkPos", true, level.getClass().getClassLoader());
                Method pack = cp.getMethod("pack", int.class, int.class);
                for (int dz = -3; dz <= 3; dz++) { sb.append(" z").append(cz + dz).append(":"); for (int dx = -3; dx <= 3; dx++) {
                    sb.append(' ');
                    Object h = call(cmap, "getUpdatingChunkIfPresent", pack.invoke(null, cx + dx, cz + dz));
                    String stt = "-";
                    if (h != null) { Object ps = call(h, "getPersistedStatus"); stt = ps == null ? "n" : String.valueOf(call(ps, "getIndex")); }
                    sb.append(stt).append('[');
                    for (int yy : new int[]{-40, 64}) sb.append(call(lsky, "getLightValue", k.newInstance((cx + dx) * 16 + 8, yy, (cz + dz) * 16 + 8))).append(yy == 64 ? "" : "/");
                    sb.append(']');
                } }
                synchronized (Dbg.class) { w().println(sb); }
            }
            synchronized (Dbg.class) { w().println("C " + cx + " " + cz + " " + x + " " + y + " " + z + " | raw=" + raw + " sky=" + sky + " blk=" + blk + " | " + below + " | " + feat + " | " + Thread.currentThread().getName() + " t=" + java.time.Instant.now()); }
        } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    /** ProtoChunk/LevelChunk.setBlockState: запись в наблюдаемую клетку с кратким стеком (кто и откуда меняет клетку — в том числе после FEATURES) */
    static final Set<String> RM = new HashSet<>(Arrays.asList(System.getProperty("dbg.rm", "").isEmpty() ? new String[0] : System.getProperty("dbg.rm").split(",")));
    public static void chunkSet(Object chunk, Object pos, Object state, int flags) {
        try {
            if (!RM.isEmpty()) {         // -Ddbg.rm=CocoaBlock,VineBlock: любая замена блока этих классов (старое → новое) со стеком
                Object old = call(chunk, "getBlockState", pos);
                String cn = call(old, "getBlock").getClass().getSimpleName();
                if (RM.contains(cn) && !old.equals(state)) {
                    int rx = (Integer) call(pos, "getX"), ry = (Integer) call(pos, "getY"), rz = (Integer) call(pos, "getZ");
                    StringBuilder sb = new StringBuilder(); int n = 0;
                    for (StackTraceElement e : new Throwable().getStackTrace()) {
                        String c2 = e.getClassName(); if (c2.equals("Dbg")) continue;
                        sb.append(c2.substring(c2.lastIndexOf('.') + 1)).append('.').append(e.getMethodName()).append(' ');
                        if (++n >= 16) break;
                    }
                    synchronized (Dbg.class) { w().println("R " + chunk.getClass().getSimpleName() + " " + rx + " " + ry + " " + rz + " | " + old + " -> " + state + " flags=" + flags + " | " + Thread.currentThread().getName() + " | " + sb); }
                }
            }
            if (WATCH.isEmpty()) return;
            int wx = (Integer) call(pos, "getX"), wy = (Integer) call(pos, "getY"), wz = (Integer) call(pos, "getZ");
            if (!WATCH.contains(wx + "," + wy + "," + wz)) return;
            StringBuilder sb = new StringBuilder();
            int n = 0;
            for (StackTraceElement e : new Throwable().getStackTrace()) {
                String cn = e.getClassName(); if (cn.equals("Dbg")) continue;
                sb.append(cn.substring(cn.lastIndexOf('.') + 1)).append('.').append(e.getMethodName()).append(' ');
                if (++n >= 14) break;
            }
            synchronized (Dbg.class) { w().println("K " + chunk.getClass().getSimpleName() + " " + wx + " " + wy + " " + wz + " -> " + state + " flags=" + flags + " | " + Thread.currentThread().getName() + " | " + sb); }
        } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
    /** ChunkAccess.markPosForPostProcessing: кто и откуда помечает клетку (watch) */
    public static void mark(Object chunk, Object pos) {
        try {
            if (WATCH.isEmpty()) return;
            int wx = (Integer) call(pos, "getX"), wy = (Integer) call(pos, "getY"), wz = (Integer) call(pos, "getZ");
            if (!WATCH.contains(wx + "," + wy + "," + wz)) return;
            StringBuilder sb = new StringBuilder(); int n = 0;
            for (StackTraceElement e : new Throwable().getStackTrace()) {
                String cn = e.getClassName(); if (cn.equals("Dbg")) continue;
                sb.append(cn.substring(cn.lastIndexOf('.') + 1)).append('.').append(e.getMethodName()).append(' ');
                if (++n >= 12) break;
            }
            synchronized (Dbg.class) { w().println("M " + wx + " " + wy + " " + wz + " | " + Thread.currentThread().getName() + " | " + sb); }
        } catch (Throwable t) { synchronized (Dbg.class) { w().println("ERR " + t); } }
    }
}

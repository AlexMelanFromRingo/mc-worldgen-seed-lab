package oracle;

import com.mojang.datafixers.util.Pair;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import net.minecraft.core.Holder;
import net.minecraft.core.QuartPos;
import net.minecraft.core.SectionPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.biome.TheEndBiomeSource;

/** P0: params, climate, climategrid, biome, noise, df, list. */
final class CmdCore {
   private CmdCore() {
   }

   static final String[] AXES = {"temperature", "humidity", "continentalness", "erosion", "depth", "weirdness"};

   // ------------------------------------------------------------------ params

   static Climate.ParameterList<Holder<Biome>> parameterList(Ctx.Dim dim) {
      BiomeSource bs = dim.biomeSource;
      if (!(bs instanceof MultiNoiseBiomeSource mn)) throw new IllegalArgumentException("Измерение " + dim.name + ": биом-источник не multi_noise");
      ResourceKey<MultiNoiseBiomeSourceParameterList> key = dim.name.equals("overworld") ? MultiNoiseBiomeSourceParameterLists.OVERWORLD : MultiNoiseBiomeSourceParameterLists.NETHER;
      if (!mn.stable(key)) throw new IllegalStateException("Биом-источник не использует пресет " + key.identifier());
      return Ctx.reg.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST).getOrThrow(key).value().parameters();
   }

   static String params(Args a) throws IOException {
      String dimName = Ctx.normDim(a.str(0));
      String preset = a.get("preset", "normal");
      // seed не важен для таблицы параметров
      Ctx.Dim dim = Ctx.dim(dimName, preset, 0L);
      String fmt = a.get("fmt", "json");
      String out = a.get("out", null);
      if (dim.isEnd()) {
         String j = endRules(dim);
         if (out != null) {
            Util.writeFile(out, j);
            return J.obj().b("ok", true).s("cmd", "params").s("dim", dimName).s("file", out).done();
         }
         return j;
      }
      Climate.ParameterList<Holder<Biome>> list = parameterList(dim);
      List<Pair<Climate.ParameterPoint, Holder<Biome>>> values = list.values();
      Map<String, Integer> palette = new java.util.LinkedHashMap<>();
      for (var p : values) palette.putIfAbsent(Util.biomeName(p.getSecond()), palette.size());
      StringBuilder sb = new StringBuilder(values.size() * 90);
      if (fmt.equals("tsv")) {
         sb.append("# version=").append(oracle.compat.Compat.FAMILY).append(" dim=").append(dimName).append(" preset=").append(preset)
            .append(" count=").append(values.size()).append(" quantization=10000 (значение*10000, усечение к 0)\n");
         sb.append("# columns: tmin tmax hmin hmax cmin cmax emin emax dmin dmax wmin wmax offset biome\n");
         for (var p : values) {
            Climate.ParameterPoint pt = p.getFirst();
            sb.append(pt.temperature().min()).append('\t').append(pt.temperature().max()).append('\t')
               .append(pt.humidity().min()).append('\t').append(pt.humidity().max()).append('\t')
               .append(pt.continentalness().min()).append('\t').append(pt.continentalness().max()).append('\t')
               .append(pt.erosion().min()).append('\t').append(pt.erosion().max()).append('\t')
               .append(pt.depth().min()).append('\t').append(pt.depth().max()).append('\t')
               .append(pt.weirdness().min()).append('\t').append(pt.weirdness().max()).append('\t')
               .append(pt.offset()).append('\t').append(Util.biomeName(p.getSecond())).append('\n');
         }
      } else {
         boolean pretty = out != null;
         sb.append("{\"ok\":true,\"cmd\":\"params\",\"version\":").append(J.q(Main.VERSION)).append(",\"dim\":").append(J.q(dimName))
            .append(",\"preset\":").append(J.q(preset)).append(",\"kind\":\"multi_noise\",\"count\":").append(values.size())
            .append(",\"quantization\":10000,\"axes\":[\"temperature\",\"humidity\",\"continentalness\",\"erosion\",\"depth\",\"weirdness\"],")
            .append("\"point_layout\":[\"tmin\",\"tmax\",\"hmin\",\"hmax\",\"cmin\",\"cmax\",\"emin\",\"emax\",\"dmin\",\"dmax\",\"wmin\",\"wmax\",\"offset\",\"biome\"],");
         sb.append("\"palette\":[");
         boolean f = true;
         for (String n : palette.keySet()) {
            if (!f) sb.append(',');
            f = false;
            J.appendStr(sb, n);
         }
         sb.append("],\"points\":[");
         if (pretty) sb.append('\n');
         for (int i = 0; i < values.size(); i++) {
            var p = values.get(i);
            Climate.ParameterPoint pt = p.getFirst();
            if (i > 0) sb.append(pretty ? ",\n" : ",");
            sb.append('[').append(pt.temperature().min()).append(',').append(pt.temperature().max()).append(',')
               .append(pt.humidity().min()).append(',').append(pt.humidity().max()).append(',')
               .append(pt.continentalness().min()).append(',').append(pt.continentalness().max()).append(',')
               .append(pt.erosion().min()).append(',').append(pt.erosion().max()).append(',')
               .append(pt.depth().min()).append(',').append(pt.depth().max()).append(',')
               .append(pt.weirdness().min()).append(',').append(pt.weirdness().max()).append(',')
               .append(pt.offset()).append(',');
            J.appendStr(sb, Util.biomeName(p.getSecond()));
            sb.append(']');
         }
         sb.append(pretty ? "\n]}\n" : "]}");
      }
      if (out != null) {
         Util.writeFile(out, sb.toString());
         return J.obj().b("ok", true).s("cmd", "params").s("dim", dimName).s("preset", preset).s("file", out).l("count", values.size()).l("palette", palette.size()).done();
      }
      return sb.toString();
   }

   static String endRules(Ctx.Dim dim) {
      // Источник: net.minecraft.world.level.biome.TheEndBiomeSource#getNoiseBiome (одинаково в 26.1/26.2/26.3)
      J j = J.obj().b("ok", true).s("cmd", "params").s("version", Main.VERSION).s("dim", "the_end").s("kind", "the_end_rules");
      j.o("rules")
         .s("center_island", "если chunkX^2+chunkZ^2 <= 4096 (chunk = блок>>4 от блока квартa qx*4,qz*4) -> minecraft:the_end")
         .l("center_island_chunk_r2", 4096)
         .s("weird_block", "weirdBlockX=((blockX>>4)*2+1)*8, weirdBlockZ=((blockZ>>4)*2+1)*8 (blockX=qx*4), y=qy*4")
         .s("height_value", "heightValue = noiseRouter.erosion (EndIslandDensityFunction, LegacyRandomSource(seed)) в (weirdBlockX, blockY, weirdBlockZ)")
         .a("thresholds")
         .eo().s("if", "heightValue > 0.25").s("biome", "minecraft:end_highlands").end()
         .eo().s("if", "heightValue >= -0.0625").s("biome", "minecraft:end_midlands").end()
         .eo().s("if", "heightValue < -0.21875").s("biome", "minecraft:small_end_islands").end()
         .eo().s("if", "иначе (-0.21875 <= h < -0.0625)").s("biome", "minecraft:end_barrens").end()
         .end().end();
      return j.done();
   }

   // ------------------------------------------------------------------ climate

   static ClimateApi clim(Ctx.Dim dim, Args a) {
      return dim.clim(a.get("mode", "point"), a.has("fullcolumn"));
   }

   static String climate(Args a) {
      Ctx.Dim dim = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int qx = a.i(2);
      int qy = a.i(3);
      int qz = a.i(4);
      ClimateApi c = clim(dim, a);
      J j = J.obj().b("ok", true).s("cmd", "climate").s("version", Main.VERSION).s("dim", dim.name).s("preset", dim.preset)
         .l("seed", dim.seed).s("mode", c.mode()).la("q", new long[]{qx, qy, qz})
         .la("block", new long[]{QuartPos.toBlock(qx), QuartPos.toBlock(qy), QuartPos.toBlock(qz)});
      if (dim.isEnd()) {
         endInfo(j, dim, c, qx, qy, qz);
         return j.done();
      }
      Climate.TargetPoint t = c.target(qx, qy, qz);
      j.o("target").l("temperature", t.temperature()).l("humidity", t.humidity()).l("continentalness", t.continentalness())
         .l("erosion", t.erosion()).l("depth", t.depth()).l("weirdness", t.weirdness()).end();
      double[] r = new double[6];
      c.raw(qx, qy, qz, r);
      j.da("raw", r);
      long[] rb = new long[6];
      long[] fb = new long[6];
      for (int k = 0; k < 6; k++) {
         rb[k] = Double.doubleToRawLongBits(r[k]);
         fb[k] = Float.floatToRawIntBits((float) r[k]);
      }
      j.la("raw_dbits", rb).la("raw_fbits", fb).b("raw_float_native", c.floatNative());
      j.s("biome", Util.biomeName(c.biome(qx, qy, qz)));
      return j.done();
   }

   static void endInfo(J j, Ctx.Dim dim, ClimateApi c, int qx, int qy, int qz) {
      int bx = QuartPos.toBlock(qx);
      int by = QuartPos.toBlock(qy);
      int bz = QuartPos.toBlock(qz);
      int cx = SectionPos.blockToSectionCoord(bx);
      int cz = SectionPos.blockToSectionCoord(bz);
      int wx = (cx * 2 + 1) * 8;
      int wz = (cz * 2 + 1) * 8;
      boolean center = (long) cx * cx + (long) cz * cz <= 4096L;
      double h = c.erosionAtBlock(wx, by, wz);
      j.la("chunk", new long[]{cx, cz}).la("weird_block", new long[]{wx, wz}).b("center_island", center)
         .d("height_value", h).l("height_value_dbits", Double.doubleToRawLongBits(h)).l("height_value_fbits", Float.floatToRawIntBits((float) h))
         .b("raw_float_native", c.floatNative());
      j.s("biome", Util.biomeName(c.biome(qx, qy, qz)));
   }

   static String climategrid(Args a) {
      Ctx.Dim dim = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int qx0 = a.i(2);
      int qz0 = a.i(3);
      int nx = a.i(4);
      int nz = a.i(5);
      int qy = a.i(6);
      ClimateApi c = clim(dim, a);
      boolean wantRaw = a.has("raw");
      boolean wantBiome = a.has("biome");
      J j = J.obj().b("ok", true).s("cmd", "climategrid").s("version", Main.VERSION).s("dim", dim.name).s("preset", dim.preset)
         .l("seed", dim.seed).s("mode", c.mode()).l("qx0", qx0).l("qz0", qz0).l("nx", nx).l("nz", nz).l("qy", qy)
         .s("layout", "index = iz*nx + ix, значения на (qx0+ix, qy, qz0+iz)");
      int n = nx * nz;
      if (dim.isEnd()) {
         double[] h = new double[n];
         List<String> names = new ArrayList<>();
         Map<String, Integer> pal = new java.util.LinkedHashMap<>();
         int[] idx = new int[n];
         for (int iz = 0; iz < nz; iz++) {
            for (int ix = 0; ix < nx; ix++) {
               int qx = qx0 + ix;
               int qz = qz0 + iz;
               int bx = QuartPos.toBlock(qx);
               int bz = QuartPos.toBlock(qz);
               int wx = (SectionPos.blockToSectionCoord(bx) * 2 + 1) * 8;
               int wz = (SectionPos.blockToSectionCoord(bz) * 2 + 1) * 8;
               h[iz * nx + ix] = c.erosionAtBlock(wx, QuartPos.toBlock(qy), wz);
               if (wantBiome) idx[iz * nx + ix] = pal.computeIfAbsent(Util.biomeName(c.biome(qx, qy, qz)), k -> pal.size());
            }
         }
         j.da("height_value", h);
         if (wantBiome) j.sa("palette", new ArrayList<>(pal.keySet())).ia("biome", idx);
         return j.done();
      }
      GridOut g = new GridOut(nx, nz, true, wantRaw, wantBiome);
      c.grid(qx0, qz0, nx, nz, qy, g);
      String[] keys = {"t", "h", "c", "e", "d", "w"};
      j.o("q");
      for (int k = 0; k < 6; k++) j.la(keys[k], g.q[k]);
      j.end();
      if (wantRaw) {
         j.o("raw");
         for (int k = 0; k < 6; k++) j.da(keys[k], g.raw[k]);
         j.end();
      }
      if (wantBiome) {
         Map<String, Integer> pal = new java.util.LinkedHashMap<>();
         int[] idx = new int[n];
         for (int i = 0; i < n; i++) idx[i] = pal.computeIfAbsent(Util.biomeName(g.biome[i]), k -> pal.size());
         j.sa("palette", new ArrayList<>(pal.keySet())).ia("biome", idx);
      }
      return j.done();
   }

   // ------------------------------------------------------------------ biome grid

   static String biome(Args a) throws IOException {
      Ctx.Dim dim = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int qx0 = a.i(2);
      int qz0 = a.i(3);
      int nx = a.i(4);
      int nz = a.i(5);
      int qy = a.i(6);
      String fmt = a.get("fmt", "json");
      ClimateApi c = clim(dim, a);
      boolean brute = a.has("brute");
      long t0 = System.nanoTime();
      GridOut g = new GridOut(nx, nz, brute, false, true);
      c.grid(qx0, qz0, nx, nz, qy, g);
      long t1 = System.nanoTime();
      int n = nx * nz;
      IdentityHashMap<Holder<Biome>, Integer> palIdx = new IdentityHashMap<>();
      List<String> palette = new ArrayList<>();
      int[] idx = new int[n];
      for (int i = 0; i < n; i++) {
         Holder<Biome> h = g.biome[i];
         Integer p = palIdx.get(h);
         if (p == null) {
            p = palette.size();
            palIdx.put(h, p);
            palette.add(Util.biomeName(h));
         }
         idx[i] = p;
      }
      J j = J.obj().b("ok", true).s("cmd", "biome").s("version", Main.VERSION).s("dim", dim.name).s("preset", dim.preset)
         .l("seed", dim.seed).s("mode", c.mode()).l("qx0", qx0).l("qz0", qz0).l("nx", nx).l("nz", nz).l("qy", qy)
         .s("layout", "index = iz*nx + ix, биом на (qx0+ix, qy, qz0+iz)").s("fmt", fmt);
      long[] counts = new long[palette.size()];
      for (int v : idx) counts[v]++;
      // хэш по ИМЕНАМ (не зависит от порядка палитры): FNV-1a 64 по UTF-8 имён + '\n'
      byte[][] nameBytes = new byte[palette.size()][];
      for (int i = 0; i < palette.size(); i++) nameBytes[i] = (palette.get(i) + "\n").getBytes(StandardCharsets.UTF_8);
      long hash = Util.FNV_OFFSET;
      for (int v : idx) hash = Util.fnv1a(hash, nameBytes[v]);
      j.s("hash_fnv1a64_names", Util.hex16(hash));
      if (brute) {
         Climate.ParameterList<Holder<Biome>> list = parameterList(dim);
         long mism = 0;
         J ex = null;
         List<String> exs = new ArrayList<>();
         for (int i = 0; i < n; i++) {
            Climate.TargetPoint t = new Climate.TargetPoint(g.q[0][i], g.q[1][i], g.q[2][i], g.q[3][i], g.q[4][i], g.q[5][i]);
            Holder<Biome> bf = list.findValueBruteForce(t);
            if (bf != g.biome[i]) {
               mism++;
               if (exs.size() < 20) {
                  exs.add("[" + (qx0 + i % nx) + "," + (qz0 + i / nx) + ",\"" + Util.biomeName(g.biome[i]) + "\",\"" + Util.biomeName(bf) + "\"]");
               }
            }
         }
         j.l("brute_mismatch", mism);
         j.raw("brute_examples", "[" + String.join(",", exs) + "]");
      }
      TreeMap<String, Long> cnt = new TreeMap<>();
      for (int i = 0; i < palette.size(); i++) cnt.put(palette.get(i), counts[i]);
      j.o("counts");
      for (var e : cnt.entrySet()) j.l(e.getKey(), e.getValue());
      j.end();
      j.d("ms", (t1 - t0) / 1e6).d("points_per_s", n / Math.max(1e-9, (t1 - t0) / 1e9));
      switch (fmt) {
         case "json" -> {
            j.sa("palette", palette).ia("idx", idx);
         }
         case "hash" -> {
            j.sa("palette", palette);
         }
         case "bin" -> {
            String out = a.get("out", null);
            if (out == null) throw new IllegalArgumentException("--fmt bin требует --out файл");
            boolean wide = palette.size() > 256;
            ByteBuffer bb = ByteBuffer.allocate(n * (wide ? 2 : 1)).order(ByteOrder.LITTLE_ENDIAN);
            for (int v : idx) {
               if (wide) bb.putShort((short) v);
               else bb.put((byte) v);
            }
            bb.flip();
            Path p = Path.of(out);
            if (p.getParent() != null) java.nio.file.Files.createDirectories(p.getParent());
            try (FileChannel ch = FileChannel.open(p, StandardOpenOption.CREATE, StandardOpenOption.WRITE, StandardOpenOption.TRUNCATE_EXISTING)) {
               ch.write(bb);
            }
            j.s("file", out).s("elem", wide ? "u16le" : "u8").sa("palette", palette);
         }
         case "csv" -> {
            StringBuilder sb = new StringBuilder();
            boolean shortNames = a.has("short");
            for (int iz = 0; iz < nz; iz++) {
               for (int ix = 0; ix < nx; ix++) {
                  if (ix > 0) sb.append(',');
                  String nm = palette.get(idx[iz * nx + ix]);
                  sb.append(shortNames && nm.startsWith("minecraft:") ? nm.substring(10) : nm);
               }
               sb.append('\n');
            }
            String out = a.get("out", null);
            if (out != null) {
               Util.writeFile(out, sb.toString());
               j.s("file", out);
            } else {
               j.s("csv", sb.toString());
            }
         }
         default -> throw new IllegalArgumentException("Неизвестный --fmt " + fmt + " (json|csv|bin|hash)");
      }
      return j.done();
   }

   // ------------------------------------------------------------------ noise / df

   static String noise(Args a) {
      Ctx.Dim dim = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      Identifier id = Util.id(a.str(2));
      NoiseEval ne = dim.noise(id);
      int npts = (a.size() - 3) / 3;
      if (npts < 1 || (a.size() - 3) % 3 != 0) throw new IllegalArgumentException("noise <dim> <seed> <noise_id> x y z [x y z ...]");
      double[] v = new double[npts];
      double[] v2 = new double[npts];
      for (int i = 0; i < npts; i++) {
         double x = a.d(3 + 3 * i);
         double y = a.d(4 + 3 * i);
         double z = a.d(5 + 3 * i);
         v[i] = ne.get3(x, y, z);
         v2[i] = ne.get2(x, z);
      }
      J j = J.obj().b("ok", true).s("cmd", "noise").s("version", Main.VERSION).s("dim", dim.name).l("seed", dim.seed).s("id", id.toString())
         .b("float_native", ne.floatNative()).da("v", v);
      long[] db = new long[npts];
      long[] fb = new long[npts];
      for (int i = 0; i < npts; i++) {
         db[i] = Double.doubleToRawLongBits(v[i]);
         fb[i] = Float.floatToRawIntBits((float) v[i]);
      }
      j.la("dbits", db).la("fbits", fb);
      if (ne.floatNative() && a.has("twod")) {
         j.da("v2d_xz", v2);
         long[] fb2 = new long[npts];
         for (int i = 0; i < npts; i++) fb2[i] = Float.floatToRawIntBits((float) v2[i]);
         j.la("v2d_fbits", fb2);
      }
      return j.done();
   }

   static String df(Args a) {
      Ctx.Dim dim = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      Identifier id = Util.id(a.str(2));
      DfEval de = dim.df(id);
      int npts = (a.size() - 3) / 3;
      if (npts < 1 || (a.size() - 3) % 3 != 0) throw new IllegalArgumentException("df <dim> <seed> <df_id> x y z [x y z ...]");
      double[] v = new double[npts];
      long[] db = new long[npts];
      long[] fb = new long[npts];
      for (int i = 0; i < npts; i++) {
         v[i] = de.at(a.i(3 + 3 * i), a.i(4 + 3 * i), a.i(5 + 3 * i));
         db[i] = Double.doubleToRawLongBits(v[i]);
         fb[i] = Float.floatToRawIntBits((float) v[i]);
      }
      return J.obj().b("ok", true).s("cmd", "df").s("version", Main.VERSION).s("dim", dim.name).s("preset", dim.preset).l("seed", dim.seed)
         .s("id", id.toString()).b("float_native", de.floatNative()).da("v", v).la("dbits", db).la("fbits", fb).done();
   }

   // ------------------------------------------------------------------ list

   static String list(Args a) {
      String what = a.str(0);
      var reg = Ctx.reg;
      List<String> ids = new ArrayList<>();
      switch (what) {
         case "noises" -> reg.lookupOrThrow(Registries.NOISE).listElementIds().forEach(k -> ids.add(k.identifier().toString()));
         case "dfs" -> reg.lookupOrThrow(Registries.DENSITY_FUNCTION).listElementIds().forEach(k -> ids.add(k.identifier().toString()));
         case "presets" -> reg.lookupOrThrow(Registries.WORLD_PRESET).listElementIds().forEach(k -> ids.add(k.identifier().toString()));
         case "biomes" -> reg.lookupOrThrow(Registries.BIOME).listElementIds().forEach(k -> ids.add(k.identifier().toString()));
         case "structure_sets" -> reg.lookupOrThrow(Registries.STRUCTURE_SET).listElementIds().forEach(k -> ids.add(k.identifier().toString()));
         case "structures" -> reg.lookupOrThrow(Registries.STRUCTURE).listElementIds().forEach(k -> ids.add(k.identifier().toString()));
         case "noise_settings" -> reg.lookupOrThrow(Registries.NOISE_SETTINGS).listElementIds().forEach(k -> ids.add(k.identifier().toString()));
         default -> throw new IllegalArgumentException("list noises|dfs|presets|biomes|structure_sets|structures|noise_settings");
      }
      java.util.Collections.sort(ids);
      return J.obj().b("ok", true).s("cmd", "list").s("version", Main.VERSION).s("what", what).l("count", ids.size()).sa("ids", ids).done();
   }
}

package mcgenref;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseRouter;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import oracle.Ctx;
import oracle.compat.Compat;

/**
 * Эталон libmcgen для 26.4-snapshot-2 (настоящие классы игры), команды из stdin, ответ — одна строка:
 *   df <noise_settings> <seed> <цель> x y z [...]   цель: rf:<поле NoiseRouter> | aq:<поле Aquifer.Config> | id функции
 *        → "ok <bits float hex> ..." (RandomState.sampleBlockValueUncached)
 *   fill <noise_settings> <seed> <cx> <cz> <minY> <height>
 *        → "ok <base64 u16 LE [y][z][x]>" — заполнение шумом как NoiseChunk.prepareColumn 26.4 (без поверхности; NoiseColumn.SOLID
 *          → stone/netherrack/end_stone по настройкам шума, как подставляет libmcgen: default_block в 26.4 убран)
 * Нужен для пресетов без мирового пресета (caves, floating_islands) и полей роутера, которых нет в реестре функций.
 */
public final class Ref {
   public static void main(String[] argv) throws Exception {
      PrintStream out = new PrintStream(new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16), false, StandardCharsets.UTF_8);
      Ctx.init();
      BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
      String line;
      while ((line = in.readLine()) != null) {
         line = line.strip();
         if (line.isEmpty()) continue;
         if (line.equals("quit")) break;
         try {
            out.println(exec(line.split("\\s+")));
         } catch (Throwable e) {
            out.println("err " + e.getClass().getSimpleName() + ": " + e.getMessage());
         }
         out.flush();
      }
   }

   static NoiseGeneratorSettings settings(String id) {
      return Ctx.reg.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(ResourceKey.create(Registries.NOISE_SETTINGS, Identifier.parse(id))).value();
   }

   static DensityFunction target(NoiseGeneratorSettings s, String t) {
      if (t.startsWith("rf:")) {
         NoiseRouter r = s.noiseRouter();
         return switch (t.substring(3)) {
            case "temperature" -> r.temperature();
            case "vegetation" -> r.vegetation();
            case "continents" -> r.continents();
            case "erosion" -> r.erosion();
            case "depth" -> r.depth();
            case "ridges" -> r.ridges();
            case "chunk_surface_level" -> r.chunkSurfaceLevel();
            case "final_density" -> r.finalDensity();
            default -> throw new IllegalArgumentException(t);
         };
      }
      if (t.startsWith("aq:")) {
         Aquifer.Config c = s.aquifers().orElseThrow();
         return switch (t.substring(3)) {
            case "barrier" -> c.barrierNoise();
            case "fluid_level_floodedness" -> c.fluidLevelFloodednessNoise();
            case "fluid_level_spread" -> c.fluidLevelSpreadNoise();
            case "lava" -> c.lavaNoise();
            case "exclusion" -> c.exclusion();
            case "surface_level" -> c.surfaceLevel();
            default -> throw new IllegalArgumentException(t);
         };
      }
      return Ctx.reg.lookupOrThrow(Registries.DENSITY_FUNCTION).getOrThrow(ResourceKey.create(Registries.DENSITY_FUNCTION, Identifier.parse(t))).value();
   }

   static String exec(String[] a) throws Exception {
      NoiseGeneratorSettings s = settings(a[1]);
      long seed = Long.parseLong(a[2]);
      RandomState rs = Compat.newRandomState(Ctx.reg, s, seed);
      if (a[0].equals("df")) {
         DensityFunction f = target(s, a[3]);
         StringBuilder sb = new StringBuilder("ok");
         for (int i = 4; i + 2 < a.length; i += 3) {
            float v = rs.sampleBlockValueUncached(f, Integer.parseInt(a[i]), Integer.parseInt(a[i + 1]), Integer.parseInt(a[i + 2]));
            sb.append(' ').append(Integer.toHexString(Float.floatToRawIntBits(v)));
         }
         return sb.toString();
      }
      if (a[0].equals("fill")) {
         int cx = Integer.parseInt(a[3]), cz = Integer.parseInt(a[4]), minY = Integer.parseInt(a[5]), height = Integer.parseInt(a[6]);
         int nMin = Math.max(s.noiseSettings().minY(), minY);
         int nTop = Math.min(s.noiseSettings().minY() + s.noiseSettings().height(), minY + height);
         int nh = nTop - nMin;
         Method fpm = NoiseBasedChunkGenerator.class.getDeclaredMethod("createFluidPicker", NoiseGeneratorSettings.class);
         fpm.setAccessible(true);
         Aquifer.FluidPicker fp = (Aquifer.FluidPicker) fpm.invoke(null, s);
         short[] blocks = new short[height * 256];
         short air = (short) Block.getId(net.minecraft.world.level.block.Blocks.AIR.defaultBlockState());
         java.util.Arrays.fill(blocks, air);
         BlockState solid = a[1].endsWith("nether") ? net.minecraft.world.level.block.Blocks.NETHERRACK.defaultBlockState()
            : a[1].endsWith("end") ? net.minecraft.world.level.block.Blocks.END_STONE.defaultBlockState()
            : net.minecraft.world.level.block.Blocks.STONE.defaultBlockState();
         if (nh > 0) {
            DensityVolume vol = new DensityVolume(16, nh, 16, cx * 16, nMin, cz * 16);
            try (NoiseChunk nc = new NoiseChunk(rs, vol, net.minecraft.util.context.ContextMap.EMPTY, s.noiseRouter().finalDensity(),
                  s.aquifers().orElse(null), fp)) {
               for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
                  net.minecraft.world.level.levelgen.NoiseColumn col = nc.prepareColumn(x, z);
                  for (int y = 0; y < nh; y++) {
                     BlockState st = col.getBlockForIndex(y);   // выше topIndexY prepareColumn оставляет AIR
                     if (st == null) st = solid;
                     blocks[((nMin + y - minY) * 16 + z) * 16 + x] = (short) Block.getId(st);
                  }
               }
            }
         }
         byte[] b = new byte[blocks.length * 2];
         for (int i = 0; i < blocks.length; i++) { b[2 * i] = (byte) blocks[i]; b[2 * i + 1] = (byte) (blocks[i] >> 8); }
         return "ok " + Base64.getEncoder().encodeToString(b);
      }
      throw new IllegalArgumentException("команда: df | fill");
   }
}

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
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.DensityFunction;
import net.minecraft.world.level.levelgen.DensityFunctions;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseRouter;
import net.minecraft.world.level.levelgen.NoiseSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.blending.Blender;
import oracle.Ctx;
import oracle.compat.Compat;

/**
 * Эталон libmcgen для 26.1/26.2 (настоящие классы игры). Команды как у 26.3-версии:
 *   df <noise_settings> <seed> <rf:поле | id функции> x y z [...]  → "ok <bits double hex> ..." (compute(SinglePointContext)
 *        поля проведённого роутера RandomState; функции реестра — через Compat.df)
 *   fill <noise_settings> <seed> <cx> <cz> <minY> <height>   → "ok <base64 u16 LE [y][z][x]>" — цикл doFill по NoiseChunk
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

   static DensityFunction field(RandomState rs, String t) {
      NoiseRouter r = rs.router();
      return switch (t) {
         case "barrier" -> r.barrierNoise();
         case "fluid_level_floodedness" -> r.fluidLevelFloodednessNoise();
         case "fluid_level_spread" -> r.fluidLevelSpreadNoise();
         case "lava" -> r.lavaNoise();
         case "temperature" -> r.temperature();
         case "vegetation" -> r.vegetation();
         case "continents" -> r.continents();
         case "erosion" -> r.erosion();
         case "depth" -> r.depth();
         case "ridges" -> r.ridges();
         case "preliminary_surface_level" -> r.preliminarySurfaceLevel();
         case "final_density" -> r.finalDensity();
         case "vein_toggle" -> r.veinToggle();
         case "vein_ridged" -> r.veinRidged();
         case "vein_gap" -> r.veinGap();
         default -> throw new IllegalArgumentException(t);
      };
   }

   static String exec(String[] a) throws Exception {
      NoiseGeneratorSettings s = settings(a[1]);
      long seed = Long.parseLong(a[2]);
      RandomState rs = Compat.newRandomState(Ctx.reg, s, seed);
      if (a[0].equals("df")) {
         StringBuilder sb = new StringBuilder("ok");
         if (a[3].startsWith("rf:")) {
            DensityFunction f = field(rs, a[3].substring(3));
            for (int i = 4; i + 2 < a.length; i += 3) {
               double v = f.compute(new DensityFunction.SinglePointContext(Integer.parseInt(a[i]), Integer.parseInt(a[i + 1]), Integer.parseInt(a[i + 2])));
               sb.append(' ').append(Long.toHexString(Double.doubleToRawLongBits(v)));
            }
         } else {
            oracle.DfEval de = Compat.df(Ctx.reg, s, rs, seed, Identifier.parse(a[3]));
            for (int i = 4; i + 2 < a.length; i += 3) {
               double v = de.at(Integer.parseInt(a[i]), Integer.parseInt(a[i + 1]), Integer.parseInt(a[i + 2]));
               sb.append(' ').append(Long.toHexString(Double.doubleToRawLongBits(v)));
            }
         }
         return sb.toString();
      }
      if (a[0].equals("fill")) {
         int cx = Integer.parseInt(a[3]), cz = Integer.parseInt(a[4]), minY = Integer.parseInt(a[5]), height = Integer.parseInt(a[6]);
         NoiseSettings ns = s.noiseSettings().clampToHeightAccessor(LevelHeightAccessor.create(minY, height));
         Method fpm = NoiseBasedChunkGenerator.class.getDeclaredMethod("createFluidPicker", NoiseGeneratorSettings.class);
         fpm.setAccessible(true);
         Aquifer.FluidPicker fp = (Aquifer.FluidPicker) fpm.invoke(null, s);
         Object beard = Class.forName("net.minecraft.world.level.levelgen.DensityFunctions$BeardifierMarker").getEnumConstants()[0];
         int cw = ns.getCellWidth(), ch = ns.getCellHeight();
         NoiseChunk nc = new NoiseChunk(16 / cw, rs, cx * 16, cz * 16, ns, (DensityFunctions.BeardifierOrMarker) beard, s, fp, Blender.empty());
         Method gis = NoiseChunk.class.getDeclaredMethod("getInterpolatedState");
         gis.setAccessible(true);
         short[] blocks = new short[height * 256];
         java.util.Arrays.fill(blocks, (short) Block.getId(net.minecraft.world.level.block.Blocks.AIR.defaultBlockState()));
         int cellMinY = Math.floorDiv(ns.minY(), ch), cellCountY = Math.floorDiv(ns.height(), ch);
         if (cellCountY > 0) {
            nc.initializeForFirstCellX();
            for (int cxi = 0; cxi < 16 / cw; cxi++) {
               nc.advanceCellX(cxi);
               for (int czi = 0; czi < 16 / cw; czi++) {
                  for (int cyi = cellCountY - 1; cyi >= 0; cyi--) {
                     nc.selectCellYZ(cyi, czi);
                     for (int yi = ch - 1; yi >= 0; yi--) {
                        int py = (cellMinY + cyi) * ch + yi;
                        nc.updateForY(py, (double) yi / ch);
                        for (int xi = 0; xi < cw; xi++) {
                           int px = cx * 16 + cxi * cw + xi;
                           nc.updateForX(px, (double) xi / cw);
                           for (int zi = 0; zi < cw; zi++) {
                              int pz = cz * 16 + czi * cw + zi;
                              nc.updateForZ(pz, (double) zi / cw);
                              BlockState st = (BlockState) gis.invoke(nc);
                              if (st == null) st = s.defaultBlock();
                              blocks[((py - minY) * 16 + (pz & 15)) * 16 + (px & 15)] = (short) Block.getId(st);
                           }
                        }
                     }
                  }
               }
               nc.swapSlices();
            }
            nc.stopInterpolation();
         }
         byte[] b = new byte[blocks.length * 2];
         for (int i = 0; i < blocks.length; i++) { b[2 * i] = (byte) blocks[i]; b[2 * i + 1] = (byte) (blocks[i] >> 8); }
         return "ok " + Base64.getEncoder().encodeToString(b);
      }
      throw new IllegalArgumentException("команда: df | fill");
   }
}

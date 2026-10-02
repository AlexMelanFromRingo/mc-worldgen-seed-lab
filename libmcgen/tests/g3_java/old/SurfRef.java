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
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.DensityFunctions;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.WorldGenerationContext;
import net.minecraft.world.level.levelgen.blending.Blender;
import oracle.Ctx;
import oracle.compat.Compat;

/**
 * Эталон стадии SURFACE для 26.1/26.2 (настоящие классы игры, без сервера): doFill (как NoiseBasedChunkGenerator.doFill) +
 * SurfaceSystem.buildSurface одного чанка на BiomeSource измерения.
 *   surface <dim> <preset> <seed> <cx> <cz> [nosurface]  → "ok <base64 u16 LE [y][z][x] по высоте измерения>"
 */
public final class SurfRef {
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
            e.printStackTrace();
         }
         out.flush();
      }
   }

   static PalettedContainerFactory factory;

   static String exec(String[] a) throws Exception {
      if (!a[0].equals("surface")) throw new IllegalArgumentException("команда: surface <dim> <preset> <seed> <cx> <cz>");
      Ctx.Dim d = Ctx.dim(a[1], a[2], Long.parseLong(a[3]));
      int cx = Integer.parseInt(a[4]), cz = Integer.parseInt(a[5]);
      NoiseGeneratorSettings s = d.settings;
      RandomState rs = d.rs;
      int minY = d.minY, height = d.height;
      if (factory == null) factory = PalettedContainerFactory.create(Compat.registryAccess());
      LevelHeightAccessor acc = LevelHeightAccessor.create(minY, height);
      ProtoChunk chunk = new ProtoChunk(new ChunkPos(cx, cz), UpgradeData.EMPTY, acc, factory, null);
      NoiseSettings ns = s.noiseSettings().clampToHeightAccessor(chunk.getHeightAccessorForGeneration());
      Method fpm = NoiseBasedChunkGenerator.class.getDeclaredMethod("createFluidPicker", NoiseGeneratorSettings.class);
      fpm.setAccessible(true);
      Aquifer.FluidPicker fp = (Aquifer.FluidPicker) fpm.invoke(null, s);
      Object beard = Class.forName("net.minecraft.world.level.levelgen.DensityFunctions$BeardifierMarker").getEnumConstants()[0];
      int cw = ns.getCellWidth(), ch = ns.getCellHeight();
      NoiseChunk nc = new NoiseChunk(16 / cw, rs, cx * 16, cz * 16, ns, (DensityFunctions.BeardifierOrMarker) beard, s, fp, Blender.empty());
      Method gis = NoiseChunk.class.getDeclaredMethod("getInterpolatedState");
      gis.setAccessible(true);
      // --- doFill (NoiseBasedChunkGenerator.doFill) ---
      int cellMinY = Math.floorDiv(ns.minY(), ch), cellCountY = Math.floorDiv(ns.height(), ch);
      Heightmap oceanFloor = chunk.getOrCreateHeightmapUnprimed(Heightmap.Types.OCEAN_FLOOR_WG);
      Heightmap worldSurface = chunk.getOrCreateHeightmapUnprimed(Heightmap.Types.WORLD_SURFACE_WG);
      BlockState air = net.minecraft.world.level.block.Blocks.AIR.defaultBlockState();
      if (cellCountY > 0) {
         nc.initializeForFirstCellX();
         for (int cxi = 0; cxi < 16 / cw; cxi++) {
            nc.advanceCellX(cxi);
            for (int czi = 0; czi < 16 / cw; czi++) {
               for (int cyi = cellCountY - 1; cyi >= 0; cyi--) {
                  nc.selectCellYZ(cyi, czi);
                  for (int yi = ch - 1; yi >= 0; yi--) {
                     int py = (cellMinY + cyi) * ch + yi;
                     LevelChunkSection section = chunk.getSection(chunk.getSectionIndex(py));
                     nc.updateForY(py, (double) yi / ch);
                     for (int xi = 0; xi < cw; xi++) {
                        int px = cx * 16 + cxi * cw + xi;
                        nc.updateForX(px, (double) xi / cw);
                        for (int zi = 0; zi < cw; zi++) {
                           int pz = cz * 16 + czi * cw + zi;
                           nc.updateForZ(pz, (double) zi / cw);
                           BlockState st = (BlockState) gis.invoke(nc);
                           if (st == null) st = s.defaultBlock();
                           if (st != air) {
                              section.setBlockState(px & 15, py & 15, pz & 15, st, false);
                              oceanFloor.update(px & 15, py, pz & 15, st);
                              worldSurface.update(px & 15, py, pz & 15, st);
                           }
                        }
                     }
                  }
               }
            }
            nc.swapSlices();
         }
         nc.stopInterpolation();
      }
      // --- buildSurface ---
      boolean doSurface = !(a.length > 6 && a[6].equals("nosurface"));
      if (doSurface) {
         Climate.Sampler sampler = rs.sampler();
         int qmin = minY >> 2, qmax = qmin + (height >> 2) - 1;
         BiomeManager bm = new BiomeManager((qx, qy, qz) -> d.biomeSource.getNoiseBiome(qx, Math.max(qmin, Math.min(qmax, qy)), qz, sampler), BiomeManager.obfuscateSeed(d.seed));
         WorldGenerationContext gctx = new WorldGenerationContext(d.gen, acc);
         Object sys = rs.surfaceSystem();
         Method bs = null;
         for (Method m : sys.getClass().getDeclaredMethods()) if (m.getName().equals("buildSurface")) bs = m;
         Class<?>[] pt = bs.getParameterTypes();
         Object[] args = new Object[pt.length];
         for (int i = 0; i < pt.length; i++) {
            Class<?> t = pt[i];
            if (t == RandomState.class) args[i] = rs;
            else if (t == BiomeManager.class) args[i] = bm;
            else if (t == Registry.class) args[i] = Compat.registryAccess().lookupOrThrow(Registries.BIOME);
            else if (t == boolean.class) args[i] = s.useLegacyRandomSource();
            else if (t == WorldGenerationContext.class) args[i] = gctx;
            else if (t == ChunkAccess.class) args[i] = chunk;
            else if (t == NoiseChunk.class) args[i] = nc;
            else if (t.getSimpleName().equals("RuleSource")) args[i] = s.surfaceRule();
            else if (t == java.util.Set.class) args[i] = null;
            else throw new IllegalStateException("неизвестный параметр buildSurface: " + t);
         }
         bs.setAccessible(true);
         bs.invoke(sys, args);
      }
      short[] blocks = new short[height * 256];
      BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
      for (int y = 0; y < height; y++) for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
         BlockState st = chunk.getBlockState(p.set(cx * 16 + x, minY + y, cz * 16 + z));
         blocks[(y * 16 + z) * 16 + x] = (short) Block.getId(st);
      }
      byte[] b = new byte[blocks.length * 2];
      for (int i = 0; i < blocks.length; i++) { b[2 * i] = (byte) blocks[i]; b[2 * i + 1] = (byte) (blocks[i] >> 8); }
      return "ok " + Base64.getEncoder().encodeToString(b);
   }
}

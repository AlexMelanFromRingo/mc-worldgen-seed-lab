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
import java.util.Set;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.BiomeResolver;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import oracle.Ctx;
import oracle.compat.Compat;

/**
 * Эталон стадии SURFACE для 26.3 (настоящие классы игры, без сервера): doFill + MaterialSystem.buildSurface одного чанка.
 *   surface <dim> <preset> <seed> <cx> <cz>   → "ok <base64 u16 LE [y][z][x] по высоте измерения>"
 * Биомы — BiomeSource измерения (с зумом BiomeManager и зажимом y в пределы чанка, как ChunkAccess.getNoiseBiome).
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
      DensityVolume vol = new DensityVolume(16, ns.height(), 16, cx * 16, ns.minY(), cz * 16);
      // биомы: BiomeManager с зажимом y по границам мира (ChunkAccess.getNoiseBiome)
      BiomeResolver base = d.biomeSource.createUncachedResolver(rs);
      int qmin = minY >> 2, qmax = qmin + (height >> 2) - 1;
      BiomeResolver clamped = (qx, qy, qz) -> base.getNoiseBiome(qx, Math.max(qmin, Math.min(qmax, qy)), qz);
      BiomeManager bm = new BiomeManager(clamped, BiomeManager.obfuscateSeed(d.seed));
      Method fill = NoiseBasedChunkGenerator.class.getDeclaredMethod("doFill", NoiseChunk.class, ChunkAccess.class);
      fill.setAccessible(true);
      Method surf = NoiseBasedChunkGenerator.class.getDeclaredMethod("buildSurface", ChunkAccess.class, NoiseChunk.class, RandomState.class,
         BiomeManager.class, Set.class, Class.forName("net.minecraft.world.level.levelgen.material.rule.MaterialRule"));
      surf.setAccessible(true);
      boolean doSurface = !(a.length > 6 && a[6].equals("nosurface"));
      try (NoiseChunk nc = new NoiseChunk(rs, null, s, fp, Blender.empty(), vol)) {
         fill.invoke(d.gen, nc, chunk);
         if (doSurface) surf.invoke(d.gen, chunk, nc, rs, bm, d.biomeSource.possibleBiomes(), s.materialRule().value());
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

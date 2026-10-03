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
public final class CarveRef {
   public static void main(String[] argv) throws Exception {
      PrintStream out = new PrintStream(new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16), false, StandardCharsets.UTF_8);
      Ctx.init();
      bindStaticTags();
      // теги блоков (blocks_motion_in_heightmap …) нужны Heightmap.Types: oracle загружает их, но не привязывает к реестру
      var layers = net.minecraft.server.RegistryLayer.createRegistryAccess();
      for (var pt : net.minecraft.tags.TagLoader.loadTagsForExistingRegistries(Compat.resourceManager(), layers.getLayer(net.minecraft.server.RegistryLayer.STATIC))) pt.apply();
      BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
      String line;
      while ((line = in.readLine()) != null) {
         line = line.strip();
         if (line.isEmpty()) continue;
         if (line.equals("quit")) break;
         try {
            out.println(exec(line.split("\\s+")));
         } catch (Throwable e) {
            Throwable c = e;
            while (c.getCause() != null) c = c.getCause();
            StringBuilder sb = new StringBuilder("err " + c.getClass().getSimpleName() + ": " + c.getMessage());
            for (int i = 0; i < Math.min(6, c.getStackTrace().length); i++) sb.append(" @ ").append(c.getStackTrace()[i]);
            out.println(sb);
         }
         out.flush();
      }
   }

   /** Oracle (Compat.loadRegistries) читает теги статических реестров, но не применяет их: без этого BlockState.is(тег) всегда false (uncarvable). */
   static void bindStaticTags() {
      var layers = net.minecraft.server.RegistryLayer.createRegistryAccess();
      java.util.List<net.minecraft.core.Registry.PendingTags<?>> pt = net.minecraft.tags.TagLoader.loadTagsForExistingRegistries(Compat.resourceManager(), layers.getLayer(net.minecraft.server.RegistryLayer.STATIC));
      for (net.minecraft.core.Registry.PendingTags<?> p : pt) p.apply();
   }

   static PalettedContainerFactory factory;

   /** Измерение для эталона: обычные пресеты — Ctx.dim; caves / floating_islands (есть только noise_settings) — генератор Overworld с их настройками. */
   static final class G {
      NoiseBasedChunkGenerator gen; NoiseGeneratorSettings settings; net.minecraft.world.level.biome.BiomeSource biomeSource; RandomState rs;
      int minY, height; long seed;
   }
   static final java.util.Map<String, G> GS = new java.util.HashMap<>();

   static G dimG(String dim, String preset, long seed) throws Exception {
      String key = dim + "/" + preset + "/" + seed;
      G g = GS.get(key);
      if (g != null) return g;
      g = new G();
      g.seed = seed;
      if (preset.equals("caves") || preset.equals("floating_islands")) {
         Ctx.Dim base = Ctx.dim("overworld", "normal", seed);
         var holder = Ctx.reg.lookupOrThrow(net.minecraft.core.registries.Registries.NOISE_SETTINGS).getOrThrow(
            net.minecraft.resources.ResourceKey.create(net.minecraft.core.registries.Registries.NOISE_SETTINGS, net.minecraft.resources.Identifier.withDefaultNamespace(preset)));
         g.settings = holder.value();
         g.biomeSource = base.biomeSource;
         g.gen = new NoiseBasedChunkGenerator(base.biomeSource, holder);
         g.rs = Compat.newRandomState(Ctx.reg, g.settings, seed);
         var dtReg = Ctx.reg.lookupOrThrow(net.minecraft.core.registries.Registries.DIMENSION_TYPE);
         var dtKey = net.minecraft.resources.ResourceKey.create(net.minecraft.core.registries.Registries.DIMENSION_TYPE, net.minecraft.resources.Identifier.withDefaultNamespace("overworld_" + preset));
         var dt = dtReg.get(dtKey);
         if (dt.isEmpty()) dt = dtReg.get(net.minecraft.resources.ResourceKey.create(net.minecraft.core.registries.Registries.DIMENSION_TYPE, net.minecraft.resources.Identifier.withDefaultNamespace("overworld")));
         g.minY = dt.get().value().minY(); g.height = dt.get().value().height();
      } else {
         Ctx.Dim d = Ctx.dim(dim, preset, seed);
         g.gen = d.gen; g.settings = d.settings; g.biomeSource = d.biomeSource; g.rs = d.rs; g.minY = d.minY; g.height = d.height;
      }
      GS.put(key, g);
      return g;
   }
   static G lastDim;
   static final java.util.LinkedHashMap<Long, ProtoChunk> bcache = new java.util.LinkedHashMap<>();

   static ProtoChunk biomeChunk(G d, int cx, int cz, LevelHeightAccessor acc) {
      long key = ((long) cx << 32) ^ (cz & 0xffffffffL);
      ProtoChunk c = bcache.get(key);
      if (c == null) {
         c = new ProtoChunk(new ChunkPos(cx, cz), UpgradeData.EMPTY, acc, factory, null);
         d.gen.createBiomes(d.rs, Blender.empty(), null, c).join();
         c.setPersistedStatus(net.minecraft.world.level.chunk.status.ChunkStatus.BIOMES);
         bcache.put(key, c);
         if (bcache.size() > 600) { var it = bcache.keySet().iterator(); it.next(); it.remove(); }
      }
      return c;
   }

   static String exec(String[] a) throws Exception {
      if (!a[0].equals("carve")) throw new IllegalArgumentException("команда: carve <dim> <preset> <seed> <cx> <cz> [nosurface]");
      G d = dimG(a[1], a[2], Long.parseLong(a[3]));
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
      // биомы — как в игре: чанки получают биомы стадией BIOMES (doCreateBiomes, объёмный путь), BiomeManager читает соседние чанки
      if (lastDim != d) { bcache.clear(); lastDim = d; }
      final G dd = d;
      BiomeManager bm = new BiomeManager((qx, qy, qz) -> biomeChunk(dd, qx >> 2, qz >> 2, acc).getNoiseBiome(qx, qy, qz), BiomeManager.obfuscateSeed(d.seed));
      Method fill = NoiseBasedChunkGenerator.class.getDeclaredMethod("doFill", NoiseChunk.class, ChunkAccess.class);
      fill.setAccessible(true);
      Method surf = NoiseBasedChunkGenerator.class.getDeclaredMethod("buildSurface", ChunkAccess.class, NoiseChunk.class, RandomState.class,
         BiomeManager.class, Set.class, Class.forName("net.minecraft.world.level.levelgen.material.rule.MaterialRule"));
      surf.setAccessible(true);
      boolean doSurface = !(a.length > 6 && a[6].equals("nosurface"));
      try (NoiseChunk nc = new NoiseChunk(rs, null, s, fp, Blender.empty(), vol)) {
         fill.invoke(d.gen, nc, chunk);
         if (doSurface) surf.invoke(d.gen, chunk, nc, rs, bm, d.biomeSource.possibleBiomes(), s.materialRule().value());
         Method gc = NoiseBasedChunkGenerator.class.getDeclaredMethod("generateCarvers", ChunkAccess.class, Blender.class, NoiseChunk.class, RandomState.class,
            BiomeManager.class, net.minecraft.server.level.WorldGenRegion.class, Class.forName("net.minecraft.world.level.levelgen.material.rule.MaterialRule"));
         gc.setAccessible(true);
         gc.invoke(d.gen, chunk, Blender.empty(), nc, rs, bm, null, s.materialRule().value());
      }
      short[] blocks = new short[height * 256];
      BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
      for (int y = 0; y < height; y++) for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
         BlockState st = chunk.getSection(chunk.getSectionIndex(minY + y)).getBlockState(x, (minY + y) & 15, z);   // не ProtoChunk.getBlockState: у секции из одного воздуха он возвращает AIR вместо cave_air
         blocks[(y * 16 + z) * 16 + x] = (short) Block.getId(st);
      }
      byte[] b = new byte[blocks.length * 2];
      for (int i = 0; i < blocks.length; i++) { b[2 * i] = (byte) blocks[i]; b[2 * i + 1] = (byte) (blocks[i] >> 8); }
      return "ok " + Base64.getEncoder().encodeToString(b);
   }
}

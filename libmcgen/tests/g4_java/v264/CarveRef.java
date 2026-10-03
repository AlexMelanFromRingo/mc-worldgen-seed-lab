package mcgenref;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import net.minecraft.core.BlockPos;
import net.minecraft.util.context.ContextMap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.CachedChunkBiomeResolver;
import net.minecraft.world.level.biome.NoiseBiomeResolver;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.ChunkTerrainBuilder;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.VerticalAnchor;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import oracle.Ctx;
import oracle.compat.Compat;

/**
 * (карверы 26.4, ChunkTerrainBuilder) Эталон стадии SURFACE для 26.4-snapshot-2 (настоящие классы игры, без сервера): createBiomes (CachedChunkBiomeResolver) +
 * NoiseBasedChunkGenerator.buildTerrain без карверов (ChunkTerrainBuilder.fillChunk) одного чанка.
 *   surface <dim> <preset> <seed> <cx> <cz>  → "ok <base64 u16 LE [y][z][x] по высоте измерения>"
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

   static ProtoChunk noiseChunk(G d, int cx, int cz, LevelHeightAccessor acc) {
      long key = ((long) cx << 32) ^ (cz & 0xffffffffL);
      ProtoChunk c = bcache.get(key);
      if (c == null) {
         c = new ProtoChunk(new ChunkPos(cx, cz), UpgradeData.EMPTY, acc, factory, null);
         d.gen.createNoiseBiomes(d.rs, net.minecraft.world.level.levelgen.blending.Blender.empty(), c, factory).join();
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
      ProtoChunk chunk = null;
      // как в игре: createNoiseBiomes (объёмный путь) для чанка и 8 соседей, затем createBiomes (CachedChunkBiomeResolver) по соседям
      if (lastDim != d) { bcache.clear(); lastDim = d; }
      for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) noiseChunk(d, cx + dx, cz + dz, acc);
      final ProtoChunk[][] nb = new ProtoChunk[3][3];
      for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) nb[dz + 1][dx + 1] = noiseChunk(d, cx + dx, cz + dz, acc);
      NoiseBiomeResolver nbr = (qx, qy, qz) -> {
         ProtoChunk n = nb[(qz >> 2) - cz + 1][(qx >> 2) - cx + 1];
         net.minecraft.world.level.biome.NoiseBiomeChunk nbc = n.getNoiseBiomeChunk();
         return nbc != null ? nbc.getNoiseBiome(qx, qy, qz) : n.getBiome(qx << 2, qy << 2, qz << 2);
      };
      chunk = nb[1][1];
      d.gen.createBiomes(nbr, rs, chunk).join();
      chunk.setPersistedStatus(net.minecraft.world.level.chunk.status.ChunkStatus.BIOMES);
      NoiseSettings ns = s.noiseSettings().clampToHeightAccessor(chunk.getHeightAccessorForGeneration());
      DensityVolume volume = new DensityVolume(16, ns.height(), 16, cx * 16, ns.minY(), cz * 16);
      ContextMap fields = ContextMap.builder().set(Beardifier.CONTEXT_KEY, Beardifier.EMPTY).build();
      Method cnc = NoiseBasedChunkGenerator.class.getDeclaredMethod("createNoiseChunk", RandomState.class, DensityVolume.class, ContextMap.class);
      cnc.setAccessible(true);
      boolean doSurface = !(a.length > 6 && a[6].equals("nosurface"));
      try (NoiseChunk nc = (NoiseChunk) cnc.invoke(d.gen, rs, volume, fields)) {
         VerticalAnchor.Context vac = VerticalAnchor.Context.from(d.gen, chunk.getHeightAccessorForGeneration());
         // как NoiseBasedChunkGenerator.buildTerrain (26.4): маска карверов по биомам исходных чанков (резолвер на кэширующем климатическом сэмплере чанка) + fillChunk(маска)
         net.minecraft.world.level.biome.Climate.Sampler cs = s.noiseRouter().createClimateSampler(nc.cachingSamplers());
         NoiseBiomeResolver res = d.biomeSource.createResolver(cs);
         Method gcm = null;
         for (Method m : NoiseBasedChunkGenerator.class.getDeclaredMethods()) if (m.getName().equals("generateCarvingMask")) gcm = m;
         gcm.setAccessible(true);
         Object mask = gcm.invoke(d.gen, chunk, net.minecraft.world.level.levelgen.blending.Blender.empty(), rs, null, res, vac);
         net.minecraft.world.level.levelgen.material.rule.MaterialRule rule;
         if (doSurface) rule = s.materialRule().value();
         else {
            String dn = a[1];
            rule = new net.minecraft.world.level.levelgen.material.rule.BlockRule((dn.contains("nether") ? net.minecraft.world.level.block.Blocks.NETHERRACK : dn.contains("end") ? net.minecraft.world.level.block.Blocks.END_STONE : net.minecraft.world.level.block.Blocks.STONE).defaultBlockState());
         }
         ChunkTerrainBuilder tb = new ChunkTerrainBuilder(rs, nc.cachingSamplers(), nc.volumeWithBlocks(), vac, rule, chunk, d.biomeSource.possibleBiomes());
         tb.fillChunk(chunk, nc, (net.minecraft.world.level.chunk.CarvingMask) mask);
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

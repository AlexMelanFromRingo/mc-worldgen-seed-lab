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
 * Эталон стадии CARVERS для 26.1/26.2 (настоящие классы игры, без сервера): doFill + [buildSurface] + NoiseBasedChunkGenerator.applyCarvers
 * одного чанка (обход исходных чанков радиуса 8, карверы биомов исходных чанков, тот же NoiseChunk/Aquifer, CarvingMask).
 *   carve <dim> <preset> <seed> <cx> <cz> [nosurface]  → "ok <base64 u16 LE [y][z][x] по высоте измерения>"
 */
public final class CarveRef {
   public static void main(String[] argv) throws Exception {
      PrintStream out = new PrintStream(new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16), false, StandardCharsets.UTF_8);
      Ctx.init();
      bindStaticTags();
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

   /** Oracle (Compat.loadRegistries) читает теги статических реестров, но не применяет их: без этого BlockState.is(тег) всегда false (replaceable карверов). */
   static void bindStaticTags() {
      var layers = net.minecraft.server.RegistryLayer.createRegistryAccess();
      java.util.List<Registry.PendingTags<?>> pt = net.minecraft.tags.TagLoader.loadTagsForExistingRegistries(Compat.resourceManager(), layers.getLayer(net.minecraft.server.RegistryLayer.STATIC));
      for (Registry.PendingTags<?> p : pt) p.apply();
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

   /** Биомы чанка как NoiseBasedChunkGenerator.doCreateBiomes: fillBiomesFromNoise с кэширующим климатическим сэмплером NoiseChunk. */
   static ProtoChunk biomeChunk(G d, int cx, int cz, LevelHeightAccessor acc, Aquifer.FluidPicker fp) {
      long key = ((long) cx << 32) ^ (cz & 0xffffffffL);
      ProtoChunk c = bcache.get(key);
      if (c == null) {
         try {
            c = new ProtoChunk(new ChunkPos(cx, cz), UpgradeData.EMPTY, acc, factory, null);
            NoiseChunk nc = NoiseChunk.forChunk(c, d.rs, net.minecraft.world.level.levelgen.Beardifier.EMPTY, d.settings, fp, Blender.empty());
            Method ccs = NoiseChunk.class.getDeclaredMethod("cachedClimateSampler", net.minecraft.world.level.levelgen.NoiseRouter.class, java.util.List.class);
            ccs.setAccessible(true);
            Climate.Sampler cs = (Climate.Sampler) ccs.invoke(nc, d.rs.router(), d.settings.spawnTarget());
            c.fillBiomesFromNoise(d.biomeSource, cs);
            c.setPersistedStatus(net.minecraft.world.level.chunk.status.ChunkStatus.BIOMES);
         } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
         }
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
         if (lastDim != d) { bcache.clear(); lastDim = d; }
         final G dd = d;
         final Aquifer.FluidPicker fpf = fp;
         BiomeManager bm = new BiomeManager((qx, qy, qz) -> biomeChunk(dd, qx >> 2, qz >> 2, acc, fpf).getNoiseBiome(qx, qy, qz), BiomeManager.obfuscateSeed(d.seed));
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
      // --- applyCarvers (NoiseBasedChunkGenerator.applyCarvers, 26.1/26.2) ---
      if (!(a.length > 7 && a[7].equals("nocarve"))) {
         final G dd = d;
         BiomeManager bm2 = new BiomeManager((qx, qy, qz) -> dd.biomeSource.getNoiseBiome(qx, qy, qz, dd.rs.sampler()), BiomeManager.obfuscateSeed(d.seed));
         net.minecraft.world.level.levelgen.WorldgenRandom random = new net.minecraft.world.level.levelgen.WorldgenRandom(new net.minecraft.world.level.levelgen.LegacyRandomSource(0L));
         ChunkPos pos = chunk.getPos();
         Aquifer aquifer = nc.aquifer();
         net.minecraft.world.level.levelgen.carver.CarvingContext context = new net.minecraft.world.level.levelgen.carver.CarvingContext(
            d.gen, Compat.registryAccess(), chunk.getHeightAccessorForGeneration(), nc, rs, s.surfaceRule());
         net.minecraft.world.level.chunk.CarvingMask mask = chunk.getOrCreateCarvingMask();
         for (int dx = -8; dx <= 8; dx++) {
            for (int dz = -8; dz <= 8; dz++) {
               ChunkPos sourcePos = new ChunkPos(pos.x() + dx, pos.z() + dz);
               Holder<Biome> biome = d.biomeSource.getNoiseBiome(net.minecraft.core.QuartPos.fromBlock(sourcePos.getMinBlockX()), 0, net.minecraft.core.QuartPos.fromBlock(sourcePos.getMinBlockZ()), rs.sampler());
               Iterable<Holder<net.minecraft.world.level.levelgen.carver.ConfiguredWorldCarver<?>>> carvers = biome.value().getGenerationSettings().getCarvers();
               int index = 0;
               for (Holder<net.minecraft.world.level.levelgen.carver.ConfiguredWorldCarver<?>> carverHolder : carvers) {
                  net.minecraft.world.level.levelgen.carver.ConfiguredWorldCarver<?> carver = carverHolder.value();
                  random.setLargeFeatureSeed(d.seed + index, sourcePos.x(), sourcePos.z());
                  if (carver.isStartChunk(random)) {
                     carver.carve(context, chunk, bm2::getBiome, random, aquifer, sourcePos, mask);
                  }
                  index++;
               }
            }
         }
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

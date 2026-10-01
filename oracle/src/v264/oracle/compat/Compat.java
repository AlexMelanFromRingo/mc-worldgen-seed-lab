package oracle.compat;

import java.util.List;
import java.util.stream.Stream;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.LayeredRegistryAccess;
import net.minecraft.core.QuartPos;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.RegistryDataLoader;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackResources;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.server.packs.resources.ResourceManager;
import net.minecraft.tags.TagLoader;
import java.util.function.Predicate;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.levelgen.structure.Structure;
import net.minecraft.world.level.levelgen.structure.StructureStart;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureTemplateManager;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.NoiseBiomeResolver;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseSpawnFinder;
import net.minecraft.world.level.levelgen.Noises;
import net.minecraft.world.level.levelgen.SpawnTargetPoint;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.densityfunction.DensityBuffer;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.levelgen.synth.Noise;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import oracle.ClimateApi;
import oracle.DfEval;
import oracle.GridOut;
import oracle.NoiseEval;

/** Слой совместимости для 26.3 (новая система density-функций: levelgen/densityfunction/*, float-накопление). */
public final class Compat {
   public static final String FAMILY = "26.3";

   private static RegistryAccess.Frozen registryAccess;
   private static ResourceManager resourceManager;

   private Compat() {
   }

   /** Полный RegistryAccess (STATIC + мир + измерения) — как у сервера. */
   public static RegistryAccess.Frozen registryAccess() {
      return registryAccess;
   }

   public static ResourceManager resourceManager() {
      return resourceManager;
   }

   public static void bootstrap() {
      SharedConstants.tryDetectVersion();
      Bootstrap.bootStrap();
   }

   public static HolderLookup.Provider loadRegistries() {
      PackRepository repo = ServerPacksSource.createVanillaTrustedRepository();
      repo.reload();
      repo.setSelected(List.of("vanilla"));
      List<PackResources> packs = repo.openAllSelected();
      MultiPackResourceManager rm = new MultiPackResourceManager(PackType.SERVER_DATA, packs);
      LayeredRegistryAccess<RegistryLayer> layers = RegistryLayer.createRegistryAccess();
      List<Registry.PendingTags<?>> staticTags = TagLoader.loadTagsForExistingRegistries(rm, layers.getLayer(RegistryLayer.STATIC));
      RegistryAccess.Frozen worldLoadContext = layers.getAccessForLoading(RegistryLayer.WORLD);
      List<HolderLookup.RegistryLookup<?>> worldCtx = TagLoader.buildUpdatedLookups(worldLoadContext, staticTags);
      RegistryAccess.Frozen loadedWorld = RegistryDataLoader.load(rm, worldCtx, RegistryDataLoader.WORLD_REGISTRIES, Runnable::run).join();
      List<HolderLookup.RegistryLookup<?>> dimCtx = Stream.concat(worldCtx.stream(), loadedWorld.listRegistries()).toList();
      RegistryAccess.Frozen dims = RegistryDataLoader.load(rm, dimCtx, RegistryDataLoader.DIMENSION_REGISTRIES, Runnable::run).join();
      resourceManager = rm;
      registryAccess = layers.replaceFrom(RegistryLayer.WORLD, loadedWorld, dims).compositeAccess();
      return HolderLookup.Provider.create(Stream.concat(dimCtx.stream(), dims.listRegistries()));
   }

   /** Точно как ChunkMap: RandomState.create(noises, seed, settings). */
   public static RandomState newRandomState(HolderLookup.Provider reg, NoiseGeneratorSettings settings, long seed) {
      return RandomState.create(reg.lookupOrThrow(Registries.NOISE), seed, settings);
   }

   // ------------------------------------------------------------------ climate / biome

   public static ClimateApi climate(BiomeSource bs, RandomState rs, boolean chunkMode, boolean fullColumn, int minY, int height) {
      return new Clim(bs, rs, chunkMode, fullColumn, minY, height);
   }

   private static final class Clim implements ClimateApi {
      final BiomeSource bs;
      final RandomState rs;
      final boolean chunk;
      final boolean full;
      final int minY;
      final int height;
      final Climate.Sampler sampler;
      final NoiseBiomeResolver resolver;

      Clim(BiomeSource bs, RandomState rs, boolean chunk, boolean full, int minY, int height) {
         this.bs = bs;
         this.rs = rs;
         this.chunk = chunk;
         this.full = full;
         this.minY = minY;
         this.height = height;
         this.sampler = rs.createClimateSampler(SamplerContext.EMPTY_UNCACHED);
         this.resolver = bs.createResolver(this.sampler);
      }

      @Override
      public Climate.TargetPoint target(int qx, int qy, int qz) {
         return sampler.sample(qx, qy, qz);
      }

      @Override
      public void raw(int qx, int qy, int qz, double[] o) {
         int x = QuartPos.toBlock(qx);
         int y = QuartPos.toBlock(qy);
         int z = QuartPos.toBlock(qz);
         o[0] = sampler.temperature().sampleValue(x, y, z);
         o[1] = sampler.humidity().sampleValue(x, y, z);
         o[2] = sampler.continentalness().sampleValue(x, y, z);
         o[3] = sampler.erosion().sampleValue(x, y, z);
         o[4] = sampler.depth().sampleValue(x, y, z);
         o[5] = sampler.weirdness().sampleValue(x, y, z);
      }

      @Override
      public double erosionAtBlock(int x, int y, int z) {
         return sampler.erosion().sampleValue(x, y, z);
      }

      @Override
      public Holder<Biome> biome(int qx, int qy, int qz) {
         return resolver.getNoiseBiome(qx, qy, qz);
      }

      @Override
      public boolean floatNative() {
         return true;
      }

      @Override
      public String mode() {
         return chunk ? "chunk" : "point";
      }

      @Override
      public void grid(int qx0, int qz0, int nx, int nz, int qy, GridOut out) {
         if (!chunk) {
            ClimateApi.super.grid(qx0, qz0, nx, nz, qy, out);
            return;
         }
         // Как ChunkGenerator.doCreateBiomes: на каждый столбец чанка (4x4 кварта) свой Climate.Sampler с кэшами,
         // resolver = BiomeSource.createResolverForChunk(...), значения берутся через sampleVolume (пакетный путь).
         int qMinY = full ? QuartPos.fromBlock(minY) : qy;
         int qSizeY = full ? QuartPos.fromBlock(height) : 1;
         int tx0 = Math.floorDiv(qx0, 4);
         int tx1 = Math.floorDiv(qx0 + nx - 1, 4);
         int tz0 = Math.floorDiv(qz0, 4);
         int tz1 = Math.floorDiv(qz0 + nz - 1, 4);
         boolean needBuf = out.q != null || out.raw != null;
         for (int tz = tz0; tz <= tz1; tz++) {
            for (int tx = tx0; tx <= tx1; tx++) {
               int mqx = tx * 4;
               int mqz = tz * 4;
               Climate.Sampler cs = rs.createClimateSampler(SamplerContext.builder().enableCaches().build());
               NoiseBiomeResolver res = out.biome != null ? bs.createResolverForChunk(cs, mqx, qMinY, mqz, 4, qSizeY, 4) : null;
               DensityVolume vol = new DensityVolume(4, qSizeY, 4, QuartPos.toBlock(mqx), QuartPos.toBlock(qMinY), QuartPos.toBlock(mqz), 4, 4, 4);
               DensityBuffer[] b = null;
               if (needBuf) {
                  b = new DensityBuffer[6];
                  for (int k = 0; k < 6; k++) b[k] = DensityBuffer.createUnpooled(vol.size());
                  cs.temperature().sampleVolume(b[0], vol);
                  cs.humidity().sampleVolume(b[1], vol);
                  cs.continentalness().sampleVolume(b[2], vol);
                  cs.erosion().sampleVolume(b[3], vol);
                  cs.depth().sampleVolume(b[4], vol);
                  cs.weirdness().sampleVolume(b[5], vol);
               }
               for (int qz = Math.max(mqz, qz0); qz <= Math.min(mqz + 3, qz0 + nz - 1); qz++) {
                  for (int qx = Math.max(mqx, qx0); qx <= Math.min(mqx + 3, qx0 + nx - 1); qx++) {
                     int i = (qz - qz0) * nx + (qx - qx0);
                     if (needBuf) {
                        int idx = vol.indexUnchecked(qx - mqx, qy - qMinY, qz - mqz);
                        if (out.raw != null) {
                           for (int k = 0; k < 6; k++) out.raw[k][i] = b[k].get(idx);
                        }
                        if (out.q != null) {
                           Climate.TargetPoint t = Climate.target(b[0].get(idx), b[1].get(idx), b[2].get(idx), b[3].get(idx), b[4].get(idx), b[5].get(idx));
                           out.q[0][i] = t.temperature();
                           out.q[1][i] = t.humidity();
                           out.q[2][i] = t.continentalness();
                           out.q[3][i] = t.erosion();
                           out.q[4][i] = t.depth();
                           out.q[5][i] = t.weirdness();
                        }
                     }
                     if (out.biome != null) out.biome[i] = res.getNoiseBiome(qx, qy, qz);
                  }
               }
            }
         }
      }
   }

   /** BiomeManager с зумом (как в WorldGenRegion / ServerLevel): резолвер — «неcached» point-резолвер. */
   public static BiomeManager biomeManager(BiomeSource bs, RandomState rs, long obfuscatedSeed) {
      return new BiomeManager(bs.createUncachedResolver(rs), obfuscatedSeed);
   }

   // ------------------------------------------------------------------ noise / df

   public static NoiseEval noise(HolderLookup.Provider reg, RandomState rs, long seed, Identifier id) {
      ResourceKey<NormalNoise> key = ResourceKey.create(Registries.NOISE, id);
      Holder<NormalNoise> holder = reg.lookupOrThrow(Registries.NOISE).getOrThrow(key);
      Noise n;
      if (holder.is(Noises.TEMPERATURE_NETHER)) {
         n = holder.value().createForLegacyNetherBiome(new LegacyRandomSource(seed + 0L));
      } else if (holder.is(Noises.VEGETATION_NETHER)) {
         n = holder.value().createForLegacyNetherBiome(new LegacyRandomSource(seed + 1L));
      } else {
         n = rs.getOrCreateNoise(key);
      }
      final Noise fn = n;
      return new NoiseEval() {
         @Override
         public double get3(double x, double y, double z) {
            return fn.get(x, y, z);
         }

         @Override
         public double get2(double x, double y) {
            return fn.get(x, y);
         }

         @Override
         public boolean floatNative() {
            return true;
         }
      };
   }

   public static DfEval df(HolderLookup.Provider reg, NoiseGeneratorSettings settings, RandomState rs, long seed, Identifier id) {
      Holder<DensityFunction> holder = reg.lookupOrThrow(Registries.DENSITY_FUNCTION).getOrThrow(ResourceKey.create(Registries.DENSITY_FUNCTION, id));
      final DensityFunction fn = holder.value();
      return new DfEval() {
         @Override
         public double at(int x, int y, int z) {
            return rs.sampleBlockValueUncached(fn, x, y, z);
         }

         @Override
         public boolean floatNative() {
            return true;
         }
      };
   }

   // ------------------------------------------------------------------ spawn

   /** Климатическая точка мирового спавна (блоки x,z) — как ChunkGenerator.getOrigin / MinecraftServer.setInitialSpawn. */
   public static int[] spawnTarget(NoiseGeneratorSettings s, RandomState rs) {
      List<SpawnTargetPoint> t = s.spawnTarget();
      if (t.isEmpty()) return new int[]{0, 0};
      BlockPos p = NoiseSpawnFinder.findSpawnPosition(t, rs.samplersWithContext(SamplerContext.builder().enableCaches().build()));
      return new int[]{p.getX(), p.getZ()};
   }

   // ------------------------------------------------------------------ structures

   /** Structure.generate(...) — сигнатура различается между 26.1/26.2 и 26.3 (climateSampler). references=0. */
   public static StructureStart generateStructure(
      Holder<Structure> selected, ResourceKey<Level> dimension, ChunkGenerator gen, BiomeSource bs, RandomState rs, StructureTemplateManager tm,
      long seed, ChunkPos pos, LevelHeightAccessor ha, Predicate<Holder<Biome>> valid
   ) {
      return selected.value().generate(selected, dimension, registryAccess, gen, bs, rs.createClimateSampler(SamplerContext.builder().enableCaches().build()), rs, tm, seed, pos, 0, ha, valid);
   }

   /** 26.4-snapshot-2: NoiseBasedChunkGenerator.getBaseHeight убран/переименован — команды height не поддержаны. */
   public static int baseHeightUnsupported(Object... args) {
      throw new UnsupportedOperationException("height-команды не поддержаны для 26.4 (изменился API NoiseBasedChunkGenerator)");
   }
}

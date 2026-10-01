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
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.levelgen.DensityFunction;
import net.minecraft.world.level.levelgen.DensityFunctions;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseRouter;
import net.minecraft.world.level.levelgen.Noises;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import oracle.ClimateApi;
import oracle.DfEval;
import oracle.NoiseEval;

/** Слой совместимости для 26.1 и 26.2 (старая система density-функций: levelgen/DensityFunction(s), double). */
public final class Compat {
   public static final String FAMILY = "26.1/26.2";

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
      RegistryAccess.Frozen worldLoadContext = layers.getAccessForLoading(RegistryLayer.WORLDGEN);
      List<HolderLookup.RegistryLookup<?>> worldCtx = TagLoader.buildUpdatedLookups(worldLoadContext, staticTags);
      RegistryAccess.Frozen loadedWorld = RegistryDataLoader.load(rm, worldCtx, RegistryDataLoader.WORLDGEN_REGISTRIES, Runnable::run).join();
      List<HolderLookup.RegistryLookup<?>> dimCtx = Stream.concat(worldCtx.stream(), loadedWorld.listRegistries()).toList();
      RegistryAccess.Frozen dims = RegistryDataLoader.load(rm, dimCtx, RegistryDataLoader.DIMENSION_REGISTRIES, Runnable::run).join();
      resourceManager = rm;
      registryAccess = layers.replaceFrom(RegistryLayer.WORLDGEN, loadedWorld, dims).compositeAccess();
      return HolderLookup.Provider.create(Stream.concat(dimCtx.stream(), dims.listRegistries()));
   }

   /** Точно как ChunkMap: RandomState.create(settings, noises, seed). */
   public static RandomState newRandomState(HolderLookup.Provider reg, NoiseGeneratorSettings settings, long seed) {
      return RandomState.create(settings, reg.lookupOrThrow(Registries.NOISE), seed);
   }

   // ------------------------------------------------------------------ climate / biome

   public static ClimateApi climate(BiomeSource bs, RandomState rs, boolean chunkMode, boolean fullColumn, int minY, int height) {
      return new Clim(bs, rs);
   }

   private static final class Clim implements ClimateApi {
      final BiomeSource bs;
      final Climate.Sampler sampler;

      Clim(BiomeSource bs, RandomState rs) {
         this.bs = bs;
         this.sampler = rs.sampler();
      }

      @Override
      public Climate.TargetPoint target(int qx, int qy, int qz) {
         return sampler.sample(qx, qy, qz);
      }

      @Override
      public void raw(int qx, int qy, int qz, double[] o) {
         DensityFunction.SinglePointContext c = new DensityFunction.SinglePointContext(QuartPos.toBlock(qx), QuartPos.toBlock(qy), QuartPos.toBlock(qz));
         o[0] = sampler.temperature().compute(c);
         o[1] = sampler.humidity().compute(c);
         o[2] = sampler.continentalness().compute(c);
         o[3] = sampler.erosion().compute(c);
         o[4] = sampler.depth().compute(c);
         o[5] = sampler.weirdness().compute(c);
      }

      @Override
      public double erosionAtBlock(int x, int y, int z) {
         return sampler.erosion().compute(new DensityFunction.SinglePointContext(x, y, z));
      }

      @Override
      public Holder<Biome> biome(int qx, int qy, int qz) {
         return bs.getNoiseBiome(qx, qy, qz, sampler);
      }

      @Override
      public boolean floatNative() {
         return false;
      }

      @Override
      public String mode() {
         return "point";
      }
   }

   /** BiomeManager с зумом (как ServerLevel.getUncachedNoiseBiome). */
   public static BiomeManager biomeManager(BiomeSource bs, RandomState rs, long obfuscatedSeed) {
      Climate.Sampler sampler = rs.sampler();
      return new BiomeManager((qx, qy, qz) -> bs.getNoiseBiome(qx, qy, qz, sampler), obfuscatedSeed);
   }

   // ------------------------------------------------------------------ noise / df

   public static NoiseEval noise(HolderLookup.Provider reg, RandomState rs, long seed, Identifier id) {
      ResourceKey<NormalNoise.NoiseParameters> key = ResourceKey.create(Registries.NOISE, id);
      Holder<NormalNoise.NoiseParameters> holder = reg.lookupOrThrow(Registries.NOISE).getOrThrow(key);
      NormalNoise n;
      if (holder.is(Noises.TEMPERATURE_NETHER)) {
         n = NormalNoise.createLegacyNetherBiome(new LegacyRandomSource(seed + 0L), holder.value());
      } else if (holder.is(Noises.VEGETATION_NETHER)) {
         n = NormalNoise.createLegacyNetherBiome(new LegacyRandomSource(seed + 1L), holder.value());
      } else {
         n = rs.getOrCreateNoise(key);
      }
      final NormalNoise fn = n;
      return new NoiseEval() {
         @Override
         public double get3(double x, double y, double z) {
            return fn.getValue(x, y, z);
         }

         @Override
         public double get2(double x, double y) {
            return Double.NaN;
         }

         @Override
         public boolean floatNative() {
            return false;
         }
      };
   }

   /**
    * В 26.1/26.2 «проводка» density-функций (NoiseWiringHelper) — локальный класс конструктора RandomState, поэтому
    * функцию подставляем в ВСЕ поля NoiseRouter копии NoiseGeneratorSettings и строим настоящий RandomState:
    * RandomState.sampler().temperature() — это ровно та же функция после реальной проводки и «flatten».
    */
   public static DfEval df(HolderLookup.Provider reg, NoiseGeneratorSettings s, RandomState unused, long seed, Identifier id) {
      Holder<DensityFunction> holder = reg.lookupOrThrow(Registries.DENSITY_FUNCTION).getOrThrow(ResourceKey.create(Registries.DENSITY_FUNCTION, id));
      DensityFunction f = new DensityFunctions.HolderHolder(holder);
      NoiseRouter r = new NoiseRouter(f, f, f, f, f, f, f, f, f, f, f, f, f, f, f);
      NoiseGeneratorSettings s2 = new NoiseGeneratorSettings(
         s.noiseSettings(), s.defaultBlock(), s.defaultFluid(), r, s.surfaceRule(), s.spawnTarget(), s.seaLevel(), s.disableMobGeneration(),
         s.isAquifersEnabled(), s.oreVeinsEnabled(), s.useLegacyRandomSource()
      );
      RandomState rs2 = RandomState.create(s2, reg.lookupOrThrow(Registries.NOISE), seed);
      final DensityFunction wired = rs2.sampler().temperature();
      return new DfEval() {
         @Override
         public double at(int x, int y, int z) {
            return wired.compute(new DensityFunction.SinglePointContext(x, y, z));
         }

         @Override
         public boolean floatNative() {
            return false;
         }
      };
   }

   // ------------------------------------------------------------------ spawn

   /** Климатическая точка мирового спавна (блоки x,z) — как MinecraftServer.setInitialSpawn: randomState.sampler().findSpawnPosition(). */
   public static int[] spawnTarget(NoiseGeneratorSettings s, RandomState rs) {
      BlockPos p = rs.sampler().findSpawnPosition();
      return new int[]{p.getX(), p.getZ()};
   }

   // ------------------------------------------------------------------ structures

   /** Structure.generate(...) — сигнатура различается между 26.1/26.2 и 26.3 (climateSampler). references=0. */
   public static StructureStart generateStructure(
      Holder<Structure> selected, ResourceKey<Level> dimension, ChunkGenerator gen, BiomeSource bs, RandomState rs, StructureTemplateManager tm,
      long seed, ChunkPos pos, LevelHeightAccessor ha, Predicate<Holder<Biome>> valid
   ) {
      return selected.value().generate(selected, dimension, registryAccess, gen, bs, rs, tm, seed, pos, 0, ha, valid);
   }
}

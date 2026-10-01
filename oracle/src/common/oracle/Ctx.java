package oracle;

import java.util.LinkedHashMap;
import java.util.Map;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.dimension.LevelStem;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.WorldDimensions;
import net.minecraft.world.level.levelgen.presets.WorldPreset;
import oracle.compat.Compat;

/** Глобальное состояние: загруженные реестры + кэш «измерений» (dim, preset, seed). */
public final class Ctx {
   public static HolderLookup.Provider reg;

   private Ctx() {
   }

   public static synchronized void init() {
      if (reg == null) {
         Compat.bootstrap();
         reg = Compat.loadRegistries();
      }
   }

   public static String normDim(String d) {
      return switch (d) {
         case "overworld", "ow", "minecraft:overworld" -> "overworld";
         case "nether", "the_nether", "minecraft:the_nether" -> "the_nether";
         case "end", "the_end", "minecraft:the_end" -> "the_end";
         default -> throw new IllegalArgumentException("Неизвестное измерение: " + d + " (overworld|nether|end)");
      };
   }

   public static boolean isDimName(String d) {
      return switch (d) {
         case "overworld", "ow", "minecraft:overworld", "nether", "the_nether", "minecraft:the_nether", "end", "the_end", "minecraft:the_end" -> true;
         default -> false;
      };
   }

   public static ResourceKey<LevelStem> stemKey(String dim) {
      return switch (dim) {
         case "overworld" -> LevelStem.OVERWORLD;
         case "the_nether" -> LevelStem.NETHER;
         default -> LevelStem.END;
      };
   }

   public static LevelStem stem(String dim, String preset) {
      ResourceKey<WorldPreset> pk = ResourceKey.create(Registries.WORLD_PRESET, Identifier.withDefaultNamespace(preset));
      WorldPreset wp = reg.lookupOrThrow(Registries.WORLD_PRESET).getOrThrow(pk).value();
      WorldDimensions wd = wp.createWorldDimensions();
      LevelStem s = wd.dimensions().get(stemKey(dim));
      if (s == null) throw new IllegalArgumentException("В пресете " + preset + " нет измерения " + dim);
      return s;
   }

   public static final class Dim {
      public final String name;
      public final String preset;
      public final long seed;
      public final LevelStem stem;
      public final NoiseBasedChunkGenerator gen;
      public final Holder<NoiseGeneratorSettings> settingsHolder;
      public final NoiseGeneratorSettings settings;
      public final BiomeSource biomeSource;
      public final RandomState rs;
      public final int minY;
      public final int height;
      private final Map<String, ClimateApi> climCache = new LinkedHashMap<>();
      private final Map<String, DfEval> dfCache = new LinkedHashMap<>();
      private final Map<String, NoiseEval> noiseCache = new LinkedHashMap<>();
      public Object structState; // ChunkGeneratorStructureState (лениво)

      Dim(String name, String preset, long seed) {
         this.name = name;
         this.preset = preset;
         this.seed = seed;
         this.stem = stem(name, preset);
         ChunkGenerator g = stem.generator();
         if (!(g instanceof NoiseBasedChunkGenerator ng)) {
            throw new IllegalArgumentException("Пресет " + preset + " / " + name + ": генератор " + g.getClass().getSimpleName() + " не noise-based");
         }
         this.gen = ng;
         this.settingsHolder = ng.generatorSettings();
         this.settings = settingsHolder.value();
         this.biomeSource = g.getBiomeSource();
         this.minY = stem.type().value().minY();
         this.height = stem.type().value().height();
         this.rs = Compat.newRandomState(reg, settings, seed);
      }

      public boolean isEnd() {
         return name.equals("the_end");
      }

      public synchronized ClimateApi clim(String mode, boolean fullColumn) {
         String key = mode + "/" + fullColumn;
         return climCache.computeIfAbsent(key, k -> Compat.climate(biomeSource, rs, mode.equals("chunk"), fullColumn, minY, height));
      }

      public synchronized DfEval df(Identifier id) {
         return dfCache.computeIfAbsent(id.toString(), k -> Compat.df(reg, settings, rs, seed, id));
      }

      public synchronized NoiseEval noise(Identifier id) {
         return noiseCache.computeIfAbsent(id.toString(), k -> Compat.noise(reg, rs, seed, id));
      }
   }

   private static final int MAX_CACHE = 6;
   private static final Map<String, Dim> DIMS = new LinkedHashMap<>();

   public static synchronized Dim dim(String dimName, String preset, long seed) {
      String d = normDim(dimName);
      String key = d + "/" + preset + "/" + seed;
      Dim dim = DIMS.get(key);
      if (dim == null) {
         dim = new Dim(d, preset, seed);
         DIMS.put(key, dim);
         if (DIMS.size() > MAX_CACHE) {
            String oldest = DIMS.keySet().iterator().next();
            DIMS.remove(oldest);
         }
      } else {
         // LRU: перенести в конец
         DIMS.remove(key);
         DIMS.put(key, dim);
      }
      return dim;
   }
}

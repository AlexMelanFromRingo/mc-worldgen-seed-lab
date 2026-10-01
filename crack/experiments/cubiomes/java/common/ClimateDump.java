package cbx;

import java.io.*;
import java.util.*;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.dimension.LevelStem;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;

/**
 * Дамп реальных климатических параметров и биомов из игры (для сверки с cubiomes).
 * Формат строки: seed qx qy qz temperature humidity continentalness erosion depth weirdness biome
 * Аргументы: <out-file> <settings: overworld|large_biomes|amplified|nether|end> <seedsCsv|@file> <pointsPerSeed> [rngSeed]
 */
public class ClimateDump {
   public static void main(String[] a) throws Exception {
      String outFile = a[0];
      String dimName = a[1];
      long[] seeds = parseSeeds(a[2]);
      int npts = Integer.parseInt(a[3]);
      long prng = a.length > 4 ? Long.parseLong(a[4]) : 1234567L;
      CbxCompat.bootstrap();
      HolderLookup.Provider p = CbxCompat.loadRegistries();
      ResourceKey<NoiseGeneratorSettings> nsKey = switch (dimName) {
         case "overworld" -> NoiseGeneratorSettings.OVERWORLD;
         case "large_biomes" -> NoiseGeneratorSettings.LARGE_BIOMES;
         case "amplified" -> NoiseGeneratorSettings.AMPLIFIED;
         case "nether" -> NoiseGeneratorSettings.NETHER;
         default -> throw new IllegalArgumentException(dimName);
      };
      ResourceKey<LevelStem> stemKey = dimName.equals("nether") ? LevelStem.NETHER : LevelStem.OVERWORLD;
      var presetKey = dimName.equals("large_biomes") ? net.minecraft.world.level.levelgen.presets.WorldPresets.LARGE_BIOMES
         : net.minecraft.world.level.levelgen.presets.WorldPresets.NORMAL;
      BiomeSource bs = p.lookupOrThrow(Registries.WORLD_PRESET).getOrThrow(presetKey).value()
         .createWorldDimensions().dimensions().get(stemKey).generator().getBiomeSource();
      MultiNoiseBiomeSource mnbs = (MultiNoiseBiomeSource) bs;
      try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(outFile)))) {
         w.println("# version=" + CbxCompat.VERSION + " dim=" + dimName);
         for (long seed : seeds) {
            Climate.Sampler s = CbxCompat.climateSampler(p, nsKey, seed);
            long st = prng ^ (seed * 0x9E3779B97F4A7C15L);
            for (int i = 0; i < npts; i++) {
               st = xs(st); int qx = (int) Math.floorMod(st >>> 8, 5001L) - 2500;
               st = xs(st); int qz = (int) Math.floorMod(st >>> 8, 5001L) - 2500;
               st = xs(st); int qy = dimName.equals("nether") ? (int) Math.floorMod(st >>> 8, 32L) : (int) Math.floorMod(st >>> 8, 96L) - 16;
               if (i % 8 == 0) { qx /= 40; qz /= 40; }  // немного точек вблизи начала координат
               Climate.TargetPoint t = s.sample(qx, qy, qz);
               Holder<Biome> b = mnbs.getNoiseBiome(t);
               w.println(seed + " " + qx + " " + qy + " " + qz + " " + t.temperature() + " " + t.humidity() + " "
                  + t.continentalness() + " " + t.erosion() + " " + t.depth() + " " + t.weirdness() + " "
                  + b.unwrapKey().get().identifier().getPath());
            }
         }
      }
      System.out.println("wrote " + outFile);
   }

   static long xs(long x) { x ^= x << 13; x ^= x >>> 7; x ^= x << 17; return x; }

   static long[] parseSeeds(String s) throws IOException {
      if (s.startsWith("@")) {
         List<Long> l = new ArrayList<>();
         for (String line : java.nio.file.Files.readAllLines(java.nio.file.Path.of(s.substring(1)))) { line = line.trim(); if (!line.isEmpty()) l.add(Long.parseLong(line)); }
         return l.stream().mapToLong(Long::longValue).toArray();
      }
      return Arrays.stream(s.split(",")).mapToLong(Long::parseLong).toArray();
   }
}

package oracle;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Predicate;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderSet;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.ResourceKey;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.chunk.ChunkGeneratorStructureState;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.Structure;
import net.minecraft.world.level.levelgen.structure.StructurePiece;
import net.minecraft.world.level.levelgen.structure.StructureSet;
import net.minecraft.world.level.levelgen.structure.StructureStart;
import net.minecraft.world.level.levelgen.structure.placement.StructurePlacement;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureTemplateManager;
import net.minecraft.world.level.storage.LevelStorageSource;
import oracle.compat.Compat;

/**
 * structstart <dim> <seed> <cx> <cz> [set_id|all] — «что реально сгенерировалось бы» в стартовом чанке (cx,cz):
 * реальные StructurePlacement.isStructureChunk + Structure.generate (findGenerationPoint + проверка биома старта) с
 * настоящим StructureTemplateManager, ChunkGenerator (высоты) и BiomeSource.
 * Цикл выбора структуры из набора (вес, повтор при неудаче) — копия glue-кода ChunkGenerator.createStructures/tryGenerateStructure.
 */
final class CmdStructStart {
   private CmdStructStart() {
   }

   private static StructureTemplateManager TM;

   static synchronized StructureTemplateManager templateManager() throws Exception {
      if (TM == null) {
         Path tmp = Files.createTempDirectory("oracle-level");
         tmp.toFile().deleteOnExit();
         LevelStorageSource.LevelStorageAccess access = LevelStorageSource.createDefault(tmp).createAccess("oracle");
         TM = new StructureTemplateManager(Compat.resourceManager(), access, DataFixers.getDataFixer(), BuiltInRegistries.BLOCK);
      }
      return TM;
   }

   static ResourceKey<Level> levelKey(String dim) {
      return switch (dim) {
         case "overworld" -> Level.OVERWORLD;
         case "the_nether" -> Level.NETHER;
         default -> Level.END;
      };
   }

   static String structstart(Args a) throws Exception {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int cx = a.i(2);
      int cz = a.i(3);
      String setId = a.size() > 4 ? a.str(4) : "all";
      String want = setId.equals("all") ? null : Util.id(setId).toString();
      ChunkGeneratorStructureState st = CmdMore.state(d);
      StructureTemplateManager tm = templateManager();
      LevelHeightAccessor ha = LevelHeightAccessor.create(d.minY, d.height);
      ChunkPos pos = new ChunkPos(cx, cz);
      J j = J.obj().b("ok", true).s("cmd", "structstart").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset).l("seed", d.seed)
         .la("chunk", new long[]{cx, cz});
      j.a("results");
      for (Holder<StructureSet> h : st.possibleStructureSets()) {
         String id = CmdMore.name(h);
         if (want != null && !id.equals(want)) continue;
         StructurePlacement p = h.value().placement();
         if (!p.isStructureChunk(st, cx, cz)) continue;
         List<StructureSet.StructureSelectionEntry> structures = h.value().structures();
         j.eo().s("set", id);
         j.a("attempts");
         StructureStart chosen = null;
         if (structures.size() == 1) {
            chosen = attempt(j, structures.get(0), d, tm, ha, pos);
         } else {
            List<StructureSet.StructureSelectionEntry> options = new ArrayList<>(structures);
            WorldgenRandom random = new WorldgenRandom(new LegacyRandomSource(0L));
            random.setLargeFeatureSeed(st.getLevelSeed(), cx, cz);
            int total = 0;
            for (var o : options) total += o.weight();
            while (!options.isEmpty()) {
               int choice = random.nextInt(total);
               int index = 0;
               for (var o : options) {
                  choice -= o.weight();
                  if (choice < 0) break;
                  index++;
               }
               StructureSet.StructureSelectionEntry sel = options.get(index);
               chosen = attempt(j, sel, d, tm, ha, pos);
               if (chosen != null) break;
               options.remove(index);
               total -= sel.weight();
            }
         }
         j.end();
         j.b("generated", chosen != null);
         j.end();
      }
      j.end();
      return j.done();
   }

   /** Одна попытка Structure.generate; пишет элемент в attempts, возвращает валидный старт или null. */
   private static StructureStart attempt(J j, StructureSet.StructureSelectionEntry sel, Ctx.Dim d, StructureTemplateManager tm, LevelHeightAccessor ha, ChunkPos pos) {
      Structure structure = sel.structure().value();
      HolderSet<Biome> allowed = structure.biomes();
      Predicate<Holder<Biome>> valid = allowed::contains;
      long t0 = System.nanoTime();
      StructureStart start = Compat.generateStructure(sel.structure(), levelKey(d.name), d.gen, d.biomeSource, d.rs, tm, d.seed, pos, ha, valid);
      double ms = (System.nanoTime() - t0) / 1e6;
      j.eo().s("structure", CmdMore.name(sel.structure())).l("weight", sel.weight()).b("valid", start.isValid()).d("ms", ms);
      if (start.isValid()) {
         BoundingBox bb = start.getBoundingBox();
         j.la("bbox", new long[]{bb.minX(), bb.minY(), bb.minZ(), bb.maxX(), bb.maxY(), bb.maxZ()});
         j.a("pieces");
         int n = 0;
         for (StructurePiece pc : start.getPieces()) {
            BoundingBox b = pc.getBoundingBox();
            if (n++ < 400) {
               j.eo().s("type", BuiltInRegistries.STRUCTURE_PIECE.getKey(pc.getType()).toString())
                  .la("bbox", new long[]{b.minX(), b.minY(), b.minZ(), b.maxX(), b.maxY(), b.maxZ()}).end();
            }
         }
         j.end().l("piece_count", n);
      }
      j.end();
      return start.isValid() ? start : null;
   }
}

package oracle;

import com.google.gson.JsonElement;
import com.mojang.serialization.JsonOps;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.RegistryOps;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.WorldGenLevel;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.chunk.ChunkGeneratorStructureState;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.feature.EndSpikeFeature;
import net.minecraft.world.level.levelgen.structure.StructureSet;
import net.minecraft.world.level.levelgen.structure.placement.ConcentricRingsStructurePlacement;
import net.minecraft.world.level.levelgen.structure.placement.StructurePlacement;
import oracle.compat.Compat;

/** P1: structs, structsets, stronghold, pillars, slime, hashed, blockbiome, height, heightgrid, defaultspawn. */
final class CmdMore {
   private CmdMore() {
   }

   static String dispatch(String cmd, Args a) throws Exception {
      return switch (cmd) {
         case "structs" -> structs(a);
         case "structsets" -> structsets(a);
         case "stronghold" -> stronghold(a);
         case "pillars" -> pillars(a);
         case "slime" -> slime(a);
         case "hashed" -> hashed(a);
         case "blockbiome" -> blockbiome(a);
         case "height" -> height(a);
         case "heightgrid" -> heightgrid(a);
         case "defaultspawn" -> defaultspawn(a);
         case "rtree" -> CmdRTree.rtree(a);
         case "structstart" -> CmdStructStart.structstart(a);
         default -> throw new IllegalArgumentException("Неизвестная команда: " + cmd);
      };
   }

   // ------------------------------------------------------------------ structures

   static ChunkGeneratorStructureState state(Ctx.Dim d) {
      if (d.structState == null) {
         // Точно как ChunkMap: generator.createState(structure_set registry, randomState, levelSeed)
         d.structState = d.gen.createState(Ctx.reg.lookupOrThrow(Registries.STRUCTURE_SET), d.rs, d.seed);
      }
      return (ChunkGeneratorStructureState) d.structState;
   }

   static String name(Holder<?> h) {
      return h.unwrapKey().map(k -> k.identifier().toString()).orElse("?direct");
   }

   static String structs(Args a) {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      String setId = a.str(2);
      int cx0 = a.i(3);
      int cz0 = a.i(4);
      int nx = a.i(5);
      int nz = a.i(6);
      boolean all = setId.equals("all");
      String want = all ? null : Util.id(setId).toString();
      ChunkGeneratorStructureState st = state(d);
      J j = J.obj().b("ok", true).s("cmd", "structs").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset).l("seed", d.seed)
         .l("cx0", cx0).l("cz0", cz0).l("nx", nx).l("nz", nz);
      j.o("sets");
      int found = 0;
      for (Holder<StructureSet> h : st.possibleStructureSets()) {
         String id = name(h);
         if (!all && !id.equals(want)) continue;
         StructurePlacement p = h.value().placement();
         j.a(id);
         for (int cz = cz0; cz < cz0 + nz; cz++) {
            for (int cx = cx0; cx < cx0 + nx; cx++) {
               if (p.isStructureChunk(st, cx, cz)) {
                  j.ea().vl(cx).vl(cz).end();
                  found++;
               }
            }
         }
         j.end();
      }
      j.end().l("total", found);
      if (!all && found == 0) {
         boolean known = st.possibleStructureSets().stream().anyMatch(h -> name(h).equals(want));
         j.b("set_possible_in_dim", known);
      }
      return j.done();
   }

   static String structsets(Args a) {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      ChunkGeneratorStructureState st = state(d);
      RegistryOps<JsonElement> ops = RegistryOps.create(JsonOps.INSTANCE, Ctx.reg);
      J j = J.obj().b("ok", true).s("cmd", "structsets").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset);
      j.a("sets");
      for (Holder<StructureSet> h : st.possibleStructureSets()) {
         StructureSet s = h.value();
         j.eo().s("id", name(h));
         String pj;
         try {
            pj = StructurePlacement.CODEC.encodeStart(ops, s.placement()).getOrThrow().toString();
         } catch (Throwable t) {
            pj = J.q("encode-error: " + t);
         }
         j.raw("placement", pj);
         j.a("structures");
         for (StructureSet.StructureSelectionEntry e : s.structures()) {
            j.eo().s("structure", name(e.structure())).l("weight", e.weight()).end();
         }
         j.end().end();
      }
      j.end();
      return j.done();
   }

   static String stronghold(Args a) {
      Ctx.Dim d = Ctx.dim("overworld", a.get("preset", "normal"), a.seed(0));
      ChunkGeneratorStructureState st = state(d);
      J j = J.obj().b("ok", true).s("cmd", "stronghold").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset).l("seed", d.seed);
      for (Holder<StructureSet> h : st.possibleStructureSets()) {
         if (h.value().placement() instanceof ConcentricRingsStructurePlacement rp) {
            List<ChunkPos> pos = st.getRingPositionsFor(rp);
            j.s("set", name(h)).l("count", pos == null ? 0 : pos.size());
            j.o("params").l("distance", rp.distance()).l("spread", rp.spread()).l("count", rp.count()).end();
            j.a("chunks");
            if (pos != null) {
               for (ChunkPos p : pos) j.ea().vl(p.x()).vl(p.z()).end();
            }
            j.end();
            // блочные координаты «центра» стартовой позиции по ChunkPos (как getLocatePos для offset 0: x*16+8, z*16+8)
            j.a("locate_xz");
            if (pos != null) {
               for (ChunkPos p : pos) {
                  BlockPos lp = h.value().placement().getLocatePos(p);
                  j.ea().vl(lp.getX()).vl(lp.getZ()).end();
               }
            }
            j.end();
            break;
         }
      }
      return j.done();
   }

   // ------------------------------------------------------------------ misc

   static String pillars(Args a) {
      long seed = a.seed(0);
      WorldGenLevel level = (WorldGenLevel) Proxy.newProxyInstance(
         CmdMore.class.getClassLoader(), new Class<?>[]{WorldGenLevel.class},
         (proxy, m, args) -> {
            if (m.getName().equals("getSeed")) return seed;
            throw new UnsupportedOperationException(m.getName());
         }
      );
      List<EndSpikeFeature.EndSpike> spikes = EndSpikeFeature.getSpikesForLevel(level);
      long key = RandomSource.createThreadLocalInstance(seed).nextLong() & 65535L;
      J j = J.obj().b("ok", true).s("cmd", "pillars").s("version", Main.VERSION).l("seed", seed).l("cache_key", key);
      j.a("spikes");
      for (EndSpikeFeature.EndSpike s : spikes) {
         j.eo().l("centerX", s.getCenterX()).l("centerZ", s.getCenterZ()).l("radius", s.getRadius()).l("height", s.getHeight())
            .b("guarded", s.isGuarded());
         if (s.isGuarded()) {
            // клетка из железных прутьев: x,z в [center-2, center+2], y в [height, height+3] (EndSpikeFeature.placeSpike)
            j.la("cage_min", new long[]{s.getCenterX() - 2, s.getHeight(), s.getCenterZ() - 2})
               .la("cage_max", new long[]{s.getCenterX() + 2, s.getHeight() + 3, s.getCenterZ() + 2});
         }
         j.end();
      }
      j.end();
      return j.done();
   }

   static boolean isSlime(long seed, int cx, int cz) {
      return WorldgenRandom.seedSlimeChunk(cx, cz, seed, 987234911L).nextInt(10) == 0;
   }

   static String slime(Args a) {
      long seed = a.seed(0);
      int cx0 = a.i(1);
      int cz0 = a.i(2);
      int nx = a.i(3);
      int nz = a.i(4);
      List<String> rows = new ArrayList<>();
      long count = 0;
      for (int z = 0; z < nz; z++) {
         StringBuilder sb = new StringBuilder(nx);
         for (int x = 0; x < nx; x++) {
            boolean s = isSlime(seed, cx0 + x, cz0 + z);
            sb.append(s ? '1' : '0');
            if (s) count++;
         }
         rows.add(sb.toString());
      }
      return J.obj().b("ok", true).s("cmd", "slime").s("version", Main.VERSION).l("seed", seed).l("cx0", cx0).l("cz0", cz0).l("nx", nx).l("nz", nz)
         .l("count", count).s("layout", "rows[iz][ix] для чанка (cx0+ix, cz0+iz); '1' = слайм-чанк").sa("rows", rows).done();
   }

   static String hashed(Args a) {
      long seed = a.seed(0);
      long h = BiomeManager.obfuscateSeed(seed);
      return J.obj().b("ok", true).s("cmd", "hashed").s("version", Main.VERSION).l("seed", seed).l("hashed_seed", h).s("hashed_seed_hex", Util.hex16(h)).done();
   }

   static String blockbiome(Args a) {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int x = a.i(2);
      int y = a.i(3);
      int z = a.i(4);
      int nx = a.size() > 5 ? a.i(5) : 1;
      int nz = a.size() > 6 ? a.i(6) : 1;
      int step = a.size() > 7 ? a.i(7) : 1;
      BiomeManager bm = Compat.biomeManager(d.biomeSource, d.rs, BiomeManager.obfuscateSeed(d.seed));
      J j = J.obj().b("ok", true).s("cmd", "blockbiome").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset).l("seed", d.seed)
         .l("hashed_seed", BiomeManager.obfuscateSeed(d.seed)).l("x", x).l("y", y).l("z", z).l("nx", nx).l("nz", nz).l("step", step);
      if (nx == 1 && nz == 1) {
         Holder<Biome> b = bm.getBiome(new BlockPos(x, y, z));
         return j.s("biome", Util.biomeName(b)).done();
      }
      List<String> pal = new ArrayList<>();
      int[] idx = new int[nx * nz];
      for (int iz = 0; iz < nz; iz++) {
         for (int ix = 0; ix < nx; ix++) {
            String n = Util.biomeName(bm.getBiome(new BlockPos(x + ix * step, y, z + iz * step)));
            int p = pal.indexOf(n);
            if (p < 0) {
               p = pal.size();
               pal.add(n);
            }
            idx[iz * nx + ix] = p;
         }
      }
      return j.s("layout", "index = iz*nx + ix, блок (x+ix*step, y, z+iz*step)").sa("palette", pal).ia("idx", idx).done();
   }

   static Heightmap.Types hm(Args a, int pos) {
      String s = a.size() > pos ? a.str(pos) : "WORLD_SURFACE_WG";
      return Heightmap.Types.valueOf(s.toUpperCase());
   }

   static String height(Args a) {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int x = a.i(2);
      int z = a.i(3);
      Heightmap.Types t = hm(a, 4);
      LevelHeightAccessor ha = LevelHeightAccessor.create(d.minY, d.height);
      int base = d.gen.getBaseHeight(x, z, t, ha, d.rs);
      int free = d.gen.getFirstFreeHeight(x, z, t, ha, d.rs);
      int occ = d.gen.getFirstOccupiedHeight(x, z, t, ha, d.rs);
      return J.obj().b("ok", true).s("cmd", "height").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset).l("seed", d.seed)
         .l("x", x).l("z", z).s("heightmap", t.name()).l("base_height", base).l("first_free_height", free).l("first_occupied_height", occ)
         .l("min_y", d.minY).l("sea_level", d.gen.getSeaLevel()).done();
   }

   static String heightgrid(Args a) {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int x0 = a.i(2);
      int z0 = a.i(3);
      int nx = a.i(4);
      int nz = a.i(5);
      int step = a.i(6);
      Heightmap.Types t = hm(a, 7);
      LevelHeightAccessor ha = LevelHeightAccessor.create(d.minY, d.height);
      int[] h = new int[nx * nz];
      long t0 = System.nanoTime();
      for (int iz = 0; iz < nz; iz++) {
         for (int ix = 0; ix < nx; ix++) {
            h[iz * nx + ix] = d.gen.getBaseHeight(x0 + ix * step, z0 + iz * step, t, ha, d.rs);
         }
      }
      double ms = (System.nanoTime() - t0) / 1e6;
      return J.obj().b("ok", true).s("cmd", "heightgrid").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset).l("seed", d.seed)
         .l("x0", x0).l("z0", z0).l("nx", nx).l("nz", nz).l("step", step).s("heightmap", t.name())
         .s("layout", "index = iz*nx + ix; блок (x0+ix*step, z0+iz*step); значение = getBaseHeight").d("ms", ms).ia("h", h).done();
   }

   static String defaultspawn(Args a) {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), a.seed(1));
      int[] xz = Compat.spawnTarget(d.settings, d.rs);
      ChunkPos cp = ChunkPos.containing(new BlockPos(xz[0], 0, xz[1]));
      LevelHeightAccessor ha = LevelHeightAccessor.create(d.minY, d.height);
      int cxb = cp.x() * 16 + 8;
      int czb = cp.z() * 16 + 8;
      int h = d.gen.getBaseHeight(cxb, czb, Heightmap.Types.WORLD_SURFACE_WG, ha, d.rs);
      return J.obj().b("ok", true).s("cmd", "defaultspawn").s("version", Main.VERSION).s("dim", d.name).s("preset", d.preset).l("seed", d.seed)
         .la("climate_spawn_xz", new long[]{xz[0], xz[1]}).la("spawn_chunk", new long[]{cp.x(), cp.z()})
         .la("spawn_chunk_center_xz", new long[]{cxb, czb}).l("surface_wg_height_at_center", h)
         .s("note", "финальный спавн игрока (PlayerSpawnFinder.getSpawnPosInChunk) требует блоков сгенерированных чанков; здесь — климатическая часть + высота WORLD_SURFACE_WG")
         .done();
   }
}

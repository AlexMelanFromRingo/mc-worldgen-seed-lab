import java.lang.reflect.*;
import java.util.*;
import java.util.stream.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.*;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.*;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackResources;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.*;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.tags.TagLoader;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.*;
import net.minecraft.world.level.chunk.ChunkGeneratorStructureState;
import net.minecraft.world.level.levelgen.*;
import net.minecraft.world.level.levelgen.structure.StructureSet;
import net.minecraft.world.level.levelgen.structure.placement.*;

public class SH {
  static final java.io.PrintStream OUT = System.out;
  static HolderLookup.Provider load() throws Exception {
    PackRepository repo = ServerPacksSource.createVanillaTrustedRepository();
    repo.reload(); repo.setSelected(List.of("vanilla"));
    List<PackResources> packs = repo.openAllSelected();
    MultiPackResourceManager rm = new MultiPackResourceManager(PackType.SERVER_DATA, packs);
    LayeredRegistryAccess<RegistryLayer> layers = RegistryLayer.createRegistryAccess();
    List<Registry.PendingTags<?>> staticTags = TagLoader.loadTagsForExistingRegistries(rm, layers.getLayer(RegistryLayer.STATIC));
    RegistryLayer wl; try { wl = RegistryLayer.valueOf("WORLD"); } catch (IllegalArgumentException e) { wl = RegistryLayer.valueOf("WORLDGEN"); }
    RegistryAccess.Frozen worldLoadContext = layers.getAccessForLoading(wl);
    List<HolderLookup.RegistryLookup<?>> worldCtx = TagLoader.buildUpdatedLookups(worldLoadContext, staticTags);
    Field wf; try { wf = RegistryDataLoader.class.getField("WORLD_REGISTRIES"); } catch (NoSuchFieldException e) { wf = RegistryDataLoader.class.getField("WORLDGEN_REGISTRIES"); }
    RegistryAccess.Frozen loadedWorld = ((java.util.concurrent.CompletableFuture<RegistryAccess.Frozen>) RegistryDataLoader.load(rm, worldCtx, (List) wf.get(null), Runnable::run)).join();
    List<HolderLookup.RegistryLookup<?>> dimCtx = Stream.concat(worldCtx.stream(), loadedWorld.listRegistries()).toList();
    RegistryAccess.Frozen dims = RegistryDataLoader.load(rm, dimCtx, RegistryDataLoader.DIMENSION_REGISTRIES, Runnable::run).join();
    return HolderLookup.Provider.create(Stream.concat(dimCtx.stream(), dims.listRegistries()));
  }
  static RandomState mkRandomState(HolderLookup.Provider p, Holder<NoiseGeneratorSettings> s, long seed) throws Exception {
    for (Method m : RandomState.class.getMethods()) {
      if (!m.getName().equals("create") || m.getParameterCount() != 3) continue;
      Class<?>[] t = m.getParameterTypes();
      if (t[0] == HolderGetter.class) return (RandomState) m.invoke(null, p.lookupOrThrow(Registries.NOISE), seed, s.value());          // 26.3
      if (t[0] == NoiseGeneratorSettings.class) return (RandomState) m.invoke(null, s.value(), p.lookupOrThrow(Registries.NOISE), seed); // 26.1/26.2
    }
    throw new IllegalStateException("RandomState.create");
  }
  public static void main(String[] a) throws Exception {
    SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
    HolderLookup.Provider p = load();
    Holder<NoiseGeneratorSettings> ns = p.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(NoiseGeneratorSettings.OVERWORLD);
    Holder<MultiNoiseBiomeSourceParameterList> preset = p.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST).getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);
    MultiNoiseBiomeSource bs = MultiNoiseBiomeSource.createFromPreset(preset);
    NoiseBasedChunkGenerator gen = new NoiseBasedChunkGenerator(bs, ns);
    HolderLookup.RegistryLookup<StructureSet> ss = p.lookupOrThrow(Registries.STRUCTURE_SET);
    ConcentricRingsStructurePlacement pl = (ConcentricRingsStructurePlacement) ss.getOrThrow(ResourceKey.create(Registries.STRUCTURE_SET, net.minecraft.resources.Identifier.withDefaultNamespace("strongholds"))).value().placement();
    int n = Integer.parseInt(a[0]);
    java.util.Random r = new java.util.Random(Long.parseLong(a[1]));
    List<Long> seeds = new ArrayList<>(List.of(0L, 1L, 123456789L, -4172144997902289642L));
    while (seeds.size() < n) seeds.add(r.nextLong());
    for (int i = 0; i < n; i++) {
      long seed = seeds.get(i);
      long t0 = System.nanoTime();
      RandomState rs = mkRandomState(p, ns, seed);
      ChunkGeneratorStructureState st = gen.createState(ss, rs, seed);
      List<ChunkPos> pos = st.getRingPositionsFor(pl);
      StringBuilder sb = new StringBuilder();
      for (ChunkPos c : pos) sb.append(c.x()).append(',').append(c.z()).append(' ');
      OUT.println("seed " + seed + " n=" + pos.size() + " ms=" + (System.nanoTime() - t0) / 1000000 + " : " + sb.toString().trim());
    }
    System.exit(0);
  }
}

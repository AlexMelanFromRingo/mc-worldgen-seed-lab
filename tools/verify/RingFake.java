import java.lang.reflect.*;
import java.util.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.*;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.*;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.*;
import net.minecraft.world.level.chunk.ChunkGeneratorStructureState;
import net.minecraft.world.level.levelgen.*;
import net.minecraft.world.level.levelgen.structure.StructureSet;
import net.minecraft.world.level.levelgen.structure.placement.*;

public class RingFake {
  public static void main(String[] a) throws Exception {
    SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
    HolderLookup.Provider p = SH.load();
    Holder<NoiseGeneratorSettings> ns = p.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(NoiseGeneratorSettings.OVERWORLD);
    Holder<Biome> plains = p.lookupOrThrow(Registries.BIOME).getOrThrow(Biomes.PLAINS);
    Holder<Biome> ocean = p.lookupOrThrow(Registries.BIOME).getOrThrow(Biomes.DEEP_OCEAN);
    FakeBS fake = new FakeBS(plains, ocean);
    HolderLookup.RegistryLookup<StructureSet> ss = p.lookupOrThrow(Registries.STRUCTURE_SET);
    ConcentricRingsStructurePlacement pl = (ConcentricRingsStructurePlacement) ss.getOrThrow(ResourceKey.create(Registries.STRUCTURE_SET, Identifier.withDefaultNamespace("strongholds"))).value().placement();
    for (String sd : a) {
      long seed = Long.parseLong(sd);
      RandomState rs = SH.mkRandomState(p, ns, seed);
      NoiseBasedChunkGenerator gen = new NoiseBasedChunkGenerator(fake, ns);
      ChunkGeneratorStructureState st = gen.createState(ss, rs, seed);
      List<ChunkPos> pos = st.getRingPositionsFor(pl);
      StringBuilder sb = new StringBuilder();
      for (ChunkPos c : pos) sb.append(c.x()).append(',').append(c.z()).append(' ');
      SH.OUT.println("seed " + seed + " n=" + pos.size() + " : " + sb.toString().trim());
    }
    System.exit(0);
  }
}

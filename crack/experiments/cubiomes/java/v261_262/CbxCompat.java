package cbx;

import java.util.List;
import java.util.stream.Stream;
import net.minecraft.SharedConstants;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.LayeredRegistryAccess;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.RegistryDataLoader;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackResources;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.tags.TagLoader;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;

/** Версионно-зависимая часть харнесса (26.1 / 26.2). */
public final class CbxCompat {
   public static final String VERSION = "26.1/26.2";

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
      return HolderLookup.Provider.create(Stream.concat(dimCtx.stream(), dims.listRegistries()));
   }

   /** Climate.Sampler для заданных настроек шума и seed. */
   public static Climate.Sampler climateSampler(HolderLookup.Provider p, ResourceKey<NoiseGeneratorSettings> key, long seed) {
      NoiseGeneratorSettings s = p.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(key).value();
      RandomState rs = RandomState.create(s, p.lookupOrThrow(Registries.NOISE), seed);
      return rs.sampler();
   }
}

import java.util.stream.Stream;
import com.mojang.serialization.MapCodec;
import net.minecraft.core.Holder;
import net.minecraft.world.level.biome.*;
public class FakeBS extends BiomeSource {
  final Holder<Biome> pref, other;
  public FakeBS(Holder<Biome> pref, Holder<Biome> other) { this.pref = pref; this.other = other; }
  public static boolean isPref(int x, int y, int z) { return (((x * 73856093) ^ (z * 19349663) ^ (y * 83492791)) >>> 7) % 5 == 0; }
  @Override protected MapCodec<? extends BiomeSource> codec() { return null; }
  @Override protected Stream<Holder<Biome>> collectPossibleBiomes() { return Stream.of(pref, other); }
  @Override public BiomeResolver createResolver(Climate.Sampler s) { return (x, y, z) -> isPref(x, y, z) ? pref : other; }
}

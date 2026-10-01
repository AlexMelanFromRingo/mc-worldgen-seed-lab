package oracle;

import net.minecraft.core.Holder;
import net.minecraft.world.level.biome.Biome;

/** Буферы результата сетки. Массивы, которые не нужны, оставляем null. */
public final class GridOut {
   public final int nx;
   public final int nz;
   public long[][] q;
   public double[][] raw;
   public Holder<Biome>[] biome;

   @SuppressWarnings("unchecked")
   public GridOut(int nx, int nz, boolean wantQ, boolean wantRaw, boolean wantBiome) {
      this.nx = nx;
      this.nz = nz;
      int n = nx * nz;
      if (wantQ) q = new long[6][n];
      if (wantRaw) raw = new double[6][n];
      if (wantBiome) biome = (Holder<Biome>[]) new Holder[n];
   }
}

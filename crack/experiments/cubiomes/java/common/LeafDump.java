package cbx;

import java.io.*;
import java.util.*;
import com.mojang.datafixers.util.Pair;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.registries.Registries;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;

/** Печатает все листья дерева биомов: biome tmin tmax hmin hmax cmin cmax emin emax dmin dmax wmin wmax offset (в квантах 1e-4). */
public class LeafDump {
   public static void main(String[] a) throws Exception {
      CbxCompat.bootstrap();
      HolderLookup.Provider p = CbxCompat.loadRegistries();
      String which = a.length > 1 ? a[1] : "overworld";
      var key = which.equals("nether") ? MultiNoiseBiomeSourceParameterLists.NETHER : MultiNoiseBiomeSourceParameterLists.OVERWORLD;
      MultiNoiseBiomeSourceParameterList l = p.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST).getOrThrow(key).value();
      try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(a[0])))) {
         for (Pair<Climate.ParameterPoint, Holder<Biome>> e : l.parameters().values()) {
            Climate.ParameterPoint pp = e.getFirst();
            w.println(e.getSecond().unwrapKey().get().identifier().getPath() + " "
               + pp.temperature().min() + " " + pp.temperature().max() + " "
               + pp.humidity().min() + " " + pp.humidity().max() + " "
               + pp.continentalness().min() + " " + pp.continentalness().max() + " "
               + pp.erosion().min() + " " + pp.erosion().max() + " "
               + pp.depth().min() + " " + pp.depth().max() + " "
               + pp.weirdness().min() + " " + pp.weirdness().max() + " " + pp.offset());
         }
      }
      System.out.println("leaves: " + l.parameters().values().size());
   }
}

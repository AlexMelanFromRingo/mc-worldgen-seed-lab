import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.resources.Identifier;
import net.minecraft.world.level.levelgen.*;
import net.minecraft.world.level.levelgen.synth.*;
import java.util.*;

/** Эталон NormalNoise для 26.3 (float, NoiseStack). Вывод: seed noise x y z floatBits(hex) */
public class NoiseRef263 {
  // {name, base_octave, octave_count, base_amplitude, modifiers or null}
  static final Object[][] NOISES = {
    {"temperature", -10, 6, 1.2453007926713473, new double[]{1.5,0,1,0,0,0}},
    {"vegetation", -8, 6, 0.9494731054427978, new double[]{1,1,0,0,0,0}},
    {"continentalness", -9, 9, 0.8880832896205223, new double[]{1,1,2,2,2,1,1,1,1}},
    {"erosion", -9, 5, 1.063180125160734, new double[]{1,1,0,1,1}},
    {"ridge", -7, 6, 0.9147152149950137, new double[]{1,2,1,0,0,0}},
    {"offset", -3, 4, 0.9381732587751005, new double[]{1,1,1,0}},
  };
  public static void main(String[] a) throws Exception {
    SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
    long[] seeds = {0L, 1L, -1L, 12345L, 8675309L, Long.MAX_VALUE, Long.MIN_VALUE, 0x123456789ABCDEFL, -7046029254386353131L};
    Random pr = new Random(42);
    java.io.PrintWriter out = new java.io.PrintWriter(a[0]);
    for (long seed : seeds) {
      PositionalRandomFactory pf = new XoroshiroRandomSource(seed).forkPositional();
      for (Object[] nz : NOISES) {
        String name = (String) nz[0];
        NormalNoise.Builder b = NormalNoise.builder().setBaseOctave((Integer) nz[1]).setOctaveCount((Integer) nz[2]).setBaseAmplitude((Double) nz[3]);
        double[] mods = (double[]) nz[4];
        for (int i = 0; i < mods.length; i++) if (mods[i] != 1.0) b.setAmplitudeModifier(i, mods[i]);
        NormalNoise nn = b.build();
        Noise noise = nn.create(pf.fromHashOf(Identifier.withDefaultNamespace(name).toString()));
        for (int i = 0; i < 40; i++) {
          double x = (pr.nextDouble() - 0.5) * (i < 20 ? 2000 : 2e7);
          double y = (i % 3 == 0) ? 0.0 : (pr.nextDouble() - 0.5) * 512;
          double z = (pr.nextDouble() - 0.5) * (i < 20 ? 2000 : 2e7);
          float v = noise.get(x, y, z);
          out.println(seed + " " + name + " " + Double.doubleToLongBits(x) + " " + Double.doubleToLongBits(y) + " " + Double.doubleToLongBits(z) + " " + Integer.toHexString(Float.floatToIntBits(v)));
        }
      }
    }
    out.close();
  }
}

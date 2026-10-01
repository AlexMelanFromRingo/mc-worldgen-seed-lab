import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.resources.Identifier;
import net.minecraft.world.level.levelgen.*;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import it.unimi.dsi.fastutil.doubles.*;
import java.util.*;

/** Эталон NormalNoise для 26.1/26.2 (double). Вывод: seed noise x y z valueBits(hex) */
public class NoiseRef262 {
  static final Object[][] NOISES = {
    {"temperature", -10, new double[]{1.5,0,1,0,0,0}},
    {"vegetation", -8, new double[]{1,1,0,0,0,0}},
    {"continentalness", -9, new double[]{1,1,2,2,2,1,1,1,1}},
    {"erosion", -9, new double[]{1,1,0,1,1}},
    {"ridge", -7, new double[]{1,2,1,0,0,0}},
    {"offset", -3, new double[]{1,1,1,0}},
  };
  public static void main(String[] a) {
    SharedConstants.tryDetectVersion(); Bootstrap.bootStrap();
    long[] seeds = {0L, 1L, -1L, 12345L, 8675309L, Long.MAX_VALUE, Long.MIN_VALUE, 0x123456789ABCDEFL, -7046029254386353131L};
    Random pr = new Random(42);
    java.io.PrintWriter out; try { out = new java.io.PrintWriter(a[0]); } catch (Exception e) { throw new RuntimeException(e); }
    for (long seed : seeds) {
      PositionalRandomFactory pf = new XoroshiroRandomSource(seed).forkPositional();
      for (Object[] nz : NOISES) {
        String name = (String) nz[0];
        NormalNoise.NoiseParameters p = new NormalNoise.NoiseParameters((Integer) nz[1], new DoubleArrayList((double[]) nz[2]));
        NormalNoise nn = NormalNoise.create(pf.fromHashOf(Identifier.withDefaultNamespace(name)), p);
        for (int i = 0; i < 40; i++) {
          double x = (pr.nextDouble() - 0.5) * (i < 20 ? 2000 : 2e7);
          double y = (i % 3 == 0) ? 0.0 : (pr.nextDouble() - 0.5) * 512;
          double z = (pr.nextDouble() - 0.5) * (i < 20 ? 2000 : 2e7);
          double v = nn.getValue(x, y, z);
          out.println(seed + " " + name + " " + Double.doubleToLongBits(x) + " " + Double.doubleToLongBits(y) + " " + Double.doubleToLongBits(z) + " " + Long.toHexString(Double.doubleToLongBits(v)));
        }
      }
    }
    out.close();
  }
}

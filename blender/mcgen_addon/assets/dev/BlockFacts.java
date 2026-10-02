// Dev-инструмент (не часть аддона): дамп фактов о состояниях блоков из настоящего кода игры. Сборка/запуск: см. gen_blockprops.py.
import java.io.*;
import java.lang.reflect.Method;
import java.util.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.*;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.VoxelShape;
import net.minecraft.world.phys.shapes.Shapes;

/** Дамп фактов о состояниях блоков из настоящего кода игры: окклюзия, рендер-форма, смещение, жидкость, класс. */
public class BlockFacts {
  static String propVal(Property<?> p, Comparable<?> v) { return ((Property) p).getName((Comparable) v); }
  static String mask(VoxelShape s, Direction d) {
    if (s == Shapes.block()) return "F";
    if (s.isEmpty()) return "0";
    boolean[][] g = new boolean[16][16];
    int a = d.getAxis().ordinal(); // X=0,Y=1,Z=2
    for (AABB b : s.toAabbs()) {
      double[] mn = {b.minX, b.minY, b.minZ}, mx = {b.maxX, b.maxY, b.maxZ};
      int u = a == 0 ? 1 : 0, v = a == 2 ? 1 : 2; // два других оси
      for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) {
        double cu = (i + 0.5) / 16, cv = (j + 0.5) / 16;
        if (cu >= mn[u] && cu <= mx[u] && cv >= mn[v] && cv <= mx[v]) g[i][j] = true;
      }
    }
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < 16; i++) { int bits = 0; for (int j = 0; j < 16; j++) if (g[i][j]) bits |= 1 << j; sb.append(String.format("%04x", bits)); }
    return sb.toString();
  }
  public static void main(String[] args) throws Exception {
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    PrintStream out = new PrintStream(new BufferedOutputStream(new FileOutputStream(args[0])), false, "UTF-8");
    Method mh = null, mv = null;
    for (Class<?> c = Block.class; c != null; c = c.getSuperclass()) {
      try { mh = c.getDeclaredMethod("getMaxHorizontalOffset"); mh.setAccessible(true); break; } catch (NoSuchMethodException e) {}
    }
    for (Class<?> c = Block.class; c != null; c = c.getSuperclass()) {
      try { mv = c.getDeclaredMethod("getMaxVerticalOffset"); mv.setAccessible(true); break; } catch (NoSuchMethodException e) {}
    }
    int n = 0;
    out.println("[");
    boolean first = true;
    for (BlockState s : Block.BLOCK_STATE_REGISTRY) {
      int id = Block.BLOCK_STATE_REGISTRY.getId(s);
      Block b = s.getBlock();
      StringBuilder props = new StringBuilder();
      for (Property.Value<?> v : (Iterable<Property.Value<?>>) s.getValues()::iterator) {
        if (props.length() > 0) props.append(',');
        props.append(v.toString());
      }
      String bn = BuiltInRegistries.BLOCK.getKey(b).toString();
      StringBuilder sup = new StringBuilder();
      for (Class<?> c = b.getClass(); c != null && c != Object.class; c = c.getSuperclass()) { if (sup.length() > 0) sup.append('>'); sup.append(c.getSimpleName()); }
      FluidState fs = s.getFluidState();
      String fluid = fs.isEmpty() ? "" : BuiltInRegistries.FLUID.getKey(fs.getType()).toString();
      int off = 0;
      float mH = 0, mV = 0;
      if (s.hasOffsetFunction()) {
        double[] vs = new double[3];
        boolean y = false;
        for (int px = -20; px < 20 && !y; px++) for (int pz = -20; pz < 20; pz++) if (s.getOffset(new BlockPos(px, 0, pz)).y != 0) { y = true; break; }
        off = y ? 2 : 1;
        mH = (Float) mh.invoke(b);
        mV = (Float) mv.invoke(b);
      }
      String skip = "";
      if (b instanceof HalfTransparentBlock) skip += "H";
      if (b instanceof LeavesBlock) skip += "L";
      if (b instanceof IronBarsBlock) skip += "B";
      if (b instanceof PowderSnowBlock) skip += "P";
      if (b instanceof MangroveRootsBlock) skip += "M";
      if (b instanceof LiquidBlock) skip += "W";
      StringBuilder fm = new StringBuilder();
      for (Direction d : Direction.values()) { if (d.ordinal() > 0) fm.append(','); fm.append('"').append(mask(s.getFaceOcclusionShape(d), d)).append('"'); }
      if (!first) out.println(","); first = false;
      out.print("{\"id\":" + id + ",\"b\":\"" + bn + "\",\"p\":\"" + props + "\",\"cls\":\"" + b.getClass().getSimpleName() + "\",\"sup\":\"" + sup + "\""
        + ",\"co\":" + (s.canOcclude() ? 1 : 0) + ",\"sr\":" + (s.isSolidRender() ? 1 : 0) + ",\"rs\":\"" + s.getRenderShape() + "\""
        + ",\"le\":" + s.getLightEmission() + ",\"air\":" + (s.isAir() ? 1 : 0) + ",\"solid\":" + (s.isSolid() ? 1 : 0)
        + ",\"off\":" + off + ",\"mh\":" + mH + ",\"mv\":" + mV + ",\"fl\":\"" + fluid + "\",\"fa\":" + (fs.isEmpty() ? 0 : fs.getAmount()) + ",\"fs\":" + (fs.isEmpty() ? 0 : (fs.isSource() ? 1 : 0))
        + ",\"skip\":\"" + skip + "\",\"fo\":[" + fm + "]}");
      n++;
    }
    out.println("\n]");
    out.close();
    System.err.println("states: " + n);
  }
}

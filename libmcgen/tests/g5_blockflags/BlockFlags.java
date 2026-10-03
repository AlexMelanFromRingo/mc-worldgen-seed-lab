package mcgenflags;

import java.io.BufferedWriter;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;

/**
 * Выгрузка свойств состояний блоков, закодированных в Java (а не в датапаке): нужны стадии декораций libmcgen
 * (BlockPredicate solid/replaceable/has_sturdy_face, matching_fluids, карты высот и т. д.).
 *   java -cp <classpath игры>:<classes> mcgenflags.BlockFlags <out.json>
 * Формат: {"version","nstates","blocks":[{"name","cls","sup":[...]}],"flags":[u32 на состояние],"fluid":[u16],"sturdy":[u8]}
 *   flags: 1 air, 2 solid(isSolid), 4 liquid(liquid()), 8 replaceable(canBeReplaced()), 16 canOcclude, 32 solidRender, 64 collisionFullBlock,
 *          128 leaves, 256 blockEntity, 512 blocksMotion (26.1/26.2: blocksMotion(); 26.3: нет — 0), 1024 propagatesSkylightDown
 *          биты 16..17 postProcess: 1 — «себя», 2 — «над собой»  (getPostProcessPos), 20..23 — lightEmission (0..15)
 *   fluid: тип*256 + amount*16 + falling*8 + source*4   (тип: 0 пусто, 1 water, 2 flowing_water, 3 lava, 4 flowing_lava)
 *   sturdy: биты 0..5 — isFaceSturdy(FULL) по направлениям DOWN, UP, NORTH, SOUTH, WEST, EAST
 * Порядок состояний — Block.BLOCK_STATE_REGISTRY (тот же id, что в reports/blocks.json).
 */
public final class BlockFlags {
   public static void main(String[] args) throws Exception {
      SharedConstants.tryDetectVersion();
      Bootstrap.bootStrap();
      int n = Block.BLOCK_STATE_REGISTRY.size();
      long[] flags = new long[n];
      int[] fluid = new int[n];
      int[] sturdy = new int[n];
      BlockPos zero = BlockPos.ZERO;
      for (int id = 0; id < n; id++) {
         BlockState s = Block.BLOCK_STATE_REGISTRY.byId(id);
         if (s == null) {
            continue;
         }
         long f = 0;
         if (s.isAir()) f |= 1;
         if (s.isSolid()) f |= 2;
         if (s.liquid()) f |= 4;
         if (s.canBeReplaced()) f |= 8;
         if (s.canOcclude()) f |= 16;
         if (s.isSolidRender()) f |= 32;
         if (s.isCollisionShapeFullBlock(EmptyBlockGetter.INSTANCE, zero)) f |= 64;
         if (s.getBlock().getClass().getSimpleName().equals("LeavesBlock") || s.getBlock() instanceof net.minecraft.world.level.block.LeavesBlock) f |= 128;
         if (s.hasBlockEntity()) f |= 256;
         f |= blocksMotion(s);
         if (s.propagatesSkylightDown()) f |= 1024;
         BlockPos pp = null;
         try {
            pp = s.getPostProcessPos(EmptyBlockGetter.INSTANCE, zero);
         } catch (Throwable t) {
            // зависит от уровня — только «над собой»/«себя» известны статически
         }
         if (pp != null) {
            f |= (pp.equals(zero) ? 1L : 2L) << 16;
         }
         f |= ((long)s.getLightEmission() & 15) << 20;
         flags[id] = f;
         FluidState fs = s.getFluidState();
         int ft = 0;
         if (!fs.isEmpty()) {
            String key = BuiltInRegistries.FLUID.getKey(fs.getType()).getPath();
            ft = switch (key) {
               case "water" -> 1;
               case "flowing_water" -> 2;
               case "lava" -> 3;
               case "flowing_lava" -> 4;
               default -> 5;
            };
         }
         int falling = 0;
         try {
            falling = fs.getValue(net.minecraft.world.level.material.FlowingFluid.FALLING) ? 1 : 0;
         } catch (Throwable t) {
            falling = 0;
         }
         fluid[id] = ft * 256 + fs.getAmount() * 16 + falling * 8 + (fs.isSource() ? 4 : 0);
         int sm = 0;
         for (Direction d : Direction.values()) {
            if (s.isFaceSturdy(EmptyBlockGetter.INSTANCE, zero, d)) sm |= 1 << d.ordinal();
         }
         sturdy[id] = sm;
      }
      try (BufferedWriter w = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8), 1 << 16)) {
         w.write("{\"version\":\"" + SharedConstants.getCurrentVersion().name() + "\",\"nstates\":" + n + ",\n\"blocks\":[\n");
         boolean first = true;
         for (Block b : BuiltInRegistries.BLOCK) {
            if (!first) w.write(",\n");
            first = false;
            List<String> sup = new ArrayList<>();
            for (Class<?> c = b.getClass().getSuperclass(); c != null && c != Object.class; c = c.getSuperclass()) {
               sup.add(c.getSimpleName());
            }
            w.write("{\"name\":\"" + BuiltInRegistries.BLOCK.getKey(b) + "\",\"cls\":\"" + b.getClass().getSimpleName() + "\",\"sup\":[");
            for (int i = 0; i < sup.size(); i++) {
               if (i > 0) w.write(",");
               w.write("\"" + sup.get(i) + "\"");
            }
            w.write("]}");
         }
         w.write("\n],\n\"flags\":[");
         for (int i = 0; i < n; i++) {
            if (i > 0) w.write(i % 64 == 0 ? ",\n" : ",");
            w.write(Long.toString(flags[i]));
         }
         w.write("],\n\"fluid\":[");
         for (int i = 0; i < n; i++) {
            if (i > 0) w.write(i % 64 == 0 ? ",\n" : ",");
            w.write(Integer.toString(fluid[i]));
         }
         w.write("],\n\"sturdy\":[");
         for (int i = 0; i < n; i++) {
            if (i > 0) w.write(i % 64 == 0 ? ",\n" : ",");
            w.write(Integer.toString(sturdy[i]));
         }
         w.write("]}\n");
      }
   }

   private static long blocksMotion(BlockState s) {
      try {
         java.lang.reflect.Method m = s.getClass().getMethod("blocksMotion");
         return ((Boolean)m.invoke(s)) ? 512 : 0;
      } catch (ReflectiveOperationException e) {
         return 0;
      }
   }
}

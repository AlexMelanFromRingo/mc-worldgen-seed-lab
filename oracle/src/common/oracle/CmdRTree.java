package oracle;

import com.mojang.datafixers.util.Pair;
import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.core.Holder;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;

/** rtree <dim> [--out файл] — дамп внутренней структуры Climate.RTree (через рефлексию) для точного воспроизведения обхода и тай-брейка. */
final class CmdRTree {
   private CmdRTree() {
   }

   private static Object get(Object o, String name) throws Exception {
      Class<?> c = o.getClass();
      while (c != null) {
         try {
            Field f = c.getDeclaredField(name);
            f.setAccessible(true);
            return f.get(o);
         } catch (NoSuchFieldException e) {
            c = c.getSuperclass();
         }
      }
      throw new NoSuchFieldException(name);
   }

   static String rtree(Args a) throws Exception {
      Ctx.Dim d = Ctx.dim(a.str(0), a.get("preset", "normal"), 0L);
      Climate.ParameterList<Holder<Biome>> list = CmdCore.parameterList(d);
      Object index = get(list, "index");
      Object root = get(index, "root");
      // сопоставление листьев с индексами таблицы: по 7 параметрам (+ идентичность значения)
      List<Pair<Climate.ParameterPoint, Holder<Biome>>> values = list.values();
      Map<String, ArrayDeque<Integer>> byKey = new HashMap<>();
      for (int i = 0; i < values.size(); i++) {
         Climate.ParameterPoint p = values.get(i).getFirst();
         String k = key(new Climate.Parameter[]{p.temperature(), p.humidity(), p.continentalness(), p.erosion(), p.depth(), p.weirdness(),
            new Climate.Parameter(p.offset(), p.offset())}) + "|" + System.identityHashCode(values.get(i).getSecond());
         byKey.computeIfAbsent(k, x -> new ArrayDeque<>()).add(i);
      }
      StringBuilder sb = new StringBuilder();
      int[] stats = new int[4]; // subtrees, leaves, maxDepth, maxChildren
      dump(root, sb, byKey, 0, stats);
      String tree = sb.toString();
      J j = J.obj().b("ok", true).s("cmd", "rtree").s("version", Main.VERSION).s("dim", d.name).l("leaves", stats[1]).l("subtrees", stats[0])
         .l("max_depth", stats[2]).l("max_children", stats[3]).l("points", values.size())
         .s("node_format", "подузел: {\"b\":[tmin,tmax,hmin,hmax,cmin,cmax,emin,emax,dmin,dmax,wmin,wmax,offmin,offmax],\"c\":[дети по порядку обхода]}; лист: {\"b\":[...],\"i\":индекс в params}")
         .s("search", "SubTree.search: minDist=dist(candidate=lastResult потока) ; для ребёнка по порядку: if(minDist > childDist){leaf=child.search(..); if(minDist>leafDist){минимум=leafDist}} — строгие неравенства => при равенстве побеждает кандидат/более ранний");
      if (a.has("out")) {
         Util.writeFile(a.get("out", null), j.raw("tree", tree).done() + "\n");
         return J.obj().b("ok", true).s("cmd", "rtree").s("file", a.get("out", null)).l("leaves", stats[1]).l("subtrees", stats[0]).l("max_depth", stats[2]).done();
      }
      return j.raw("tree", tree).done();
   }

   private static String key(Climate.Parameter[] ps) {
      StringBuilder sb = new StringBuilder();
      for (Climate.Parameter p : ps) sb.append(p.min()).append(',').append(p.max()).append(';');
      return sb.toString();
   }

   private static void dump(Object node, StringBuilder sb, Map<String, ArrayDeque<Integer>> byKey, int depth, int[] stats) throws Exception {
      Climate.Parameter[] ps = (Climate.Parameter[]) get(node, "parameterSpace");
      sb.append("{\"b\":[");
      for (int i = 0; i < ps.length; i++) {
         if (i > 0) sb.append(',');
         sb.append(ps[i].min()).append(',').append(ps[i].max());
      }
      sb.append(']');
      String cn = node.getClass().getSimpleName();
      if (cn.equals("Leaf")) {
         Object value = get(node, "value");
         ArrayDeque<Integer> q = byKey.get(key(ps) + "|" + System.identityHashCode(value));
         int idx = q == null || q.isEmpty() ? -1 : q.poll();
         sb.append(",\"i\":").append(idx).append('}');
         stats[1]++;
         stats[2] = Math.max(stats[2], depth);
      } else {
         Object[] ch = (Object[]) get(node, "children");
         stats[0]++;
         stats[3] = Math.max(stats[3], ch.length);
         sb.append(",\"c\":[");
         for (int i = 0; i < ch.length; i++) {
            if (i > 0) sb.append(',');
            dump(ch[i], sb, byKey, depth + 1, stats);
         }
         sb.append("]}");
      }
   }
}

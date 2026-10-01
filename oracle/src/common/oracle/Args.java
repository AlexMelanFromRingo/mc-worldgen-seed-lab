package oracle;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Разбор аргументов: позиционные + опции --name [value] (флаги без значения перечислены в FLAGS). */
public final class Args {
   private static final java.util.Set<String> FLAGS = java.util.Set.of("raw", "biome", "brute", "fullcolumn", "short", "twod", "targets", "pretty", "all");
   public final List<String> pos = new ArrayList<>();
   public final Map<String, String> opt = new HashMap<>();
   private int cursor = 0;

   public static Args parse(List<String> tokens) {
      Args a = new Args();
      for (int i = 0; i < tokens.size(); i++) {
         String t = tokens.get(i);
         if (t.startsWith("--") && t.length() > 2 && !isNumber(t)) {
            String name = t.substring(2);
            int eq = name.indexOf('=');
            if (eq >= 0) {
               a.opt.put(name.substring(0, eq), name.substring(eq + 1));
            } else if (FLAGS.contains(name)) {
               a.opt.put(name, "true");
            } else {
               if (i + 1 >= tokens.size()) throw new IllegalArgumentException("Опция --" + name + " требует значение");
               a.opt.put(name, tokens.get(++i));
            }
         } else {
            a.pos.add(t);
         }
      }
      return a;
   }

   private static boolean isNumber(String s) {
      try {
         Double.parseDouble(s);
         return true;
      } catch (NumberFormatException e) {
         return false;
      }
   }

   public boolean has(String name) {
      return opt.containsKey(name);
   }

   public String get(String name, String def) {
      return opt.getOrDefault(name, def);
   }

   public int size() {
      return pos.size();
   }

   public String str(int i) {
      if (i >= pos.size()) throw new IllegalArgumentException("Не хватает аргумента #" + (i + 1));
      return pos.get(i);
   }

   public int i(int i) {
      return (int) parseLong(str(i));
   }

   public long l(int i) {
      return parseLong(str(i));
   }

   public double d(int i) {
      return Double.parseDouble(str(i));
   }

   /** Seed: десятичное (знаковое) число, 0x-hex, либо "s:текст" -> String.hashCode() (как в WorldOptions.parseSeed). */
   public long seed(int i) {
      return parseSeed(str(i));
   }

   public static long parseSeed(String s) {
      if (s.startsWith("s:")) return (long) s.substring(2).hashCode();
      return parseLong(s);
   }

   public static long parseLong(String s) {
      s = s.trim();
      if (s.startsWith("0x") || s.startsWith("0X")) return Long.parseUnsignedLong(s.substring(2), 16);
      if (s.startsWith("-0x") || s.startsWith("-0X")) return -Long.parseUnsignedLong(s.substring(3), 16);
      try {
         return Long.parseLong(s);
      } catch (NumberFormatException e) {
         // допускаем беззнаковое представление 2^63..2^64-1
         return Long.parseUnsignedLong(s);
      }
   }
}

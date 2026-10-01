package oracle;

import java.util.List;
import java.util.Locale;

/** Минимальный построитель JSON (без зависимостей). double печатается в кратчайшем round-trip виде, NaN/Inf -> null. */
public final class J {
   private final StringBuilder sb = new StringBuilder(128);
   private final boolean[] first = new boolean[64];
   private final char[] closer = new char[64];
   private int depth = 0;

   private J() {
   }

   /** Начало корневого объекта. */
   public static J obj() {
      J j = new J();
      j.sb.append('{');
      j.first[0] = true;
      j.closer[0] = '}';
      return j;
   }

   private void comma() {
      if (first[depth]) first[depth] = false;
      else sb.append(',');
   }

   private void k(String key) {
      comma();
      appendStr(sb, key);
      sb.append(':');
   }

   // ---- вложенные контейнеры
   public J o(String key) {
      k(key);
      sb.append('{');
      depth++;
      first[depth] = true;
      closer[depth] = '}';
      return this;
   }

   public J a(String key) {
      k(key);
      sb.append('[');
      depth++;
      first[depth] = true;
      closer[depth] = ']';
      return this;
   }

   /** Элемент-объект внутри массива. */
   public J eo() {
      comma();
      sb.append('{');
      depth++;
      first[depth] = true;
      closer[depth] = '}';
      return this;
   }

   /** Элемент-массив внутри массива. */
   public J ea() {
      comma();
      sb.append('[');
      depth++;
      first[depth] = true;
      closer[depth] = ']';
      return this;
   }

   public J end() {
      sb.append(closer[depth]);
      depth--;
      return this;
   }

   // ---- поля объекта
   public J s(String key, String v) {
      k(key);
      if (v == null) sb.append("null");
      else appendStr(sb, v);
      return this;
   }

   public J l(String key, long v) {
      k(key);
      sb.append(v);
      return this;
   }

   public J b(String key, boolean v) {
      k(key);
      sb.append(v);
      return this;
   }

   public J d(String key, double v) {
      k(key);
      appendDouble(sb, v);
      return this;
   }

   public J raw(String key, CharSequence json) {
      k(key);
      sb.append(json);
      return this;
   }

   public J la(String key, long[] arr) {
      k(key);
      sb.append('[');
      for (int i = 0; i < arr.length; i++) {
         if (i > 0) sb.append(',');
         sb.append(arr[i]);
      }
      sb.append(']');
      return this;
   }

   public J ia(String key, int[] arr) {
      k(key);
      sb.append('[');
      for (int i = 0; i < arr.length; i++) {
         if (i > 0) sb.append(',');
         sb.append(arr[i]);
      }
      sb.append(']');
      return this;
   }

   public J da(String key, double[] arr) {
      k(key);
      sb.append('[');
      for (int i = 0; i < arr.length; i++) {
         if (i > 0) sb.append(',');
         appendDouble(sb, arr[i]);
      }
      sb.append(']');
      return this;
   }

   public J sa(String key, List<String> arr) {
      k(key);
      sb.append('[');
      for (int i = 0; i < arr.size(); i++) {
         if (i > 0) sb.append(',');
         appendStr(sb, arr.get(i));
      }
      sb.append(']');
      return this;
   }

   // ---- элементы массива
   public J vs(String v) {
      comma();
      appendStr(sb, v);
      return this;
   }

   public J vl(long v) {
      comma();
      sb.append(v);
      return this;
   }

   public J vd(double v) {
      comma();
      appendDouble(sb, v);
      return this;
   }

   public J vraw(CharSequence json) {
      comma();
      sb.append(json);
      return this;
   }

   /** Добавить перевод строки (для читаемости файлов). */
   public J nl() {
      sb.append('\n');
      return this;
   }

   public String done() {
      while (depth > 0) end();
      sb.append('}');
      return sb.toString();
   }

   // ---- статические помощники
   public static void appendDouble(StringBuilder sb, double v) {
      if (Double.isNaN(v) || Double.isInfinite(v)) sb.append("null");
      else sb.append(Double.toString(v));
   }

   public static void appendStr(StringBuilder sb, String s) {
      sb.append('"');
      for (int i = 0; i < s.length(); i++) {
         char c = s.charAt(i);
         switch (c) {
            case '"' -> sb.append("\\\"");
            case '\\' -> sb.append("\\\\");
            case '\n' -> sb.append("\\n");
            case '\r' -> sb.append("\\r");
            case '\t' -> sb.append("\\t");
            default -> {
               if (c < 0x20) sb.append(String.format(Locale.ROOT, "\\u%04x", (int) c));
               else sb.append(c);
            }
         }
      }
      sb.append('"');
   }

   public static String q(String s) {
      StringBuilder sb = new StringBuilder();
      appendStr(sb, s);
      return sb.toString();
   }
}

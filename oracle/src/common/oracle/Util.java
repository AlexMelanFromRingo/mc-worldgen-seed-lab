package oracle;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import net.minecraft.core.Holder;
import net.minecraft.resources.Identifier;
import net.minecraft.world.level.biome.Biome;

final class Util {
   private Util() {
   }

   static String biomeName(Holder<Biome> h) {
      return h.unwrapKey().map(k -> k.identifier().toString()).orElse("?direct");
   }

   static Identifier id(String s) {
      Identifier i = Identifier.tryParse(s.contains(":") ? s : "minecraft:" + s);
      if (i == null) throw new IllegalArgumentException("Плохой идентификатор: " + s);
      return i;
   }

   static void writeFile(String path, String content) throws IOException {
      Path p = Path.of(path);
      if (p.getParent() != null) Files.createDirectories(p.getParent());
      Files.writeString(p, content, StandardCharsets.UTF_8);
   }

   static long fnv1a(long h, byte[] b) {
      for (byte x : b) {
         h ^= (x & 0xFF);
         h *= 0x100000001b3L;
      }
      return h;
   }

   static final long FNV_OFFSET = 0xcbf29ce484222325L;

   static String hex16(long v) {
      return String.format("%016x", v);
   }
}

package oracle;

import java.io.BufferedReader;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.SharedConstants;

/**
 * Эталон (oracle): настоящий код Mojang + настоящий датапак из jars/game-V.jar, внутри процесса.
 * CLI: Main <команда> args...   |   Main serve  (команды построчно из stdin, ответ — одна JSON-строка на команду).
 */
public final class Main {
   public static String VERSION = "?";
   static PrintStream OUT;

   public static void main(String[] argv) throws Exception {
      // stdout захватываем ДО Bootstrap: Bootstrap.wrapStreams() подменяет System.out на логгер.
      OUT = new PrintStream(new java.io.BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16), false, StandardCharsets.UTF_8);
      if (argv.length == 0) {
         usage();
         System.exit(2);
      }
      long t0 = System.nanoTime();
      Ctx.init();
      VERSION = SharedConstants.getCurrentVersion().name();
      long t1 = System.nanoTime();
      if (argv[0].equals("serve")) {
         System.err.println("[oracle] " + VERSION + " (" + oracle.compat.Compat.FAMILY + ") готов за " + (t1 - t0) / 1_000_000 + " мс; жду команды в stdin");
         BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
         String line;
         while ((line = in.readLine()) != null) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#")) continue;
            if (line.equals("quit") || line.equals("exit")) break;
            OUT.println(execute(tokenize(line)));
            OUT.flush();
         }
      } else {
         String r = execute(List.of(argv));
         OUT.println(r);
         OUT.flush();
         if (r.startsWith("{\"ok\":false")) System.exit(1);
      }
      OUT.flush();
   }

   private static final java.util.Set<String> NEEDS_DIM = java.util.Set.of(
      "climate", "climategrid", "biome", "noise", "df", "structs", "structsets", "blockbiome", "height", "heightgrid", "defaultspawn", "structstart", "rtree");

   static List<String> tokenize(String line) {
      List<String> t = new ArrayList<>();
      for (String s : line.split("\\s+")) if (!s.isEmpty()) t.add(s);
      return t;
   }

   static String execute(List<String> tokens) {
      try {
         String cmd = tokens.get(0);
         Args a = Args.parse(tokens.subList(1, tokens.size()));
         // dim необязателен для команд, где он первый аргумент: "noise 12345 minecraft:temperature x y z" == "noise overworld 12345 ..."
         if (NEEDS_DIM.contains(cmd) && !a.pos.isEmpty() && !Ctx.isDimName(a.pos.get(0))) a.pos.add(0, "overworld");
         return switch (cmd) {
            case "info" -> J.obj().b("ok", true).s("cmd", "info").s("version", VERSION).s("family", oracle.compat.Compat.FAMILY)
               .l("world_version", SharedConstants.getCurrentVersion().dataVersion().version()).s("java", System.getProperty("java.version")).done();
            case "params" -> CmdCore.params(a);
            case "climate" -> a.size() >= 7 ? CmdCore.climategrid(a) : CmdCore.climate(a);
            case "climategrid" -> CmdCore.climategrid(a);
            case "biome" -> CmdCore.biome(a);
            case "noise" -> CmdCore.noise(a);
            case "df" -> CmdCore.df(a);
            case "list" -> CmdCore.list(a);
            default -> CmdMore.dispatch(cmd, a);
         };
      } catch (Throwable e) {
         StringBuilder tr = new StringBuilder();
         StackTraceElement[] st = e.getStackTrace();
         for (int i = 0; i < Math.min(6, st.length); i++) tr.append(st[i]).append(" | ");
         return J.obj().b("ok", false).s("error", e.getClass().getSimpleName() + ": " + e.getMessage()).s("where", tr.toString()).done();
      }
   }

   static void usage() {
      System.err.println("""
         oracle <команда> ...   |   oracle serve
         Команды (dim: overworld|nether|end; seed: число, 0x.., s:текст; опции: --preset normal|large_biomes|amplified --mode point|chunk):
           info
           list noises|dfs|presets|biomes|structure_sets|structures|noise_settings
           params <dim> [--fmt json|tsv] [--out файл]
           climate <dim> <seed> <qx> <qy> <qz>            climategrid <dim> <seed> <qx0> <qz0> <nx> <nz> <qy> [--raw] [--biome]
           biome <dim> <seed> <qx0> <qz0> <nx> <nz> <qy> [--fmt json|csv|bin|hash] [--out f] [--brute]
           noise <dim> <seed> <noise_id> x y z [x y z ...]
           df <dim> <seed> <density_function_id> x y z [x y z ...]
           (P1) structs, structsets, stronghold, pillars, slime, hashed, blockbiome, height, defaultspawn, rtree
         """);
   }
}

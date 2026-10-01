package oracle;

import net.minecraft.core.Holder;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;

/**
 * Версионно-независимый фасад над реальным Climate.Sampler / BiomeSource.
 * Реализации лежат в oracle.compat.Compat (по одной на семейство версий).
 * Все координаты — в квартах (quart = блок/4), если не сказано иное.
 */
public interface ClimateApi {
   /** Реальный Climate.Sampler.sample(qx,qy,qz) — 6 квантованных значений (T,H,C,E,D,W). */
   Climate.TargetPoint target(int qx, int qy, int qz);

   /** Сырые значения 6 density-функций ДО квантования (в 26.3 это float, расширенный до double). Порядок T,H,C,E,D,W. */
   void raw(int qx, int qy, int qz, double[] out6);

   /** Значение функции erosion в произвольной БЛОЧНОЙ точке (для End: heightValue). */
   double erosionAtBlock(int blockX, int blockY, int blockZ);

   /** Реальный биом «сырой сетки» (без зума BiomeManager). */
   Holder<Biome> biome(int qx, int qy, int qz);

   /** true, если raw() возвращает нативные float (26.3). */
   boolean floatNative();

   /** Режим: "point" (по одной точке, как uncached-резолвер) или "chunk" (как при реальной генерации чанка). */
   String mode();

   /**
    * Заполнение сетки [nx * nz] на слое qy (индекс = iz*nx+ix). Любой из массивов out может быть null.
    * По умолчанию — цикл по точкам; режим chunk в 26.3 переопределяет.
    */
   default void grid(int qx0, int qz0, int nx, int nz, int qy, GridOut out) {
      double[] r = new double[6];
      for (int iz = 0; iz < nz; iz++) {
         for (int ix = 0; ix < nx; ix++) {
            int i = iz * nx + ix;
            int qx = qx0 + ix;
            int qz = qz0 + iz;
            if (out.q != null) {
               Climate.TargetPoint t = target(qx, qy, qz);
               out.q[0][i] = t.temperature();
               out.q[1][i] = t.humidity();
               out.q[2][i] = t.continentalness();
               out.q[3][i] = t.erosion();
               out.q[4][i] = t.depth();
               out.q[5][i] = t.weirdness();
            }
            if (out.raw != null) {
               raw(qx, qy, qz, r);
               for (int k = 0; k < 6; k++) out.raw[k][i] = r[k];
            }
            if (out.biome != null) {
               out.biome[i] = biome(qx, qy, qz);
            }
         }
      }
   }
}

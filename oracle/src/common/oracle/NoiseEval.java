package oracle;

/** Именованный NormalNoise, построенный как в RandomState (Noises.instantiate). */
public interface NoiseEval {
   double get3(double x, double y, double z);

   /** 2D-вариант (есть только в 26.3, иначе NaN). */
   double get2(double x, double y);

   /** true, если результат нативно float (26.3). */
   boolean floatNative();
}

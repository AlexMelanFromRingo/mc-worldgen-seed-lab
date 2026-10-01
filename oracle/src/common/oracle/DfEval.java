package oracle;

/** Density-функция, «проводка» (wiring) которой сделана настоящим RandomState для данного seed. */
public interface DfEval {
   double at(int x, int y, int z);

   boolean floatNative();
}

#!/usr/bin/env python3
"""Версии <= 1.12.2: все фичи чанка берут числа из ОДНОГО последовательного java.util.Random. Проверка: есть ли статистическая связь между
nextInt(16) на шаге n и nextInt(16) на шаге n+k (k = 1..K) при случайном начальном состоянии (2D-хи-квадрат 16x16 против равномерного).
  tools/l3_lcg_chain.py [K=300] [N=400000]
Ожидание: связи нет (старшие 4 бита состояний на расстоянии k >= 2 шагов практически независимы, A^k mod 2^48 — «большое» число)."""
import sys, numpy as np
A = 0x5DEECE66D; C = 0xB; M = (1 << 48) - 1
K = int(sys.argv[1]) if len(sys.argv) > 1 else 300
N = int(sys.argv[2]) if len(sys.argv) > 2 else 400000
rng = np.random.default_rng(5)
s = rng.integers(0, 1 << 48, size=N, dtype=np.uint64)
x0 = (s >> np.uint64(44)).astype(np.int64)
cur = s.copy(); res = []
for k in range(1, K + 1):
    cur = (cur * np.uint64(A) + np.uint64(C)) & np.uint64(M)
    xk = (cur >> np.uint64(44)).astype(np.int64)
    T = np.bincount(x0 * 16 + xk, minlength=256).reshape(16, 16)
    e = N / 256
    chi = ((T - e) ** 2 / e).sum()       # 255 степеней свободы: среднее 255, сигма ~22.6
    res.append((k, chi, T.max() / e))
bad = [r for r in res if r[1] > 255 + 4 * 22.6]
print(f'K={K}, N={N}: хи-квадрат среднее {np.mean([r[1] for r in res]):.1f} (ожидание 255±22.6), макс {max(r[1] for r in res):.1f}; k с хи2 > 255+4σ: {[(r[0], round(r[1])) for r in bad]}')
print('k=1..6:', [(r[0], round(r[1]), round(r[2], 2)) for r in res[:6]])

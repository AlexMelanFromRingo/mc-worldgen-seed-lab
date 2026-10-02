"""java.util.Random / LegacyRandomSource игры (48-битный LCG) и Mth.getSeed — для выбора варианта модели по позиции и шумов оттенка.
Чистый Python; эталон для C-ядра (mesh.c реализует то же самое)."""

MASK48 = (1 << 48) - 1
MULT = 0x5DEECE66D
INC = 0xB


def _i32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _i64(v):
    v &= 0xFFFFFFFFFFFFFFFF
    return v - (1 << 64) if v & (1 << 63) else v


def mth_get_seed(x, y, z):
    """Mth.getSeed(x, y, z): `x * 3129871` — int-умножение с переполнением; остальное long."""
    seed = (_i32(x * 3129871)) ^ (_i64(z * 116129781)) ^ y
    seed = _i64(seed)
    seed = _i64(_i64(seed * seed) * 42317861 + _i64(seed * 11))
    return seed >> 16


class LegacyRandom:
    __slots__ = ('seed',)

    def __init__(self, seed=0):
        self.set_seed(seed)

    def set_seed(self, seed):
        self.seed = (seed ^ MULT) & MASK48

    def next(self, bits):
        self.seed = (self.seed * MULT + INC) & MASK48
        return _i32(self.seed >> (48 - bits))

    def next_int(self, bound):
        if bound <= 0:
            raise ValueError('bound')
        if (bound & (bound - 1)) == 0:
            return _i32((bound * self.next(31)) >> 31)
        while True:
            bits = self.next(31)
            val = bits % bound
            if _i32(bits - val + (bound - 1)) >= 0:
                return val

    def next_long(self):
        hi = self.next(32)
        lo = self.next(32)
        return _i64((hi << 32) + lo)

    def next_double(self):
        return ((self.next(26) << 27) + self.next(27)) * 1.1102230246251565e-16

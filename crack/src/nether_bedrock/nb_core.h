/*
 * nb_core.h — ядро восстановления structure seed по бедроку потолка/пола Незера (Minecraft 26.1 / 26.2 / 26.3).
 * Общий host+device код: LCG Java, позиционный хэш Mth.getSeed, проверка интервалом, обращение nextLong().
 *
 * ФОРМУЛА (сверена с реальным кодом игры, см. docs/23-crack-nether-bedrock.md и docs/12 §1.1):
 *   R   = LegacyRandomSource(worldSeed).nextLong()                     (RandomState: random.forkPositional())
 *   F_n = LegacyRandomSource(hash(n) ^ R).nextLong()                    (getOrCreateRandomFactory(n) = fromHashOf(n).forkPositional())
 *   n ∈ {"minecraft:bedrock_roof" (hash 343340730), "minecraft:bedrock_floor" (hash 2042456806)}
 *   блок (x,y,z):  state0 = (Mth.getSeed(x,y,z) ^ F_n ^ 0x5DEECE66D) mod 2^48,  state1 = (state0*M + 11) mod 2^48,
 *                  nextFloat() = (state1 >> 24) * 2^-24
 *   пол:    бедрок <=> nextFloat() <  p(y),   p(y) = Mth.map(y, 0,   5,   1, 0) = (5-y)/5      (y=0 всегда, y>=5 никогда)
 *   потолок: бедрок <=> !(nextFloat() < p(y)), p(y) = Mth.map(y, 122, 127, 1, 0) = (127-y)/5    (y=127 всегда, y<=122 никогда)
 * Значим только 48-битный младший кусок F и R => по бедроку определяется 48-битный structure seed.
 */
#ifndef NB_CORE_H
#define NB_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __CUDACC__
#define NB_HD __host__ __device__ __forceinline__
#define NB_TPL __host__ __device__       /* шаблоны: на хосте инстанцируются с host-функторами (предупреждения nvcc 20011/20013/20015 безвредны) */
#pragma nv_diag_suppress 20011
#pragma nv_diag_suppress 20013
#pragma nv_diag_suppress 20014
#pragma nv_diag_suppress 20015
#else
#define NB_HD static inline
#define NB_TPL static inline
#endif

typedef uint64_t u64;
typedef int64_t  i64;
typedef uint32_t u32;
typedef int32_t  i32;

#define NB_MASK48 0xFFFFFFFFFFFFULL
#define NB_MUL    0x5DEECE66DULL
#define NB_ADD    11ULL
#define NB_MINV   0xDFE05BCB1365ULL     /* M^-1 mod 2^48 (проверяется в selftest) */
#define NB_ROOF_HASH  343340730ULL      /* "minecraft:bedrock_roof".hashCode()  */
#define NB_FLOOR_HASH 2042456806ULL     /* "minecraft:bedrock_floor".hashCode() */
#define NB_ONE48  (1ULL << 48)

/* ---------- Java LCG ---------- */
NB_HD u64 nb_step(u64 s) { return (s * NB_MUL + NB_ADD) & NB_MASK48; }
/* внутреннее состояние после setSeed(seed) */
NB_HD u64 nb_scramble(u64 seed) { return (seed ^ NB_MUL) & NB_MASK48; }
/* nextLong() по ВНУТРЕННЕМУ состоянию s0 (до первого next): полные 64 бита */
NB_HD u64 nb_nextlong_state(u64 s0) {
    u64 s1 = nb_step(s0), s2 = nb_step(s1);
    i32 hi = (i32)(s1 >> 16), lo = (i32)(s2 >> 16);
    return ((u64)(u32)hi << 32) + (u64)(i64)lo;
}
/* Random(seed).nextLong(), полные 64 бита */
NB_HD u64 nb_nextlong_seed(u64 seed) { return nb_nextlong_state(nb_scramble(seed)); }
/* nextFloat() по состоянию state1 (после одного next): (state1>>24) * 2^-24 (точно как (float)next(24)*5.9604645E-8f) */
NB_HD float nb_float_of_state1(u64 s1) { return (float)(u32)(s1 >> 24) * 5.9604645E-8f; }

/* Mth.getSeed(x,y,z) */
NB_HD i64 nb_mth_getSeed(i32 x, i32 y, i32 z) {
    i64 i = (i64)(i32)((u32)x * 3129871u) ^ ((i64)z * 116129781LL) ^ (i64)y;
    i = (i64)((u64)i * (u64)i * 42317861ULL + (u64)i * 11ULL);
    return i >> 16;
}
/* h(x,y,z) = (Mth.getSeed ^ 0x5DEECE66D) & MASK48  — XOR-часть state0, не зависящая от F */
NB_HD u64 nb_poshash(i32 x, i32 y, i32 z) { return ((u64)nb_mth_getSeed(x, y, z) ^ NB_MUL) & NB_MASK48; }
/* nextFloat() для блока при заданном фабричном F (любые 64 бита; значимы младшие 48) */
NB_HD float nb_bedrock_float(u64 F, i32 x, i32 y, i32 z) {
    u64 s0 = (nb_poshash(x, y, z) ^ F) & NB_MASK48;
    return nb_float_of_state1(nb_step(s0));
}
/* фабричные F для заданного structure/world seed (младшие 48 бит W определяют всё) */
NB_HD u64 nb_base_R(u64 worldSeed) { return nb_nextlong_seed(worldSeed); }
NB_HD u64 nb_factory_F(u64 R, u64 nameHash) { return nb_nextlong_seed(nameHash ^ R); }

/* ---------- обращение nextLong(): по младшим 48 битам результата находит ВСЕ внутренние состояния s0 ----------
 * nextLong = (hi<<32) + lo,  hi=(int)(s1>>16),  lo=(int)(s2>>16),  s2 = s1*M+11.
 * T48 = nextLong mod 2^48 даёт: s2[16..47] = T48[0..31] (точно) и s1[16..31] (с поправкой на знак lo).
 * Перебор 16 неизвестных младших бит s2 (e2): s1 = (s2-11)*M^-1, условие s1[16..31] == m проверяется 32-битной арифметикой
 * (s1 mod 2^32 = B0 + e2*M^-1 mod 2^32, шаг c = M^-1 mod 2^32) — одно сложение на итерацию. В среднем 1 решение (0..несколько).
 * Возвращает число найденных s0 (внутренних состояний; seed аргумента Random = s0 ^ 0x5DEECE66D). Каждое решение проверено. */
#define NB_REV_MAX 16
NB_HD int nb_reverse_nextlong48(u64 T48, u64 *out_s0) {
    const u32 Y = (u32)T48;                                     /* s2 >> 16 */
    const u32 m = (u32)(((T48 >> 32) + (u64)(Y >> 31)) & 0xFFFFu); /* (s1 >> 16) & 0xFFFF */
    const u64 B0 = ((((u64)Y << 16) - NB_ADD) * NB_MINV) & NB_MASK48;  /* s1 при e2 = 0 */
    const u32 c = (u32)NB_MINV;
    u32 x = (u32)B0 - (m << 16);
    int n = 0;
    for (u32 e2 = 0; e2 < 65536u; e2++, x += c) {
        if (x < 65536u) {
            u64 s2 = ((u64)Y << 16) | e2;
            u64 s1 = ((s2 - NB_ADD) * NB_MINV) & NB_MASK48;
            u64 s0 = ((s1 - NB_ADD) * NB_MINV) & NB_MASK48;
            if ((nb_nextlong_state(s0) & NB_MASK48) == T48 && n < NB_REV_MAX) out_s0[n++] = s0;
        }
    }
    return n;
}

/* ---------- интервальная проверка наблюдения ----------
 * Наблюдение = интервал [lo, lo+len) для state1 = ((F ^ h)*M + 11) mod 2^48.
 * Слой b: младшие b бит F неизвестны, P = F с обнулёнными младшими b битами. Тогда
 *   (F^h) = V + w,  V = P ^ (h & ~lbm),  w ∈ [0, 2^b)  => state1 ∈ дуга [V*M+11, V*M+11 + W],  W = (2^b-1)*M.
 * Дуга пересекает [lo, lo+len) (mod 2^48) <=> ((V*M + 11 - lo + W) mod 2^48) < len + W  (при len + W < 2^48; иначе проверка пуста).
 * Условие b <= 13 (W < 2^48). Хранится как: pass <=> (((P ^ h') * M + C) & MASK48) < D.  */
struct NbChk { u64 h, C, D; };

NB_HD bool nb_chk_pass(u64 P, const NbChk &c) {
    u64 v = P ^ c.h;
    u32 vl = (u32)v, vh = (u32)(v >> 32);
    u64 lo = (u64)vl * 0xDEECE66Du;
    u32 hi = (u32)(lo >> 32) + vl * 5u + vh * 0xDEECE66Du;    /* старшие 32 бита произведения (нужны только младшие 16) */
    u64 t = (((u64)hi << 32) | (u32)lo) + c.C;
    return (t & NB_MASK48) < c.D;
}


/* ---------- быстрый необходимый фильтр верхнего слоя (32-битный) ----------
 * Из t = (X*M + C) mod 2^48 берутся только старшие 32 бита (биты 16..47) произведения и константы: th' = Yh + (C>>16) + 1 (mod 2^32),
 * где Yh = (X*M mod 2^48) >> 16. Всегда  (t>>16) <= th' <= (t>>16)+1  (перенос из младших 16 бит отброшен, +1 его перекрывает), поэтому
 *   t < D  =>  th' <= min((D>>16)+1, 2^32-1)        — ложноотрицательных нет; ложноположительные (~2^-32) снимаются нижними слоями.
 * Запись: {h mod 2^32, h>>32, (C>>16)+1, Dlim} — 16 байт (одна 128-битная загрузка). */
struct NbChk32 { u32 hl, hh, c1, dl; };
static inline NbChk32 nb_chk32_of(const NbChk &c) {
    NbChk32 r; r.hl = (u32)c.h; r.hh = (u32)(c.h >> 32);
    r.c1 = (u32)(c.C >> 16) + 1u;
    u64 d = (c.D >> 16) + 1ULL; r.dl = d > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (u32)d;
    return r;
}
#ifdef __CUDACC__
NB_HD u32 nb_yh32(u32 xl, u32 xh) {       /* биты 16..47 от (x*M) mod 2^48, x = xh:xl (48 бит) */
    u64 lo = (u64)xl * 0xDEECE66Du;
    u32 hi = (u32)(lo >> 32) + xl * 5u + xh * 0xDEECE66Du;
    return (u32)((((u64)hi << 32) | (u32)lo) >> 16);
}
#else
static inline u32 nb_yh32(u32 xl, u32 xh) {
    u64 lo = (u64)xl * 0xDEECE66Du;
    u32 hi = (u32)(lo >> 32) + xl * 5u + xh * 0xDEECE66Du;
    return (u32)((((u64)hi << 32) | (u32)lo) >> 16);
}
#endif
NB_HD bool nb_chk32_pass(u32 pl, u32 ph, const NbChk32 &c) {
    u32 y = nb_yh32(pl ^ c.hl, ph ^ c.hh);
    return (u32)(y + c.c1) <= c.dl;
}


/* ---------- Gray-обход группы из 32 соседних префиксов (быстрый плотный фильтр) ----------
 * Группа: idx = 32*G + t, t = 0..31 (P = idx<<K). Для проверки c  X(t) = Xg + ((t ^ hs) << K), Xg = P_группы ^ (h' без битов K..K+4), hs = биты K..K+4 числа h'.
 * Значит  Y(t) = (X*M + C) mod 2^48 = Base + TU[t ^ hs],  TU[u] = (u<<K)*M.  В Gray-порядке t_s = s ^ (s>>1) на шаге s -> s+1 переворачивается
 * один бит k = ctz(s+1) числа t, X меняется на ±(1<<(K+k)), а Y — на ±W_k, W_k = ((1<<k)<<K)*M (знак зависит от бита u = t^hs: известен заранее).
 * Поэтому значение проверки на каждом шаге обновляется ОДНИМ сложением с константой из таблицы (старшие 32 бита Y; накопленная ошибка отбрасывания
 * переносов <= 32 единиц по 2^16, перекрывается смещением +32 в c1 и dl). Фильтр необходимый: ложноотрицательных нет. */
#define NB_GRAY_BIAS 32u
struct NbGray {
    u32 hl, hh, c1, dl;
    u32 delta[31];
};
static inline NbGray nb_gray_of(const NbChk &c, int K) {
    NbGray g;
    const u64 gm = 31ULL << K;
    u64 hg = c.h & ~gm & NB_MASK48;
    u64 hs = (c.h >> K) & 31ULL;
    u64 Cp = (c.C + (((hs << K) * NB_MUL) & NB_MASK48)) & NB_MASK48;      /* C' = C + TU[hs]  (t = 0 => u = hs) */
    g.hl = (u32)hg; g.hh = (u32)(hg >> 32);
    g.c1 = (u32)(Cp >> 16) + NB_GRAY_BIAS;
    u64 d = (c.D >> 16) + (u64)NB_GRAY_BIAS;
    g.dl = d > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (u32)d;
    for (int s = 0; s < 31; s++) {
        int k = 0; while (!(((s + 1) >> k) & 1)) k++;                     /* ctz(s+1) */
        u32 gr = (u32)(s ^ (s >> 1));
        u32 tb = (gr >> k) & 1u, hb = (u32)((hs >> k) & 1ULL);
        u64 W = (((1ULL << k) << K) * NB_MUL) & NB_MASK48;
        u64 delta = (tb ^ hb) ? ((NB_ONE48 - W) & NB_MASK48) : W;
        g.delta[s] = (u32)(delta >> 16);
    }
    return g;
}
/* вакуозная запись (всегда проходит) */
static inline NbGray nb_gray_vacuous() { NbGray g; g.hl = g.hh = 0; g.c1 = 0; g.dl = 0xFFFFFFFFu; for (int s = 0; s < 31; s++) g.delta[s] = 0; return g; }

/* точная проверка (b=0): F полностью известно; lo/len — интервал state1 */
NB_HD bool nb_exact_in(u64 F48, u64 h, u64 lo, u64 len) {
    u64 s1 = nb_step((F48 ^ h) & NB_MASK48);
    return ((s1 - lo) & NB_MASK48) < len;
}

/* ---------- обход слоёв (DFS по младшим битам) ----------
 * Tab: n(b) — число проверок слоя b; at(b, j) — проверка j слоя b (отсортированы: самые селективные первыми).
 * idx — индекс префикса (старшие 48-K бит F). Emit: emit(F) для каждого F, прошедшего все слои до b=0. */
template <class Tab>
NB_TPL bool nb_layer_pass(const Tab &tab, int b, u64 P) {
    const int n = tab.n(b);
    for (int j = 0; j < n; j++)
        if (!nb_chk_pass(P, tab.at(b, j))) return false;
    return true;
}

template <class Tab, class Emit>
NB_TPL void nb_dfs_below(const Tab &tab, int K, u64 P, Emit &emit) {
    /* (P, K) уже прошёл слой K; раскрываем бит b-1 слоя b */
    if (K == 0) { emit(P); return; }
    u64 stP[16]; int stB[16]; int sp = 0;
    stP[sp] = P;                          stB[sp] = K - 1; sp++;
    stP[sp] = P | (1ULL << (K - 1));      stB[sp] = K - 1; sp++;
    while (sp > 0) {
        sp--;
        u64 p = stP[sp]; int b = stB[sp];
        if (!nb_layer_pass(tab, b, p)) continue;
        if (b == 0) { emit(p); continue; }
        stP[sp] = p;                      stB[sp] = b - 1; sp++;
        stP[sp] = p | (1ULL << (b - 1));  stB[sp] = b - 1; sp++;
    }
}

#endif

/* mc_noise.h — ImprovedNoise/Perlin/NormalNoise/Simplex, два численных режима:
 *   MC_NOISE_DOUBLE (26.1, 26.2):  ImprovedNoise/PerlinNoise/NormalNoise на double
 *   MC_NOISE_FLOAT  (26.3):        GradientNoise/PerlinNoise/NoiseStack на float
 * Источники: 26.2 synth/ImprovedNoise.java, PerlinNoise.java, NormalNoise.java, SimplexNoise.java;
 *            26.3 synth/GradientNoise.java, PerlinNoise.java, NormalNoise.java, NoiseStack.java, LegacyFbmInitializer.java
 *
 * Разделение:  McNoiseSpec  — НЕ зависит от seed (октавы, множители, MD5 имён октав); строится на хосте.
 *              McNormal     — зависит от seed (таблицы перестановок октав); создаётся на хосте/устройстве.
 * Компилировать с -ffp-contract=off / --fmad=false (бит-точность с Java)!
 */
#ifndef MC_NOISE_H
#define MC_NOISE_H
#include "mc_rng.h"

#define MC_NOISE_DOUBLE 0
#define MC_NOISE_FLOAT  1

/* ---------------- градиенты (ImprovedNoise / SimplexNoise / GradientNoise) ---------------- */
MC_HD MC_INLINE void mc_grad(int idx, int *gx, int *gy, int *gz) {
    static const signed char G[16][3] = {
        {1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
        {0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1},{1,1,0},{0,-1,1},{-1,1,0},{0,-1,-1}};
    *gx = G[idx][0]; *gy = G[idx][1]; *gz = G[idx][2];
}

/* ---------------- один октавный шум (ImprovedNoise / GradientNoise) ---------------- */
typedef struct { double xo, yo, zo; u8 p[256]; } McImproved;

#define MC_IMPROVED_INIT_BODY(NEXT_DOUBLE, NEXT_INT_BOUND, RND, SCALE)          \
    n->xo = NEXT_DOUBLE(RND) * (SCALE);                                        \
    n->yo = NEXT_DOUBLE(RND) * (SCALE);                                        \
    n->zo = NEXT_DOUBLE(RND) * (SCALE);                                        \
    for (int i = 0; i < 256; i++) n->p[i] = (u8)i;                             \
    for (int i = 0; i < 256; i++) {                                            \
        int off = NEXT_INT_BOUND(RND, 256 - i);                                \
        u8 t = n->p[i]; n->p[i] = n->p[i + off]; n->p[i + off] = t;            \
    }

MC_HD MC_INLINE void improved_init_xoro(McImproved *n, McXoro *r, double scale) {
    MC_IMPROVED_INIT_BODY(xoro_next_double, xoro_next_int_bound, r, scale)
}
MC_HD MC_INLINE void improved_init_lcg(McImproved *n, McLcg *r, double scale) {
    MC_IMPROVED_INIT_BODY(lcg_next_double, lcg_next_int_bound, r, scale)
}
MC_HD MC_INLINE int mc_p(const McImproved *n, int x) { return n->p[x & 0xFF]; }

/* ---- режим DOUBLE: ImprovedNoise.noise(x,y,z) при yScale = 0 (26.1/26.2) ---- */
MC_HD MC_INLINE double mc_lerp_d(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
MC_HD MC_INLINE double mc_smooth_d(double x) { return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
MC_HD MC_INLINE double mc_graddot_d(int h, double x, double y, double z) {
    int gx, gy, gz; mc_grad(h & 15, &gx, &gy, &gz);
    return gx * x + gy * y + gz * z;
}
MC_HD MC_INLINE double improved_noise_d(const McImproved *n, double _x, double _y, double _z) {
    double x = _x + n->xo, y = _y + n->yo, z = _z + n->zo;
    i32 xf = mc_floor_d(x), yf = mc_floor_d(y), zf = mc_floor_d(z);
    double xr = x - xf, yr = y - yf, zr = z - zf;
    int x0 = mc_p(n, xf), x1 = mc_p(n, xf + 1);
    int xy00 = mc_p(n, x0 + yf), xy01 = mc_p(n, x0 + yf + 1);
    int xy10 = mc_p(n, x1 + yf), xy11 = mc_p(n, x1 + yf + 1);
    double d000 = mc_graddot_d(mc_p(n, xy00 + zf),     xr,       yr,       zr);
    double d100 = mc_graddot_d(mc_p(n, xy10 + zf),     xr - 1.0, yr,       zr);
    double d010 = mc_graddot_d(mc_p(n, xy01 + zf),     xr,       yr - 1.0, zr);
    double d110 = mc_graddot_d(mc_p(n, xy11 + zf),     xr - 1.0, yr - 1.0, zr);
    double d001 = mc_graddot_d(mc_p(n, xy00 + zf + 1), xr,       yr,       zr - 1.0);
    double d101 = mc_graddot_d(mc_p(n, xy10 + zf + 1), xr - 1.0, yr,       zr - 1.0);
    double d011 = mc_graddot_d(mc_p(n, xy01 + zf + 1), xr,       yr - 1.0, zr - 1.0);
    double d111 = mc_graddot_d(mc_p(n, xy11 + zf + 1), xr - 1.0, yr - 1.0, zr - 1.0);
    double xa = mc_smooth_d(xr), ya = mc_smooth_d(yr), za = mc_smooth_d(zr);
    double a0 = mc_lerp_d(za, mc_lerp_d(ya, mc_lerp_d(xa, d000, d100), mc_lerp_d(xa, d010, d110)),
                              mc_lerp_d(ya, mc_lerp_d(xa, d001, d101), mc_lerp_d(xa, d011, d111)));
    /* Java: lerp3(a1=x,a2=y,a3=z,...) = lerp(a3, lerp2(a1,a2,x000,x100,x010,x110), lerp2(a1,a2,x001,...))
     *       lerp2(a1,a2,x00,x10,x01,x11) = lerp(a2, lerp(a1,x00,x10), lerp(a1,x01,x11)) */
    return a0;
}
/* PerlinNoise.wrap (26.2): x - lfloor(x/2^25 + 0.5) * 2^25 */
MC_HD MC_INLINE double mc_wrap_old(double x) { return x - (double)mc_lfloor_d(x / 3.3554432E7 + 0.5) * 3.3554432E7; }

/* ---- режим FLOAT: GradientNoise/PerlinNoise.get (26.3) ---- */
MC_HD MC_INLINE float mc_lerp_f(float a, float p0, float p1) { return p0 + a * (p1 - p0); }
MC_HD MC_INLINE float mc_smooth_f(float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
MC_HD MC_INLINE float mc_graddot_f(int h, float x, float y, float z) {
    int gx, gy, gz; mc_grad(h & 15, &gx, &gy, &gz);
    return (float)gx * x + (float)gy * y + (float)gz * z;
}
MC_HD MC_INLINE double mc_wrap_new(double x) {
    const double HALF = 16777215.999999998;    /* Math.nextDown(1.6777216E7) */
    return (x >= -HALF && x < HALF) ? x : x - (double)mc_lfloor_d(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}
MC_HD MC_INLINE float improved_noise_f(const McImproved *n, double _x, double _y, double _z) {
    double x = mc_wrap_new(_x) + n->xo;
    double y = mc_wrap_new(_y) + n->yo;
    double z = mc_wrap_new(_z) + n->zo;
    i32 xf = mc_floor_d(x), yf = mc_floor_d(y), zf = mc_floor_d(z);
    float xr = (float)(x - xf), yr = (float)(y - yf), zr = (float)(z - zf);
    int x0 = mc_p(n, xf), x1 = mc_p(n, xf + 1);
    int xy00 = mc_p(n, x0 + yf), xy01 = mc_p(n, x0 + yf + 1);
    int xy10 = mc_p(n, x1 + yf), xy11 = mc_p(n, x1 + yf + 1);
    float d000 = mc_graddot_f(mc_p(n, xy00 + zf),     xr,        yr,        zr);
    float d100 = mc_graddot_f(mc_p(n, xy10 + zf),     xr - 1.0f, yr,        zr);
    float d010 = mc_graddot_f(mc_p(n, xy01 + zf),     xr,        yr - 1.0f, zr);
    float d110 = mc_graddot_f(mc_p(n, xy11 + zf),     xr - 1.0f, yr - 1.0f, zr);
    float d001 = mc_graddot_f(mc_p(n, xy00 + zf + 1), xr,        yr,        zr - 1.0f);
    float d101 = mc_graddot_f(mc_p(n, xy10 + zf + 1), xr - 1.0f, yr,        zr - 1.0f);
    float d011 = mc_graddot_f(mc_p(n, xy01 + zf + 1), xr,        yr - 1.0f, zr - 1.0f);
    float d111 = mc_graddot_f(mc_p(n, xy11 + zf + 1), xr - 1.0f, yr - 1.0f, zr - 1.0f);
    float xa = mc_smooth_f(xr), ya = mc_smooth_f(yr), za = mc_smooth_f(zr);
    return mc_lerp_f(za, mc_lerp_f(ya, mc_lerp_f(xa, d000, d100), mc_lerp_f(xa, d010, d110)),
                         mc_lerp_f(ya, mc_lerp_f(xa, d001, d101), mc_lerp_f(xa, d011, d111)));
}

/* ======================================================================================
 *                                     NormalNoise
 * ====================================================================================== */
#define MC_MAX_OCT    12   /* октав в noise/NAME.json (макс. 9 у continentalness) */
#define MC_MAX_LAYERS 24   /* слоёв (first+second) на один NormalNoise */

/* Kahan-суммирование как DoubleStream.sum() в JDK 17+ (Collectors.sumWithCompensation/computeFinalSum) */
MC_HD MC_INLINE double mc_stream_sum(const double *v, int n) {
    double sum = 0.0, comp = 0.0;
    for (int i = 0; i < n; i++) {
        double tmp = v[i] - comp;
        double velvel = sum + tmp;
        comp = (velvel - sum) - tmp;
        sum = velvel;
    }
    return sum - comp;
}

/* Параметры одного шума из worldgen/noise/NAME.json (обе формы) */
typedef struct {
    int    new_format;              /* 0: firstOctave+amplitudes (26.1/26.2);  1: base_octave/... (26.3) */
    int    first_octave;            /* firstOctave / base_octave */
    int    n_amp;                   /* len(amplitudes) / octave_count */
    double amp[MC_MAX_OCT];         /* amplitudes / amplitude_modifiers (если has_mod) */
    double base_amplitude;          /* 26.3 */
    int    normalize;               /* 26.3: 0 DISABLED, 1 ENABLED, 2 LEGACY */
    int    has_mod;                 /* 26.3: заданы amplitude_modifiers */
} McNoiseParams;

/* Спецификация — не зависит от seed. Строится на хосте (или на устройстве, если хэши заданы таблицей). */
typedef struct {
    int mode;                       /* MC_NOISE_DOUBLE/FLOAT */
    int legacy;                     /* 1: Nether-биомный шум (LegacyRandomSource, последовательная инициализация) */
    int n_levels;                   /* число присутствующих октав в ОДНОМ стеке (first/second) */
    int octs;                       /* размер массива амплитуд / octave_count */
    int first_octave;
    int lvl_index[MC_MAX_OCT];      /* индекс i октавы в массиве амплитуд для k-го присутствующего уровня */
    u64 hash_lo[MC_MAX_OCT], hash_hi[MC_MAX_OCT];   /* md5("octave_<n>") — для Xoroshiro-инициализации */
    double freq[MC_MAX_OCT];        /* DOUBLE: factor уровня; FLOAT: Layer.frequency первого стека */
    double amp_d[MC_MAX_OCT];       /* DOUBLE: amplitudes[i] */
    double vf_d[MC_MAX_OCT];        /* DOUBLE: valueFactor уровня */
    double value_factor;            /* DOUBLE: NormalNoise.valueFactor */
    float  amp_f[MC_MAX_OCT];       /* FLOAT: Layer.amplitude */
} McNoiseSpec;

/* Экземпляр — зависит от seed */
typedef struct {
    McImproved lev[2 * MC_MAX_OCT]; /* [0..n) первый стек, [n..2n) второй */
} McNormal;

#ifndef __CUDA_ARCH__
static inline void mc_octave_name(char *nm, int o) {
    int len = 0; const char *pre = "octave_"; while (*pre) nm[len++] = *pre++;
    if (o < 0) { nm[len++] = '-'; o = -o; }
    char dg[12]; int dn = 0; if (o == 0) dg[dn++] = '0';
    while (o) { dg[dn++] = (char)('0' + o % 10); o /= 10; }
    while (dn) nm[len++] = dg[--dn];
    nm[len] = 0;
}
static inline void mc_fill_hashes(McNoiseSpec *s) {
    for (int k = 0; k < s->n_levels; k++) {
        char nm[32]; mc_octave_name(nm, s->first_octave + s->lvl_index[k]);
        mc_md5_seed128(nm, &s->hash_lo[k], &s->hash_hi[k]);
    }
}

/* 26.1/26.2: NormalNoise(random, NoiseParameters, useNewInitialization) — константы */
static inline void mc_spec_old(McNoiseSpec *s, const McNoiseParams *P, int legacy) {
    memset(s, 0, sizeof(*s));
    s->mode = MC_NOISE_DOUBLE; s->legacy = legacy;
    int octs = P->n_amp; s->octs = octs; s->first_octave = P->first_octave;
    int zeroIdx = -P->first_octave;
    double lowestIn  = pow(2.0, (double)(-zeroIdx));
    double lowestVal = pow(2.0, (double)(octs - 1)) / (pow(2.0, (double)octs) - 1.0);
    int minO = 0x7fffffff, maxO = -0x7fffffff - 1;
    for (int i = 0; i < octs; i++) if (P->amp[i] != 0.0) { if (i < minO) minO = i; if (i > maxO) maxO = i; }
    s->value_factor = 0.16666666666666666 / (0.1 * (1.0 + 1.0 / (double)(maxO - minO + 1)));
    double f = lowestIn, vf = lowestVal; int k = 0;
    for (int i = 0; i < octs; i++) {
        if (P->amp[i] != 0.0) { s->freq[k] = f; s->amp_d[k] = P->amp[i]; s->vf_d[k] = vf; s->lvl_index[k] = i; k++; }
        f *= 2.0; vf /= 2.0;
    }
    s->n_levels = k;
    mc_fill_hashes(s);
}

/* 26.3: NormalNoise(Parameters) + create()/createForLegacyNetherBiome() — константы */
static inline void mc_spec_new(McNoiseSpec *s, const McNoiseParams *P, int legacy) {
    memset(s, 0, sizeof(*s));
    s->mode = MC_NOISE_FLOAT; s->legacy = legacy;
    int octaveCount = P->n_amp; s->octs = octaveCount; s->first_octave = P->first_octave;
    /* buildOctaves */
    double frequency = pow(2.0, (double)P->first_octave);
    double amplitude = P->base_amplitude;
    if (P->normalize != 0)
        amplitude *= pow(0.5, (double)(-(octaveCount - 1))) / (pow(0.5, (double)(-octaveCount)) - 1.0);
    double oa[MC_MAX_OCT], of[MC_MAX_OCT]; int oi[MC_MAX_OCT]; int m = 0;
    for (int i = 0; i < octaveCount; i++) {
        double mod = P->has_mod ? P->amp[i] : 1.0;
        if (mod != 0.0) { oa[m] = amplitude * mod; of[m] = frequency; oi[m] = i; m++; }
        frequency *= 2.0; amplitude *= 0.5;
    }
    /* computeNormalizationFactor */
    double absamp[MC_MAX_OCT]; for (int i = 0; i < m; i++) absamp[i] = fabs(oa[i]);
    double target = mc_stream_sum(absamp, m);
    double variance = 0.0;
    for (int i = 0; i < m; i++) { double d = 0.2702247831245211 * absamp[i]; variance += d * d; }
    double inputDev = sqrt(variance);
    double nf = 0.0;
    if (inputDev != 0.0) nf = (target * 0.3333333333333333) / (inputDev * sqrt(2.0));
    if (P->normalize == 2 && nf != 0.0) {
        int mn = 0x7fffffff, mx = -0x7fffffff - 1;
        for (int i = 0; i < octaveCount; i++) { double mod = P->has_mod ? P->amp[i] : 1.0; if (mod != 0.0) { if (i < mn) mn = i; if (i > mx) mx = i; } }
        double parity = P->base_amplitude * 0.5 * 0.3333333333333333 / (0.1 * (1.0 + 1.0 / (double)(mx - mn + 1)));
        nf = parity;
    }
    if (!legacy) {
        for (int k = 0; k < m; k++) {
            s->lvl_index[k] = oi[k]; s->freq[k] = of[k];
            s->amp_f[k] = (float)(nf * oa[k]);
        }
        s->n_levels = m;
    } else {
        /* createForLegacyNetherBiome: LegacyFbmInitializer + addStack(stack, 1.0 / 1.0181268882175227, valueFactor) */
        float outer = (float)(nf * P->base_amplitude);
        double factor = pow(2.0, (double)(-(-P->first_octave)));
        double vfac = pow(2.0, (double)(octaveCount - 1)) / (pow(2.0, (double)octaveCount) - 1.0);
        int k = 0;
        for (int i = 0; i < octaveCount; i++) {
            double amp_i = P->has_mod ? P->amp[i] : 1.0;
            if (amp_i != 0.0) {
                s->lvl_index[k] = i; s->freq[k] = factor * 1.0;
                s->amp_f[k] = (float)(vfac * amp_i) * outer;
                k++;
            }
            factor *= 2.0; vfac /= 2.0;
        }
        s->n_levels = k;
    }
    mc_fill_hashes(s);
}
#endif

/* ---- инициализация экземпляра ---- */
/* Xoroshiro-шум: random = positional.fromHashOf("minecraft:<noise>"); дальше два forkPositional (first/second) */
MC_HD MC_INLINE void normal_init_xoro(McNormal *nn, const McNoiseSpec *s, McXoro *random) {
    for (int st = 0; st < 2; st++) {
        McXoroPos pos = xoro_fork_positional(random);
        for (int k = 0; k < s->n_levels; k++) {
            McXoro r = xoro_from_hash(&pos, s->hash_lo[k], s->hash_hi[k]);
            improved_init_xoro(&nn->lev[st * s->n_levels + k], &r, 256.0);
        }
    }
}
/* Legacy Nether: PerlinNoise.createLegacyForLegacyNetherBiome / LegacyFbmInitializer, последовательно из одного LCG */
MC_HD MC_INLINE void normal_init_legacy_stack(McImproved *lev, const McNoiseSpec *s, McLcg *r) {
    int octs = s->octs, zeroIdx = -s->first_octave;
    McImproved zero; improved_init_lcg(&zero, r, 256.0);
    /* lvl_index -> k */
    int kOf[MC_MAX_OCT]; for (int i = 0; i < MC_MAX_OCT; i++) kOf[i] = -1;
    for (int k = 0; k < s->n_levels; k++) kOf[s->lvl_index[k]] = k;
    if (zeroIdx >= 0 && zeroIdx < octs && kOf[zeroIdx] >= 0) lev[kOf[zeroIdx]] = zero;
    for (int i = zeroIdx - 1; i >= 0; i--) {
        if (i < octs && kOf[i] >= 0) improved_init_lcg(&lev[kOf[i]], r, 256.0);
        else lcg_skip(r, 262);
    }
}
MC_HD MC_INLINE void normal_init_legacy(McNormal *nn, const McNoiseSpec *s, McLcg *r) {
    normal_init_legacy_stack(&nn->lev[0], s, r);
    normal_init_legacy_stack(&nn->lev[s->n_levels], s, r);
}

/* ---- вычисление ---- */
/* DOUBLE (26.1/26.2): NormalNoise.getValue */
MC_HD MC_INLINE double normal_get_d(const McNormal *nn, const McNoiseSpec *s, double x, double y, double z) {
    int n = s->n_levels;
    double x2 = x * 1.0181268882175227, y2 = y * 1.0181268882175227, z2 = z * 1.0181268882175227;
    double v1 = 0.0, v2 = 0.0;
    for (int k = 0; k < n; k++) {
        double f = s->freq[k];
        double a = improved_noise_d(&nn->lev[k], mc_wrap_old(x * f), mc_wrap_old(y * f), mc_wrap_old(z * f));
        v1 += s->amp_d[k] * a * s->vf_d[k];
    }
    for (int k = 0; k < n; k++) {
        double f = s->freq[k];
        double a = improved_noise_d(&nn->lev[n + k], mc_wrap_old(x2 * f), mc_wrap_old(y2 * f), mc_wrap_old(z2 * f));
        v2 += s->amp_d[k] * a * s->vf_d[k];
    }
    return (v1 + v2) * s->value_factor;
}
/* FLOAT (26.3): NoiseStack.get */
MC_HD MC_INLINE float normal_get_f(const McNormal *nn, const McNoiseSpec *s, double x, double y, double z) {
    int n = s->n_levels;
    float value = 0.0f;
    if (!s->legacy) {
        for (int k = 0; k < n; k++) {      /* слои: first_k, second_k, first_{k+1}, ... */
            double f = s->freq[k];
            value += s->amp_f[k] * improved_noise_f(&nn->lev[k], x * f, y * f, z * f);
            double f2 = f * 1.0181268882175227;
            value += s->amp_f[k] * improved_noise_f(&nn->lev[n + k], x * f2, y * f2, z * f2);
        }
    } else {                               /* слои: весь first-стек, затем весь second-стек */
        for (int k = 0; k < n; k++) {
            double f = s->freq[k];
            value += s->amp_f[k] * improved_noise_f(&nn->lev[k], x * f, y * f, z * f);
        }
        for (int k = 0; k < n; k++) {
            double f2 = s->freq[k] * 1.0181268882175227;
            value += s->amp_f[k] * improved_noise_f(&nn->lev[n + k], x * f2, y * f2, z * f2);
        }
    }
    return value;
}
/* Универсально: значение как double (для DOUBLE — точное; для FLOAT — float, расширенный до double) */
MC_HD MC_INLINE double normal_get(const McNormal *nn, const McNoiseSpec *s, double x, double y, double z) {
    return s->mode == MC_NOISE_DOUBLE ? normal_get_d(nn, s, x, y, z) : (double)normal_get_f(nn, s, x, y, z);
}

#endif /* MC_NOISE_H */

/* noise.h — шумы libmcgen поверх engine/ (RNG, MD5, градиентный шум).
 *
 * Две численные ветки (docs/05):
 *   «старая» (26.1/26.2): ImprovedNoise/PerlinNoise/NormalNoise на double, BlendedNoise со «смазыванием» по y;
 *   «новая»  (26.3+):     GradientNoise/PerlinNoise/SmearedPerlinNoise, NoiseStack с float-накоплением,
 *                          у каждого шума два пути — точечный get() и объёмный addToVolume() (числа различаются!).
 * Источники случайности: Xoroshiro128++ (позиционные фабрики, fromHashOf(MD5)) и Legacy (java.util.Random,
 * fromHashOf = String.hashCode ^ seed) — выбираются флагом legacy_random_source в noise_settings.
 */
#ifndef MCGEN_NOISE_H
#define MCGEN_NOISE_H
#include "util.h"
#include "mc_noise.h"     /* engine/: McXoro, McLcg, McImproved, improved_init_* , md5 */

/* ---------------- источник случайности (RandomSource) ---------------- */
typedef struct { int legacy; McXoro x; McLcg l; } Rnd;
typedef struct { int legacy; u64 lo, hi; i64 seed; } PosRnd;      /* PositionalRandomFactory */

Rnd rnd_xoro_seed(i64 seed);                 /* new XoroshiroRandomSource(seed) */
Rnd rnd_legacy_seed(i64 seed);               /* new LegacyRandomSource(seed) */
i64 rnd_next_long(Rnd *r);
i32 rnd_next_int_bound(Rnd *r, i32 bound);
double rnd_next_double(Rnd *r);
float rnd_next_float(Rnd *r);
void rnd_consume(Rnd *r, int n);
PosRnd rnd_fork_positional(Rnd *r);
Rnd pos_from_hash(const PosRnd *p, const char *name);   /* fromHashOf(String) */
Rnd pos_at(const PosRnd *p, i32 x, i32 y, i32 z);

/* ---------------- одна октава (ImprovedNoise / GradientNoise) ---------------- */
typedef McImproved GNoise;          /* xo,yo,zo + perm[256] */
void gn_init(GNoise *n, Rnd *r, double offset_scale);

/* новая ветка: точечные значения */
float gn_perlin_get_f(const GNoise *n, double x, double y, double z);
float gn_smeared_get_f(const GNoise *n, double fudge_y_scale, double x, double y, double z);
/* старая ветка: ImprovedNoise.noise(x,y,z,yScale,yFudge) (координаты уже обёрнуты вызывающим) */
double gn_noise_d(const GNoise *n, double x, double y, double z, double y_scale, double y_fudge);

/* ---------------- объём (DensityVolume 26.3) ---------------- */
typedef struct { int sx, sy, sz, x0, y0, z0, dx, dy, dz; } Vol;
static inline int vol_size(const Vol *v) { return v->sx * v->sy * v->sz; }
static inline int vol_idx(const Vol *v, int ix, int iy, int iz) { return iy + (ix + iz * v->sx) * v->sy; }
static inline int vol_bx(const Vol *v, int i) { return v->x0 + i * v->dx; }
static inline int vol_by(const Vol *v, int i) { return v->y0 + i * v->dy; }
static inline int vol_bz(const Vol *v, int i) { return v->z0 + i * v->dz; }
static inline int vol_max_x(const Vol *v) { return v->x0 + v->sx * v->dx - 1; }
static inline int vol_max_y(const Vol *v) { return v->y0 + v->sy * v->dy - 1; }
static inline int vol_max_z(const Vol *v) { return v->z0 + v->sz * v->dz - 1; }
static inline int vol_eq(const Vol *a, const Vol *b) {
    return a->sx == b->sx && a->sy == b->sy && a->sz == b->sz && a->x0 == b->x0 && a->y0 == b->y0 && a->z0 == b->z0 &&
           a->dx == b->dx && a->dy == b->dy && a->dz == b->dz;
}
int vol_index_of_block(const Vol *v, int bx, int by, int bz);   /* DensityVolume.indexOfBlock, −1 если нет */

void gn_perlin_add_volume(const GNoise *n, float *buf, const Vol *v, double xz_scale, double y_scale, float amp);
void gn_smeared_add_volume(const GNoise *n, double fudge_y_scale, float *buf, const Vol *v, double xz_scale, double y_scale, float amp);

/* ---------------- интервалы (net.minecraft.util.Interval, float) ---------------- */
typedef struct { float lo, hi; int nai; } Ival;
Ival iv_of(float lo, float hi);
static inline Ival iv_exact(float v) { return iv_of(v, v); }
static inline Ival iv_sym(float r) { return iv_of(-r, r); }
Ival iv_nai(void);
Ival iv_inf(void);
Ival iv_add(Ival a, Ival b);
Ival iv_sub(Ival a, Ival b);
Ival iv_mul(Ival a, Ival b);
Ival iv_reciprocal(Ival a);
Ival iv_div(Ival a, Ival b);
Ival iv_min(Ival a, Ival b);
Ival iv_max(Ival a, Ival b);
Ival iv_clamp(Ival a, float lo, float hi);
Ival iv_abs(Ival a);
Ival iv_square(Ival a);
Ival iv_pow(Ival base, Ival exp);
Ival iv_log(Ival a);
Ival iv_sign(Ival a);
Ival iv_lerp(Ival alpha, Ival first, Ival second);
Ival iv_encaps2(float a, float b);
Ival iv_encaps(const Ival *v, int n);
int iv_contains(Ival a, float v);
typedef float (*FloatOp)(float);
Ival iv_map_monotonic(Ival a, FloatOp op);

/* ---------------- NoiseStack (26.3) ---------------- */
enum { NL_PERLIN = 0, NL_SMEARED = 1 };
typedef struct { GNoise n; double freq; float amp; int kind; double fudge; } NLayer;
typedef struct { int n; NLayer *l; Ival range; } NStack;   /* range — NormalNoise.range (из параметров) */
float ns_get(const NStack *s, double x, double y, double z);
float ns_get2(const NStack *s, double x, double y);             /* Noise.get(x,y) — для 2D (не используется рельефом) */
void ns_add_volume(const NStack *s, float *buf, const Vol *v, double xz_scale, double y_scale, float amp);
void ns_free(NStack *s);

/* Параметры шума из worldgen/noise/<имя>.json (оба формата) */
typedef struct {
    int new_format;            /* 26.3: base_octave/octave_count/...; 26.1/26.2: firstOctave/amplitudes */
    int first_octave, count;
    double amp[32];            /* amplitude_modifiers (если has_mod) / amplitudes */
    int has_mod;
    double base_amplitude;
    int normalize;             /* 0 DISABLED, 1 ENABLED, 2 LEGACY */
} NoiseParams;
int noise_params_parse(const void *js /* Js* */, int new_format, NoiseParams *out, char *err, size_t errlen);

/* NormalNoise 26.3: create(random) и createForLegacyNetherBiome(random); range() */
void nn_new_create(NStack *out, const NoiseParams *P, Rnd *random);
void nn_new_create_legacy_nether(NStack *out, const NoiseParams *P, Rnd *random);
Ival nn_new_range(const NoiseParams *P);

/* BlendedNoise 26.3: три fbm-стека */
typedef struct { NStack min_lim, max_lim, main; } BlendFbm;
void blended_new_create(BlendFbm *out, Rnd *random, double y_scale, double smear_mult, double y_factor);
Ival blended_new_range(double y_scale, double smear_mult);

/* ---------------- старая ветка (26.1/26.2): PerlinNoise/NormalNoise на double ---------------- */
typedef struct {
    int n;                 /* размер массива амплитуд */
    int first_octave;
    GNoise *lev;           /* n элементов; присутствие — has[i] */
    u8 has[32];
    double amp[32];
    double lowest_in, lowest_val, max_value;
} OldPerlin;
typedef struct { OldPerlin first, second; double value_factor, max_value; } OldNormal;
void old_perlin_create(OldPerlin *p, Rnd *random, int first_octave, const double *amps, int n, int new_init);
double old_perlin_get(const OldPerlin *p, double x, double y, double z, double y_scale, double y_fudge);
double old_perlin_max_broken(const OldPerlin *p, double y_scale);
const GNoise *old_perlin_octave(const OldPerlin *p, int i);    /* getOctaveNoise(i) */
void old_perlin_free(OldPerlin *p);
void old_normal_create(OldNormal *nn, Rnd *random, const NoiseParams *P, int new_init);
double old_normal_get(const OldNormal *nn, double x, double y, double z);
void old_normal_free(OldNormal *nn);

typedef struct { OldPerlin min_lim, max_lim, main; double xz_mul, y_mul, xz_factor, y_factor, smear, max_value; } OldBlended;
void old_blended_create(OldBlended *b, Rnd *random, double xz_scale, double y_scale, double xz_factor, double y_factor, double smear);
double old_blended_compute(const OldBlended *b, int bx, int by, int bz);
void old_blended_free(OldBlended *b);

/* ---------------- Simplex 2D (End) ---------------- */
double simplex2_d(const GNoise *n, double x, double y);   /* 70*(n0+n1+n2) без смещений (old: getValue, new: (float)) */

#endif

/* feature.h — стадия FEATURES (декорации): внутренний интерфейс каркаса (поток W8).
 *
 * Устройство (подробно — docs/blender/features.md):
 *   feature.c         стадия: окно чанков, порядок чанков, регистрация типов, разбор конфигураций из JSON, освобождение
 *   feature_sort.c    списки фич биомов, FeatureSorter (глобальный индекс фич по шагам), множество фич чанка
 *   feature_region.c  «WorldGenRegion»: чтение/запись блоков окна 3×3 чанков, карты высот (в т. ч. ленивые WG), биом блока
 *   feature_prov.c    IntProvider / FloatProvider / HeightProvider / VerticalAnchor / WeightedList
 *   feature_bpred.c   BlockPredicate и RuleTest
 *   feature_bsp.c     BlockStateProvider
 *   placement.c       PlacementModifier'ы и FeaturePlacer (цепочка модификаторов → фича)
 *   feature_<группа>.c  типы фич (ore, disk, …) — каждый регистрируется в feature_register_all() (feature.c)
 *
 * Как добавить тип фичи: написать parse(FParse*, cfg) → конфигурация и place(FCtx*, cfg, x, y, z) → «поставлено ли что-то»,
 * объявить FeatType и добавить вызов feature_register_<группа>() в feature_register_all(). Как добавить модификатор — placement.c
 * (таблица PM_TYPES). Порядок и число вызовов ГСЧ — как в игре (проверка по эталону).
 */
#ifndef MCGEN_FEATURE_H
#define MCGEN_FEATURE_H
#include "mcgen_internal.h"
#include "blockstate.h"
#include "fluidpp.h"

/* ====================================================================== ГСЧ: WorldgenRandom поверх Xoroshiro
 * WorldgenRandom наследует LegacyRandomSource: все методы (nextInt(bound), nextLong, nextDouble, nextBoolean…) строятся на next(bits) =
 * (int)(xoroshiro.nextLong() >>> 64 − bits) по алгоритмам BitRandomSource — а НЕ на собственных методах XoroshiroRandomSource.
 * Состояние гауссиана (MarsagliaPolarGaussian) — часть WorldgenRandom и setSeed его не сбрасывает. */
typedef struct FRnd { McXoro x; int have_g; double next_g; } FRnd;
static inline void frnd_seed(FRnd *r, i64 seed) { r->x = xoro_from_long_seed(seed); }
static inline i32 frnd_next(FRnd *r, int bits) { return (i32)(u32)(xoro_next_long(&r->x) >> (64 - bits)); }
static inline i32 frnd_int(FRnd *r) { return frnd_next(r, 32); }
static inline i32 frnd_int_bound(FRnd *r, i32 bound) {
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * (i64)frnd_next(r, 31)) >> 31);
    i32 sample, modulo;
    do { sample = frnd_next(r, 31); modulo = sample % bound; } while ((i32)((u32)sample - (u32)modulo + (u32)(bound - 1)) < 0);
    return modulo;
}
static inline i64 frnd_long(FRnd *r) { i32 upper = frnd_next(r, 32); i32 lower = frnd_next(r, 32); return (i64)((u64)(i64)upper << 32) + (i64)lower; }
static inline int frnd_bool(FRnd *r) { return frnd_next(r, 1) != 0; }
static inline float frnd_float(FRnd *r) { return (float)frnd_next(r, 24) * 5.9604645E-8f; }
static inline double frnd_double(FRnd *r) {
    i32 upper = frnd_next(r, 26); i32 lower = frnd_next(r, 27);
    i64 combined = (i64)((u64)(i64)upper << 27) + (i64)lower;
    return (double)combined * 1.1102230246251565e-16;
}
double frnd_gauss(FRnd *r);
/* RandomSource.nextIntBetweenInclusive / Mth.nextInt / Mth.randomBetweenInclusive */
static inline i32 frnd_between(FRnd *r, i32 min, i32 max_incl) { return frnd_int_bound(r, max_incl - min + 1) + min; }
static inline i32 mth_next_int(FRnd *r, i32 min, i32 max_incl) { return min >= max_incl ? min : frnd_int_bound(r, max_incl - min + 1) + min; }
static inline float mth_next_float(FRnd *r, float min, float max) { return min >= max ? min : frnd_float(r) * (max - min) + min; }

/* ====================================================================== общие типы */
typedef struct FWorld FWorld;
typedef struct FCtx FCtx;
typedef struct FParse FParse;
typedef struct Feat Feat;
typedef struct Placed Placed;
typedef struct IntProv IntProv;
typedef struct FloatProv FloatProv;
typedef struct HeightProv HeightProv;
typedef struct BPred BPred;
typedef struct RuleTest RuleTest;
typedef struct BSProv BSProv;

/* направления Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST */
enum { DIR_DOWN = 0, DIR_UP = 1, DIR_NORTH = 2, DIR_SOUTH = 3, DIR_WEST = 4, DIR_EAST = 5 };
static const int DIR_DX[6] = { 0, 0, 0, 0, -1, 1 };
static const int DIR_DY[6] = { -1, 1, 0, 0, 0, 0 };
static const int DIR_DZ[6] = { 0, 0, -1, 1, 0, 0 };
int dir_from_name(const char *s);        /* "down"… → индекс, −1 */

/* ====================================================================== окно чанков (WorldGenRegion) */
typedef struct FChunk {
    int cx, cz;
    u16 *blocks;                 /* [y][z][x], height*256 */
    u8 *biomes;                  /* [qy][qz][qx], (height/4)*16 */
    i16 hm[HM__COUNT][256];      /* «первая свободная» y (Heightmap.getFirstAvailable): абсолютная, min_y если колонка пуста */
    u8 hm_has;                   /* биты праймированных карт */
    PPMarks *marks;              /* пометки пост-обработки (NULL у внешних колец) */
    u8 bio_mask[32];             /* биомы, присутствующие в чанке (256 бит) */
} FChunk;

struct FCtx {
    McWorld *w; const McGen *g; const BsTab *bs; FWorld *fw;
    FRnd *rnd;
    FChunk **grid; int gx0, gz0, gnx, gnz;     /* сетка чанков, доступных стадии */
    int ccx, ccz;                              /* центральный чанк (читать/писать можно только 3×3 вокруг него) */
    int min_y, height, sea_level;              /* мир */
    int gen_min_y, gen_depth;                  /* NoiseSettings (WorldGenerationContext) */
    int lazy_wg;                               /* 26.3+: карты WORLD_SURFACE_WG/OCEAN_FLOOR_WG праймятся при первом запросе и не обновляются */
    int st_air, st_cave_air, st_void_air, st_water, st_lava, bedrock_blk, plains;
    int fail;
    long n_chunks, n_calls, n_skipped;            /* счётчики потока */
};

static inline FChunk *fc_chunk(const FCtx *c, int x, int z) {
    int cx = x >> 4, cz = z >> 4;
    if (cx < c->ccx - 1 || cx > c->ccx + 1 || cz < c->ccz - 1 || cz > c->ccz + 1) return NULL;
    return c->grid[(cz - c->gz0) * c->gnx + (cx - c->gx0)];
}
static inline int fc_outside(const FCtx *c, int y) { return y < c->min_y || y >= c->min_y + c->height; }
/* BlockGetter.getBlockState: вне высот — void_air; чанк вне окна (ещё не сгенерирован игрой) — воздух */
static inline int fc_get(const FCtx *c, int x, int y, int z) {
    if (fc_outside(c, y)) return c->st_void_air;
    FChunk *ch = fc_chunk(c, x, z);
    return ch ? ch->blocks[((size_t)(y - c->min_y) * 16 + (z & 15)) * 16 + (x & 15)] : c->st_air;
}
static inline u32 fc_flags(const FCtx *c, int st) { return c->bs->flags[st]; }
static inline int fc_is_air(const FCtx *c, int st) { return (c->bs->flags[st] & BSF_AIR) != 0; }
/* WorldGenRegion.setBlock(pos, state, flags): вне окна записи — false; обновляет карты высот; помечает пост-обработку (если нет флага 16) */
int fc_set(FCtx *c, int x, int y, int z, int st, int flags);
/* LevelSection.setBlockState напрямую (OreFeature): без карт высот и пометок */
void fc_set_raw(FCtx *c, int x, int y, int z, int st);
/* ChunkAccess.markPosForPostProcessing(pos) для чанка позиции (внутри окна) */
void fc_mark(FCtx *c, int x, int y, int z);
/* Feature.markAboveForPostProcessing */
void fc_mark_above(FCtx *c, int x, int y, int z);
/* WorldGenRegion.getHeight(type, x, z): «первая свободная» y (карта праймится лениво) */
int fc_height(FCtx *c, int type, int x, int z);
/* WorldGenRegion.getBiome(pos): BiomeManager.getBiome по клеткам чанков окна; id биома libmcgen */
int fc_biome(const FCtx *c, int x, int y, int z);
/* биом клетки (qx,qy,qz) чанков окна (y зажат), чанк вне окна — plains */
int fc_biome_cell(const FCtx *c, int qx, int qy, int qz);
static inline int fc_is_empty_block(const FCtx *c, int x, int y, int z) { return fc_is_air(c, fc_get(c, x, y, z)); }
static inline int fc_ensure_can_write(const FCtx *c, int x, int z) { return fc_chunk(c, x, z) != NULL; }
/* пересчёт/праймирование карт чанка (после терраформинга) */
void fchunk_prime_final(const McGen *g, const BsTab *bs, FChunk *ch, int min_y, int height, int lazy_wg);
void fchunk_prime_type(const BsTab *bs, FChunk *ch, int type, int min_y, int height);

/* ====================================================================== разбор: контекст и арена */
struct FParse {
    FWorld *fw; const McGen *g; const BsTab *bs; McWorld *w;
    int version, newf;
    int nunimpl;                 /* счётчик нереализованных фич, встреченных при разборе (для Feat.partial) */
    char err[256];
};
void *fp_alloc(FParse *p, size_t n);            /* арена мира: освобождается вместе с FWorld */
char *fp_strdup(FParse *p, const char *s);
int fp_fail(FParse *p, const char *fmt, ...);   /* записывает первую ошибку, возвращает 0 */
static inline int fp_ok(const FParse *p) { return p->err[0] == 0; }
/* конфигурация фичи: 26.1/26.2 — объект "config", 26.3+ — поля прямо в объекте фичи */
const Js *fp_cfg(const Js *feature);
/* Js по ссылке: идентификатор датапака → документ (кэш; живёт до освобождения мира) */
const Js *fp_load_json(FParse *p, const char *kind, const char *id);   /* kind: «feature», «placed_feature», «block_state_provider»… */

/* ====================================================================== провайдеры значений (feature_prov.c) */
IntProv *fp_intprov(FParse *p, const Js *v);                 /* число или {"type":…} */
int intprov_sample(const IntProv *ip, FRnd *r);
int intprov_min(const IntProv *ip);
int intprov_max(const IntProv *ip);
FloatProv *fp_floatprov(FParse *p, const Js *v);
float floatprov_sample(const FloatProv *fp, FRnd *r);
float floatprov_min(const FloatProv *fp);
float floatprov_max(const FloatProv *fp);
HeightProv *fp_heightprov(FParse *p, const Js *v);
int heightprov_sample(const HeightProv *hp, FCtx *c);       /* ГСЧ — c->rnd */
typedef struct VAnchor { int kind, off; } VAnchor;            /* 0 absolute, 1 above_bottom, 2 below_top, 3 relative_to_sea_level */
int fp_vanchor(FParse *p, const Js *v, VAnchor *out);
int vanchor_resolve(const VAnchor *a, const FCtx *c);
/* Mth.sin/cos: таблица игры (fm_init вызывается из feature_register_all) */
void fm_init(void);
float fm_sin(double v);
float fm_cos(double v);
/* Biome.BIOME_INFO_NOISE.get(x, z) (SimplexNoise seed 2345): float (26.3+) или double (26.1/26.2: PerlinSimplexNoise) */
double biome_info_noise(const McGen *g, double x, double z);

/* ====================================================================== предикаты блоков и правила (feature_bpred.c) */
BPred *fp_bpred(FParse *p, const Js *v);
int bpred_test(FCtx *c, const BPred *bp, int x, int y, int z);
BPred *bpred_true(FParse *p);
u8 *fp_blockset(FParse *p, const Js *v);       /* HolderSet<Block> → u8[nblocks] в арене разбора */
RuleTest *fp_ruletest(FParse *p, const Js *v);
int ruletest_test(FCtx *c, const RuleTest *rt, int state, int x, int y, int z);   /* ГСЧ — c->rnd */
/* ---- состояния блоков: у состояний есть BlockState.canSurvive и жидкость — таблица поведения (feature_bpred.c) */
int block_can_survive(FCtx *c, int state, int x, int y, int z);   /* BlockState.canSurvive(level, pos) для классов, нужных стадии */

/* ====================================================================== провайдеры состояний (feature_bsp.c) */
BSProv *fp_bsprov(FParse *p, const Js *v);                  /* состояние, {"type":…} или ссылка на worldgen/block_state_provider */
int bsprov_state(FCtx *c, const BSProv *bp, int x, int y, int z);      /* BlockStateProvider.getState */
int bsprov_optional(FCtx *c, const BSProv *bp, int x, int y, int z);   /* getOptionalState: −1 вместо null */

/* ====================================================================== фичи */
typedef struct FeatType {
    const char *name;                                              /* «minecraft:ore» */
    void *(*parse)(FParse *p, const Js *cfg);                      /* NULL при ошибке (fp_fail) */
    int (*place)(FCtx *c, const void *cfg, int x, int y, int z);   /* Feature.place: поставлено ли что-то */
} FeatType;
void feature_register_type(const FeatType *t);
const FeatType *feature_find_type(const char *name);
void feature_register_all(void);            /* feature.c: единственное место со списком групп фич */

struct Feat { const FeatType *t; void *cfg; const char *id; const char *type_name; int partial; };   /* partial: вложенная фича не реализована (молча пропускается) */
Feat *fp_feature(FParse *p, const Js *v);   /* ссылка на настроенную фичу (идентификатор) или встроенное описание {"type","config"…} */
static inline int feat_place(FCtx *c, const Feat *f, int x, int y, int z) { return f && f->t ? f->t->place(c, f->cfg, x, y, z) : 0; }

/* PlacedFeature: фича + цепочка модификаторов размещения (placement.c) */
struct PMod;
struct Placed { const char *id; Feat *feat; struct PMod **mods; int nmods; int index; };
Placed *fp_placed(FParse *p, const Js *v);                         /* ссылка (идентификатор) или встроенная {"feature","placement"} */
/* FeaturePlacer.place / placeWithBiomeCheck: biome_check — проверка BiomeFilter по верхней фиче */
int placed_place(FCtx *c, const Placed *pf, int x, int y, int z, int biome_check);
/* поддержка вложенных вызовов: разбор списка модификаторов */
int placement_parse_list(FParse *p, const Js *arr, struct PMod ***out, int *n);

/* ====================================================================== мир фич (feature_sort.c) */
struct FWorld {
    McWorld *w; const McGen *g; const BsTab *bs;
    int version, newf;
    struct FArena *arena;
    StrMap docs;                 /* "kind/id" → JsDoc* */
    StrMap feat_cache;           /* id → Feat* */
    StrMap placed_cache;         /* id → Placed* */
    StrMap bsp_cache;            /* id → BSProv* */
    int nplaced; Placed **placed;                  /* все PlacedFeature из биомов (индекс = Placed.index, порядок первой встречи) */
    int nsteps;                                    /* число шагов декорации (макс. длина списка биома) */
    int *step_n; int **step_list;                  /* [шаг] → глобальный индекс → индекс в placed[] */
    int *step_words;                               /* [шаг] слов u64 в масках */
    u64 ***biome_step;                             /* [биом][шаг] → маска по глобальному индексу шага (NULL — у биома пусто) */
    u64 **biome_has;                               /* [биом] → маска по индексу placed[] (BiomeGenerationSettings.hasFeature) */
    int nbiomes;
    int ok;                                        /* порядок построен */
    char err[256];
    int n_unimpl;                                  /* фич с нереализованным типом (молча пропускаются) */
    char *only;                                    /* фильтр изоляции: список placed_feature через запятую (MCGEN_FEATURES_ONLY) */
    const char *cfg_dir;                           /* «feature» или «configured_feature» */
    int debug;
};
FWorld *features_world_get(McWorld *w);                 /* лениво строит; NULL при ошибке (текст — в w->… через stderr при MCGEN_FEATURES_DEBUG) */
int fsort_build(FWorld *fw, const int *src_biomes, int nsrc);   /* feature_sort.c */
/* подготовка списков биомов: читает worldgen/biome/*.json, компилирует PlacedFeature; src — биомы источника в порядке possibleBiomes */
int fworld_load(FWorld *fw, const int *src_biomes, int nsrc);

/* ====================================================================== стадия (feature.c) */
struct McRegion;
/* region.c: доступ стадии FEATURES к закрытым данным региона */
uint32_t region_stages(const McRegion *r);
PPMarks *region_chunk_marks(McRegion *r, int cx, int cz);
/* вызывается из region.c после стадий BIOMES/TERRAIN/SURFACE/CARVERS и до пост-обработки жидкостей */
int features_apply_region(McWorld *w, struct McRegion *r, int threads, McProgressFn cb, void *ud, char *err, size_t errlen);
/* статистика последнего вызова (тест/CLI) */
typedef struct FeatStats { long chunks; long placed_calls; double secs; long unimpl_skipped; } FeatStats;
void features_get_stats(FeatStats *out);

#endif

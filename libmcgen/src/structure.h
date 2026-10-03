/* structure.h — стадия STRUCTURES (постройки): внутренний интерфейс (поток W9, docs/blender/structures.md).
 *
 * Устройство (соответствие игровым классам — в docs/blender/structures.md):
 *   structure.c        реестры structure_set / structure (датапак), биом-теги, размещение (random_spread, concentric_rings),
 *                      старты (Structure.generate), кэш стартов и ссылок (StructureManager), Beardifier, цикл placeInChunk
 *   structure_place.c  формулы размещения (WorldgenRandom на LCG): random_spread, редукторы, exclusion_zone, кольца strongholds
 *   template.c/.h      StructureTemplate: загрузка .nbt, палитры, повороты/отражения блоков и их свойств, размещение шаблона
 *   processor.c/.h     StructureProcessor: rule, protected_blocks, block_rot, capped, gravity, block_ignore, jigsaw_replacement…
 *   jigsaw.c/.h        JigsawPlacement: пулы, элементы, обход «сочленений», свободные объёмы, PoolElementStructurePiece
 *   structures/<имя>.c постройки, закодированные в Java: каждая регистрирует StructType и PieceVT (structure_register_all)
 *
 * Как добавить тип постройки: заполнить StructType (разбор конфигурации, find, build) и, если нужны свои части, PieceVT
 * (post — рисование части в чанке); добавить вызов регистрации в structure_register_all() (structure.c).
 * ГСЧ: старт — WorldgenRandom поверх LCG (RS kind 0), рисование частей — WorldgenRandom поверх Xoroshiro (RS kind 1; общий
 * на структуру-шаг, setFeatureSeed(decSeed, индекс в шаге, шаг)). Все методы строятся на next(bits) (BitRandomSource).
 */
#ifndef MCGEN_STRUCTURE_H
#define MCGEN_STRUCTURE_H
#include "feature.h"

/* ====================================================================== BoundingBox (оба конца включительно) */
typedef struct BB { int x0, y0, z0, x1, y1, z1; } BB;
static inline BB bb_make(int x0, int y0, int z0, int x1, int y1, int z1) { BB b = { x0, y0, z0, x1, y1, z1 }; return b; }
static inline BB bb_empty(void) { BB b = { 0x7fffffff, 0x7fffffff, 0x7fffffff, (int)0x80000000u, (int)0x80000000u, (int)0x80000000u }; return b; }
static inline int bb_is_empty(const BB *b) { return b->x0 > b->x1; }
static inline int bb_inside(const BB *b, int x, int y, int z) { return x >= b->x0 && x <= b->x1 && z >= b->z0 && z <= b->z1 && y >= b->y0 && y <= b->y1; }
static inline int bb_intersects(const BB *a, const BB *b) { return a->x1 >= b->x0 && a->x0 <= b->x1 && a->z1 >= b->z0 && a->z0 <= b->z1 && a->y1 >= b->y0 && a->y0 <= b->y1; }
static inline int bb_intersects_xz(const BB *a, int x0, int z0, int x1, int z1) { return a->x1 >= x0 && a->x0 <= x1 && a->z1 >= z0 && a->z0 <= z1; }
static inline BB bb_moved(BB b, int dx, int dy, int dz) { b.x0 += dx; b.x1 += dx; b.y0 += dy; b.y1 += dy; b.z0 += dz; b.z1 += dz; return b; }
static inline BB bb_inflated(BB b, int d) { b.x0 -= d; b.y0 -= d; b.z0 -= d; b.x1 += d; b.y1 += d; b.z1 += d; return b; }
static inline BB bb_union(BB a, BB b) {
    if (bb_is_empty(&a)) return b;
    if (bb_is_empty(&b)) return a;
    a.x0 = a.x0 < b.x0 ? a.x0 : b.x0; a.y0 = a.y0 < b.y0 ? a.y0 : b.y0; a.z0 = a.z0 < b.z0 ? a.z0 : b.z0;
    a.x1 = a.x1 > b.x1 ? a.x1 : b.x1; a.y1 = a.y1 > b.y1 ? a.y1 : b.y1; a.z1 = a.z1 > b.z1 ? a.z1 : b.z1; return a;
}
static inline BB bb_encaps_pt(BB b, int x, int y, int z) { BB p = { x, y, z, x, y, z }; return bb_union(b, p); }
static inline BB bb_from_corners(int ax, int ay, int az, int bx, int by, int bz) {
    BB b = { ax < bx ? ax : bx, ay < by ? ay : by, az < bz ? az : bz, ax > bx ? ax : bx, ay > by ? ay : by, az > bz ? az : bz }; return b;
}
static inline int bb_xspan(const BB *b) { return b->x1 - b->x0 + 1; }
static inline int bb_yspan(const BB *b) { return b->y1 - b->y0 + 1; }
static inline int bb_zspan(const BB *b) { return b->z1 - b->z0 + 1; }
static inline int bb_cx(const BB *b) { return b->x0 + ((b->x1 - b->x0 + 1) / 2); }       /* getCenter: minX + (maxX - minX + 1) / 2 */
static inline int bb_cy(const BB *b) { return b->y0 + ((b->y1 - b->y0 + 1) / 2); }
static inline int bb_cz(const BB *b) { return b->z0 + ((b->z1 - b->z0 + 1) / 2); }

/* ====================================================================== WorldgenRandom: LCG (старты) или Xoroshiro (части) */
typedef struct RS { int kind; McLcg l; FRnd f; } RS;
static inline void rs_seed_lcg(RS *r, i64 seed) { r->kind = 0; r->l = lcg_new(seed); }
static inline void rs_seed_xoro(RS *r, i64 seed) { r->kind = 1; frnd_seed(&r->f, seed); }
static inline void rs_set_seed(RS *r, i64 seed) { if (r->kind) frnd_seed(&r->f, seed); else lcg_set_seed(&r->l, seed); }
static inline i32 rs_next(RS *r, int bits) { return r->kind ? frnd_next(&r->f, bits) : lcg_next(&r->l, bits); }
static inline i32 rs_int(RS *r) { return rs_next(r, 32); }
static inline i32 rs_bound(RS *r, i32 bound) {
    if ((bound & (bound - 1)) == 0) return (i32)(((i64)bound * (i64)rs_next(r, 31)) >> 31);
    i32 sample, modulo;
    do { sample = rs_next(r, 31); modulo = sample % bound; } while ((i32)((u32)sample - (u32)modulo + (u32)(bound - 1)) < 0);
    return modulo;
}
static inline i64 rs_long(RS *r) { i32 hi = rs_next(r, 32); i32 lo = rs_next(r, 32); return (i64)((u64)(i64)hi << 32) + (i64)lo; }
static inline int rs_bool(RS *r) { return rs_next(r, 1) != 0; }
static inline float rs_float(RS *r) { return (float)rs_next(r, 24) * 5.9604645E-8f; }
static inline double rs_double(RS *r) {
    i32 hi = rs_next(r, 26); i32 lo = rs_next(r, 27);
    return (double)((i64)((u64)(i64)hi << 27) + (i64)lo) * 1.1102230246251565e-16;
}
/* RandomSource.nextIntBetweenInclusive / Mth.randomBetweenInclusive */
static inline i32 rs_between(RS *r, i32 lo, i32 hi_incl) { return rs_bound(r, hi_incl - lo + 1) + lo; }
/* Mth.nextInt(random, min, max): min >= max ? min : nextInt(max - min + 1) + min */
static inline i32 rs_mth_int(RS *r, i32 lo, i32 hi_incl) { return lo >= hi_incl ? lo : rs_bound(r, hi_incl - lo + 1) + lo; }
double rs_gauss(RS *r);                                   /* Random.nextGaussian (Marsaglia) — состояние в RS */
/* WorldgenRandom сидеры (LCG) */
static inline void rs_large_feature_seed(RS *r, i64 seed, int cx, int cz) {
    rs_set_seed(r, seed); i64 xs = rs_long(r); i64 zs = rs_long(r);
    rs_set_seed(r, (i64)((u64)((i64)cx * (u64)xs) ^ (u64)((i64)cz * (u64)zs) ^ (u64)seed));
}
static inline void rs_large_feature_salt(RS *r, i64 seed, int x, int z, int salt) {
    rs_set_seed(r, (i64)((u64)(i64)x * 341873128712ULL + (u64)(i64)z * 132897987541ULL + (u64)seed + (u64)(i64)salt));
}
/* Mth.getSeed(x, y, z) (RandomSource.create(Mth.getSeed(pos)) = LegacyRandomSource) */
static inline i64 mth_get_seed(int x, int y, int z) {
    i64 seed = (i64)(i32)((u32)x * 3129871u) ^ ((i64)z * 116129781LL) ^ (i64)y;
    seed = (i64)((u64)seed * (u64)seed * 42317861ULL + (u64)seed * 11ULL);
    return seed >> 16;
}

/* ====================================================================== перечисления игры */
/* Direction.get2DDataValue: SOUTH 0, WEST 1, NORTH 2, EAST 3; Direction (индексы DIR_*) — в feature.h: DOWN, UP, NORTH, SOUTH, WEST, EAST */
enum { ROT_NONE = 0, ROT_CW90 = 1, ROT_CW180 = 2, ROT_CCW90 = 3 };      /* порядок Rotation.values() */
enum { MIR_NONE = 0, MIR_LEFT_RIGHT = 1, MIR_FRONT_BACK = 2 };          /* порядок Mirror.values() */
enum { ST_RAW_GENERATION = 0, ST_LAKES, ST_LOCAL_MODIFICATIONS, ST_UNDERGROUND_STRUCTURES, ST_SURFACE_STRUCTURES, ST_STRONGHOLDS, ST_UNDERGROUND_ORES,
       ST_UNDERGROUND_DECORATION, ST_FLUID_SPRINGS, ST_VEGETAL_DECORATION, ST_TOP_LAYER_MODIFICATION, ST__COUNT };
enum { TA_NONE = 0, TA_BURY, TA_BEARD_THIN, TA_BEARD_BOX, TA_ENCAPSULATE };   /* TerrainAdjustment */
/* направление по 2D-индексу (SOUTH 0, WEST 1, NORTH 2, EAST 3) → индекс DIR_* */
static inline int dir_from_2d(int v) { static const int M[4] = { DIR_SOUTH, DIR_WEST, DIR_NORTH, DIR_EAST }; return M[v & 3]; }
static inline int dir_to_2d(int d) { return d == DIR_SOUTH ? 0 : d == DIR_WEST ? 1 : d == DIR_NORTH ? 2 : d == DIR_EAST ? 3 : -1; }
static inline int dir_cw(int d) { return d == DIR_NORTH ? DIR_EAST : d == DIR_EAST ? DIR_SOUTH : d == DIR_SOUTH ? DIR_WEST : d == DIR_WEST ? DIR_NORTH : d; }
static inline int dir_ccw(int d) { return d == DIR_NORTH ? DIR_WEST : d == DIR_WEST ? DIR_SOUTH : d == DIR_SOUTH ? DIR_EAST : d == DIR_EAST ? DIR_NORTH : d; }
static inline int dir_opp(int d) { static const int O[6] = { DIR_UP, DIR_DOWN, DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST }; return O[d]; }
static inline int dir_rotate(int d, int rot) { for (int i = 0; i < rot; i++) d = dir_cw(d); return d; }   /* Rotation.rotate(Direction): ROT_* = число поворотов по часовой */

/* ====================================================================== типы */
typedef struct TerrainCtx TerrainCtx;
typedef struct StructWorld StructWorld;
typedef struct StructDef StructDef;
typedef struct StructSet StructSet;
typedef struct StPiece StPiece;
typedef struct StStart StStart;
typedef struct StCtx StCtx;
typedef struct GenCtx GenCtx;

/* контекст рисования частей в чанке (WorldGenRegion + ГСЧ + писаемая область) */
struct StCtx {
    McWorld *w; StructWorld *sw; FCtx *fc; RS *rs;
    BB chunk;                 /* chunkBB = getWritableArea(chunk): чанк × [min_y+1, max_y] */
    int cx, cz;               /* центральный чанк */
};

typedef struct PieceVT {
    const char *id;                                                              /* «minecraft:jigsaw»… */
    void (*post)(StCtx *c, StPiece *p, int ref_x, int ref_y, int ref_z);         /* postProcess; reference — referencePos старта */
    void (*move)(StPiece *p, int dx, int dy, int dz);                            /* StructurePiece.move (NULL — только bb) */
    void (*free_data)(void *data);
    void (*dump)(const StPiece *p, StrBuf *out);                                 /* отладочная строка JSON (тесты); NULL — только общее */
    void (*reset)(StPiece *p);                                                   /* вернуть изменяемое состояние (heightPosition…) к исходному перед новым прогоном региона */
    int (*can_replace)(StCtx *c, const StPiece *p, int x, int y, int z);         /* StructurePiece.canBeReplaced(level, x, y, z, chunkBB); NULL — всегда да */
} PieceVT;

struct StPiece {
    const PieceVT *vt;
    BB bb;
    int orient;               /* StructurePiece.orientation: −1 (null) или 2D-индекс 0..3 (S, W, N, E) */
    int rot, mir;             /* Rotation/Mirror части (StructurePiece.rotation/mirror) */
    int depth;                /* genDepth */
    BB bb_init;               /* bounding box после создания старта (восстанавливается перед каждым прогоном региона) */
    void *data;
};
StPiece *piece_new(const PieceVT *vt, BB bb, int orient, int depth, void *data);
void piece_free(StPiece *p);
void piece_move(StPiece *p, int dx, int dy, int dz);

typedef struct PieceVec { StPiece **v; int n, cap; } PieceVec;
void pvec_push(PieceVec *a, StPiece *p);
BB pvec_bb(const PieceVec *a);                                                   /* StructurePiece.createBoundingBox */
StPiece *pvec_collision(const PieceVec *a, const BB *box);                       /* findCollisionPiece */
void pvec_offset_vertically(PieceVec *a, int dy);                                /* StructurePiecesBuilder.offsetPiecesVertically */
int pvec_move_below_sea_level(PieceVec *a, int sea_level, int min_y, RS *r, int offset);   /* moveBelowSeaLevel: возвращает dy */
void pvec_move_inside_heights(PieceVec *a, RS *r, int lowest, int highest);      /* moveInsideHeights */

struct StStart {
    const StructDef *def;
    int cx, cz;               /* чанк-источник */
    int n; StPiece **pieces;  /* пусто — старт недействителен (INVALID) */
    BB bb, adj_bb;            /* getBoundingBox до/после adjustBoundingBox */
};

/* Structure.GenerationStub: позиция (биом проверяется в ней) + состояние для build */
typedef struct Stub { int x, y, z; void *state; } Stub;

/* контекст генерации старта (Structure.GenerationContext) */
struct GenCtx {
    McWorld *w; StructWorld *sw; const BsTab *bs;
    RS rs;                    /* WorldgenRandom(Legacy) с setLargeFeatureSeed(seed, cx, cz) */
    i64 seed; int cx, cz;
    const StructDef *def;
    int min_y, height;        /* heightAccessor (чанк): getMinY, getHeight */
    int gen_min_y, gen_depth; /* WorldGenerationContext */
};
/* ChunkGenerator.getBaseHeight / getFirstFreeHeight / getFirstOccupiedHeight (кэшируется) */
int gen_first_free_height(GenCtx *c, int x, int z, int hm_type);
static inline int gen_first_occupied_height(GenCtx *c, int x, int z, int hm_type) { return gen_first_free_height(c, x, z, hm_type) - 1; }
int gen_biome_quart(GenCtx *c, int qx, int qy, int qz);                          /* BiomeResolver.getNoiseBiome (id биома) */
int gen_biome_valid(GenCtx *c, int qx, int qy, int qz);                          /* validBiome.test(getNoiseBiome) */
int gen_could_exist_in_column(GenCtx *c, int bx, int bz, int min_y, int max_y);  /* Context.couldStructureExistInColumn */
int gen_could_exist_on_chunk_center(GenCtx *c);                                  /* couldValidBiomeExistOnTopOfChunkCenter */
int gen_mean_first_occupied_height(GenCtx *c, int minx, int sx, int minz, int sz);
int gen_lowest_y(GenCtx *c, int minx, int minz, int sx, int sz);
/* VerticalAnchor/HeightProvider (на LCG-ГСЧ; конфиг — Js датапака) */
typedef struct HProv HProv;
HProv *hprov_parse(const Js *v, char *err, size_t errlen);
int hprov_sample(const HProv *h, RS *r, int gen_min_y, int gen_depth, int sea_level);
void hprov_free(HProv *h);

/* Тип постройки (Structure.type()): разбор конфигурации из JSON structure/*.json, поиск точки, построение частей */
typedef struct StructType {
    const char *name;                                                            /* «minecraft:jigsaw» */
    void *(*parse)(StructWorld *sw, const Js *cfg, char *err, size_t errlen);    /* NULL — ошибка */
    int (*find)(GenCtx *c, const void *cfg, Stub *out);                          /* findGenerationPoint: 1 — есть точка */
    int (*build)(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out);         /* GenerationStub.getPiecesBuilder */
    void (*after_place)(StCtx *c, const StStart *s);                             /* Structure.afterPlace (NULL — нет) */
    void (*free_cfg)(void *cfg);
    void (*free_stub)(Stub *stub);                                               /* освободить состояние stub, если build не вызван (биом не подошёл) */
} StructType;
void structure_register_type(const StructType *t);
void piece_register_type(const PieceVT *vt);
const PieceVT *piece_find_type(const char *id);

struct StructDef {
    char *id;                 /* «minecraft:village_plains» */
    int index;                /* позиция в реестре structure (порядок setFeatureSeed внутри шага) */
    int index_in_step;
    const StructType *type; void *cfg;
    u8 *biome_ok;             /* [nbiomes]: HolderSet<Biome> структуры */
    int step, adapt;
};

typedef struct StructEntry { StructDef *def; int weight; } StructEntry;
struct StructSet {
    char *id;
    int ptype;                /* 0 random_spread, 1 concentric_rings, 2 dimension_origin */
    int spacing, separation, triangular;
    i32 salt; float frequency; int freq_method;
    int excl_set, excl_chunks;               /* индекс набора (−1 нет), радиус в чанках */
    int distance, spread, count; u8 *preferred;   /* кольца */
    int n; StructEntry *e;
    int possible;             /* ChunkGeneratorStructureState.possibleStructureSets: у набора есть структура с биомом источника */
    int *ring_x, *ring_z, nring; i64 *ring_seed; u8 *ring_done; int ring_init;   /* кольца: сырые позиции, зерно ГСЧ поиска биома, готовность (лениво) */
};

struct StructWorld {
    McWorld *w; const McGen *g; const BsTab *bs;
    int version, newf;
    int nsets; StructSet *sets;
    int ndefs; StructDef **defs;             /* порядок реестра structure */
    int nstep_defs[ST__COUNT]; StructDef **step_defs[ST__COUNT];
    StrMap def_by_id, set_by_id;
    McMutex *lock;                           /* кэш стартов и высот */
    void *start_cache;                       /* хэш (cx,cz) → набор стартов чанка */
    void *height_cache;
    void *wg_cache;                          /* карты WORLD_SURFACE_WG/OCEAN_FLOOR_WG чанков (по заполнению шумом) */
    TerrainCtx **tcv; int ntc, ctc;          /* пул контекстов заполнения для колонок высот */
    u8 *shape_chk; StrMap st_cache;          /* structure_piece.c: блоки SHAPE_CHECK_BLOCKS и кэш состояний по имени */
    void *templates;                         /* template.c: StructureTemplateManager */
    void *pools;                             /* jigsaw.c: реестр пулов элементов */
    void *procs;                             /* processor.c: реестр списков процессоров */
    int ok; char err[256];
    int debug;
    long n_starts_valid, n_starts_tried;
};

StructWorld *structures_world_get(McWorld *w);               /* лениво строит; NULL при ошибке */
void structures_world_free(McWorld *w);
int structure_is_start_chunk(StructWorld *sw, const StructSet *set, int cx, int cz);   /* StructurePlacement.isStructureChunk */
/* старты чанка-источника (все наборы): кэш; массив живёт до освобождения мира */
int structure_starts_at(StructWorld *sw, int cx, int cz, StStart ***out);
/* старты, ссылающиеся на чанк (StructureManager.startsForStructure/getAllStarts по ссылкам): в порядке игры по структурам */
typedef struct StRef { const StStart *start; } StRef;
int structure_refs_for_chunk(StructWorld *sw, int cx, int cz, StStart ***out);        /* только для terrain_adaptation != NONE при adapt_only */
int structure_refs_for_chunk_def(StructWorld *sw, int cx, int cz, const StructDef *def, const StStart ***out);
void structure_free_refs(StStart **a);

/* WorldGenRegion.getHeight(WORLD_SURFACE_WG | OCEAN_FLOOR_WG, x, z) 26.3+: «первая свободная» y по результату заполнения шумом чанка (кэшируется) */
int structure_height_wg(McWorld *w, int type, int x, int z);

/* перед каждым прогоном региона: вернуть части кэшированных стартов к исходному состоянию (ScatteredFeaturePiece.heightPosition и т. п.) */
void structures_begin_region(McWorld *w);

/* стадия: рисование построек шага в чанке (вызывается из цикла декорации чанка; ГСЧ rs уже засеян под шаг) */
void structures_decorate_step(FCtx *fc, FRnd *rnd, i64 dec_seed, int step, int cx, int cz);

/* Beardifier (terrain_adaptation): поле плотности чанка; NULL если построек рядом нет */
typedef struct Beard Beard;
Beard *beard_for_chunk(McWorld *w, int cx, int cz);
void beard_free(Beard *b);
float beard_value(const Beard *b, int x, int y, int z);
void beard_volume(const Beard *b, float *out, const Vol *v);
double beard_value_d(const Beard *b, int x, int y, int z);      /* 26.1/26.2: вклад в точке (double-версия, как Beardifier.compute) */
int beard_active(const Beard *b);

/* ====================================================================== общее для jigsaw/шаблонов */

#endif

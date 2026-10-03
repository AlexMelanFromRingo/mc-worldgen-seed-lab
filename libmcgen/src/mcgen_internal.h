/* mcgen_internal.h — внутренние структуры libmcgen (McGen, McWorld, McRegion). */
#ifndef MCGEN_INTERNAL_H
#define MCGEN_INTERNAL_H
#include "../include/mcgen.h"
#include "util.h"
#include "json.h"
#include "noise.h"
#include "df.h"
#include "df_new.h"

#define MCGEN_SEMVER "0.1.0"

/* версии (совпадают с engine/mc_common.h) */
enum { V26_1 = 0, V26_2 = 1, V26_3 = 2, V26_4 = 3 };

/* поля NoiseRouter (объединение схем 26.1/26.2 и 26.3) */
enum {
    RF_TEMPERATURE, RF_VEGETATION, RF_CONTINENTS, RF_EROSION, RF_DEPTH, RF_RIDGES, RF_FINAL_DENSITY,
    RF_CHUNK_SURFACE_LEVEL,     /* 26.3 */
    RF_PRELIMINARY_SURFACE,     /* 26.1/26.2 */
    RF_BARRIER, RF_FLUID_FLOOD, RF_FLUID_SPREAD, RF_LAVA, RF_VEIN_TOGGLE, RF_VEIN_RIDGED, RF_VEIN_GAP,   /* 26.1/26.2 */
    RF__COUNT
};
/* поля Aquifer.Config (26.3) */
enum { AQ_BARRIER, AQ_FLOOD, AQ_SPREAD, AQ_LAVA, AQ_EXCLUSION, AQ_SURFACE_LEVEL, AQ__COUNT };

typedef struct {
    char *id;
    int min_y, height;          /* noise.min_y / noise.height */
    int size_h, size_v;         /* 26.1/26.2: noise.size_horizontal/size_vertical (ячейка = size*4 по XZ, size*4 по Y) */
    int sea_level, legacy_random;
    int aquifers_enabled, ore_veins_enabled;   /* 26.1/26.2 */
    int default_block, default_fluid;          /* id состояний */
    Df *rf[RF__COUNT];
    Df *aq[AQ__COUNT]; int has_aquifers;       /* 26.3 */
    JsDoc *doc;                 /* весь JSON (surface_rule/material_rule — для стадии SURFACE) */
} NoiseSettings;

enum { BS_MULTI_OVERWORLD, BS_MULTI_NETHER, BS_THE_END, BS_FIXED };
typedef struct {
    char *name;                 /* имя пресета: "normal", "large_biomes", …, "caves", "floating_islands" */
    char *settings;             /* id noise_settings */
    int biome_source; int fixed_biome;
    char *dim_type;             /* id dimension_type */
    int min_y, height;          /* из dimension_type */
    int fast_lava;              /* dimension_type attributes gameplay/fast_lava (или ultrawarm) */
} McPreset;
typedef struct { char *name; int kind; int n; McPreset *p; } McDim;   /* kind: 0 OW, 1 Nether, 2 End */

/* точка таблицы климата: 7 интервалов (×10000) и биом */
typedef struct { i64 lo[7], hi[7]; int biome; } ClimPoint;

struct McGen {
    int version, newf;
    char *pack;
    /* блоки */
    int nstates; char **state_names; StrMap state_ids;    /* имя → id+1 */
    int st_air, st_stone, st_water, st_lava, st_cave_air;
    u8 *state_cls;              /* классы состояний для карт высот (region.c) */
    u16 *state_block;           /* номер блока (порядок blocks.json) для каждого состояния: BlockState.is(Block) */
    int nblocks; int blk_water, blk_lava;
    StrMap tags;                /* теги блоков: id → u8[nblocks] (gen_block_tag) */
    McMutex *lock;
    /* биомы */
    int nbiomes; char **biome_names; StrMap biome_ids;     /* «minecraft:x» → id+1 */
    /* реестры датапака */
    StrMap noises;              /* id → NoiseParams* */
    StrMap dfs;                 /* id → Df* */
    StrMap nsettings;           /* id → NoiseSettings* */
    /* измерения и пресеты */
    int ndims; McDim dims[3];
    /* таблицы климата (из кода игры, по версии) */
    ClimPoint *ow_points; int n_ow;
    ClimPoint nether_points[5]; int n_nether;
    void *ow_tree, *nether_tree;            /* McBiomeTree* (engine/mc_biomes.h) */
    int *ow_leaf_biome, *nether_leaf_biome; /* лист дерева → id биома libmcgen */
    /* тонкие настройки */
    int ntweaks; const McTweakInfo *tweaks;
    /* стадия FEATURES (поток W8): таблица состояний блоков (blockstate.c) */
    void *bs_tab;
};

void gen_compute_state_classes(McGen *g);
const u8 *gen_block_tag(const McGen *g, const char *name);
static inline int gen_is_block(const McGen *g, int state, int blk) { return state >= 0 && state < g->nstates && g->state_block[state] == blk; }
static inline int gen_is_air(const McGen *g, int state) { return state >= 0 && state < g->nstates && (g->state_cls[state] & 1); }
int gen_state_id(const McGen *g, const char *name);     /* «minecraft:stone» или с [свойствами]; −1 если нет */
int gen_biome_id(const McGen *g, const char *name);
int gen_state_from_json(const McGen *g, const Js *v);  /* {"Name":…,"Properties":…} или строка */
const McPreset *gen_find_preset(const McGen *g, const char *dim, const char *preset, int *dim_kind);

/* ---- мир ---- */
typedef struct NoiseInst { const char *name; int domain; NStack ns; OldNormal on; int ready; } NoiseInst;

struct McWorld {
    McGen *g;
    const McPreset *preset;
    const NoiseSettings *ns;
    int dim_kind;               /* 0 OW, 1 Nether, 2 End */
    McSeeds seeds;
    double *tweak;              /* значения по g->tweaks */
    int min_y, height, sea_level;
    /* случайность: позиционные фабрики по доменам (RandomState.random) */
    PosRnd pos_climate, pos_terrain;
    /* шумы (по имени) */
    StrMap noise_inst;          /* id → NoiseInst* */
    McMutex *lock;              /* только для ленивого создания на этапе компиляции */
    /* 26.3+: компилятор и сэмплеры */
    NComp *nc;
    const S *s_rf[RF__COUNT];
    const S *s_aq[AQ__COUNT];
    BlendFbm *blended[4]; const Df *blended_key[4]; int nblended;
    GNoise end_simplex; int has_end_simplex;
    PosRnd aquifer_pos, ore_pos;
    /* 26.1/26.2: «проводка» (см. df_old.c) */
    void *old;
    /* климатическая модель: какие шумы считаются «климатом» */
    StrMap climate_noises;
    i64 biome_zoom_seed;        /* BiomeManager.obfuscateSeed(seed) */
    /* заполнение */
    int def_block, def_fluid;
    void *veins;                /* 26.3+: правила ore_vein (terrain_veins.c) */
    /* тонкие настройки (tweaks.c): подмена функций и масштабы шумов; при значениях по умолчанию пусто/1.0 */
    StrMap df_over;             /* id → изменённое дерево density-функции */
    Df *rf_over[RF__COUNT];     /* изменённые поля роутера */
    Df *aq_over[AQ__COUNT];     /* изменённые поля Aquifer.Config (26.3+) */
    double noise_mxz, noise_my, cave_m;
    void *carvers;              /* стадия CARVERS (carver.c): определения карверов, списки по биомам; строится лениво */
    void *surface;              /* стадия SURFACE (surface.c): дерево правил, шумы, полосы; строится в mcgen_world_new */
    void *features;             /* стадия FEATURES (feature*.c): скомпилированные фичи и порядок по шагам; строится лениво */
    void *structures;           /* стадия STRUCTURES (structure*.c, jigsaw.c, template.c): реестры, кэш стартов; строится лениво */
    long struct_run;            /* номер прогона региона со стадией STRUCTURES (сбрасывает состояния частей и ГСЧ региона) */
    int struct_on;              /* Beardifier включён (region.c ставит перед генерацией, если запрошена стадия STRUCTURES) */
};
void structures_world_free(McWorld *w);  /* structure.c */
void bs_free(void *tab);                 /* blockstate.c */
void features_world_free(McWorld *w);    /* feature.c */

void tweaks_prepare(McWorld *w);
void tweaks_release(McWorld *w);
void world_noise_scale(const McWorld *w, const char *name, double *mxz, double *my);
Df *df_clone(const Df *f);
static inline const Df *world_df_ref(const McWorld *w, const char *id) { const Df *o = sm_get(&w->df_over, id); return o ? o : (const Df *)sm_get(&w->g->dfs, id); }
static inline const Df *world_rf(const McWorld *w, int k) { return w->rf_over[k] ? w->rf_over[k] : w->ns->rf[k]; }
static inline const Df *world_aq(const McWorld *w, int k) { return w->aq_over[k] ? w->aq_over[k] : w->ns->aq[k]; }

/* noise.c уровня мира */
const NStack *world_noise_new(McWorld *w, const char *name, char *err, size_t errlen);
const OldNormal *world_noise_old(McWorld *w, const char *name, char *err, size_t errlen);
Ival world_noise_range(McWorld *w, const char *name);

/* биомы */
int biome_params_build(McGen *g, char *err, size_t errlen);   /* OverworldBiomeBuilder + Nether (код игры) */
void biome_params_free(McGen *g);
int world_biome_noise(const McWorld *w, int qx, int qy, int qz);   /* «сырой» биом клетки (как BiomeSource.getNoiseBiome) */
int world_biome_cell(const McWorld *w, int qx, int qy, int qz);    /* биом клетки, как его хранит чанк (ничьи R-дерева по порядку заполнения, y зажат) */
int world_biome_from_climate(const McWorld *w, const float v[6]);   /* temperature…weirdness → биом */
/* биомы чанка (26.3: пакетно через sampleVolume, как doCreateBiomes); out: (height/4)*16 */
void world_chunk_biomes(const McWorld *w, SCtx *x, int cx, int cz, uint8_t *out);

/* density-функции (G1): точечное значение зарегистрированной функции без кэшей */
int world_df_point(McWorld *w, const char *id, int n, const int *xyz, double *out, char *err, size_t errlen);
const Df *world_df_lookup(const McWorld *w, const char *id);

/* рельеф */
typedef struct TerrainCtx TerrainCtx;
struct PPMarks;
TerrainCtx *terrain_ctx_new(McWorld *w);
void terrain_ctx_free(TerrainCtx *t);
/* заполнение шумом одного чанка: blocks [y][z][x] (height мира), биомы в out_biomes (может быть NULL) */
int veins_init(McWorld *w, char *err, size_t errlen);
void veins_free(McWorld *w);
int terrain_fill_chunk(McWorld *w, TerrainCtx *t, int cx, int cz, uint16_t *blocks, char *err, size_t errlen);
/* пометки пост-обработки жидкостей последнего заполненного чанка (ProtoChunk.markPosForPostProcessing) */
const struct PPMarks *terrain_marks(const TerrainCtx *t);

#endif

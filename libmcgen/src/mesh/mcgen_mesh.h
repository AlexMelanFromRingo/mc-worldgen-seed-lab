/* mcgen_mesh.h — C-ядро меширования чанка Minecraft 26.x (поток W4). Собственный заголовок и ABI, независим от mcgen.h.
 *
 * Вход: блоки чанка (u16, [y][z][x]) + 4/8 соседей для отсечения граней и окна оттенка, биомы (u8, клетки 4×4×4, [qy][qz][qx]),
 * таблицы «состояние → квады» (их строит blender/mcgen_addon/assets/state_table.py по ресурсам клиента — внутрь ядра ничего не зашито).
 * Выход: массивы четырёхугольников (позиции, UV атласа, цвета вершин = оттенок биома [× затенение], материал, исходный блок и направление
 * грани для выбора лучом). Всё в виде SoA-массивов под numpy/foreach_set.
 *
 * Поведение следует игре 26.x: Block.shouldRenderFace (маски окклюзии граней 16×16 + skipRendering), выбор варианта модели по
 * Mth.getSeed(x,y,z) → LegacyRandomSource → взвешенный список (для multipart — nextLong и setSeed на каждую часть), смещение моделей
 * (OffsetType), оттенок биома как среднее по окну (2r+1)² (ClientLevel.calculateBlockTint), FluidRenderer (высоты по соседям, поток,
 * текстуры still/flow/overlay, обратные грани).
 */
#ifndef MCGEN_MESH_H
#define MCGEN_MESH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#  define MCMESH_API __declspec(dllexport)
#else
#  define MCMESH_API __attribute__((visibility("default")))
#endif

#define MCMESH_ABI_VERSION 2

/* Биты state_flags (совпадают с assets/state_table.py: class F). */
enum {
    MCM_F_GEOM = 1u << 0,        /* у состояния есть квады модели */
    MCM_F_AIR = 1u << 1,
    MCM_F_OPAQUE = 1u << 2,      /* полный непрозрачный куб (isSolidRender) */
    MCM_F_SOLID = 1u << 3,       /* BlockState.isSolid() */
    MCM_F_WATER = 1u << 4,
    MCM_F_LAVA = 1u << 5,
    MCM_F_FALLING = 1u << 6,
    MCM_F_RANDOM = 1u << 7,      /* есть часть с несколькими вариантами */
    MCM_F_MULTIPART = 1u << 8,
    MCM_F_OFF_XZ = 1u << 9,
    MCM_F_OFF_XYZ = 1u << 10,
    MCM_F_LEAVES = 1u << 11,
    MCM_F_BARS = 1u << 12,       /* тег #minecraft:bars */
    MCM_F_BLOCKS_FLOW = 1u << 13,/* тег #minecraft:blocks_fluid_flow */
    MCM_F_TINT_BELOW = 1u << 14, /* оттенок брать на блок ниже (верхняя половина двойной травы) */
    MCM_FLUID_AMOUNT_SHIFT = 16, /* 4 бита: FluidState.getAmount() */
    MCM_SKIP_SHIFT = 20,         /* 3 бита: класс skipRendering */
    MCM_CONN_SHIFT = 24          /* 4 бита: связи панелей N,S,W,E */
};
enum { MCM_SKIP_NONE = 0, MCM_SKIP_SAME = 1, MCM_SKIP_LEAVES = 2, MCM_SKIP_BARS = 3, MCM_SKIP_POWDER = 4, MCM_SKIP_ROOTS = 5, MCM_SKIP_LIQUID = 6 };
/* Виды оттенка квада. */
enum { MCM_TINT_NONE = 0, MCM_TINT_CONST = 1, MCM_TINT_GRASS = 2, MCM_TINT_FOLIAGE = 3, MCM_TINT_DRY_FOLIAGE = 4, MCM_TINT_WATER = 5 };
/* Материалы выхода. */
enum { MCM_MAT_SOLID = 0, MCM_MAT_CUTOUT = 1, MCM_MAT_TRANSLUCENT = 2, MCM_MAT_WATER = 3, MCM_N_MAT = 4 };
/* Направления как Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST. */
enum { MCM_DOWN = 0, MCM_UP = 1, MCM_NORTH = 2, MCM_SOUTH = 3, MCM_WEST = 4, MCM_EAST = 5 };

/* Опции (поле flags). */
enum {
    MCM_OPT_CUTOUT_LEAVES = 1u << 0,  /* «красивая» листва: cutout; иначе листва — сплошной слой, листва к листве скрыта */
    MCM_OPT_BAKE_SHADE = 1u << 1,     /* домножить цвет вершины на игровое направленное затенение shade[6] */
    MCM_OPT_BLENDER_AXES = 1u << 2,   /* выход в осях Blender: (x, -z, y) вместо (x, y, z) */
    MCM_OPT_BLENDER_UV = 1u << 3,     /* v := 1 - v (начало координат UV внизу) */
    MCM_OPT_NO_FLUIDS = 1u << 4,
    MCM_OPT_NO_MODELS = 1u << 5,
    MCM_OPT_AO = 1u << 6,             /* запечь затенение углов (ambient occlusion) в цвет вершины */
    MCM_OPT_MERGE = 1u << 7,          /* жадное слияние плоских граней с повтором тайла в шейдере (см. McMeshOutput.merged/rect) */
    MCM_OPT_NO_VARIANTS = 1u << 8     /* всегда первый вариант модели (без случайных поворотов) — лучше сливаются грани */
};

/* Таблицы. Все указатели принадлежат вызывающему и должны жить, пока ядро с ними работает; формат — см. assets/state_table.py. */
typedef struct McMeshTables {
    int32_t abi;                    /* = MCMESH_ABI_VERSION */
    int32_t n_states, n_biomes, n_masks;
    int32_t mangrove_roots_block;   /* индекс блока mangrove_roots (для SKIP_ROOTS) или -1 */
    /* по состояниям */
    const uint32_t *st_flags;       /* [n_states] */
    const uint16_t *st_block;       /* [n_states] */
    const uint16_t *st_occ;         /* [n_states][6] id маски окклюзии грани */
    const uint8_t *st_sturdy;       /* [n_states] биты граней, целиком закрытых геометрией */
    const int32_t *st_grp_off;      /* [n_states+1] -> grp_list */
    const float *st_off_h, *st_off_v;/* [n_states] макс. смещение модели */
    /* группы (части) и варианты */
    const int32_t *grp_list;
    const int32_t *grp_var_off;     /* [n_groups+1] -> var_* */
    const int32_t *grp_total;       /* [n_groups] сумма весов */
    const int32_t *var_baked;       /* [n_variants] */
    const int32_t *var_weight;      /* [n_variants] */
    const int32_t *baked_q_off;     /* [n_baked+1] -> квады */
    /* квады */
    const float *q_pos;             /* [n_quads][4][3] доли блока */
    const float *q_uv;              /* [n_quads][4][2] координаты в атласе, v вниз */
    const int8_t *q_cull;           /* -1 или направление */
    const uint8_t *q_dir, *q_shade, *q_tint, *q_layer;
    const uint8_t *q_merge;         /* 1 — грань: единичный квад на границе блока с UV на весь спрайт (годится для слияния) */
    const uint32_t *q_rgb;          /* константный оттенок 0xRRGGBB */
    /* маски окклюзии: cover[a*n_masks+b] = 1, если маска a целиком покрыта маской b; 0 — пусто, 1 — полная */
    const uint8_t *occ_masks;       /* [n_masks][32] (16×16 бит, packbits) */
    const uint8_t *occ_cover;       /* [n_masks][n_masks] */
    /* жидкости: прямоугольники в атласе (u0,v0,u1,v1): water still, water flow, water overlay, lava still, lava flow */
    float fluid_rect[5][4];
    uint8_t fluid_layer[2];         /* слой воды, лавы: 0 solid, 1 cutout, 2 translucent */
    /* биомы: [n_biomes][4] = трава, листва, сухая листва, вода (0xRRGGBB); модификатор травы: 0 нет, 1 тёмный лес (уже в цвете), 2 болото */
    const uint32_t *biome_rgb;
    const uint8_t *biome_mod;
    uint8_t swamp_perm[256];        /* перестановка SimplexNoise болот */
} McMeshTables;

typedef struct McMeshOptions {
    uint32_t flags;                 /* MCM_OPT_* */
    int32_t blend_radius;           /* радиус смешивания биомов 0..7 (по умолчанию 2 => окно 5×5) */
    int32_t cx, cz;                 /* координаты чанка (для абсолютных x, z: сиды вариантов, смещения, шум болот) */
    int32_t min_y;                  /* y нижней границы мира (кратно 16) */
    int32_t n_sections;             /* высота/16 */
    float shade[6];                 /* затенение по направлениям (DOWN, UP, N, S, W, E) при MCM_OPT_BAKE_SHADE */
    float scale;                    /* масштаб координат на выходе (1.0 — блок = единица) */
    int32_t y_offset;               /* добавка к y-координате выхода (в блоках, до масштаба); 0 — от min_y */
} McMeshOptions;

/* Вход: окрестность чанка 3×3 (индекс (dz+1)*3 + (dx+1); центр = 4). NULL — соседа нет (граница рисуется как открытая). */
typedef struct McMeshInput {
    const uint16_t *blocks[9];      /* height*256 значений u16 [y][z][x] */
    const uint8_t *biomes[9];       /* (height/4)*16 значений u8 [qy][qz][qx]; центр может быть NULL (биом 0) */
} McMeshInput;

typedef struct McMeshOutput {
    int32_t n_quads, capacity;
    float *pos;                     /* [n][4][3] */
    float *uv;                      /* [n][4][2] */
    uint8_t *col;                   /* [n][4][4] RGBA */
    uint8_t *mat;                   /* [n] MCM_MAT_* */
    uint32_t *block;                /* [n] локальный индекс блока ((y*16)+z)*16+x */
    uint8_t *dir;                   /* [n] направление грани (куда смотрит нормаль) */
    uint8_t *merged;                /* [n] 1 — слитая грань: uv = локальные координаты тайла (0..w, 0..h), rect = прямоугольник спрайта */
    float *rect;                    /* [n][4] (u0, v0, du, dv) спрайта в атласе (в системе UV выхода) — только для merged */
    /* статистика */
    int32_t n_blocks_visited;
    int32_t n_sections_skipped;
    int32_t n_merged_from;          /* сколько граней вошло в слитые */
    int32_t n_mat[MCM_N_MAT];
} McMeshOutput;

MCMESH_API int mcmesh_abi_version(void);
/* 0 — успех; <0 — ошибка (-1 аргументы, -2 память, -3 неверный ABI таблиц). out инициализируется вызовом. Освобождение — mcmesh_output_free. */
MCMESH_API int mcmesh_chunk(const McMeshTables *t, const McMeshInput *in, const McMeshOptions *opt, McMeshOutput *out);
MCMESH_API void mcmesh_output_free(McMeshOutput *out);

/* Для тестов: выбор варианта (Mth.getSeed → LegacyRandomSource) — вернуть индекс варианта в группе `total` весов по позиции. */
MCMESH_API int64_t mcmesh_mth_get_seed(int32_t x, int32_t y, int32_t z);
MCMESH_API int mcmesh_pick_weighted(int64_t seed, int32_t total);
MCMESH_API float mcmesh_swamp_noise(const uint8_t *perm, double x, double z);

#ifdef __cplusplus
}
#endif
#endif /* MCGEN_MESH_H */

/* mcgen.h — публичный C-ABI библиотеки libmcgen (генератор мира Minecraft 26.x).
 *
 * КОНТРАКТ между потоками работ (рельеф, меши, аддон, эталоны). Менять только осознанно: поднимать MCGEN_ABI_VERSION и
 * править docs/superpowers/specs/2026-10-02-blender-worldgen-addon-design.md.
 *
 * Принципы: C11, без внешних зависимостей; McGen и McWorld неизменяемы после создания и безопасны для чтения из многих потоков;
 * все функции возвращают MCGEN_OK (0) или код ошибки, текст ошибки — в err[errlen]; указатели на данные региона живут до mcgen_region_free.
 * Игровые данные берутся из pack-каталога (tools/make_pack.py): <pack>/data/minecraft/**, <pack>/reports/blocks.json, registries.json.
 */
#ifndef MCGEN_H
#define MCGEN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#  define MCGEN_API __declspec(dllexport)
#else
#  define MCGEN_API __attribute__((visibility("default")))
#endif

#define MCGEN_ABI_VERSION 1

typedef struct McGen McGen;      /* данные версии: датапак, реестры блоков/биомов (создаётся один раз) */
typedef struct McWorld McWorld;  /* измерение + пресет + сиды + тонкие настройки: предвычисленные шумы, дерево биомов */
typedef struct McRegion McRegion;/* результат генерации области чанков */

enum {
    MCGEN_OK = 0, MCGEN_E_ARG = 1, MCGEN_E_IO = 2, MCGEN_E_DATA = 3, MCGEN_E_VERSION = 4,
    MCGEN_E_NOMEM = 5, MCGEN_E_CANCEL = 6, MCGEN_E_UNSUPPORTED = 7, MCGEN_E_INTERNAL = 8
};

/* Стадии генерации (битовая маска). Порядок и зависимости — как у цепочки статусов игры.
 *  BIOMES     : биомы по клеткам 4×4×4 (BiomeManager не применяется — это «сырые» биомы чанка, как в palette biomes секций)
 *  TERRAIN    : заполнение шумом: камень/вода/лава/воздух, aquifer, жилы руд (noise_settings.ore_veins_enabled), Beardifier при STRUCTURES
 *  SURFACE    : noise_settings.surface_rule целиком (трава, песок, бедрок, глубинный сланец, полосы бесплодных земель, лёд…)
 *  CARVERS    : пещеры, каньоны, карверы Нижнего мира (с aquifer-жидкостями)
 *  FEATURES   : placed_feature по шагам декорации (деревья, руды-фичи, растения, озёра…)
 *  STRUCTURES : старты и части построек (+ terrain_adaptation/Beardifier)
 * Каждая стадия включает необходимые предыдущие для расчёта, но в вывод попадает только запрошенное сочетание
 * (например TERRAIN без SURFACE даёт «сырой» камень без бедрока).
 */
enum {
    MC_STAGE_BIOMES = 1u, MC_STAGE_TERRAIN = 2u, MC_STAGE_SURFACE = 4u, MC_STAGE_CARVERS = 8u,
    MC_STAGE_FEATURES = 16u, MC_STAGE_STRUCTURES = 32u, MC_STAGE_ALL = 63u
};

/* Сиды по доменам. «Единый» seed ваниль = все четыре поля равны (mcgen_seeds_unified). */
typedef struct McSeeds {
    int64_t climate;     /* шумы климата: temperature, vegetation, continentalness, erosion, ridge, shift → биомы и форма рельефа */
    int64_t terrain;     /* остальные именованные шумы (пещеры, aquifer, жилы, поверхность, полосы, jagged …) */
    int64_t structures;  /* размещение и генерация построек (48-битный LCG-домен + позиционные фабрики) */
    int64_t features;    /* seed декорации чанка (setDecorationSeed) */
} McSeeds;
static inline McSeeds mcgen_seeds_unified(int64_t s) { McSeeds r = { s, s, s, s }; return r; }

/* Тонкие настройки («режим настройки мира»). Значения по умолчанию воспроизводят ваниль побитово. */
typedef struct McTweakInfo {
    const char *id, *label, *group, *description;
    double def, min, max, soft_min, soft_max;
    int is_int;
} McTweakInfo;
typedef struct McTweakValue { const char *id; double value; } McTweakValue;

typedef int (*McProgressFn)(void *ud, double fraction, const char *what); /* вернуть != 0 — отмена */

/* ---- версия, данные ------------------------------------------------------------------------------------------------ */
MCGEN_API const char *mcgen_version(void);                    /* "libmcgen <semver> abi <N>" */
MCGEN_API int mcgen_abi_version(void);
/* version: "26.1" | "26.2" | "26.3" | "26.4-snapshot-2" (должна соответствовать pack-каталогу) */
MCGEN_API int mcgen_open(const char *pack_dir, const char *version, McGen **out, char *err, size_t errlen);
MCGEN_API void mcgen_close(McGen *g);

MCGEN_API int mcgen_dimension_count(const McGen *g);
MCGEN_API const char *mcgen_dimension_name(const McGen *g, int i);          /* "minecraft:overworld" … */
MCGEN_API int mcgen_preset_count(const McGen *g, const char *dimension);
MCGEN_API const char *mcgen_preset_name(const McGen *g, const char *dimension, int i); /* "normal", "large_biomes", "amplified", … */

MCGEN_API int mcgen_block_state_count(const McGen *g);                       /* число состояний (id < count, помещается в u16) */
MCGEN_API const char *mcgen_block_state_name(const McGen *g, int id);        /* "minecraft:oak_stairs[facing=north,half=bottom,…]" */
MCGEN_API int mcgen_block_state_from_name(const McGen *g, const char *name);   /* -1 если нет */
MCGEN_API int mcgen_biome_count(const McGen *g);
MCGEN_API const char *mcgen_biome_name(const McGen *g, int id);              /* "minecraft:plains"; id — индекс в u8-массивах биомов */

MCGEN_API int mcgen_tweak_count(const McGen *g);
MCGEN_API const McTweakInfo *mcgen_tweak_info(const McGen *g, int i);

/* ---- мир ----------------------------------------------------------------------------------------------------------- */
MCGEN_API int mcgen_world_new(McGen *g, const char *dimension, const char *preset, const McSeeds *seeds,
                              const McTweakValue *tweaks, int ntweaks, McWorld **out, char *err, size_t errlen);
MCGEN_API void mcgen_world_free(McWorld *w);
MCGEN_API int mcgen_world_min_y(const McWorld *w);
MCGEN_API int mcgen_world_height(const McWorld *w);        /* кратно 16 */
MCGEN_API int mcgen_world_sea_level(const McWorld *w);

/* Быстрый предпросмотр без рельефа: биом на блок-координате (с зумом и «размытием» как BiomeManager.getBiome). */
MCGEN_API int mcgen_biome_at(const McWorld *w, int x, int y, int z);
/* Сетка биомов: nx*nz значений u8, шаг step блоков, уровень y; индекс iz*nx+ix. */
MCGEN_API int mcgen_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out);

/* ---- регион -------------------------------------------------------------------------------------------------------- */
typedef struct McRegionInfo { int cx0, cz0, nx, nz, min_y, height; } McRegionInfo;
enum { MC_HM_WORLD_SURFACE = 0, MC_HM_OCEAN_FLOOR = 1, MC_HM_MOTION_BLOCKING = 2, MC_HM_MOTION_BLOCKING_NO_LEAVES = 3 };

/* Генерирует чанки [cx0, cx0+nx) × [cz0, cz0+nz). threads: 0 = по числу ядер. «Гало» соседей для стадий считается внутри. */
MCGEN_API int mcgen_generate_region(McWorld *w, int cx0, int cz0, int nx, int nz, uint32_t stages, int threads,
                                    McProgressFn cb, void *ud, McRegion **out, char *err, size_t errlen);
MCGEN_API void mcgen_region_free(McRegion *r);
MCGEN_API void mcgen_region_info(const McRegion *r, McRegionInfo *info);
/* Блоки чанка: height*256 значений u16, порядок [y][z][x] (y от min_y вверх; индекс ((y - min_y)*16 + z)*16 + x). Изменяемо (редактирование). */
MCGEN_API uint16_t *mcgen_region_blocks(McRegion *r, int cx, int cz);
/* Биомы чанка: (height/4)*16 значений u8, порядок [qy][qz][qx], клетки 4×4×4. */
MCGEN_API uint8_t *mcgen_region_biomes(McRegion *r, int cx, int cz);
/* Карта высот: 256 значений i16 [z][x] — абсолютная y первой свободной клетки над поверхностью (как Heightmap игры). */
MCGEN_API int16_t *mcgen_region_heightmap(McRegion *r, int cx, int cz, int kind);

/* ---- дамп региона (для тестов/эталонов): формат MCR1, см. libmcgen/README.md ---------------------------------------- */
MCGEN_API int mcgen_region_write_mcr(const McRegion *r, const McGen *g, const char *path, char *err, size_t errlen);

/* ---- постройки (стадия STRUCTURES): старты ----------------------------------------------------------------------------- */
typedef struct McStructureStart {
    const char *id;            /* "minecraft:village_plains" (строка живёт до mcgen_world_free) */
    int chunk_x, chunk_z;      /* чанк-источник */
    int bb[6];                 /* x0,y0,z0,x1,y1,z1 — общий bounding box частей */
    int piece_count;
} McStructureStart;
/* Старты построек, чей чанк-источник лежит в [cx0, cx0+nx)×[cz0, cz0+nz) (по умолчанию все наборы измерения; считается лениво, потокобезопасно).
 * Возвращает общее число найденных стартов (может быть больше cap; в out пишется не более cap). */
MCGEN_API int mcgen_structure_starts(McWorld *w, int cx0, int cz0, int nx, int nz, McStructureStart *out, int cap);
/* bounding box piece-й части index-го старта чанка-источника (порядок как в mcgen_structure_starts для одного чанка); 0 — успех */
MCGEN_API int mcgen_structure_piece_bb(McWorld *w, int chunk_x, int chunk_z, int index, int piece, int bb[6]);

/* ---- вычисления на видеокарте (необязательно; библиотека libmcgen_cuda — спека §3.6, ворота G9) -------------------------------
 * Без libmcgen_cuda / без устройства NVIDIA / при любой ошибке всё считается на CPU; результат GPU совпадает с CPU побитно
 * (проверяется самопроверкой при первом использовании мира). По умолчанию режим Auto. */
enum { MCGEN_COMPUTE_CPU = 0, MCGEN_COMPUTE_GPU = 1, MCGEN_COMPUTE_AUTO = 2 };
/* путь к libmcgen_cuda (NULL/"" — искать рядом с libmcgen, в MCGEN_CUDA_LIB и в системных путях) */
MCGEN_API int mcgen_gpu_set_library_path(const char *path);
/* число устройств CUDA (0 — библиотеки/устройства нет) */
MCGEN_API int mcgen_gpu_device_count(void);
MCGEN_API int mcgen_gpu_device_info(int index, char *name, size_t namelen, int *cc_major, int *cc_minor, size_t *mem_mb);
/* режим вычислений (MCGEN_COMPUTE_*) и устройство (device < 0 — не менять); действует на mcgen_biome_grid и прочие GPU-пути */
MCGEN_API int mcgen_gpu_set_compute(int mode, int device);
MCGEN_API int mcgen_gpu_get_compute(void);
/* статус в виде строк «ключ: значение» (mode, state, device, library, reason/last_fallback, selftest); возврат 1 — GPU готово */
MCGEN_API int mcgen_gpu_status(char *buf, size_t buflen);
/* самопроверка GPU = CPU на тестовых мирах версии (Overworld ×3 пресета, Nether, End; npoints случайных точек каждый).
 * MCGEN_OK — расхождений нет; report — подробности */
MCGEN_API int mcgen_gpu_selftest(McGen *g, int npoints, char *report, size_t replen);
/* то же, что mcgen_biome_grid, но строго на GPU (MCGEN_E_UNSUPPORTED, если GPU недоступно) / строго на CPU */
MCGEN_API int mcgen_gpu_biome_grid(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out);
MCGEN_API int mcgen_biome_grid_cpu(const McWorld *w, int x0, int z0, int nx, int nz, int step, int y, uint8_t *out);
/* биомы в произвольных блоковых точках xyz[3n] строго на GPU (тесты); *n_cpu — сколько точек GPU отдал на пересчёт CPU (может быть NULL) */
MCGEN_API int mcgen_gpu_biome_points(const McWorld *w, int n, const int *xyz, uint8_t *out, int *n_cpu);

#ifdef __cplusplus
}
#endif
#endif /* MCGEN_H */

/* mcg_host.h — хостовая часть CUDA-порта: загрузка данных версии (через engine/mc_data.h) и эталонный
 * CPU-расчёт «как в engine/» (те же функции, что в tools/mcquery.c).
 *
 * ПОЧЕМУ ОТДЕЛЬНЫЙ .c: engine/mc_data.h вызывает хостовые функции (mc_spec_*, mc_rt_create, mc_md5*), которые
 * в engine/*.h обёрнуты в `#ifndef __CUDA_ARCH__`, поэтому mc_data.h не компилируется внутри .cu (nvcc видит проход
 * device). Реализация — в mcg_host.c (компилируется gcc как C, линкуется с .cu). Сами заголовки engine/ не менялись.
 */
#ifndef MCG_HOST_H
#define MCG_HOST_H
#include "../mc_biomes.h"
#include "../mc_end.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct McgHost {
    int version;                /* MC_26_1 / MC_26_2 / MC_26_3 */
    int dim;                    /* MC_OVERWORLD / MC_NETHER / MC_END */
    int mode;                   /* MC_NOISE_DOUBLE (26.1/26.2) | MC_NOISE_FLOAT (26.3) */
    char preset[16];            /* overworld: "overworld" | "amplified" | "large_biomes" */
    McClimateSpec clim;         /* Overworld: выбранный пресет */
    McNoiseSpec nether_temp, nether_veg;   /* Nether (legacy) */
    McBiomeTree *tree;          /* R-дерево биомов (Overworld/Nether), NULL для End */
} McgHost;

/* preset: "normal"|"overworld"|"amplified"|"large_biomes" (для не-Overworld игнорируется). Возврат 0 = ок. */
int mcg_host_load(McgHost *H, int version, int dim, const char *preset);
void mcg_host_free(McgHost *H);

int mcg_version_from_name(const char *s);   /* "26.1"|"26.2"|"26.3" -> MC_26_x или -1 */
const char *mcg_version_name(int v);
int mcg_biome_count(void);
const char *mcg_biome_name(int id);
/* "minecraft:plains" | "plains" -> id или -1 */
int mcg_biome_id(const char *name);

/* Эталон (engine, «в лоб», OpenMP по seed'ам).
 * pts: 3 i32 на точку (quartX, quartY, quartZ); out_biome[s*np+p] (u8); out_tg (опц.) — 6 i32 на точку
 * [(s*np+p)*6 ..]: t,h,c,e,d,w (для End — height*1000 не выдаётся: нули). Nether: t,h,0,0,0,0. */
void mcg_ref_points(const McgHost *H, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out_biome, i32 *out_tg);

/* Эталон тай-брейка: перебором ВСЕХ листьев дерева (engine: mc_rnode_distance) — маска биомов листьев с минимальным fitness для цели
 * tg = {t,h,c,e,d,w} (квантованные). out[2] — маска (бит = id биома). */
void mcg_ref_tie_mask(const McgHost *H, const i32 tg[6], u64 out[2]);

#ifdef __cplusplus
}
#endif
#endif

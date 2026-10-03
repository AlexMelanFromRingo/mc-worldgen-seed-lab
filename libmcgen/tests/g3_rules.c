/* g3_rules.c — самопроверка стадии SURFACE без эталонов (поток W2): для каждой найденной версии пака, измерения и пресета
 *   1) разбор правил поверхности проходит (mcgen_world_new), регион 3×3 чанка со стадиями BIOMES|TERRAIN|SURFACE генерируется;
 *   2) результат не зависит от числа потоков (1 против 4) и от порядка/окружения чанка: центральный чанк региона 3×3 равен чанку,
 *      сгенерированному отдельным регионом 1×1 (растекание жидкостей выключено твиком fluid_flow = 0);
 *   3) поверхность действительно применена: в Overworld/Nether есть бедрок, в Overworld — глубинный сланец или трава/песок.
 * Без аргументов (make test); корень репозитория — $MCGEN_ROOT или ../ (run/pack-<V>). Нет пака — версия пропускается. */
#include "../include/mcgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* контракт для карверов (surface.c): материал верхнего слоя в позиции (абсолютные x, y, z) */
int mcgen_surface_top_material(McWorld *w, void *t, int cx, int cz, const uint16_t *blocks, int x, int y, int z, int under_fluid);

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  ПРОВАЛ: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static long count_named(McGen *g, const uint16_t *b, size_t n, const char *prefix) {
    int ns = mcgen_block_state_count(g);
    char *hit = calloc((size_t)ns, 1);
    for (int i = 0; i < ns; i++) if (!strncmp(mcgen_block_state_name(g, i), prefix, strlen(prefix))) hit[i] = 1;
    long c = 0;
    for (size_t i = 0; i < n; i++) c += hit[b[i]];
    free(hit);
    return c;
}

static int run_one(McGen *g, const char *ver, const char *dim, const char *preset) {
    char err[512] = {0};
    McSeeds sd = mcgen_seeds_unified(8675309);
    McTweakValue tw[1] = { { "fluid_flow", 0.0 } };
    McWorld *w;
    if (mcgen_world_new(g, dim, preset, &sd, tw, 1, &w, err, sizeof err)) { printf("  %s %s/%s: world_new: %s\n", ver, dim, preset, err); failures++; return 1; }
    int H = mcgen_world_height(w);
    size_t csz = (size_t)H * 256;
    McRegion *a, *b, *c;
    uint32_t st = MC_STAGE_BIOMES | MC_STAGE_TERRAIN | MC_STAGE_SURFACE;
    int rc = mcgen_generate_region(w, -1, -1, 3, 3, st, 4, NULL, NULL, &a, err, sizeof err);
    if (rc) { printf("  %s %s/%s: generate: %s\n", ver, dim, preset, err); failures++; mcgen_world_free(w); return 1; }
    rc = mcgen_generate_region(w, -1, -1, 3, 3, st, 1, NULL, NULL, &b, err, sizeof err);
    CHECK(!rc, "%s %s/%s: повторная генерация: %s", ver, dim, preset, err);
    if (!rc) {
        int same = 1;
        for (int cz = -1; cz <= 1 && same; cz++) for (int cx = -1; cx <= 1 && same; cx++)
            same = !memcmp(mcgen_region_blocks(a, cx, cz), mcgen_region_blocks(b, cx, cz), csz * 2);
        CHECK(same, "%s %s/%s: результат зависит от числа потоков", ver, dim, preset);
        mcgen_region_free(b);
    }
    rc = mcgen_generate_region(w, 0, 0, 1, 1, st, 1, NULL, NULL, &c, err, sizeof err);
    CHECK(!rc, "%s %s/%s: одиночный чанк: %s", ver, dim, preset, err);
    if (!rc) {
        CHECK(!memcmp(mcgen_region_blocks(a, 0, 0), mcgen_region_blocks(c, 0, 0), csz * 2), "%s %s/%s: чанк в регионе != одиночный чанк", ver, dim, preset);
        mcgen_region_free(c);
    }
    /* поверхность применена */
    const uint16_t *blk = mcgen_region_blocks(a, 0, 0);
    long bedrock = count_named(g, blk, csz, "minecraft:bedrock");
    int is_end = strstr(dim, "the_end") != NULL, is_ow = strstr(dim, "overworld") != NULL;
    if (!is_end && strcmp(preset, "floating_islands")) CHECK(bedrock > 0, "%s %s/%s: нет бедрока", ver, dim, preset);
    if (is_ow && strcmp(preset, "floating_islands")) CHECK(count_named(g, blk, csz, "minecraft:deepslate") > 0, "%s %s/%s: нет глубинного сланца", ver, dim, preset);
    /* topMaterial (SurfaceSystem/MaterialSystem.topMaterial) на верхнем открытом блоке столбца: stoneAbove = stoneBelow = 1 даёт
     * тот же материал, что полный проход (кроме потолочных правил, жил и расширений) — доля совпадений должна быть ≈ 100 % */
    {
        long tot = 0, same = 0;
        for (int z = 0; z < 16; z += 3) for (int x = 0; x < 16; x += 3) {
            int y = H - 1;
            for (; y > 0; y--) { int s = blk[(size_t)y * 256 + z * 16 + x]; if (strcmp(mcgen_block_state_name(g, s), "minecraft:air")) break; }
            if (y <= 0) continue;
            if (!strncmp(mcgen_block_state_name(g, blk[(size_t)y * 256 + z * 16 + x]), "minecraft:water", 15)) continue;   /* столбец под водой: topMaterial по замыслу даёт «верхний» материал */
            int top = mcgen_surface_top_material(w, NULL, 0, 0, blk, x, mcgen_world_min_y(w) + y, z, 0);
            tot++; if (top == blk[(size_t)y * 256 + z * 16 + x]) same++;
        }
        CHECK(tot == 0 || same * 100 >= tot * 90, "%s %s/%s: topMaterial совпал с проходом поверхности в %ld из %ld столбцов", ver, dim, preset, same, tot);
        if (getenv("G3_VERBOSE")) printf("    topMaterial: %ld / %ld\n", same, tot);
    }
    printf("  %-16s %-22s %-18s ok (бедрок %ld)\n", ver, dim, preset, bedrock);
    mcgen_region_free(a);
    mcgen_world_free(w);
    return 0;
}

int main(void) {
    const char *root = getenv("MCGEN_ROOT");
    static const char *V[] = { "26.1", "26.2", "26.3", "26.4-snapshot-2" };
    int ran = 0;
    for (int v = 0; v < 4; v++) {
        char pack[1024], err[512] = {0};
        snprintf(pack, sizeof pack, "%s/run/pack-%s", root ? root : "..", V[v]);
        McGen *g;
        if (mcgen_open(pack, V[v], &g, err, sizeof err)) { printf("версия %s: пак не найден (%s) — пропуск\n", V[v], pack); continue; }
        printf("версия %s\n", V[v]);
        for (int d = 0; d < mcgen_dimension_count(g); d++) {
            const char *dim = mcgen_dimension_name(g, d);
            for (int p = 0; p < mcgen_preset_count(g, dim); p++) { run_one(g, V[v], dim, mcgen_preset_name(g, dim, p)); ran++; }
        }
        mcgen_close(g);
    }
    printf("g3_rules: %d комбинаций, провалов %d\n", ran, failures);
    return failures ? 1 : 0;
}

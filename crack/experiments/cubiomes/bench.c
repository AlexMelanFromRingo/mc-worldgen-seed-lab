// Бенчмарк cubiomes (ref: refs/cubiomes, MC_1_21_3/WD) на одной машине.
// Сборка: ./build.sh [release|native|fastmath]. Запуск: ./bench [секция ...] [reps=N]
// Секции: sizes setup sample biomeat structs threads quad
// Машина в момент измерения может быть загружена другими процессами, поэтому
// везде берётся минимум по g_reps повторов (best-of-N).
#define _GNU_SOURCE
#include "generator.h"
#include "finders.h"
#include "quadbase.h"
#include "util.h"
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <string.h>

// wall-clock (для многопоточных замеров)
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
// CPU-время потока: меньше зависит от того, что процесс вытесняли другие задачи
static double cpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t g_sink;
static int g_reps = 5;

// TMIN(N, stmts...) -> ns на итерацию (минимум по g_reps прогонов по N итераций)
#define TMIN(N, ...) ({ double _best = 1e30; \
    for (int _r = 0; _r < g_reps; _r++) { double _t0 = cpu_now(); \
        for (int _i = 0; _i < (N); _i++) { __VA_ARGS__; } \
        double _t = (cpu_now() - _t0) / (N); if (_t < _best) _best = _t; } \
    _best * 1e9; })

static uint64_t xs(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}

static int want(int argc, char **argv, const char *name)
{
    int any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "reps=", 5)) continue;
        any = 1;
        if (!strcmp(argv[i], name)) return 1;
    }
    return !any;
}

//------------------------------------------------------------------------------
static void bench_sizes(void)
{
    printf("[sizes]\n");
    printf("sizeof(PerlinNoise)=%zu  OctaveNoise=%zu  DoublePerlinNoise=%zu\n",
        sizeof(PerlinNoise), sizeof(OctaveNoise), sizeof(DoublePerlinNoise));
    printf("sizeof(BiomeNoise)=%zu  (climate[6]=%zu, oct[46]=%zu, SplineStack=%zu)\n",
        sizeof(BiomeNoise), sizeof(((BiomeNoise*)0)->climate),
        sizeof(((BiomeNoise*)0)->oct), sizeof(SplineStack));
    printf("sizeof(Spline)=%zu  FixSpline=%zu\n", sizeof(Spline), sizeof(FixSpline));
    printf("sizeof(NetherNoise)=%zu  EndNoise=%zu  SurfaceNoise=%zu\n",
        sizeof(NetherNoise), sizeof(EndNoise), sizeof(SurfaceNoise));
    printf("sizeof(Generator)=%zu  LayerStack=%zu\n", sizeof(Generator), sizeof(LayerStack));
    Generator g;
    setupGenerator(&g, MC_1_21_WD, 0);
    applySeed(&g, DIM_OVERWORLD, 12345);
    int n = 0;
    for (int i = 0; i < NP_MAX; i++)
    {
        printf("  climate[%d]: octA=%d octB=%d amp=%.6f\n", i,
            g.bn.climate[i].octA.octcnt, g.bn.climate[i].octB.octcnt,
            g.bn.climate[i].amplitude);
        n += g.bn.climate[i].octA.octcnt + g.bn.climate[i].octB.octcnt;
    }
    printf("  total perlin octaves in use: %d (buffer 46)\n", n);
    printf("  spline stack used: %d Spline + %d FixSpline (capacity 42/151)\n",
        g.bn.ss.len, g.bn.ss.flen);
    printf("  tree btree21wd: nodes=9113 (uint64) = %d B; param pairs=139 (int32) = %d B; steps=6 (uint32)\n",
        9113 * 8, 139 * 8);
}

//------------------------------------------------------------------------------
static void bench_seed_setup(void)
{
    printf("[seed setup]  (best-of-%d)\n", g_reps);
    Generator g;
    setupGenerator(&g, MC_1_21_WD, 0);
    uint64_t k = 0x9e3779b97f4a7c15ULL;
    double ns;

    ns = TMIN(20000, { BiomeNoise bn; initBiomeNoise(&bn, MC_1_21_WD); g_sink += bn.ss.len; });
    printf("initBiomeNoise (spline stack, once per generator): %.0f ns\n", ns);

    ns = TMIN(5000, { applySeed(&g, DIM_OVERWORLD, (uint64_t)_i * k); g_sink += g.sha; });
    printf("applySeed(OVERWORLD 1.18+, incl. sha256): %.2f us/seed -> %.0f seeds/s/core\n", ns / 1e3, 1e9 / ns);

    ns = TMIN(5000, { setBiomeSeed(&g.bn, (uint64_t)_i * k, 0); g_sink += g.bn.nptype; });
    printf("setBiomeSeed only (no sha): %.2f us/seed -> %.0f seeds/s/core\n", ns / 1e3, 1e9 / ns);

    ns = TMIN(200000, { g_sink += getVoronoiSHA((uint64_t)_i * k); });
    printf("getVoronoiSHA: %.1f ns\n", ns);

    ns = TMIN(2000000, { Xoroshiro x; xSetSeed(&x, _i); g_sink += xNextLong(&x); });
    printf("xSetSeed+xNextLong: %.2f ns\n", ns);

    ns = TMIN(200000, { PerlinNoise p; Xoroshiro x; xSetSeed(&x, _i); xPerlinInit(&p, &x); g_sink += p.d[7]; });
    printf("xPerlinInit (1 Perlin: 3 doubles + 256 Fisher-Yates): %.0f ns\n", ns);

    static const char *nm[] = {"temperature", "humidity", "continentalness", "erosion", "shift", "weirdness"};
    for (int t = 0; t < 6; t++)
    {
        ns = TMIN(20000, { setClimateParaSeed(&g.bn, (uint64_t)_i * k, 0, t, -1); g_sink += g.bn.nptype; });
        printf("setClimateParaSeed(%-16s all octaves): %.2f us\n", nm[t], ns / 1e3);
    }
    ns = TMIN(20000, { setClimateParaSeed(&g.bn, (uint64_t)_i * k, 0, NP_TEMPERATURE, 2); g_sink += g.bn.nptype; });
    printf("setClimateParaSeed(temperature, nmax=2 octaves): %.2f us\n", ns / 1e3);
    ns = TMIN(5000, { setClimateParaSeed(&g.bn, (uint64_t)_i * k, 0, NP_DEPTH, -1); g_sink += g.bn.nptype; });
    printf("setClimateParaSeed(NP_DEPTH = c+e+w): %.2f us\n", ns / 1e3);
    ns = TMIN(5000, { setBiomeSeed(&g.bn, (uint64_t)_i * k, LARGE_BIOMES); g_sink += g.bn.nptype; });
    printf("setBiomeSeed(large=1): %.2f us\n", ns / 1e3);
}

//------------------------------------------------------------------------------
static void bench_sample(void)
{
    printf("[sampleBiomeNoise]  (best-of-%d)\n", g_reps);
    Generator g;
    setupGenerator(&g, MC_1_21_WD, 0);
    applySeed(&g, DIM_OVERWORLD, 123456789);
    int N = 200000;
    uint64_t r = 88172645463325252ULL;
    int *xs_ = malloc(sizeof(int) * N * 3);
    for (int i = 0; i < N; i++)
    {
        xs_[3*i+0] = (int)(xs(&r) % 20001) - 10000;
        xs_[3*i+1] = (int)(xs(&r) % 100) - 16;
        xs_[3*i+2] = (int)(xs(&r) % 20001) - 10000;
    }
    struct { const char *n; uint32_t f; } modes[] = {
        {"flags=0 (полный: shift+depth+tree)", 0},
        {"SAMPLE_NO_SHIFT", SAMPLE_NO_SHIFT},
        {"SAMPLE_NO_DEPTH", SAMPLE_NO_DEPTH},
        {"NO_SHIFT|NO_DEPTH", SAMPLE_NO_SHIFT | SAMPLE_NO_DEPTH},
        {"NO_BIOME (6 шумов + сплайн, без дерева)", SAMPLE_NO_BIOME},
        {"NO_BIOME|NO_SHIFT|NO_DEPTH (c,e,w,t,h)", SAMPLE_NO_BIOME | SAMPLE_NO_SHIFT | SAMPLE_NO_DEPTH},
    };
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
    {
        double ns = TMIN(N, g_sink += sampleBiomeNoise(&g.bn, NULL, xs_[3*_i], xs_[3*_i+1], xs_[3*_i+2], NULL, modes[m].f));
        printf("%-44s %8.0f ns/point (%.3f Mpoints/s/core)\n", modes[m].n, ns, 1e3 / ns);
    }
    {
        uint64_t dat = 0;
        int M = 4000;
        double ns = TMIN(M, g_sink += sampleBiomeNoise(&g.bn, NULL, _i, 15, 7, &dat, 0));
        printf("%-44s %8.0f ns/point\n", "sequential row + dat (leaf cache)", ns);
        ns = TMIN(M, g_sink += sampleBiomeNoise(&g.bn, NULL, _i, 15, 7, NULL, 0));
        printf("%-44s %8.0f ns/point\n", "sequential row, no cache", ns);
    }
    {
        struct { const char *n; int t; } nn[] = {
            {"temperature", NP_TEMPERATURE}, {"humidity", NP_HUMIDITY},
            {"continentalness", NP_CONTINENTALNESS}, {"erosion", NP_EROSION},
            {"shift (offset)", NP_SHIFT}, {"weirdness", NP_WEIRDNESS}};
        for (int k = 0; k < 6; k++)
        {
            const DoublePerlinNoise *d = &g.bn.climate[nn[k].t];
            double acc = 0;
            double ns = TMIN(N, acc += sampleDoublePerlin(d, xs_[3*_i], 0, xs_[3*_i+2]));
            g_sink += (uint64_t)acc;
            printf("sampleDoublePerlin %-16s octA=%d octB=%d (%d Perlin): %6.1f ns => %.1f ns/Perlin\n",
                nn[k].n, d->octA.octcnt, d->octB.octcnt, d->octA.octcnt + d->octB.octcnt, ns,
                ns / (d->octA.octcnt + d->octB.octcnt));
        }
        const PerlinNoise *p = &g.bn.climate[NP_CONTINENTALNESS].octA.octaves[0];
        double acc = 0;
        double ns = TMIN(N, acc += samplePerlin(p, xs_[3*_i] * 0.37, 0, xs_[3*_i+2] * 0.37, 0, 0));
        g_sink += (uint64_t)acc;
        printf("samplePerlin (1 octave, y=0 fast path): %.1f ns\n", ns);
        ns = TMIN(N, acc += samplePerlin(p, xs_[3*_i] * 0.37, xs_[3*_i+1] * 0.37 + 0.11, xs_[3*_i+2] * 0.37, 0, 0));
        g_sink += (uint64_t)acc;
        printf("samplePerlin (1 octave, y!=0): %.1f ns\n", ns);
    }
    {
        int64_t *np = malloc(sizeof(int64_t) * 6 * 4096);
        for (int i = 0; i < 4096; i++)
            sampleBiomeNoise(&g.bn, &np[6 * i], xs_[3*i], xs_[3*i+1], xs_[3*i+2], NULL, SAMPLE_NO_BIOME);
        double ns = TMIN(200000, g_sink += climateToBiome(MC_1_21_WD, (const uint64_t*)&np[6 * (_i & 4095)], NULL));
        printf("climateToBiome (реальные np из шумов, без cache): %.0f ns\n", ns);
        uint64_t dat = 0;
        ns = TMIN(200000, g_sink += climateToBiome(MC_1_21_WD, (const uint64_t*)&np[6 * (_i & 4095)], &dat));
        printf("climateToBiome (с dat, случайный порядок): %.0f ns\n", ns);
        for (int i = 0; i < 4096; i++)
        {
            np[6*i+0] = (int64_t)(xs(&r) % 20000) - 10000;
            np[6*i+1] = (int64_t)(xs(&r) % 20000) - 10000;
            np[6*i+2] = (int64_t)(xs(&r) % 22000) - 12000;
            np[6*i+3] = (int64_t)(xs(&r) % 20000) - 10000;
            np[6*i+4] = (int64_t)(xs(&r) % 20000) - 10000;
            np[6*i+5] = (int64_t)(xs(&r) % 20000) - 10000;
        }
        ns = TMIN(200000, g_sink += climateToBiome(MC_1_21_WD, (const uint64_t*)&np[6 * (_i & 4095)], NULL));
        printf("climateToBiome (равномерные np в кубе): %.0f ns\n", ns);
        free(np);
    }
    free(xs_);
}

//------------------------------------------------------------------------------
static void bench_getbiomeat(void)
{
    printf("[getBiomeAt / genBiomes / other dims]  (best-of-%d)\n", g_reps);
    Generator g;
    setupGenerator(&g, MC_1_21_WD, 0);
    applySeed(&g, DIM_OVERWORLD, 123456789);
    int scales[] = {1, 4, 16, 64, 256};
    for (int s = 0; s < 5; s++)
    {
        uint64_t r = 12345;
        double ns = TMIN(50000, {
            int x = (int)(xs(&r) % 4001) - 2000, z = (int)(xs(&r) % 4001) - 2000;
            g_sink += getBiomeAt(&g, scales[s], x, scales[s] == 1 ? 63 : 15, z); });
        printf("getBiomeAt scale=%-3d (Overworld 1.21wd, via malloc'ed cache): %8.0f ns/call\n", scales[s], ns);
    }
    {
        uint64_t r = 12345;
        double ns = TMIN(50000, {
            int x = (int)(xs(&r) % 4001) - 2000, z = (int)(xs(&r) % 4001) - 2000;
            g_sink += sampleBiomeNoise(&g.bn, NULL, x, 15, z, NULL, 0); });
        printf("sampleBiomeNoise direct (scale 4, no malloc): %8.0f ns/call\n", ns);
    }
    setupGenerator(&g, MC_1_21_WD, LARGE_BIOMES);
    applySeed(&g, DIM_OVERWORLD, 123456789);
    {
        uint64_t r = 12345;
        double ns = TMIN(50000, {
            int x = (int)(xs(&r) % 4001) - 2000, z = (int)(xs(&r) % 4001) - 2000;
            g_sink += getBiomeAt(&g, 4, x, 15, z); });
        printf("getBiomeAt scale=4 LARGE_BIOMES: %8.0f ns/call\n", ns);
    }
    setupGenerator(&g, MC_1_21_WD, 0);
    applySeed(&g, DIM_OVERWORLD, 123456789);
    for (int s = 0; s < 4; s++)
    {
        int sc = scales[s];
        Range r = {sc, -32, -32, 64, 64, sc == 1 ? 63 : 15, 1};
        int *c = allocCache(&g, r);
        double ns = TMIN(sc == 1 ? 3 : 10, { genBiomes(&g, c, r); g_sink += c[0]; });
        printf("genBiomes 64x64 scale=%-3d: %.2f ms/area = %.0f ns/cell\n", sc, ns / 1e6, ns / 4096);
        free(c);
    }
    {
        Generator gn; setupGenerator(&gn, MC_1_21_WD, 0);
        double ns = TMIN(5000, { applySeed(&gn, DIM_NETHER, _i); g_sink += gn.sha; });
        printf("applySeed(NETHER): %.2f us/seed\n", ns / 1e3);
        applySeed(&gn, DIM_NETHER, 42);
        uint64_t r2 = 3;
        ns = TMIN(200000, g_sink += getNetherBiome(&gn.nn, (int)(xs(&r2) % 2000) - 1000, 0, (int)(xs(&r2) % 2000) - 1000, NULL));
        printf("getNetherBiome: %.0f ns/point\n", ns);
        ns = TMIN(5000, { applySeed(&gn, DIM_END, _i); g_sink += gn.sha; });
        printf("applySeed(END): %.2f us/seed\n", ns / 1e3);
        applySeed(&gn, DIM_END, 42);
        ns = TMIN(20000, g_sink += getBiomeAt(&gn, 16, (int)(xs(&r2) % 2000) - 1000, 0, (int)(xs(&r2) % 2000) - 1000));
        printf("getBiomeAt(END, scale=16): %.0f ns/point\n", ns);
        ns = TMIN(20000, g_sink += getBiomeAt(&gn, 1, (int)(xs(&r2) % 2000) - 1000, 60, (int)(xs(&r2) % 2000) - 1000));
        printf("getBiomeAt(END, scale=1): %.0f ns/point\n", ns);
        g_reps = 3;
        ns = TMIN(300, g_sink += getEndSurfaceHeight(MC_1_21_WD, _i, (int)(xs(&r2) % 4000) - 2000, (int)(xs(&r2) % 4000) - 2000));
        printf("getEndSurfaceHeight (incl. setEndSeed+initSurfaceNoise): %.1f us/call\n", ns / 1e3);
        g_reps = 5;
    }
}

//------------------------------------------------------------------------------
static void bench_structs(void)
{
    printf("[structures]  (best-of-%d)\n", g_reps);
    int types[] = {Swamp_Hut, Desert_Pyramid, Village, Monument, Mansion, Outpost,
        Ruined_Portal, Ancient_City, Trail_Ruins, Trial_Chambers, Ocean_Ruin, Shipwreck, Fortress, Bastion, End_City};
    Generator g;
    int mc = MC_1_21_WD;
    for (unsigned k = 0; k < sizeof(types) / sizeof(types[0]); k++)
    {
        int st = types[k];
        StructureConfig sc;
        if (!getStructureConfig(st, mc, &sc)) continue;
        setupGenerator(&g, mc, 0);
        int dim = sc.dim ? sc.dim : DIM_OVERWORLD;
        uint64_t r = 99;
        double t_pos = TMIN(200000, {
            Pos p;
            g_sink += getStructurePos(st, mc, xs(&r) & MASK48, (int)(xs(&r) % 200) - 100, (int)(xs(&r) % 200) - 100, &p);
            g_sink += p.x; });
        applySeed(&g, dim, 777);
        enum { M = 4000 };
        Pos *ps = malloc(sizeof(Pos) * M);
        int n = 0;
        for (int i = 0; i < 20 * M && n < M; i++)
        {
            Pos p;
            int rx = (int)(xs(&r) % 400) - 200, rz = (int)(xs(&r) % 400) - 200;
            if (getStructurePos(st, mc, 777, rx, rz, &p)) ps[n++] = p;
        }
        int viable = 0;
        double ns = TMIN(n, { g_sink += isViableStructurePos(st, &g, ps[_i].x, ps[_i].z, 0); });
        for (int i = 0; i < n; i++) viable += !!isViableStructurePos(st, &g, ps[i].x, ps[i].z, 0);
        printf("%-15s getStructurePos: %6.1f ns | isViableStructurePos (fixed seed, %d pos): %8.0f ns/call, viable=%.1f%%\n",
            struct2str(st), t_pos, n, ns, 100.0 * viable / (n ? n : 1));
        free(ps);
    }
    {
        setupGenerator(&g, mc, 0);
        applySeed(&g, DIM_OVERWORLD, 777);
        uint64_t r = 5;
        double ns = TMIN(20000, g_sink += isViableStructureTerrain(Desert_Pyramid, &g, (int)(xs(&r) % 4000) - 2000, (int)(xs(&r) % 4000) - 2000));
        printf("isViableStructureTerrain(Desert_Pyramid) [4 x NP_DEPTH samples]: %.0f ns\n", ns);
    }
    {
        uint64_t r = 5; int cnt = 0;
        double ns = TMIN(2000000, cnt += isSlimeChunk(0x123456789abcULL + _i, (int)(xs(&r) % 200) - 100, (int)(xs(&r) % 200) - 100));
        g_sink += cnt;
        printf("isSlimeChunk: %.1f ns\n", ns);
    }
    {
        setupGenerator(&g, mc, 0);
        double ns = TMIN(100, {
            applySeed(&g, DIM_OVERWORLD, 1000 + _i);
            StrongholdIter sh;
            initFirstStronghold(&sh, mc, 1000 + _i);
            for (int k = 0; k < 3; k++) { nextStronghold(&sh, &g); g_sink += sh.pos.x; } });
        printf("applySeed + first 3 strongholds (locateBiome r=112): %.1f us/seed\n", ns / 1e3);
        ns = TMIN(200000, { StrongholdIter sh; initFirstStronghold(&sh, mc, 1000 + _i); g_sink += sh.nextapprox.x; });
        printf("initFirstStronghold only (approx position, lower 48 bits): %.1f ns\n", ns);
        ns = TMIN(100, { applySeed(&g, DIM_OVERWORLD, 5000 + _i); Pos p = estimateSpawn(&g, NULL); g_sink += p.x; });
        printf("applySeed + estimateSpawn (findFittestPos): %.1f us/seed\n", ns / 1e3);
        g_reps = 3;
        ns = TMIN(30, { applySeed(&g, DIM_OVERWORLD, 5000 + _i); Pos p = getSpawn(&g); g_sink += p.x; });
        printf("applySeed + getSpawn (mapApproxHeight scan): %.1f us/seed\n", ns / 1e3);
        g_reps = 5;
    }
}

//------------------------------------------------------------------------------
typedef struct { uint64_t start, count; int mode; uint64_t hits; double secs; } job_t;

static void *worker(void *p)
{
    job_t *j = (job_t*)p;
    Generator g;
    setupGenerator(&g, MC_1_21_WD, 0);
    uint64_t hits = 0;
    double t0 = now();
    if (j->mode == 0)
    {
        for (uint64_t i = 0; i < j->count; i++)
        {
            applySeed(&g, DIM_OVERWORLD, j->start + i);
            hits += getBiomeAt(&g, 4, 0, 15, 0) == mushroom_fields;
        }
    }
    else if (j->mode == 1)
    {
        for (uint64_t i = 0; i < j->count; i++)
        {
            setBiomeSeed(&g.bn, j->start + i, 0);
            hits += sampleBiomeNoise(&g.bn, NULL, 0, 15, 0, NULL, 0) == mushroom_fields;
        }
    }
    else if (j->mode == 2)
    {
        StructureConfig sc; getStructureConfig(Swamp_Hut, MC_1_21_WD, &sc);
        for (uint64_t i = 0; i < j->count; i++)
        {
            Pos p = getFeaturePos(sc, j->start + i, 0, 0);
            hits += (p.x < 64 && p.z < 64);
        }
    }
    else if (j->mode == 3)
    {
        StructureConfig sc; getStructureConfig(Swamp_Hut, MC_1_21_WD, &sc);
        for (uint64_t i = 0; i < j->count; i++)
            hits += isQuadBaseFeature24(sc, (j->start + i) & MASK48, 7 + 1, 7 + 1, 9 + 1) != 0;
    }
    else if (j->mode == 4)
    {
        StructureConfig sc; getStructureConfig(Swamp_Hut, MC_1_21_WD, &sc);
        for (uint64_t i = 0; i < j->count; i++)
            hits += isQuadBaseFeature24Classic(sc, (j->start + i) & MASK48) != 0;
    }
    else if (j->mode == 5)
    {   // README: lower48 -> Village pos в регионе (0,0) < 96 блоков -> applySeed+isViableStructurePos
        for (uint64_t i = 0; i < j->count; i++)
        {
            uint64_t s = j->start + i;
            Pos p;
            getStructurePos(Village, MC_1_21_WD, s, 0, 0, &p);
            if (p.x >= 96 || p.z >= 96) continue;
            applySeed(&g, DIM_OVERWORLD, s);
            hits += isViableStructurePos(Village, &g, p.x, p.z, 0);
        }
    }
    j->secs = now() - t0;
    j->hits = hits;
    g_sink += hits;
    return NULL;
}

static double run_threads(int nthr, int mode, uint64_t per_thread, uint64_t *hits)
{
    pthread_t th[64]; job_t jobs[64];
    double t0 = now();
    for (int i = 0; i < nthr; i++)
    {
        jobs[i].start = 0x100000000ULL * (i + 1); jobs[i].count = per_thread; jobs[i].mode = mode;
        pthread_create(&th[i], NULL, worker, &jobs[i]);
    }
    uint64_t h = 0;
    for (int i = 0; i < nthr; i++) { pthread_join(th[i], NULL); h += jobs[i].hits; }
    double t1 = now();
    if (hits) *hits = h;
    return t1 - t0;
}

static void bench_threads(void)
{
    int reps = g_reps > 3 ? 3 : g_reps;
    printf("[seed search throughput]  (best-of-%d)\n", reps);
    struct { const char *name; int mode; uint64_t per; } t[] = {
        {"applySeed + getBiomeAt(4,0,15,0) per seed", 0, 20000},
        {"setBiomeSeed + sampleBiomeNoise(1 point) per seed", 1, 20000},
        {"getFeaturePos(Swamp_Hut region 0,0) per seed [lower48]", 2, 200000000},
        {"isQuadBaseFeature24 (quad-hut base filter) per seed [lower48]", 3, 200000000},
        {"isQuadBaseFeature24Classic per seed [lower48]", 4, 400000000},
        {"README pipeline: village(0,0)<96 [lower48] -> applySeed+isViableStructurePos", 5, 10000000},
    };
    int nthr[] = {1, 6, 12};
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++)
    {
        printf("%s\n", t[k].name);
        for (int q = 0; q < 3; q++)
        {
            uint64_t hits = 0;
            uint64_t per = t[k].per;
            double best = 1e30;
            for (int rr = 0; rr < reps; rr++)
            {
                uint64_t h;
                double s = run_threads(nthr[q], t[k].mode, per, &h);
                if (s < best) { best = s; hits = h; }
            }
            double rate = (double)per * nthr[q] / best;
            printf("   threads=%2d: %.2f s for %.3g seeds -> %.4g seeds/s total (%.4g per thread), hits=%llu\n",
                nthr[q], best, (double)per * nthr[q], rate, rate / nthr[q], (unsigned long long)hits);
        }
    }
}

//------------------------------------------------------------------------------
static int chk_ideal(uint64_t s48, void *data)
{
    const StructureConfig sconf = *(const StructureConfig*)data;
    return isQuadBase(sconf, s48 - sconf.salt, 128);
}

static void bench_quad(int threads)
{
    printf("[quad-hut searchAll48, threads=%d]\n", threads);
    StructureConfig sconf;
    getStructureConfig(Swamp_Hut, MC_1_21_WD, &sconf);
    uint64_t *bases = NULL, basecnt = 0;
    double t0 = now();
    int err = searchAll48(&bases, &basecnt, NULL, threads, low20QuadIdeal, 20, chk_ideal, &sconf, NULL);
    double t1 = now();
    printf("searchAll48(low20QuadIdeal, 3 x 2^28 candidates): err=%d, %llu bases, %.2f s\n", err, (unsigned long long)basecnt, t1 - t0);
    for (uint64_t i = 0; i < basecnt && i < 6; i++)
        printf("   base[%llu]=%llu (0x%llx)\n", (unsigned long long)i, (unsigned long long)bases[i], (unsigned long long)bases[i]);
    free(bases);
    bases = NULL; basecnt = 0;
    t0 = now();
    err = searchAll48(&bases, &basecnt, NULL, threads, low20QuadClassic, 20, chk_ideal, &sconf, NULL);
    t1 = now();
    printf("searchAll48(low20QuadClassic, 4 x 2^28): err=%d, %llu bases, %.2f s\n", err, (unsigned long long)basecnt, t1 - t0);
    free(bases);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "reps=", 5)) g_reps = atoi(argv[i] + 5);
    if (want(argc, argv, "sizes")) bench_sizes();
    if (want(argc, argv, "setup")) bench_seed_setup();
    if (want(argc, argv, "sample")) bench_sample();
    if (want(argc, argv, "biomeat")) bench_getbiomeat();
    if (want(argc, argv, "structs")) bench_structs();
    if (want(argc, argv, "threads")) bench_threads();
    if (want(argc, argv, "quad")) { bench_quad(12); bench_quad(1); }
    printf("sink=%llu\n", (unsigned long long)g_sink);
    return 0;
}

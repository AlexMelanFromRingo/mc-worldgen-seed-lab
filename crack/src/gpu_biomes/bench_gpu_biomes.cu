/* bench_gpu_biomes.cu — тест (c): пропускная способность.
 *   gpu-biomes-bench [--versions 26.1 26.2 26.3] [--dims ow,ow_large,ow_amp,nether,end] [--quick] [--cpu]
 * Метрики (GPU, время только ядра, лучший из 3 запусков):
 *   init/с        : seed'ов/с при np=1 точке на seed (инициализация шумов + 1 точка) — «кандидатов/с» при 1 наблюдении
 *   точек/с (вал) : seed'ы x M точек на поток (инициализация амортизирована) — чистая скорость вычисления биома
 *   поиск/с       : k_search, 2^N случайных кандидатов, 8 наблюдений (реальные точки мира seed X): отсев после 1-го наблюдения
 *   поиск полный  : то же, но маски «любой биом» => все 8 наблюдений вычисляются у каждого кандидата (цена наблюдения)
 * --cpu добавляет те же измерения для CPU-режима (тот же код, OpenMP, все ядра).
 */
#include "gpu_biomes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

static u64 rs = 0x9E3779B97F4A7C15ULL;
static u64 rnd64() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs * 0x2545F4914F6CDD1DULL; }
static i32 rrange(i32 lo, i32 hi) { return lo + (i32)(rnd64() % (u64)((i64)hi - lo + 1)); }
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Cfg { const char *name; int dim; const char *preset; };

/* Калибровка доли GPU, доступной процессу (GPU общая с другими процессами — WSL2/хост/другие агенты): ядро из чистых FFMA (8 независимых
 * цепочек), идеальное время = FMA/(SM*128*такт). s = идеальное/измеренное (<=1). Оценка грубая (±15 %): пик FFMA не проверялся на полностью
 * свободной карте. Нормированная скорость = сырая/s — нижняя граница скорости на эксклюзивной GPU (берётся максимум s по нескольким замерам). */
__global__ void k_fma(float *out, int iters) {
    float a0 = threadIdx.x * 1e-3f, a1 = a0 + 1, a2 = a0 + 2, a3 = a0 + 3, a4 = a0 + 4, a5 = a0 + 5, a6 = a0 + 6, a7 = a0 + 7;
    const float m = 0.99999f, c = 1e-7f;
    for (int i = 0; i < iters; i++) {
        a0 = fmaf(a0, m, c); a1 = fmaf(a1, m, c); a2 = fmaf(a2, m, c); a3 = fmaf(a3, m, c);
        a4 = fmaf(a4, m, c); a5 = fmaf(a5, m, c); a6 = fmaf(a6, m, c); a7 = fmaf(a7, m, c);
    }
    if (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 == 12345.f) out[0] = a0;
}
static double calib_share(const cudaDeviceProp &pr) {
    float *d; cudaMalloc(&d, 4);
    int blocks = pr.multiProcessorCount * 8, thr = 256, iters = 20000;
    k_fma<<<blocks, thr>>>(d, 100); cudaDeviceSynchronize();
    double best = 0;
    for (int r = 0; r < 4; r++) {
        cudaEvent_t e0, e1; cudaEventCreate(&e0); cudaEventCreate(&e1); cudaEventRecord(e0);
        k_fma<<<blocks, thr>>>(d, iters); cudaEventRecord(e1); cudaEventSynchronize(e1);
        float ms; cudaEventElapsedTime(&ms, e0, e1);
        double fma = (double)blocks * thr * iters * 8, ideal = fma / (pr.multiProcessorCount * 128.0 * pr.clockRate * 1e3) * 1e3;
        best = std::max(best, ideal / ms);
        cudaEventDestroy(e0); cudaEventDestroy(e1);
    }
    cudaFree(d); return std::min(best, 1.0);
}

static std::string fmt(double v) { char b[32]; snprintf(b, sizeof b, "%.3g", v); return b; }

int main(int argc, char **argv) {
    std::vector<int> vers = {MC_26_1, MC_26_2, MC_26_3};
    std::string dims = "ow,ow_large,ow_amp,nether,end"; bool quick = false, cpu = false; int logn = 20; int REPS = 4;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dims")) dims = argv[++i];
        else if (!strcmp(argv[i], "--quick")) { quick = true; logn = 16; }
        else if (!strcmp(argv[i], "--cpu")) cpu = true;
        else if (!strcmp(argv[i], "--logn")) logn = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reps")) REPS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--versions")) { vers.clear(); while (i + 1 < argc && argv[i + 1][0] != '-') vers.push_back(mcg_version_from_name(argv[++i])); }
    }
    std::vector<Cfg> cfgs;
    auto has = [&](const char *t) { std::string d = "," + dims + ","; return d.find(std::string(",") + t + ",") != std::string::npos; };
    if (has("ow")) cfgs.push_back({"overworld/normal", MC_OVERWORLD, "normal"});
    if (has("ow_large")) cfgs.push_back({"overworld/large_biomes", MC_OVERWORLD, "large_biomes"});
    if (has("ow_amp")) cfgs.push_back({"overworld/amplified", MC_OVERWORLD, "amplified"});
    if (has("nether")) cfgs.push_back({"nether", MC_NETHER, "-"});
    if (has("end")) cfgs.push_back({"end", MC_END, "-"});

    cudaDeviceProp pr; cudaGetDeviceProperties(&pr, 0);
    printf("# GPU: %s, %d SM, sm_%d%d, %.0f МГц; потоков CPU (OpenMP): %s\n", pr.name, pr.multiProcessorCount, pr.major, pr.minor, pr.clockRate / 1e3, getenv("OMP_NUM_THREADS") ? getenv("OMP_NUM_THREADS") : "все");
    printf("| версия | конфиг | seed/с (1 точка) | точек/с (амортиз.: 64 т./seed, пачки по 8) | поиск: канд./с (8 набл., отсев) | поиск: канд./с (8 набл. все считаются) | нс агрегатно: init таблиц / 1 точка |%s\n", cpu ? " CPU: seed/с (1 точка) | CPU: точек/с |" : "");
    printf("|---|---|---|---|---|---|---|%s\n", cpu ? "---|---|" : "");
    std::vector<std::string> blocklines, normlines;
    for (int ver : vers) for (const Cfg &cf : cfgs) {
        GbCtx c; if (gb_open(c, ver, cf.dim, cf.preset)) return 2;
        double share = calib_share(pr);
        int B = 64;   /* точек на seed для «амортизированного» замера */
        int ns_pts = cf.dim == MC_END ? 4096 : 16384; if (quick) ns_pts /= 4;      /* seed'ов; точек на seed — B=64, пачки по 8 (chunk=8) */
        std::vector<i64> seeds(65536 * (quick ? 1 : 2)); for (auto &s : seeds) s = (i64)rnd64();
        std::vector<i32> pts(3 * B);
        for (int p = 0; p < B; p++) {
            int R = cf.dim == MC_END ? 50000 : 5000;
            pts[3 * p] = rrange(-R, R); pts[3 * p + 2] = rrange(-R, R);
            pts[3 * p + 1] = cf.dim == MC_OVERWORLD ? rrange(-16, 80) : (cf.dim == MC_NETHER ? rrange(0, 32) : 16);
            if (cf.dim == MC_END) while ((i64)pts[3 * p] * pts[3 * p] + (i64)pts[3 * p + 2] * pts[3 * p + 2] < 400 * 400) pts[3 * p] = rrange(-R, R);
        }
        std::vector<u8> out;
        /* 1) seed/с при 1 точке */
        double best1 = 1e30; int n1 = (int)seeds.size();
        out.resize((size_t)n1);
        for (int rep = 0; rep < REPS; rep++) { double ms = gb_points_gpu(c, seeds.data(), n1, pts.data(), 1, out.data(), nullptr, 1); if (rep) best1 = std::min(best1, ms); }
        /* 2) точек/с амортизированно */
        double bestp = 1e30; int nsd = ns_pts; out.resize((size_t)nsd * B);
        for (int rep = 0; rep < REPS; rep++) { double ms = gb_points_gpu(c, seeds.data(), nsd, pts.data(), B, out.data(), nullptr, 8); if (rep) bestp = std::min(bestp, ms); }
        /* 2b) разложение: t(np)=ceil(np/8)*I + np*e  (пачка по 8 точек = одна инициализация таблиц) */
        double t8 = 1e30; { std::vector<u8> o8((size_t)nsd * 8); for (int rep = 0; rep < REPS; rep++) { double ms = gb_points_gpu(c, seeds.data(), nsd, pts.data(), 8, o8.data(), nullptr, 8); if (rep) t8 = std::min(t8, ms); } }
        double t1s = 1e30; { std::vector<u8> o1((size_t)nsd); for (int rep = 0; rep < REPS; rep++) { double ms = gb_points_gpu(c, seeds.data(), nsd, pts.data(), 1, o1.data(), nullptr, 1); if (rep) t1s = std::min(t1s, ms); } }
        /* на один seed (агрегатно по GPU): T1 = I + e, T8 = I + 8e, T64 = 8I + 64e -> e = (T8 - T1)/7, I = T1 - e */
        double T1 = t1s / nsd * 1e6, T8 = t8 / nsd * 1e6, T64 = bestp / nsd * 1e6;   /* нс на seed */
        double e_ns = (T8 - T1) / 7.0, I_ns = T1 - e_ns; (void)T64;
        /* 3) поиск: 8 наблюдений из «реального» мира (seed X), маска = биом в этой точке */
        std::vector<McgObs> obs(8), obs_all(8);
        i64 world = (i64)0x1234567890ABCDEFLL;
        { std::vector<u8> ob(8); i64 sw = world; gb_points_gpu(c, &sw, 1, pts.data(), 8, ob.data(), nullptr, 8);
          for (int j = 0; j < 8; j++) {
              memset(&obs[j], 0, sizeof(McgObs)); obs[j].qx = pts[3 * j]; obs[j].qy = pts[3 * j + 1]; obs[j].qz = pts[3 * j + 2]; obs[j].rad = 12;
              obs[j].mask[ob[j] >> 6] |= 1ULL << (ob[j] & 63);
              obs_all[j] = obs[j]; obs_all[j].mask[0] = obs_all[j].mask[1] = ~0ULL;
          } }
        gb_finalize_obs(c, obs);
        std::vector<double> psel; gb_estimate_selectivity(c, obs, 2048, psel);
        { std::vector<int> ord(8); for (int i = 0; i < 8; i++) ord[i] = i; std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return psel[a] < psel[b]; });
          std::vector<McgObs> o2; for (int k : ord) o2.push_back(obs[k]); obs = o2; }
        for (auto &o : obs_all) o.rad = 12;       /* «все считаются»: для End полное окно — честная цена */
        GbSource src; src.kind = 2; src.base = rnd64(); src.n = 1ull << logn;
        double bests = 1e30, bestf = 1e30; GbSearch sr;
        for (int rep = 0; rep < (REPS > 3 ? REPS - 1 : 3); rep++) {
            gb_search(c, true, src, obs, sr); if (rep) bests = std::min(bests, sr.ms);
            GbSearch sf; gb_search(c, true, src, obs_all, sf); if (rep) bestf = std::min(bestf, sf.ms);
        }
        double cost_obs_ns = (bestf - bests) / (double)src.n * 1e6 / 7.0 * 1.0;   /* грубо: (t_full - t_early)/N/7 — нс на поток */
        share = std::max(share, calib_share(pr));
        double inv = 1.0 / share;
        normlines.push_back(std::string(mcg_version_name(ver)) + " " + cf.name + ": доля GPU s=" + std::to_string(share).substr(0, 4) + " -> нормир.: seed/с(1 т.) " + fmt(n1 / (best1 / 1e3) * inv) + ", точек/с " + fmt((double)nsd * B / (bestp / 1e3) * inv) +
                            ", поиск(отсев) " + fmt(src.n / (bests / 1e3) * inv) + ", поиск(все) " + fmt(src.n / (bestf / 1e3) * inv) + ", init " + fmt(I_ns * share) + " нс, точка " + fmt(e_ns * share) + " нс");
        printf("| %s | %s | %.3g | %.3g | %.3g | %.3g | %.1f / %.1f |", mcg_version_name(ver), cf.name, n1 / (best1 / 1e3), (double)nsd * B / (bestp / 1e3),
               src.n / (bests / 1e3), src.n / (bestf / 1e3), I_ns, e_ns);
        if (cpu) {
            int nc = 2048; std::vector<u8> oc((size_t)nc);
            double t0 = now_ms(); gb_points_cpu(c, seeds.data(), nc, pts.data(), 1, oc.data(), nullptr); double c1 = now_ms() - t0;
            int ncp = 256; std::vector<u8> ocp((size_t)ncp * B);
            t0 = now_ms(); gb_points_cpu(c, seeds.data(), ncp, pts.data(), B, ocp.data(), nullptr); double cp = now_ms() - t0;
            printf(" %.3g | %.3g |", nc / (c1 / 1e3), (double)ncp * B / (cp / 1e3));
        }
        printf("\n"); fflush(stdout);
        if (cf.dim == MC_OVERWORLD && !strcmp(cf.preset, "normal")) {     /* A/B размера блока (один процесс, поочерёдно) */
            char line[512]; int len = snprintf(line, sizeof line, "| %s | блок (потоков): ", mcg_version_name(ver));
            double bb[4] = {1e30, 1e30, 1e30, 1e30}; const int BS[4] = {32, 64, 128, 256};
            for (int rep = 0; rep < (REPS > 3 ? REPS - 1 : 3); rep++) for (int bi = 0; bi < 4; bi++) { gb_block = BS[bi]; GbSearch r2; gb_search(c, true, src, obs, r2); bb[bi] = std::min(bb[bi], r2.ms); }
            for (int bi = 0; bi < 4; bi++) len += snprintf(line + len, sizeof line - len, "%d: %.3g канд./с; ", BS[bi], src.n / (bb[bi] / 1e3));
            gb_block = 32; blocklines.push_back(line);
        }
        gb_close(c);
    }
    for (auto &l : blocklines) printf("%s\n", l.c_str());
    printf("# нормировано на эксклюзивную GPU (сырые / s, см. калибровку в коде; нижняя оценка, ±15%%):\n");
    for (auto &l : normlines) printf("# %s\n", l.c_str());
    return 0;
}

/*
 * struct48.cu — восстановление 48-битного structure seed по позициям структур random_spread.
 *
 * Режимы:
 *   --mode full : полный перебор 2^48 с ранним выходом (GPU или CPU) — нижняя оценка скорости «в лоб».
 *   --mode lift : «lifting»: сначала ищем младшие L бит (L<=20) по ограничениям на младшие биты
 *                 nextInt(bound) при bound = 2^tz * odd (tz бит), затем перебираем старшие 48-L бит.
 *   --dev gpu|cpu, --limit N (для full: сколько значений W перебрать, 2^k; экстраполяция на 2^48).
 * Данные: --gen "имя,имя,..." --seed S  — генерирует псевдонаблюдения из известного seed (проверка «с ответом»).
 *
 * Сборка: nvcc -O3 -arch=sm_89 -Xcompiler -fopenmp -o struct48 struct48.cu
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <omp.h>
#include "../mcrand.h"

struct Obs {
    i32 spacing, sep, salt, type;   /* набор структур */
    i32 rx, rz;                     /* регион */
    i32 ox, oz;                     /* наблюдаемое смещение внутри региона */
    u64 cst;                        /* (rx*A + rz*B + salt) mod 2^48 */
    i32 lim;                        /* spacing - separation */
    i32 tz;                         /* число бит, доступных для lifting (0 если не применимо) */
    u64 magic;                      /* Lemire fastmod: 2^64/lim + 1 */
};

struct SpDef { const char *name; int spacing, sep, salt, type; };
static const SpDef DEFS[] = {
    {"desert_pyramid", 32, 8, 14357617, 0}, {"igloo", 32, 8, 14357618, 0}, {"jungle_pyramid", 32, 8, 14357619, 0},
    {"swamp_hut", 32, 8, 14357620, 0}, {"shipwreck", 24, 4, 165745295, 0}, {"village", 34, 8, 10387312, 0},
    {"trial_chambers", 34, 12, 94251327, 0}, {"ancient_city", 24, 8, 20083232, 0}, {"monument", 32, 5, 10387313, 1},
    {"mansion", 80, 20, 10387319, 1}, {"end_city", 20, 11, 10387313, 1}, {"ruined_portal", 40, 15, 34222645, 0},
    {"nether_complex", 27, 4, 30084232, 0}, {"trail_ruins", 34, 8, 83469867, 0}, {"abandoned_camp", 37, 8, 91231127, 0}};


/* быстрый nextInt(bound) для НЕ степени двойки: r % lim через умножение (Lemire). Редкий случай отбраковки — медленный путь. */
HD i32 nextInt_fast(u64 *s, i32 lim, u64 magic) {
    u64 st = lcg_step(*s);
    u32 r = (u32)(st >> 17);
#ifdef __CUDA_ARCH__
    u32 m = (u32)__umul64hi(magic * (u64)r, (u64)lim);
#else
    u32 m = (u32)(((unsigned __int128)(magic * (u64)r) * (u64)lim) >> 64);
#endif
    if (r > 0x7FFFFFFFu - 64u) return lcg_nextInt(s, lim);   /* отбраковка возможна — точный путь */
    *s = st; return (i32)m;
}

HD bool check_obs(u64 W, const Obs &o) {
    u64 s = ((W + o.cst) & MASK48) ^ LCG_MUL;   /* setSeed(regionSeed): scramble */
    if (o.type == 0) {
        if (o.tz != 99 && (o.lim & (o.lim - 1)) != 0) {
            if (nextInt_fast(&s, o.lim, o.magic) != o.ox) return false;
            return nextInt_fast(&s, o.lim, o.magic) == o.oz;
        }
        if (lcg_nextInt(&s, o.lim) != o.ox) return false;
        return lcg_nextInt(&s, o.lim) == o.oz;
    } else {
        bool np2 = (o.lim & (o.lim - 1)) != 0;
        i32 a = np2 ? nextInt_fast(&s, o.lim, o.magic) : lcg_nextInt(&s, o.lim);
        i32 b = np2 ? nextInt_fast(&s, o.lim, o.magic) : lcg_nextInt(&s, o.lim);
        if ((a + b) / 2 != o.ox) return false;
        a = np2 ? nextInt_fast(&s, o.lim, o.magic) : lcg_nextInt(&s, o.lim);
        b = np2 ? nextInt_fast(&s, o.lim, o.magic) : lcg_nextInt(&s, o.lim);
        return (a + b) / 2 == o.oz;
    }
}

#define MAXOBS 16
__constant__ Obs c_obs[MAXOBS];
__constant__ int c_nobs;

/* --- GPU: полный перебор диапазона [base, base+count) --- */
#define MAXRES 4096
__device__ u64 d_res[MAXRES];
__device__ unsigned d_nres;

__global__ void k_full(u64 base, u64 count) {
    u64 stride = (u64)gridDim.x * blockDim.x;
    for (u64 i = (u64)blockIdx.x * blockDim.x + threadIdx.x; i < count; i += stride) {
        u64 W = base + i;
        bool ok = true;
        for (int k = 0; k < c_nobs && ok; k++) ok = check_obs(W, c_obs[k]);
        if (ok) { unsigned p = atomicAdd(&d_nres, 1u); if (p < MAXRES) d_res[p] = W; }
    }
}
/* --- GPU: lifting-расширение: W = (hi << L) | lo для lo из списка --- */
__constant__ u64 c_lows[1024];
__global__ void k_lift(int L, int nlows, u64 hi_base, u64 hi_count) {
    u64 stride = (u64)gridDim.x * blockDim.x;
    u64 total = hi_count * (u64)nlows;
    for (u64 i = (u64)blockIdx.x * blockDim.x + threadIdx.x; i < total; i += stride) {
        u64 hi = hi_base + i / nlows, lo = c_lows[i % nlows];
        u64 W = (hi << L) | lo;
        bool ok = true;
        for (int k = 0; k < c_nobs && ok; k++) ok = check_obs(W, c_obs[k]);
        if (ok) { unsigned p = atomicAdd(&d_nres, 1u); if (p < MAXRES) d_res[p] = W; }
    }
}

static void prep(Obs &o) {
    o.cst = (u64)((i64)o.rx * REGION_A + (i64)o.rz * REGION_B + (i64)o.salt) & MASK48;
    o.lim = o.spacing - o.sep;
    o.magic = (~0ULL) / (u64)o.lim + 1;
    int tz = 0; while (((o.lim >> tz) & 1) == 0 && tz < 3) tz++;
    /* lifting только для linear и НЕ степени двойки (степень двойки использует старшие биты) */
    o.tz = (o.type == 0 && (o.lim & (o.lim - 1)) != 0) ? tz : 0;
}

/* ограничения младших L бит W (L >= 18): true если lo (L бит) совместимо со всеми наблюдениями */
static bool low_ok(u64 lo, int L, const std::vector<Obs> &obs) {
    u64 mask = (L >= 48) ? MASK48 : ((1ULL << L) - 1);
    for (const Obs &o : obs) {
        if (!o.tz) continue;
        int k = std::min(o.tz, L - 17);        /* сколько бит r доступно */
        if (k <= 0) continue;
        u64 s = ((lo + o.cst) & mask) ^ (LCG_MUL & mask);          /* младшие L бит состояния */
        u64 s1 = (s * LCG_MUL + LCG_ADD) & mask;                    /* младшие L бит после шага (корректно mod 2^L) */
        u64 r1 = (s1 >> 17);
        u64 s2 = (s1 * LCG_MUL + LCG_ADD) & mask;
        u64 r2 = (s2 >> 17);
        u64 m = (1ULL << k) - 1;
        if ((r1 & m) != ((u64)o.ox & m)) return false;
        if ((r2 & m) != ((u64)o.oz & m)) return false;
    }
    return true;
}

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char **argv) {
    std::string gen = "desert_pyramid,igloo,swamp_hut,shipwreck,trial_chambers,village", mode = "lift", dev = "gpu";
    u64 seed = 0x1234567890ABULL; int limitBits = 40; unsigned rngs = 12345;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gen")) gen = argv[++i];
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[++i], 0, 0) & MASK48;
        else if (!strcmp(argv[i], "--mode")) mode = argv[++i];
        else if (!strcmp(argv[i], "--dev")) dev = argv[++i];
        else if (!strcmp(argv[i], "--limit")) limitBits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rng")) rngs = atoi(argv[++i]);
    }
    /* --- псевдо-наблюдения --- */
    std::vector<Obs> obs;
    { std::string g = gen; size_t p = 0; srand(rngs);
      while (p <= g.size()) { size_t q = g.find(',', p); if (q == std::string::npos) q = g.size();
        std::string nm = g.substr(p, q - p); p = q + 1; if (nm.empty()) continue;
        const SpDef *d = nullptr; for (auto &x : DEFS) if (nm == x.name) d = &x;
        if (!d) { fprintf(stderr, "unknown %s\n", nm.c_str()); return 1; }
        Obs o{}; o.spacing = d->spacing; o.sep = d->sep; o.salt = d->salt; o.type = d->type;
        o.rx = rand() % 60 - 30; o.rz = rand() % 60 - 30;
        spread_offset(seed, o.rx, o.rz, o.spacing, o.sep, o.salt, o.type, &o.ox, &o.oz);
        prep(o); obs.push_back(o);
        printf("obs %-15s region(%3d,%3d) chunk(%d,%d) offset(%d,%d) lift_bits=%d\n", nm.c_str(), o.rx, o.rz,
               o.rx * o.spacing + o.ox, o.rz * o.spacing + o.oz, o.ox, o.oz, o.tz);
      } }
    /* порядок: сначала самые селективные (для раннего выхода) — 1/lim^2 */
    std::stable_sort(obs.begin(), obs.end(), [](const Obs &a, const Obs &b) { return a.lim > b.lim; });
    double bits = 0; for (auto &o : obs) bits += 2 * log2((double)o.lim);
    printf("true structure seed = %llu (0x%llx); K=%zu, info ~ %.1f bits\n", (unsigned long long)seed, (unsigned long long)seed, obs.size(), bits);

    cudaMemcpyToSymbol(c_obs, obs.data(), sizeof(Obs) * obs.size());
    int n = (int)obs.size(); cudaMemcpyToSymbol(c_nobs, &n, sizeof n);
    int dummy = 0; cudaMemcpyToSymbol(d_nres, &dummy, sizeof dummy);
    std::vector<u64> found;
    int sms = 0; cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, 0);
    int blocks = sms * 16, threads = 256;

    if (mode == "full") {
        u64 total = 1ULL << limitBits;
        /* чтобы найти ответ в усечённом диапазоне, начинаем так, чтобы диапазон содержал seed */
        u64 base = (seed >= total) ? (seed & ~(total - 1)) : 0;
        double t0 = now();
        if (dev == "gpu") {
            u64 chunk = 1ULL << 34;
            for (u64 off = 0; off < total; off += chunk) {
                u64 cnt = std::min(chunk, total - off);
                k_full<<<blocks, threads>>>(base + off, cnt);
                cudaDeviceSynchronize();
            }
            unsigned nr; cudaMemcpyFromSymbol(&nr, d_nres, sizeof nr);
            std::vector<u64> r(std::min<unsigned>(nr, MAXRES)); cudaMemcpyFromSymbol(r.data(), d_res, sizeof(u64) * r.size());
            found = r;
        } else {
            #pragma omp parallel
            {
                std::vector<u64> loc;
                #pragma omp for schedule(static, 1<<20)
                for (long long i = 0; i < (long long)total; i++) {
                    u64 W = base + i; bool ok = true;
                    for (auto &o : obs) { if (!check_obs(W, o)) { ok = false; break; } }
                    if (ok) loc.push_back(W);
                }
                #pragma omp critical
                found.insert(found.end(), loc.begin(), loc.end());
            }
        }
        double dt = now() - t0;
        printf("[full/%s] W in [%llu, +2^%d): %.3f s, %.3e W/s; found %zu\n", dev.c_str(), (unsigned long long)base, limitBits, dt, (double)total / dt, found.size());
        printf("  extrapolation to 2^48: %.0f s (%.1f min)  [оценка = измеренная скорость * 2^48]\n", 281474976710656.0 / ((double)total / dt), 281474976710656.0 / ((double)total / dt) / 60);
    } else {
        /* lift: L = 17 + max tz по наблюдениям */
        int maxtz = 0; for (auto &o : obs) maxtz = std::max(maxtz, o.tz);
        int L = 17 + maxtz;
        if (maxtz == 0) { fprintf(stderr, "нет liftable наблюдений\n"); return 1; }
        double t0 = now();
        std::vector<u64> lows;
        { std::vector<u64> cur = {0};       /* инкрементальное расширение: l бит -> l+1 бит */
          for (int l = 0; l < L; l++) {
              std::vector<u64> nxt;
              for (u64 v : cur) for (int b = 0; b < 2; b++) { u64 c = v | ((u64)b << l);
                  if (l + 1 < 18 || low_ok(c, l + 1, obs)) nxt.push_back(c); }
              cur.swap(nxt);
          }
          lows = cur; }
        double t1 = now();
        printf("[lift] L=%d бит: %zu кандидатов младших бит (перечисление %.4f s)\n", L, lows.size(), t1 - t0);
        u64 hiCount = 1ULL << (48 - L);
        size_t lowLimit = lows.size();
        if (dev == "gpu") {
            for (size_t o0 = 0; o0 < lowLimit; o0 += 1024) {
                size_t nl = std::min<size_t>(1024, lowLimit - o0);
                cudaMemcpyToSymbol(c_lows, lows.data() + o0, sizeof(u64) * nl);
                u64 chunk = 1ULL << 28;
                for (u64 off = 0; off < hiCount; off += chunk) {
                    u64 cnt = std::min(chunk, hiCount - off);
                    k_lift<<<blocks, threads>>>(L, (int)nl, off, cnt);
                    cudaDeviceSynchronize();
                }
            }
            unsigned nr; cudaMemcpyFromSymbol(&nr, d_nres, sizeof nr);
            std::vector<u64> r(std::min<unsigned>(nr, MAXRES)); cudaMemcpyFromSymbol(r.data(), d_res, sizeof(u64) * r.size());
            found = r;
        } else {
            #pragma omp parallel
            {
                std::vector<u64> loc;
                #pragma omp for schedule(dynamic, 1) collapse(2)
                for (long long li = 0; li < (long long)lows.size(); li++)
                    for (long long h = 0; h < (long long)(hiCount >> 12); h++)
                        for (long long h2 = 0; h2 < 4096; h2++) {
                            u64 W = (((u64)h << 12 | (u64)h2) << L) | lows[li]; bool ok = true;
                            for (auto &o : obs) { if (!check_obs(W, o)) { ok = false; break; } }
                            if (ok) loc.push_back(W);
                        }
                #pragma omp critical
                found.insert(found.end(), loc.begin(), loc.end());
            }
        }
        double t2 = now();
        double tested = (double)lows.size() * (double)hiCount;
        printf("[lift/%s] перебор старших %d бит для %zu низов = %.3e проверок: %.3f s (%.3e проверок/с); всего с lifting %.3f s; found %zu\n",
               dev.c_str(), 48 - L, lows.size(), tested, t2 - t1, tested / (t2 - t1), t2 - t0, found.size());
    }
    std::sort(found.begin(), found.end());
    for (size_t i = 0; i < found.size() && i < 10; i++) printf("  candidate %llu%s\n", (unsigned long long)found[i], found[i] == seed ? "  <== TRUE SEED" : "");
    bool hit = std::find(found.begin(), found.end(), seed) != found.end();
    printf("%s\n", hit ? "RESULT: true seed recovered" : "RESULT: true seed NOT in found set");
    return 0;
}

/*
 * crack-struct — восстановление 48-битного structure seed по позициям структур (random_spread / frequency reducers).
 *
 *   crack-struct --version 26.3 observations.txt            (lifting по умолчанию, GPU; fallback CPU/OpenMP)
 *   crack-struct --version 26.3 --mode full obs.txt         (полный перебор 2^48: нет liftable-структур)
 *
 * Параметры placement читаются из data/structure_sets-<V>.json. Справка: crack-struct --help, docs/20-crack-struct-lift.md.
 *
 * Полный перебор 2^48 ускорен «перечислением решений первого ограничения» (вместо перебора всех W):
 *   W <-> s0 = ((W+cst)&M48)^MUL <-> s1 = A*s0+C (биекция mod 2^48).  Для linear-наблюдения первая координата — условие на
 *   r = s1>>17 (31 бит): r mod lim == ox (или старшие биты r при lim = 2^k). Перечисляем ТОЛЬКО такие s1 (r = ox + k*lim; m = 17
 *   младших бит s1 — внутренний цикл, s2 = A*s1+C обновляется сложением), и для каждого проверяем вторую координату одним
 *   умножением (тест делимости, Hacker's Delight 10-17). Пространство 2^48 сжимается в lim раз, затем «тяжёлая» проверка
 *   остальных предикатов — лишь для ~1/lim^2 кандидатов. Для reducer'ов с nextFloat/next(24) < f перечисляется диапазон s1.
 *   triangular/legacy_type_1/3 — прямой перебор (медленнее). Подробности и замеры: docs/20-crack-struct-lift.md.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <omp.h>
#ifdef __CUDACC__
#include <cuda_runtime.h>
#endif
#include "sets.h"

/* ============================================================================================
 *  Параметры устройства
 * ============================================================================================ */
#define MAXP 28
enum { DK_GEN = 0, DK_DIV = 1, DK_POW2 = 2, DK_TRI = 3, DK_RANGE = 4, DK_EDGE = 5 };
struct DevParams {
    int n;              /* число предикатов в pred[] (порядок проверки) */
    int dk;             /* драйвер: pred[0] */
    u64 ainv;           /* обратный к K_MUL по модулю 2^48 */
    Pred pred[MAXP];
};
#define RES_CAP (1u << 22)
#ifdef __CUDACC__
__constant__ DevParams c_par;
__device__ u64 d_res[RES_CAP];
__device__ unsigned d_nres;

struct DevSink { __device__ __forceinline__ void operator()(u64 W) const { unsigned p = atomicAdd(&d_nres, 1u); if (p < RES_CAP) d_res[p] = W; } };
#endif
struct HostSink { std::vector<u64> *v; void operator()(u64 W) const { v->push_back(W); } };

HD bool check_all(const DevParams *P, u64 W) {
    for (int i = 0; i < P->n; i++) if (!pred_eval(W, P->pred[i])) return false;
    return true;
}
HD u32 rotr32(u32 x, int t) {
#ifdef __CUDA_ARCH__
    return __funnelshift_r(x, x, t);
#else
    return t ? (x >> t) | (x << (32 - t)) : x;
#endif
}

/* s1 -> W: s0 = (s1 - C) * A^-1; W = ((s0 ^ MUL) - cst) mod 2^48 (cst драйвера — pred[0]) */
HD u64 s1_to_W(const DevParams *P, u64 s1) {
    u64 s0 = ((s1 - K_ADD) * P->ainv) & K_MASK48;
    return ((s0 ^ K_MUL) - P->pred[0].cst) & K_MASK48;
}
HD bool check_from(const DevParams *P, u64 W, int from) {
    for (int i = from; i < P->n; i++) if (!pred_eval(W, P->pred[i])) return false;
    return true;
}

#define BLK_BITS 17
/* DK_DIV: один r = ox + k*lim (первая координата выполнена по построению); внутри — 2^17 значений m (младшие биты s1) */
#pragma nv_exec_check_disable
template <class Sink>
HD void run_div(const DevParams *P, u64 k, Sink &sink) {
    const Pred &D = P->pred[0];
    const u32 oz = (u32)D.oz, inv = D.invq, dbnd = D.divbound, thr = D.thr_rej;
    const int t2 = D.t2;
    u32 r = (u32)D.ox + (u32)k * (u32)D.lim;
    u64 s1b = (u64)r << BLK_BITS;
    u64 s2 = s1b * K_MUL + K_ADD;
    for (u32 m = 0; m < (1u << BLK_BITS); m++, s2 += K_MUL) {
        u32 r2 = (u32)(s2 >> 17) & 0x7FFFFFFFu;
        u32 x = rotr32((r2 - oz) * inv, t2);
        if (x > dbnd && r2 < thr) continue;
        u64 W = s1_to_W(P, s1b + m);
        if (((r2 >= thr) | (r2 < oz)) && !pred_eval(W, D)) continue;      /* редкие случаи: точный путь */
        if (check_from(P, W, 1)) sink(W);
    }
}
/* DK_POW2: lim = 2^kb; первая координата — старшие kb бит s1 == ox: s1 в непрерывном диапазоне; t — блок из 2^17 значений */
#pragma nv_exec_check_disable
template <class Sink>
HD void run_pow2(const DevParams *P, u64 t, Sink &sink) {
    const Pred &D = P->pred[0];
    const int sh = 48 - D.pow2k; const u32 limm1 = (u32)D.lim - 1, oz = (u32)D.oz;
    u64 s1b = ((u64)D.ox << sh) + (t << BLK_BITS);
    u64 s2 = s1b * K_MUL + K_ADD;
    for (u32 m = 0; m < (1u << BLK_BITS); m++, s2 += K_MUL) {
        if ((u32)((s2 >> sh) & limm1) != oz) continue;
        u64 W = s1_to_W(P, s1b + m);
        if (check_from(P, W, 1)) sink(W);
    }
}
/* DK_RANGE: reducer next(24) < T24  <=>  s1 < T24 << 24 (непрерывный диапазон) */
#pragma nv_exec_check_disable
template <class Sink>
HD void run_range(const DevParams *P, u64 t, Sink &sink) {
    u64 s1b = t << BLK_BITS;
    for (u32 m = 0; m < (1u << BLK_BITS); m++) {
        u64 W = s1_to_W(P, s1b + m);
        if (check_from(P, W, 1)) sink(W);
    }
}
/* DK_EDGE: r в [thr, 2^31) — возможна отбраковка nextInt; полный точный перебор (lim * 2^17 значений s1) */
#pragma nv_exec_check_disable
template <class Sink>
HD void run_edge(const DevParams *P, u64 j, Sink &sink) {
    u32 r = P->pred[0].thr_rej + (u32)j;
    u64 s1b = (u64)r << BLK_BITS;
    for (u32 m = 0; m < (1u << BLK_BITS); m++) {
        u64 W = s1_to_W(P, s1b + m);
        if (check_all(P, W)) sink(W);
    }
}
/* DK_TRI (triangular, lim не степень двойки): x = (a+b)/2 == ox, a = r1 % lim, b = r2 % lim. Допустимые a: [a_lo, a_hi], для каждого a —
 * не более двух допустимых b (2ox-a, 2ox+1-a). Перечисляем r1 = a + k*lim (по допустимым a), внутри — 2^17 значений m; вторая проверка
 * — тест делимости r2-b0 и r2-b1. Индекс idx = ia*KAMAX + k (KAMAX = ceil(thr/lim)); r1 >= thr обрабатывает DK_EDGE. */
struct TriGeom { u32 a_lo, na, kamax; };
HD TriGeom tri_geom(const Pred &D) {
    TriGeom g; i32 lim = D.lim, ox = D.ox;
    i32 lo = 2 * ox - (lim - 1); if (lo < 0) lo = 0;
    i32 hi = 2 * ox + 1; if (hi > lim - 1) hi = lim - 1;
    g.a_lo = (u32)lo; g.na = (u32)(hi - lo + 1); g.kamax = (D.thr_rej + (u32)lim - 1) / (u32)lim;
    return g;
}
#pragma nv_exec_check_disable
template <class Sink>
HD void run_tri(const DevParams *P, u64 idx, Sink &sink) {
    const Pred &D = P->pred[0];
    const TriGeom g = tri_geom(D);
    const u32 thr = D.thr_rej, inv = D.invq, dbnd = D.divbound; const int t2 = D.t2;
    u32 ia = (u32)(idx / g.kamax), k = (u32)(idx % g.kamax);
    i32 a = (i32)g.a_lo + (i32)ia;
    u32 r1 = (u32)a + k * (u32)D.lim;
    if (r1 >= thr) return;
    i32 b0 = 2 * D.ox - a, b1 = b0 + 1;
    bool v0 = b0 >= 0 && b0 < D.lim, v1 = b1 >= 0 && b1 < D.lim;
    u64 s1b = (u64)r1 << BLK_BITS, s2 = s1b * K_MUL + K_ADD;
    for (u32 m = 0; m < (1u << BLK_BITS); m++, s2 += K_MUL) {
        u32 r2 = (u32)(s2 >> 17) & 0x7FFFFFFFu;
        bool p0 = v0 && rotr32((r2 - (u32)b0) * inv, t2) <= dbnd, p1 = v1 && rotr32((r2 - (u32)b1) * inv, t2) <= dbnd;
        if (!(p0 | p1 | (r2 >= thr))) continue;
        u64 W = s1_to_W(P, s1b + m);
        if (pred_eval(W, D) && check_from(P, W, 1)) sink(W);
    }
}
#define TILE_BITS 14
/* DK_GEN: прямой перебор W */
#pragma nv_exec_check_disable
template <class Sink>
HD void run_gen(const DevParams *P, u64 tile, Sink &sink) {
    u64 W0 = tile << TILE_BITS;
    for (u32 j = 0; j < (1u << TILE_BITS); j++) { u64 W = (W0 + j) & K_MASK48; if (check_all(P, W)) sink(W); }
}

#pragma nv_exec_check_disable
template <class Sink>
HD void run_index(const DevParams *P, int dk, u64 idx, Sink &sink) {
    switch (dk) {
    case DK_DIV: run_div(P, idx, sink); break;
    case DK_POW2: run_pow2(P, idx, sink); break;
    case DK_RANGE: run_range(P, idx, sink); break;
    case DK_EDGE: run_edge(P, idx, sink); break;
    case DK_TRI: run_tri(P, idx, sink); break;
    default: run_gen(P, idx, sink); break;
    }
}
#ifdef __CUDACC__
/* ---- GPU: warp-пакетная обработка «попаданий» (DK_DIV/DK_POW2) ----
 * Прошедшие вторую координату кандидаты (~1/lim) складываются в очередь в shared-памяти варпа; когда набирается 32, все линии варпа
 * одновременно вычисляют W и проверяют остальные предикаты — без дивергенции «тяжёлого» пути. */
__device__ __forceinline__ void hit_process(const DevParams *P, u64 s1, int dk, DevSink &sink) {
    const Pred &D = P->pred[0];
    u64 W = s1_to_W(P, s1);
    if (dk == DK_TRI) { if (!pred_eval(W, D)) return; }
    else if (dk == DK_DIV) {
        u64 s2 = s1 * K_MUL + K_ADD; u32 r2 = (u32)(s2 >> 17) & 0x7FFFFFFFu;
        if (((r2 >= D.thr_rej) | (r2 < (u32)D.oz)) && !pred_eval(W, D)) return;     /* редкие случаи: точный путь */
    }
    if (check_from(P, W, 1)) sink(W);
}
template <int DK>
__device__ __forceinline__ void run_warp(const DevParams *P, u64 idx, bool active, volatile u64 *q, DevSink &sink) {
    const Pred &D = P->pred[0];
    const unsigned lane = threadIdx.x & 31;
    const u32 oz = (u32)D.oz, inv = D.invq, dbnd = D.divbound, thr = D.thr_rej;
    const int t2 = D.t2, sh = 48 - (D.pow2k < 0 ? 0 : D.pow2k);
    const u32 limm1 = (u32)D.lim - 1;
    u64 s1b; i32 b0 = 0, b1 = 0; bool v0 = false, v1 = false;
    if (DK == DK_DIV) s1b = (u64)((u32)D.ox + (u32)idx * (u32)D.lim) << BLK_BITS;
    else if (DK == DK_POW2) s1b = ((u64)D.ox << sh) + (idx << BLK_BITS);
    else {
        const TriGeom g = tri_geom(D);
        u32 ia = (u32)(idx / g.kamax), k = (u32)(idx % g.kamax);
        i32 a = (i32)g.a_lo + (i32)ia; u32 r1 = (u32)a + k * (u32)D.lim;
        active = active && r1 < thr;
        s1b = (u64)r1 << BLK_BITS;
        b0 = 2 * D.ox - a; b1 = b0 + 1; v0 = b0 >= 0 && b0 < D.lim; v1 = b1 >= 0 && b1 < D.lim;
    }
    u64 s2 = s1b * K_MUL + K_ADD;
    unsigned qh = 0, qc = 0;
    for (u32 m = 0; m < (1u << BLK_BITS); m++, s2 += K_MUL) {
        bool pass;
        if (DK == DK_DIV) {
            u32 r2 = (u32)(s2 >> 17) & 0x7FFFFFFFu;
            u32 x = rotr32((r2 - oz) * inv, t2);
            pass = active && !(x > dbnd && r2 < thr);
        } else if (DK == DK_POW2) pass = active && ((u32)((s2 >> sh) & limm1) == oz);
        else {
            u32 r2 = (u32)(s2 >> 17) & 0x7FFFFFFFu;
            bool p0 = v0 && rotr32((r2 - (u32)b0) * inv, t2) <= dbnd, p1 = v1 && rotr32((r2 - (u32)b1) * inv, t2) <= dbnd;
            pass = active && (p0 | p1 | (r2 >= thr));
        }
        unsigned mask = __ballot_sync(0xFFFFFFFFu, pass);
        if (mask) {
            unsigned pos = qh + qc + __popc(mask & ((1u << lane) - 1u));
            if (pass) q[pos & 63] = s1b + m;
            qc += __popc(mask);
            if (qc >= 32) {
                __syncwarp();
                u64 e = q[(qh + lane) & 63];
                hit_process(P, e, DK, sink);
                qh += 32; qc -= 32;
                __syncwarp();
            }
        }
    }
    if (qc) {
        __syncwarp();
        if (lane < qc) { u64 e = q[(qh + lane) & 63]; hit_process(P, e, DK, sink); }
    }
}

template <int DK>
__global__ void __launch_bounds__(256) k_run(u64 base, u64 count) {
    DevSink sink;
    if (DK == DK_DIV || DK == DK_POW2 || DK == DK_TRI) {
        __shared__ u64 q[8][64];
        const unsigned lane = threadIdx.x & 31, wid = threadIdx.x >> 5;
        u64 warp = ((u64)blockIdx.x * blockDim.x + threadIdx.x) >> 5, nwarps = ((u64)gridDim.x * blockDim.x) >> 5;
        u64 ntask = (count + 31) / 32;
        for (u64 t = warp; t < ntask; t += nwarps) {
            u64 i = t * 32 + lane;
            run_warp<DK>(&c_par, base + i, i < count, q[wid], sink);
        }
        return;
    }
    u64 gid = (u64)blockIdx.x * blockDim.x + threadIdx.x, stride = (u64)gridDim.x * blockDim.x;
    for (u64 i = gid; i < count; i += stride) {
        if (DK == DK_RANGE) run_range(&c_par, base + i, sink);
        else if (DK == DK_EDGE) run_edge(&c_par, base + i, sink);
        else run_gen(&c_par, base + i, sink);
    }
}

/* lifting: W = (h << L) | lows[j]; линейный индекс i = j * 2^hbits + (h - hbase), h в [hbase, hbase + 2^hbits) */
__global__ void __launch_bounds__(256) k_lift(const u64 *lows, int L, int hbits, u64 hbase, u64 i0, u64 count) {
    u64 gid = (u64)blockIdx.x * blockDim.x + threadIdx.x, stride = (u64)gridDim.x * blockDim.x;
    DevSink sink;
    const u64 hmask = (1ULL << hbits) - 1;
    for (u64 i = gid; i < count; i += stride) {
        u64 ii = i0 + i;
        u64 W = (((ii & hmask) + hbase) << L) | lows[ii >> hbits];
        if (check_all(&c_par, W)) sink(W);
    }
}

#endif /* __CUDACC__ */

/* ============================================================================================
 *  Хост: стратегия
 * ============================================================================================ */
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/* ограничения младших nbits бит W (nbits >= 18) для liftable-предикатов */
static bool low_ok(u64 lo, int nbits, const std::vector<Pred> &preds) {
    u64 mask = (nbits >= 48) ? K_MASK48 : ((1ULL << nbits) - 1);
    for (const Pred &p : preds) {
        if (p.kind != PK_LIN || p.tz <= 0) continue;
        int k = std::min(p.tz, nbits - 17);
        if (k <= 0) continue;
        u64 s = ((lo + p.cst) & mask) ^ (K_MUL & mask);
        u64 s1 = (s * K_MUL + K_ADD) & mask;
        u64 s2 = (s1 * K_MUL + K_ADD) & mask;
        u64 m = (1ULL << k) - 1;
        if (((s1 >> 17) & m) != ((u64)p.ox & m)) return false;
        if (((s2 >> 17) & m) != ((u64)p.oz & m)) return false;
    }
    return true;
}

struct Options {
    std::string version = "26.3", data_dir, sets_path, obs_path, mode = "auto", dev = "auto", out_path, fmt = "dec";
    bool blocks = false, verbose = false, list_sets = false, force = false;
    size_t max_out = 100000;
    int part_i = 0, part_n = 1;
    bool have_assume = false; u64 assume = 0; int window_bits = 0;
    std::string selftest_path, gen_sets; u64 gen_seed = 0; unsigned gen_rng = 1; int gen_count = 0, gen_radius = 40;
};

static void usage() {
    fprintf(stderr,
"crack-struct — восстановление 48-битного structure seed по позициям структур.\n\n"
"  crack-struct [опции] <файл наблюдений | ->\n\n"
"Файл наблюдений: строки `имя;chunkX;chunkZ` (формат SeedcrackerX StructureSave; ; , или пробел), # — комментарий.\n"
"`имя` — id structure_set (desert_pyramids) или структуры (desert_pyramid, monument, nether_complex...), см. --list-sets.\n"
"chunkX/chunkZ — координаты ЧАНКА, в котором структура стартует (ChunkPos start; для блоковых координат: --block-coords).\n\n"
"Опции:\n"
"  --version V        версия игры (26.1 | 26.2 | 26.3; по умолчанию 26.3): читается data/structure_sets-V.json — новые версии добавляются файлом таблицы\n"
"  --sets FILE        явный файл structure_sets (датапак/мод/новая версия) вместо data/structure_sets-V.json\n"
"  --data-dir DIR     каталог data/ (по умолчанию <корень проекта>/data)\n"
"  --mode M           auto (по умолчанию) | lift | full\n"
"  --dev D            auto | gpu | cpu\n"
"  --block-coords     числа в файле — блоковые координаты (chunk = floor(x/16))\n"
"  --max-out N        максимум выводимых кандидатов (по умолчанию 100000)\n"
"  -o FILE            писать кандидатов в файл (иначе stdout); формат --format dec|hex\n"
"  --part i/n         full: только часть i из n пространства перебора (разбиение длинного запуска; i=0..n-1)\n"
"  --list-sets        показать таблицу structure_set выбранной версии\n"
"  --force            не останавливаться при слишком малой информации (ожидаемо > 4e6 кандидатов)\n"
"  -v                 подробный вывод\n"
"Тестовые/служебные:\n"
"  --assume-seed W --window-bits K   искать только в окне 2^K значений, содержащем W (проверка ядер без полного перебора)\n"
"  --selftest FILE    сверить placement (host) с векторами oracle (tests/vectors/<V>/structs.tsv)\n"
"  --gen-obs a,b,c --seed S [--gen-rng R] [--gen-radius N]   сгенерировать наблюдения для известного seed (host-реализация)\n\n"
"Вывод: по одному structure seed (десятичное, 0..2^48-1) на строку; сводка — в stderr.\n");
}

/* ---------------------------------- selftest / gen ---------------------------------- */
static int run_selftest(const SetTable &T, const std::string &path) {
    std::string txt; if (!read_file_str(path, txt)) { fprintf(stderr, "не могу прочитать %s\n", path.c_str()); return 2; }
    std::map<std::pair<std::string, long long>, std::map<std::string, std::set<std::pair<int, int>>>> vec;
    std::istringstream in(txt); std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line); std::string dim, set; long long seed; int cx, cz;
        if (!(ls >> dim >> seed >> set >> cx >> cz)) continue;
        vec[{dim, seed}][strip_ns(set)].insert({cx, cz});
    }
    long total = 0, bad = 0, checked_sets = 0;
    for (auto &kv : vec) {
        std::string dim = norm_dim(kv.first.first); u64 seed = (u64)kv.first.second;
        for (size_t si = 0; si < T.sets.size(); si++) {
            const StructSet &S = T.sets[si];
            if (S.type != "random_spread" || !S.in_dim(dim)) continue;
            std::set<std::pair<int, int>> mine;
            for (int w = 0; w < (dim == "overworld" ? 2 : 1); w++) {   /* в векторах nether/end — только окно [-32,32)^2 */
                int x0 = w == 0 ? -32 : 600, z0 = w == 0 ? -32 : -664;
                for (int cz = z0; cz < z0 + 64; cz++) for (int cx = x0; cx < x0 + 64; cx++)
                    if (host_is_structure_chunk(T, (int)si, seed, cx, cz)) mine.insert({cx, cz});
            }
            auto it = kv.second.find(S.id);
            std::set<std::pair<int, int>> ref; if (it != kv.second.end()) ref = it->second;
            checked_sets++; total += (long)ref.size();
            if (mine != ref) {
                bad++;
                fprintf(stderr, "РАСХОЖДЕНИЕ %s seed=%lld set=%s: моих %zu, эталон %zu\n", kv.first.first.c_str(), kv.first.second, S.id.c_str(), mine.size(), ref.size());
            }
        }
        /* наборы из вектора, которых нет в таблице (кроме strongholds) */
        for (auto &sv : kv.second) if (T.find_set(sv.first) < 0) { bad++; fprintf(stderr, "набор %s из вектора отсутствует в таблице\n", sv.first.c_str()); }
    }
    printf("selftest %s: (dim,seed,set) проверено %ld, чанков в эталоне %ld, расхождений %ld\n", T.version.c_str(), checked_sets, total, bad);
    return bad ? 1 : 0;
}

static int run_gen(const SetTable &T, const Options &o) {
    std::vector<std::string> names; { std::stringstream ss(o.gen_sets); std::string x; while (std::getline(ss, x, ',')) if (!x.empty()) names.push_back(x); }
    unsigned rng = o.gen_rng ? o.gen_rng : 1;
    auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    printf("# сгенерировано crack-struct --gen-obs для seed=%llu (structure seed %llu), версия %s\n", (unsigned long long)o.gen_seed, (unsigned long long)(o.gen_seed & K_MASK48), T.version.c_str());
    for (auto &nm : names) {
        int si = T.resolve(nm); if (si < 0) { fprintf(stderr, "неизвестное имя %s\n", nm.c_str()); return 2; }
        const StructSet &S = T.sets[si];
        bool ok = false;
        for (int tries = 0; tries < 2000000 && !ok; tries++) {
            i32 cx, cz;
            if (S.lim() > 1) {
                i32 rx = (i32)(rnd() % (2 * o.gen_radius + 1)) - o.gen_radius, rz = (i32)(rnd() % (2 * o.gen_radius + 1)) - o.gen_radius;
                host_potential(S, o.gen_seed, rx * S.spacing, rz * S.spacing, &cx, &cz);
            } else { cx = (i32)(rnd() % 4001) - 2000; cz = (i32)(rnd() % 4001) - 2000; }
            if (host_is_structure_chunk(T, si, o.gen_seed, cx, cz)) { printf("%s;%d;%d\n", nm.c_str(), cx, cz); ok = true; }
        }
        if (!ok) { fprintf(stderr, "не нашёл чанк для %s\n", nm.c_str()); return 2; }
    }
    return 0;
}

/* ---------------------------------- поиск ---------------------------------- */
struct Stats { double t_total = 0, t_low = 0, t_search = 0; double tested = 0; size_t nlows = 0; int L = 0; std::string mode, dev; };

static void fill_dev_params(DevParams &dp, std::vector<Pred> preds, int dk) {
    dp.n = (int)std::min<size_t>(preds.size(), MAXP); dp.dk = dk; dp.ainv = 0;
    memset(dp.pred, 0, sizeof dp.pred);
    for (int i = 0; i < dp.n; i++) dp.pred[i] = preds[i];
}

static int cuda_device_ok() {
#ifdef __CUDACC__
    int n = 0; if (cudaGetDeviceCount(&n) != cudaSuccess || n <= 0) { cudaGetLastError(); return 0; }
    return 1;
#else
    return 0;     /* сборка без CUDA (make cpu) */
#endif
}

int main(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char *nm) -> const char * { if (i + 1 >= argc) { fprintf(stderr, "опция %s требует значение\n", nm); exit(2); } return argv[++i]; };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--version") o.version = need("--version");
        else if (a == "--data-dir") o.data_dir = need("--data-dir");
        else if (a == "--sets") o.sets_path = need("--sets");
        else if (a == "--mode") o.mode = need("--mode");
        else if (a == "--dev") o.dev = need("--dev");
        else if (a == "--block-coords") o.blocks = true;
        else if (a == "--max-out") o.max_out = (size_t)strtoull(need("--max-out"), 0, 10);
        else if (a == "-o") o.out_path = need("-o");
        else if (a == "--format") o.fmt = need("--format");
        else if (a == "--part") { const char *v = need("--part"); if (sscanf(v, "%d/%d", &o.part_i, &o.part_n) != 2 || o.part_n < 1 || o.part_i < 0 || o.part_i >= o.part_n) { fprintf(stderr, "--part i/n\n"); return 2; } }
        else if (a == "--list-sets") o.list_sets = true;
        else if (a == "-v") o.verbose = true;
        else if (a == "--force") o.force = true;
        else if (a == "--assume-seed") { o.have_assume = true; o.assume = strtoull(need("--assume-seed"), 0, 0) & K_MASK48; }
        else if (a == "--window-bits") o.window_bits = atoi(need("--window-bits"));
        else if (a == "--selftest") o.selftest_path = need("--selftest");
        else if (a == "--gen-obs") o.gen_sets = need("--gen-obs");
        else if (a == "--seed") o.gen_seed = strtoull(need("--seed"), 0, 0);   /* знаковое тоже: strtoull принимает -N как 2^64-N */
        else if (a == "--gen-rng") o.gen_rng = (unsigned)strtoul(need("--gen-rng"), 0, 10);
        else if (a == "--gen-radius") o.gen_radius = atoi(need("--gen-radius"));
        else if (!a.empty() && a[0] == '-' && a != "-") { fprintf(stderr, "неизвестная опция %s (см. --help)\n", a.c_str()); return 2; }
        else o.obs_path = a;
    }
    std::string root = find_root(argv[0]);
    if (o.data_dir.empty()) o.data_dir = root + "/data";
    SetTable T;
    std::string sets_file = o.sets_path.empty() ? o.data_dir + "/structure_sets-" + o.version + ".json" : o.sets_path;
    try { T.load(sets_file, o.version); }
    catch (std::exception &e) {
        fprintf(stderr, "ошибка загрузки таблиц: %s\n(для новой версии достаточно получить data/structure_sets-<V>.json скриптом tools/extract_structure_sets.py или указать файл через --sets)\n", e.what());
        return 2;
    }

    if (o.list_sets) {
        printf("# structure_sets версии %s (из %s)\n# id  тип  spacing separation limit spread salt frequency(метод) exclusion измерения  структуры\n", T.version.c_str(), sets_file.c_str());
        for (auto &s : T.sets) {
            std::string dims, sts; for (auto &d : s.dims) dims += d + ","; for (auto &x : s.structures) sts += x + ",";
            if (s.type == "random_spread")
                printf("%s %s %d %d %d %s %d %g(m%d) %s %s %s\n", s.id.c_str(), s.type.c_str(), s.spacing, s.separation, s.lim(), s.triangular ? "triangular" : "linear", s.salt, (double)s.frequency, s.freq_method,
                       s.excl_set.empty() ? "-" : (s.excl_set + "/" + std::to_string(s.excl_chunks)).c_str(), dims.c_str(), sts.c_str());
            else printf("%s %s distance=%d spread=%d count=%d %s %s\n", s.id.c_str(), s.type.c_str(), s.rings_distance, s.rings_spread, s.rings_count, dims.c_str(), sts.c_str());
        }
        return 0;
    }
    if (!o.selftest_path.empty()) return run_selftest(T, o.selftest_path);
    if (!o.gen_sets.empty()) return run_gen(T, o);
    if (o.obs_path.empty()) { usage(); return 2; }

    /* ---- наблюдения -> предикаты ---- */
    std::vector<Obs> obs; std::string err;
    if (!parse_obs_file(o.obs_path, T, o.blocks, obs, err)) { fprintf(stderr, "ошибка: %s\n", err.c_str()); return 2; }
    Problem PR;
    if (!build_problem(T, obs, PR, err)) { fprintf(stderr, "ошибка: %s\n", err.c_str()); return 2; }
    for (auto &w : PR.warnings) fprintf(stderr, "предупреждение: %s\n", w.c_str());
    bool any_search = false; for (auto &p : PR.preds) if (p.kind <= PK_RED_L3) any_search = true;
    if (!any_search) {
        if (!PR.rings.empty()) fprintf(stderr, "ошибка: strongholds (concentric_rings) — только фильтр (~8 бит на позицию): добавьте наблюдения random_spread/reducer, по которым идёт поиск\n");
        else fprintf(stderr, "ошибка: нет наблюдений, несущих информацию о seed\n");
        return 2;
    }
    if (!PR.rings.empty()) fprintf(stderr, "strongholds: %zu позиций используются как фильтр кандидатов (окно ±7 чанков вокруг геометрии колец)\n", PR.rings.size());
    {
        double lift_bits = 0; int nlift = 0;
        for (auto &p : PR.preds) if (p.kind == PK_LIN && p.tz > 0) { nlift++; lift_bits += 2 * std::min(p.tz, 3); }
        fprintf(stderr, "версия %s: %zu наблюдений (%d использовано), %zu предикатов, информация ~ %.1f бит, ожидаемо ложных кандидатов ~ %.3g (при 2^48 кандидатах)\n",
                o.version.c_str(), obs.size(), PR.used, PR.preds.size(), PR.info_bits, std::pow(2.0, 48.0 - PR.info_bits));
        if (std::pow(2.0, 48.0 - PR.info_bits) > 4e6 && !o.force) {
            fprintf(stderr, "ошибка: наблюдений слишком мало (информация ~ %.1f бит, ожидаемо ~%.3g кандидатов): перебор бессмыслен. Добавьте наблюдения (цель >= 55 бит) или используйте --force\n", PR.info_bits, std::pow(2.0, 48.0 - PR.info_bits));
            return 2;
        }
        if (PR.info_bits < 40.0) fprintf(stderr, "предупреждение: информации < 40 бит — список кандидатов будет большим (>=256); добавьте наблюдения (цель: >= 55 бит)\n");
        if (o.verbose) { for (size_t i = 0; i < PR.preds.size(); i++) fprintf(stderr, "  pred[%zu] %s p=%.3g%s\n", i, PR.desc[i].c_str(), PR.prob[i], PR.preds[i].tz ? " (liftable)" : ""); (void)lift_bits; (void)nlift; }
    }

    /* ---- выбор режима и устройства ---- */
    bool gpu = (o.dev == "gpu") || (o.dev == "auto" && cuda_device_ok());
    if (o.dev == "gpu" && !cuda_device_ok()) { fprintf(stderr, "ошибка: --dev gpu, но CUDA-устройство недоступно\n"); return 3; }
    if (o.dev == "auto" && !gpu) fprintf(stderr, "предупреждение: CUDA-устройство не найдено — CPU (OpenMP, %d потоков); полный перебор 2^48 на CPU: десятки минут (linear/reducer) — часы (triangular), см. ETA в прогрессе\n", omp_get_max_threads());
    int sms = 0;
    double t_init = 0;
#ifdef __CUDACC__
    if (gpu) { double a = now(); cudaFree(0); cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, 0); t_init = now() - a;
               if (o.verbose) fprintf(stderr, "CUDA-контекст: %.3f с, SM=%d\n", t_init, sms); }
#endif

    /* liftable */
    int maxtz = 0; for (auto &p : PR.preds) maxtz = std::max(maxtz, p.tz);
    std::string mode = o.mode;
    std::vector<u64> lows; int L = 0; double t0 = now(), t_low = 0;
    auto enumerate_lows = [&]() {
        L = 17 + maxtz;
        std::vector<u64> cur = {0};
        for (int l = 0; l < L; l++) {
            std::vector<u64> nxt; nxt.reserve(cur.size() * 2);
            for (u64 v : cur) for (int b = 0; b < 2; b++) { u64 c = v | ((u64)b << l); if (l + 1 < 18 || low_ok(c, l + 1, PR.preds)) nxt.push_back(c); }
            cur.swap(nxt);
        }
        lows.swap(cur);
    };
    if (mode == "auto" || mode == "lift") {
        if (maxtz > 0) {
            enumerate_lows(); t_low = now() - t0;
            double cost = (double)lows.size() * std::ldexp(1.0, 48 - L);
            if (mode == "auto" && cost > std::ldexp(1.0, 48) / 8) { fprintf(stderr, "lifting даёт малый выигрыш (%zu низов из 2^%d) — полный перебор\n", lows.size(), L); mode = "full"; }
            else mode = "lift";
        } else {
            if (mode == "lift") { fprintf(stderr, "ошибка: --mode lift, но нет liftable-наблюдений (linear random_spread с чётным не-степенью-двойки spacing-separation: desert/igloo/jungle/swamp/shipwreck/ocean_ruins/village/trial/trail...)\n"); return 2; }
            mode = "full";
        }
    }
    if (mode != "lift" && mode != "full") { fprintf(stderr, "неизвестный режим %s\n", mode.c_str()); return 2; }

    std::vector<u64> found; bool overflow = false; u64 nfound_dev = 0;
    double t_search0 = now(), tested = 0;

    /* порядок проверки: по возрастанию вероятности прохождения (в lift-режиме — с учётом уже выполненных низких бит) */
    auto eff_prob = [&](size_t i) {
        const Pred &p = PR.preds[i]; double pr = PR.prob[i];
        if (mode == "lift" && p.kind == PK_LIN && p.tz > 0) { int k = std::min(p.tz, L - 17); pr *= std::ldexp(1.0, 2 * k); }
        return pr;
    };
    std::vector<size_t> ord(PR.preds.size()); for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return eff_prob(a) < eff_prob(b); });

    int dk = DK_GEN;
    if (mode == "full") {
        /* драйвер: предикат с минимальной оценкой стоимости (число перечисляемых кандидатов * стоимость одного) */
        int best = -1; double bestc = std::ldexp(1.0, 48) * 60.0; int bestdk = DK_GEN;
        for (size_t i = 0; i < PR.preds.size(); i++) {
            const Pred &p = PR.preds[i]; double c; int k;
            if (p.kind == PK_LIN && p.pow2k >= 0) { k = DK_POW2; c = std::ldexp(1.0, 48) / p.lim * (9.0 + 45.0 / p.lim); }
            else if (p.kind == PK_LIN) { k = DK_DIV; c = std::ldexp(1.0, 48) / p.lim * (14.0 + 45.0 / p.lim); }
            else if ((p.kind == PK_RED_DEF || p.kind == PK_RED_L2) && p.T24 > 0) { k = DK_RANGE; c = std::ldexp(1.0, 48) * ((double)p.T24 / 16777216.0) * 40.0; }
            else if (p.kind == PK_TRI && p.pow2k < 0) { k = DK_TRI; c = std::ldexp(1.0, 48) * ((double)tri_geom(p).na / p.lim) * 40.0; }
            else continue;
            if (c < bestc) { bestc = c; best = (int)i; bestdk = k; }
        }
        if (best >= 0) {
            dk = bestdk;
            ord.erase(std::find(ord.begin(), ord.end(), (size_t)best)); ord.insert(ord.begin(), (size_t)best);
        }
    }
    std::vector<Pred> ordered; for (size_t i : ord) ordered.push_back(PR.preds[i]);
    if (ordered.size() > MAXP) fprintf(stderr, "предупреждение: предикатов %zu > %d: на устройстве проверяются %d самых селективных, остальные — на хосте\n", ordered.size(), MAXP, MAXP);
    DevParams dp; fill_dev_params(dp, ordered, dk);
    { u64 x = K_MUL; for (int i = 0; i < 6; i++) x *= 2 - K_MUL * x; dp.ainv = x & K_MASK48; }
    static const char *dkn[] = {"generic", "enumerate-residues (r = ox + k*lim)", "enumerate-range (старшие биты)", "triangular", "enumerate-range (reducer)", "edge"};
    Stats st; st.mode = mode; st.dev = gpu ? "gpu" : "cpu"; st.L = L; st.nlows = lows.size(); st.t_low = t_low;

    if (mode == "lift") {
        fprintf(stderr, "[lift] L=%d бит: %zu кандидатов младших бит (%.3f с); перебор старших %d бит: %.3e проверок\n", L, lows.size(), t_low, 48 - L, (double)lows.size() * std::ldexp(1.0, 48 - L));
        if (lows.empty()) { fprintf(stderr, "нет кандидатов младших бит — наблюдения противоречивы\n"); }
        else {
            double pe = 1; for (size_t i = 0; i < PR.preds.size(); i++) pe *= eff_prob(i);
            fprintf(stderr, "[lift] ожидаемо ложных кандидатов ~ %.3g (с учётом уже проверенных младших бит; строка выше «ожидаемо» — грубая оценка независимых бит)\n", (double)lows.size() * std::ldexp(1.0, 48 - L) * pe);
        }
        int Hbits = 48 - L;
        int hbits = Hbits; u64 hbase = 0;      /* окно по старшим битам (только для тестов) */
        if (o.have_assume && o.window_bits > 0 && o.window_bits < Hbits) {
            hbits = o.window_bits; hbase = (o.assume >> L) & ~((1ULL << hbits) - 1);
            fprintf(stderr, "[тест] окно поиска: 2^%d значений старших бит вокруг предполагаемого seed\n", hbits);
        }
        u64 hcount = 1ULL << hbits, hmask = hcount - 1;
        u64 total = (u64)lows.size() << hbits;
        tested = (double)total;
        if (total) {
#ifdef __CUDACC__
            if (gpu) {
                u64 *d_lows; cudaMalloc(&d_lows, sizeof(u64) * lows.size());
                cudaMemcpy(d_lows, lows.data(), sizeof(u64) * lows.size(), cudaMemcpyHostToDevice);
                cudaMemcpyToSymbol(c_par, &dp, sizeof dp);
                unsigned zero = 0; cudaMemcpyToSymbol(d_nres, &zero, sizeof zero);
                int blocks = sms * 16;
                const u64 slice = 1ULL << 32;
                for (u64 off = 0; off < total; off += slice) {
                    u64 cnt = std::min(slice, total - off);
                    k_lift<<<blocks, 256>>>(d_lows, L, hbits, hbase, off, cnt);
                    cudaError_t e = cudaDeviceSynchronize(); if (e != cudaSuccess) { fprintf(stderr, "CUDA: %s\n", cudaGetErrorString(e)); return 3; }
                }
                unsigned nr; cudaMemcpyFromSymbol(&nr, d_nres, sizeof nr);
                nfound_dev = nr; if (nr > RES_CAP) overflow = true;
                found.resize(std::min<unsigned>(nr, RES_CAP)); cudaMemcpyFromSymbol(found.data(), d_res, sizeof(u64) * found.size());
                cudaFree(d_lows);
            } else
#endif
            {
                std::vector<std::vector<u64>> per(omp_get_max_threads());
                u64 ctile = 4096, ntile = (hcount + ctile - 1) / ctile;
                #pragma omp parallel
                {
                    std::vector<u64> &loc = per[omp_get_thread_num()]; HostSink sink{&loc};
                    #pragma omp for schedule(dynamic, 1) collapse(2)
                    for (long long j = 0; j < (long long)lows.size(); j++)
                        for (long long hc = 0; hc < (long long)ntile; hc++) {
                            u64 h0 = (u64)hc * ctile, h1 = std::min<u64>(h0 + ctile, hcount);
                            for (u64 h = h0; h < h1; h++) { u64 W = (((h & hmask) + hbase) << L) | lows[j]; if (check_all(&dp, W)) sink(W); }
                        }
                }
                for (auto &v : per) found.insert(found.end(), v.begin(), v.end());
                nfound_dev = found.size();
            }
        }
    } else {
        /* ---------- полный перебор ---------- */
        const Pred &D = dp.pred[0];
        struct Job { int dk; u64 nidx; double equiv; };           /* equiv — сколько значений пространства 2^48 покрывает один индекс */
        std::vector<Job> jobs;
        const double B = std::ldexp(1.0, BLK_BITS);
        u64 s1t = 0; bool have_truth = o.have_assume;
        if (o.have_assume) { u64 s0t = ((o.assume + D.cst) & K_MASK48) ^ K_MUL; s1t = (s0t * K_MUL + K_ADD) & K_MASK48; }
        u64 tidx = 0;                                            /* индекс, содержащий предполагаемый seed (только для тестов) */
        switch (dk) {
        case DK_DIV: {
            u64 KA = ((u64)D.thr_rej - (u64)D.ox + (u64)D.lim - 1) / (u64)D.lim;
            jobs.push_back({DK_DIV, KA, (double)D.lim * B}); jobs.push_back({DK_EDGE, (u64)(0x80000000u - D.thr_rej), 1.0});
            u64 r = s1t >> 17; tidx = (r < D.thr_rej && r >= (u64)D.ox && (r - D.ox) % D.lim == 0) ? (r - D.ox) / D.lim : 0; break; }
        case DK_POW2: {
            int sh = 48 - D.pow2k; jobs.push_back({DK_POW2, 1ULL << (sh - BLK_BITS), (double)D.lim * B});
            tidx = (s1t - ((u64)D.ox << sh)) >> BLK_BITS; if ((s1t >> sh) != (u64)D.ox) tidx = 0; break; }
        case DK_RANGE:
            jobs.push_back({DK_RANGE, (u64)D.T24 << (24 - BLK_BITS), B * 16777216.0 / (double)D.T24}); tidx = s1t >> BLK_BITS; break;
        case DK_TRI: {
            TriGeom g = tri_geom(D); u64 n = (u64)g.na * g.kamax;
            jobs.push_back({DK_TRI, n, std::ldexp(1.0, 48) / (double)n}); jobs.push_back({DK_EDGE, (u64)(0x80000000u - D.thr_rej), 1.0});
            u64 r1 = s1t >> 17; i32 a = (i32)(r1 % (u64)D.lim);
            tidx = (r1 < D.thr_rej && a >= (i32)g.a_lo && a < (i32)(g.a_lo + g.na)) ? (u64)(a - (i32)g.a_lo) * g.kamax + r1 / (u64)D.lim : 0; break; }
        default: jobs.push_back({DK_GEN, 1ULL << (48 - TILE_BITS), (double)(1 << TILE_BITS)}); tidx = o.assume >> TILE_BITS; break;
        }
        fprintf(stderr, "[full] драйвер: %s [%s], предикатов на устройстве: %d; индексов %llu (каждый покрывает ~%.3g значений из 2^48)\n", dkn[dk], PR.desc[ord[0]].c_str(), dp.n,
                (unsigned long long)jobs[0].nidx, jobs[0].equiv);
        u64 ilo = 0, ihi = jobs[0].nidx; bool run_edge_job = true;
        if (o.part_n > 1) { u64 per = jobs[0].nidx / (u64)o.part_n; ilo = per * (u64)o.part_i; ihi = (o.part_i == o.part_n - 1) ? jobs[0].nidx : ilo + per; run_edge_job = (o.part_i == 0); }
        if (have_truth && o.part_n > 1) fprintf(stderr, "[тест] предполагаемый seed лежит в индексе %llu из %llu, т.е. в части %llu/%d при --part i/%d\n", (unsigned long long)tidx,
                                                 (unsigned long long)jobs[0].nidx, (unsigned long long)(tidx / std::max<u64>(1, jobs[0].nidx / (u64)o.part_n)), o.part_n, o.part_n);
        if (o.have_assume && o.window_bits > 0 && o.window_bits < 48) {
            u64 nwin = (u64)std::max(1.0, std::ldexp(1.0, o.window_bits) / jobs[0].equiv);
            nwin = std::min(nwin, jobs[0].nidx); ilo = tidx - tidx % nwin; ihi = std::min(jobs[0].nidx, ilo + nwin); run_edge_job = false;
            fprintf(stderr, "[тест] окно поиска: %llu индексов (~2^%d значений), содержит предполагаемый seed\n", (unsigned long long)(ihi - ilo), o.window_bits);
        }
        if (!run_edge_job && jobs.size() > 1) jobs.resize(1);
        jobs[0].nidx = ihi; /* диапазон [ilo, ihi) для основного задания; прочие задания — целиком */
        double covered = (double)(ihi - ilo) * jobs[0].equiv;
        tested = covered;
        double tstart = now(), tlast = tstart; u64 total_idx = 0; for (size_t ji = 0; ji < jobs.size(); ji++) total_idx += jobs[ji].nidx - (ji == 0 ? ilo : 0);
        u64 done_idx = 0;
#ifdef __CUDACC__
        if (gpu) {
            cudaMemcpyToSymbol(c_par, &dp, sizeof dp);
            unsigned zero = 0; cudaMemcpyToSymbol(d_nres, &zero, sizeof zero);
            int blocks = sms * 16;
            for (size_t ji = 0; ji < jobs.size(); ji++) {
                u64 lo = ji == 0 ? ilo : 0, hi = jobs[ji].nidx, pos = lo;
                u64 slice = (jobs[ji].dk == DK_TRI || jobs[ji].dk == DK_GEN) ? (1ULL << 17) : (1ULL << 15);
                while (pos < hi) {
                    u64 cnt = std::min<u64>(slice, hi - pos);
                    double a = now();
                    switch (jobs[ji].dk) {
                    case DK_DIV: k_run<DK_DIV><<<blocks, 256>>>(pos, cnt); break;
                    case DK_POW2: k_run<DK_POW2><<<blocks, 256>>>(pos, cnt); break;
                    case DK_RANGE: k_run<DK_RANGE><<<blocks, 256>>>(pos, cnt); break;
                    case DK_EDGE: k_run<DK_EDGE><<<blocks, 256>>>(pos, cnt); break;
                    case DK_TRI: k_run<DK_TRI><<<blocks, 256>>>(pos, cnt); break;
                    default: k_run<DK_GEN><<<blocks, 256>>>(pos, cnt); break;
                    }
                    cudaError_t e = cudaDeviceSynchronize(); if (e != cudaSuccess) { fprintf(stderr, "CUDA: %s\n", cudaGetErrorString(e)); return 3; }
                    double dt = now() - a; pos += cnt; done_idx += cnt;
                    if (dt < 0.12 && cnt == slice) slice <<= 1; else if (dt > 0.5 && slice > 64) slice >>= 1;
                    double t = now();
                    if (t - tlast > 5.0 && t - tstart > 10.0) {
                        tlast = t; unsigned nr; cudaMemcpyFromSymbol(&nr, d_nres, sizeof nr);
                        double frac = (double)done_idx / (double)total_idx;
                        fprintf(stderr, "[full] %.1f%%  осталось ~%.0f с  найдено %u\n", 100 * frac, (t - tstart) / frac - (t - tstart), nr);
                    }
                }
            }
            unsigned nr; cudaMemcpyFromSymbol(&nr, d_nres, sizeof nr);
            nfound_dev = nr; if (nr > RES_CAP) overflow = true;
            found.resize(std::min<unsigned>(nr, RES_CAP)); cudaMemcpyFromSymbol(found.data(), d_res, sizeof(u64) * found.size());
        } else
#endif
        {
            std::vector<std::vector<u64>> per(omp_get_max_threads());
            for (size_t ji = 0; ji < jobs.size(); ji++) {
                u64 lo = ji == 0 ? ilo : 0, hi = jobs[ji].nidx; int jdk = jobs[ji].dk;
                long long progress = 0;
                #pragma omp parallel
                {
                    std::vector<u64> &loc = per[omp_get_thread_num()]; HostSink sink{&loc};
                    #pragma omp for schedule(dynamic, 1)
                    for (long long i = (long long)lo; i < (long long)hi; i++) {
                        run_index(&dp, jdk, (u64)i, sink);
                        if (omp_get_thread_num() == 0 && ((++progress) & 0xFFF) == 0) {
                            double t1 = now() - tstart; double frac = (double)(progress * omp_get_num_threads()) / (double)(hi - lo);
                            if (t1 > 10 && now() - tlast > 10) { tlast = now(); fprintf(stderr, "[full/cpu] ~%.2f%%, осталось ~%.0f с\n", 100 * frac, t1 / frac - t1); }
                        }
                    }
                }
            }
            for (auto &v : per) found.insert(found.end(), v.begin(), v.end());
            nfound_dev = found.size();
        }
    }
    double t_search = now() - t_search0;

    /* ---- хостовая перепроверка всех предикатов и exclusion zones ---- */
    std::sort(found.begin(), found.end()); found.erase(std::unique(found.begin(), found.end()), found.end());
    std::vector<u64> good;
    for (u64 W : found) if (host_check_all(T, PR, W)) good.push_back(W);
    if (overflow) fprintf(stderr, "предупреждение: кандидатов > %u — выдана лишь часть; добавьте наблюдения\n", RES_CAP);
    if (o.have_assume && o.window_bits > 0) {
        bool hit = std::binary_search(good.begin(), good.end(), o.assume);
        fprintf(stderr, "[тест] assume-seed %llu %s среди кандидатов\n", (unsigned long long)o.assume, hit ? "НАЙДЕН" : "НЕ найден");
    }
    st.t_search = t_search; st.t_total = now() - t0;
    fprintf(stderr, "режим %s/%s: кандидатов %zu (после проверки %zu)%s; поиск %.3f с (%.3e значений 2^48 в секунду, «эквивалентных проверок»), всего %.3f с\n", st.mode.c_str(), st.dev.c_str(), found.size(), good.size(),
            nfound_dev > found.size() ? " [усечено]" : "", t_search, tested / std::max(t_search, 1e-9), st.t_total);

    FILE *out = stdout; if (!o.out_path.empty()) { out = fopen(o.out_path.c_str(), "w"); if (!out) { perror("fopen"); return 2; } }
    size_t np = 0;
    for (u64 W : good) {
        if (np++ >= o.max_out) { fprintf(stderr, "вывод усечён до %zu кандидатов (--max-out)\n", o.max_out); break; }
        if (o.fmt == "hex") fprintf(out, "0x%012llx\n", (unsigned long long)W); else fprintf(out, "%llu\n", (unsigned long long)W);
    }
    if (out != stdout) fclose(out);
    return good.empty() ? 1 : 0;
}

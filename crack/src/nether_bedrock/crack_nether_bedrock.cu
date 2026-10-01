/*
 * crack-nether-bedrock — восстановление 48-битного structure seed по паттерну бедрока потолка (y=123..126) и пола (y=1..4)
 * Нижнего мира, Minecraft 26.1 / 26.2 / 26.3 (арифметика бедрока во всех трёх версиях одинакова — см. docs/23).
 *
 * Идея перенесена из Nether_Bedrock_Cracker (19MisterX98, Rust): каждое наблюдение «бедрок / не бедрок» — интервал на 48-битное
 * состояние state1 = ((F ^ h)*M + 11) mod 2^48, где F — фабричное число поверхности (потолок/пол) и h зависит только от (x,y,z).
 * Поиск идёт сверху вниз по старшим битам F: 2^(48-K) префиксов (K=12 младших бит неизвестны), ранний отсев интервалами, затем
 * DFS по оставшимся K битам слоями (раскрывается по одному биту; на каждом слое — свой набор проверок). GPU: ядро верхнего слоя
 * (ядро 4 — Gray-обход групп по 32 префикса с константными приращениями, остальные проверки только для выживших), прошедшие F (точные
 * 48 бит более селективной поверхности) пишутся через atomicAdd в буфер; этап A на GPU: обращение nextLong() (F -> R) и проверка второй
 * поверхности через общий R; на хосте: обращение R -> structure seed и полная проверка всех наблюдений по формуле игры (float-сравнение).
 *
 * Формат входа: строки "x y z bedrock|other" (регистр не важен; без типа = bedrock как в оригинале), '#' — комментарий.
 * Сборка: см. Makefile в этом каталоге.   Запуск: crack-nether-bedrock [опции] файл_наблюдений
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <tuple>
#include <chrono>
#include <omp.h>
#include "nb_core.h"

#ifndef NO_CUDA
#include <cuda_runtime.h>
#define CK(call) do { cudaError_t e_ = (call); if (e_ != cudaSuccess) { fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(3); } } while (0)
#endif

static inline double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

#define NB_MAX_LAYERS 14
#define NB_MAX_CHK 160                    /* 14*160*24 = 53760 байт constant memory */

/* ============================================================================================================
 *  Наблюдения и блоки
 * ============================================================================================================ */
struct Obs { i32 x, y, z; bool bedrock; };

struct Blk {
    i32 x, y, z, hy;          /* hy — y, подставляемый в Mth.getSeed (в режиме Paper1_18 — 0 / 122) */
    bool bedrock;
    int surf;                 /* 0 пол, 1 потолок */
    double p;                 /* порог вероятности Mth.map (nextFloat() < p) */
    u64 h, lo, len;           /* интервал state1: [lo, lo+len) */
};

/* Mth.map(y, trueAtAndBelow, falseAtAndAbove, 1.0, 0.0) — побитно как в игре (double) */
static double nb_prob(int y, int surf) {
    double t = surf == 0 ? 0.0 : 122.0, f = surf == 0 ? 5.0 : 127.0;
    double alpha = ((double)y - t) / (f - t);        /* Mth.inverseLerp */
    return 1.0 + alpha * (0.0 - 1.0);                /* Mth.lerp(alpha, 1.0, 0.0) = p0 + alpha*(p1-p0) */
}
/* наименьшее T такое, что k/2^24 < p  <=>  k < T  (k = state1 >> 24) */
static u64 nb_thr(double p) {
    double v = std::ceil(p * 16777216.0);
    if (v < 0) v = 0;
    u64 T = (u64)v;
    while (T > 0 && (double)(T - 1) / 16777216.0 >= p) T--;
    while ((double)T / 16777216.0 < p) T++;
    return T;
}
static bool y_valid(int y, int *surf) {
    if (y >= 1 && y <= 4) { *surf = 0; return true; }
    if (y >= 123 && y <= 126) { *surf = 1; return true; }
    return false;
}
static Blk make_blk(const Obs &o, bool paper) {
    Blk b; b.x = o.x; b.y = o.y; b.z = o.z; b.bedrock = o.bedrock;
    y_valid(o.y, &b.surf);
    b.hy = paper ? (b.surf == 1 ? 122 : 0) : o.y;
    b.p = nb_prob(o.y, b.surf);
    u64 T = nb_thr(b.p) << 24;                        /* state1 < T  <=>  nextFloat() < p */
    bool in_low;                                      /* интервал [0,T) или [T,2^48)? */
    if (b.surf == 0) in_low = b.bedrock; else in_low = !b.bedrock;
    if (in_low) { b.lo = 0; b.len = T; } else { b.lo = T; b.len = NB_ONE48 - T; }
    b.h = nb_poshash(b.x, b.hy, b.z);
    return b;
}
/* точная проверка по формуле игры (через float) при известных F_roof/F_floor */
static bool blk_float_ok(const Blk &b, u64 Ffloor, u64 Froof) {
    float f = nb_bedrock_float(b.surf == 0 ? Ffloor : Froof, b.x, b.hy, b.z);
    bool lt = (double)f < b.p;
    bool bed = b.surf == 0 ? lt : !lt;
    return bed == b.bedrock;
}
static bool seed_consistent(u64 S, const std::vector<Blk> &all, int *nbad = nullptr) {
    u64 R = nb_base_R(S);
    u64 Ff = nb_factory_F(R, NB_FLOOR_HASH), Fr = nb_factory_F(R, NB_ROOF_HASH);
    int bad = 0;
    for (const Blk &b : all) if (!blk_float_ok(b, Ff, Fr)) bad++;
    if (nbad) *nbad = bad;
    return bad == 0;
}

/* ---------- чтение файла ---------- */
struct ParseStats { int lines = 0, used = 0, dup = 0, skipped_y = 0, bad = 0; };
static bool parse_obs(const char *path, std::vector<Obs> &out, ParseStats &ps, bool quiet) {
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
    if (!f) { fprintf(stderr, "не могу открыть %s: %s\n", path, strerror(errno)); return false; }
    char line[512];
    std::map<std::tuple<int,int,int>, bool> seen;
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *c = strchr(line, '#'); if (c) *c = 0;
        char a[64], bb[64], cc[64], t[64] = "";
        int n = sscanf(line, "%63s %63s %63s %63s", a, bb, cc, t);
        if (n <= 0) continue;
        ps.lines++;
        if (n < 3) { if (!quiet) fprintf(stderr, "строка %d: нужно 'x y z [bedrock|other]' — пропуск\n", lineno); ps.bad++; continue; }
        char *e1, *e2, *e3;
        long x = strtol(a, &e1, 10), y = strtol(bb, &e2, 10), z = strtol(cc, &e3, 10);
        if (*e1 || *e2 || *e3) { if (!quiet) fprintf(stderr, "строка %d: нечисловые координаты — пропуск\n", lineno); ps.bad++; continue; }
        if (labs(x) > 30000000L || labs(z) > 30000000L) { if (!quiet) fprintf(stderr, "строка %d: |x| или |z| > 30000000 — пропуск\n", lineno); ps.bad++; continue; }
        bool bed = true;
        if (n >= 4) {
            for (char *q = t; *q; q++) *q = (char)tolower((unsigned char)*q);
            if (!strcmp(t, "other") || !strcmp(t, "o") || !strcmp(t, "air") || !strcmp(t, "netherrack")) bed = false;
            else if (!strcmp(t, "bedrock") || !strcmp(t, "b")) bed = true;
            else { if (!quiet) fprintf(stderr, "строка %d: неизвестный тип '%s' (ожидается bedrock|other) — пропуск\n", lineno, t); ps.bad++; continue; }
        }
        int surf;
        if (!y_valid((int)y, &surf)) {
            if (!quiet) fprintf(stderr, "строка %d: y=%ld не несёт информации (значимы y=1..4 и y=123..126) — пропуск\n", lineno, y);
            ps.skipped_y++; continue;
        }
        auto key = std::make_tuple((int)x, (int)y, (int)z);
        auto it = seen.find(key);
        if (it != seen.end()) {
            if (it->second != bed) { fprintf(stderr, "строка %d: противоречие — (%ld %ld %ld) указан и как bedrock, и как other\n", lineno, x, y, z); return false; }
            ps.dup++; continue;
        }
        seen[key] = bed;
        out.push_back({(i32)x, (i32)y, (i32)z, bed});
        ps.used++;
    }
    if (f != stdin) fclose(f);
    return true;
}

/* ============================================================================================================
 *  План поиска: слои проверок для первичной поверхности
 * ============================================================================================================ */
struct Plan {
    int K = 12;
    int n[NB_MAX_LAYERS] = {0};
    std::vector<NbChk> chk[NB_MAX_LAYERS];
    double survive_frac_top = 1.0;       /* ожидаемая доля префиксов, проходящих слой K */
};
struct HostTab {
    const Plan *p;
    int n(int b) const { return p->n[b]; }
    const NbChk &at(int b, int j) const { return p->chk[b][j]; }
};

static void build_layer(const std::vector<Blk> &prim, int b, std::vector<NbChk> &out, double *frac) {
    const u64 lbm = (1ULL << b) - 1ULL;
    const u64 W = lbm * NB_MUL;
    std::vector<NbChk> v;
    for (const Blk &k : prim) {
        u64 D = k.len + W;
        if (D >= NB_ONE48) continue;                                 /* слой для этого блока пуст */
        NbChk c; c.h = k.h & ~lbm & NB_MASK48; c.C = (NB_ADD - k.lo + W) & NB_MASK48; c.D = D;
        v.push_back(c);
    }
    std::sort(v.begin(), v.end(), [](const NbChk &a, const NbChk &b2) { if (a.D != b2.D) return a.D < b2.D; if (a.h != b2.h) return a.h < b2.h; return a.C < b2.C; });
    v.erase(std::unique(v.begin(), v.end(), [](const NbChk &a, const NbChk &b2) { return a.D == b2.D && a.h == b2.h && a.C == b2.C; }), v.end());
    double f = 1.0;
    for (const NbChk &c : v) f *= (double)c.D / (double)NB_ONE48;
    if (frac) *frac = f;                                             /* доля, прошедшая ВСЕ проверки слоя (до обрезки) */
    if ((int)v.size() > NB_MAX_CHK) v.resize(NB_MAX_CHK);
    out = v;
}
static void build_plan(const std::vector<Blk> &prim, int K, Plan &pl) {
    pl.K = K;
    for (int b = 0; b <= K; b++) {
        double f;
        build_layer(prim, b, pl.chk[b], &f);
        pl.n[b] = (int)pl.chk[b].size();
        if (b == K) pl.survive_frac_top = f;
    }
}
/* информация (бит) первичной поверхности: -log2 Π len/2^48 */
static double info_bits(const std::vector<Blk> &v) {
    double s = 0; for (const Blk &b : v) s += -std::log2((double)b.len / (double)NB_ONE48); return s;
}

/* ============================================================================================================
 *  Сканирование: CPU (OpenMP) и GPU (CUDA)
 * ============================================================================================================ */
struct ScanStats { unsigned long long top_survivors = 0; unsigned long long cand_total = 0; bool truncated = false; };

static void scan_cpu(const Plan &pl, u64 idx0, u64 count, int threads, std::vector<u64> &cands, ScanStats &st) {
    HostTab tab{&pl};
    const int K = pl.K;
    unsigned long long surv = 0;
    std::vector<std::vector<u64>> loc(threads);
    const u64 CH = 1ULL << 14;
    const u64 nchunks = (count + CH - 1) / CH;
#pragma omp parallel num_threads(threads) reduction(+ : surv)
    {
        std::vector<u64> &mine = loc[omp_get_thread_num()];
        auto emit = [&](u64 F) { mine.push_back(F); };
        unsigned long long s = 0;
#pragma omp for schedule(dynamic, 1)
        for (long long ci = 0; ci < (long long)nchunks; ci++) {
            u64 a = idx0 + (u64)ci * CH, e = std::min(idx0 + count, a + CH);
            for (u64 i = a; i < e; i++) {
                u64 P = i << K;
                if (!nb_layer_pass(tab, K, P)) continue;
                s++;
                nb_dfs_below(tab, K, P, emit);
            }
        }
        surv += s;
    }
    st.top_survivors += surv;
    for (auto &v : loc) { cands.insert(cands.end(), v.begin(), v.end()); }
}

#ifndef NO_CUDA
__constant__ NbChk c_chk[NB_MAX_LAYERS * NB_MAX_CHK];
__constant__ int c_n[NB_MAX_LAYERS];

struct DevTab {
    __device__ __forceinline__ int n(int b) const { return c_n[b]; }
    __device__ __forceinline__ const NbChk &at(int b, int j) const { return c_chk[b * NB_MAX_CHK + j]; }
};
struct DevEmit {
    u64 *out; u32 *cnt; u32 cap;
    __device__ __forceinline__ void operator()(u64 F) { u32 i = atomicAdd(cnt, 1u); if (i < cap) out[i] = F; }
};

__global__ void k_scan(u64 idx0, u64 count, int K, u64 *out, u32 *cnt, u32 cap, unsigned long long *surv) {
    DevTab tab; DevEmit em{out, cnt, cap};
    const u64 stride = (u64)gridDim.x * blockDim.x;
    const u64 end = idx0 + count;
    u32 ns = 0;
    for (u64 i = idx0 + (u64)blockIdx.x * blockDim.x + threadIdx.x; i < end; i += stride) {
        u64 P = i << K;
        if (!nb_layer_pass(tab, K, P)) continue;
        ns++;
        if (*(volatile u32 *)cnt >= cap) break;                 /* буфер переполнен — данных слишком мало, дальше считать бессмысленно */
        nb_dfs_below(tab, K, P, em);
    }
    if (ns) atomicAdd(surv, (unsigned long long)ns);
}

/* Вариант 1: «refill» — каждая линия варпа на каждой итерации делает ОДНУ проверку своего префикса; при провале сразу берёт следующий
 * префикс. Расходимость по числу проверок исчезает (в варианте 0 варп ждёт самую «везучую» линию: ~6-7 проверок вместо ~2.3).
 * Таблица верхнего слоя лежит в shared memory (индекс проверки у линий разный). */
__global__ void __launch_bounds__(256) k_scan_refill(u64 idx0, u64 count, int K, u64 *out, u32 *cnt, u32 cap, unsigned long long *surv) {
    __shared__ NbChk sh[NB_MAX_CHK];
    const int n = c_n[K];
    for (int t = threadIdx.x; t < n; t += blockDim.x) sh[t] = c_chk[K * NB_MAX_CHK + t];
    __syncthreads();
    DevTab tab; DevEmit em{out, cnt, cap};
    const u64 stride = (u64)gridDim.x * blockDim.x;
    const u64 end = idx0 + count;
    u64 idx = idx0 + (u64)blockIdx.x * blockDim.x + threadIdx.x;
    int j = 0;
    u32 ns = 0;
    if (n == 0) { /* нет проверок: каждый префикс — выживший */
        for (; idx < end; idx += stride) { ns++; if (*(volatile u32 *)cnt >= cap) break; nb_dfs_below(tab, K, idx << K, em); }
    } else {
        while (idx < end) {
            const u64 P = idx << K;
            const bool pass = nb_chk_pass(P, sh[j]);
            j++;
            if (pass && j == n) {
                ns++;
                if (*(volatile u32 *)cnt >= cap) break;
                nb_dfs_below(tab, K, P, em);
            }
            const bool adv = !pass || j == n;
            j = adv ? 0 : j;
            idx += adv ? stride : 0;
        }
    }
    if (ns) atomicAdd(surv, (unsigned long long)ns);
}

/* Вариант 2: как refill, но верхний слой проверяется 32-битным необходимым фильтром (NbChk32: одна 128-битная загрузка, меньше команд) */
__constant__ NbChk32 c_chk32[NB_MAX_CHK];
__global__ void __launch_bounds__(256) k_scan_refill32(u64 idx0, u64 count, int K, u64 *out, u32 *cnt, u32 cap, unsigned long long *surv) {
    __shared__ uint4 sh[NB_MAX_CHK];
    const int n = c_n[K];
    for (int t = threadIdx.x; t < n; t += blockDim.x) { NbChk32 c = c_chk32[t]; sh[t] = make_uint4(c.hl, c.hh, c.c1, c.dl); }
    __syncthreads();
    DevTab tab; DevEmit em{out, cnt, cap};
    const u64 stride = (u64)gridDim.x * blockDim.x;
    const u64 end = idx0 + count;
    u64 idx = idx0 + (u64)blockIdx.x * blockDim.x + threadIdx.x;
    int j = 0;
    u32 ns = 0;
    if (n == 0) {
        for (; idx < end; idx += stride) { ns++; if (*(volatile u32 *)cnt >= cap) break; nb_dfs_below(tab, K, idx << K, em); }
    } else {
        while (idx < end) {
            const u64 P = idx << K;
            const uint4 c = sh[j];
            const u32 y = nb_yh32((u32)P ^ c.x, (u32)(P >> 32) ^ c.y);
            const bool pass = (u32)(y + c.z) <= c.w;
            j++;
            if (pass && j == n) {
                ns++;
                if (*(volatile u32 *)cnt >= cap) break;
                nb_dfs_below(tab, K, P, em);
            }
            const bool adv = !pass || j == n;
            j = adv ? 0 : j;
            idx += adv ? stride : 0;
        }
    }
    if (ns) atomicAdd(surv, (unsigned long long)ns);
}

/* Этап A на GPU: по одному потоку на кандидата F (16 бит перебора e2 на обращение nextLong — тысячи простых операций на кандидата) */
#define NB_MAX_SEC 256
__constant__ NbChk c_sec[NB_MAX_SEC];
__constant__ int c_nsec;
__constant__ u64 c_prim_hash, c_sec_hash;
__global__ void k_stageA(const u64 *F, u64 n, u64 *outR, u32 *cntR, u32 capR, unsigned long long *nrev) {
    const u64 stride = (u64)gridDim.x * blockDim.x;
    unsigned long long loc_rev = 0;
    for (u64 i = (u64)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        u64 s0s[NB_REV_MAX];
        int m = nb_reverse_nextlong48(F[i] & NB_MASK48, s0s);
        loc_rev += m;
        for (int a = 0; a < m; a++) {
            u64 R48 = ((s0s[a] ^ NB_MUL) ^ c_prim_hash) & NB_MASK48;
            u64 Fsec = nb_nextlong_state(nb_scramble(c_sec_hash ^ R48)) & NB_MASK48;
            bool ok = true;
            for (int j = 0; j < c_nsec && ok; j++) ok = nb_chk_pass(Fsec, c_sec[j]);
            if (ok) { u32 k = atomicAdd(cntR, 1u); if (k < capR) outR[k] = R48; }
        }
    }
    if (loc_rev) atomicAdd(nrev, loc_rev);
}

/* Вариант 3: refill32 + DFS вынесен в noinline-функцию (горячий цикл без локальных стеков) + префикс ведётся как (lo,hi) 32-битные половины */
__device__ __noinline__ void nb_survivor(int K, u64 P, u64 *out, u32 *cnt, u32 cap) {
    DevTab tab; DevEmit em{out, cnt, cap};
    nb_dfs_below(tab, K, P, em);
}
__global__ void __launch_bounds__(256) k_scan_v3(u64 idx0, u64 count, int K, u64 *out, u32 *cnt, u32 cap, unsigned long long *surv) {
    __shared__ uint4 sh[NB_MAX_CHK];
    const int n = c_n[K];
    for (int t = threadIdx.x; t < n; t += blockDim.x) { NbChk32 c = c_chk32[t]; sh[t] = make_uint4(c.hl, c.hh, c.c1, c.dl); }
    __syncthreads();
    const u64 stride = (u64)gridDim.x * blockDim.x;
    const u64 end = idx0 + count;
    u64 idx = idx0 + (u64)blockIdx.x * blockDim.x + threadIdx.x;
    u64 P = idx << K;
    const u64 strideP = stride << K, endP = end << K;
    int j = 0;
    u32 ns = 0;
    if (n == 0) {
        for (; P < endP; P += strideP) { ns++; if (*(volatile u32 *)cnt >= cap) break; nb_survivor(K, P, out, cnt, cap); }
    } else {
        while (P < endP) {
            const uint4 c = sh[j];
            const u32 y = nb_yh32((u32)P ^ c.x, (u32)(P >> 32) ^ c.y);
            const bool pass = (u32)(y + c.z) <= c.w;
            j++;
            if (pass && j == n) {
                ns++;
                if (*(volatile u32 *)cnt >= cap) break;
                nb_survivor(K, P, out, cnt, cap);
            }
            const bool adv = !pass || j == n;
            j = adv ? 0 : j;
            P += adv ? strideP : 0ULL;
        }
    }
    if (ns) atomicAdd(surv, (unsigned long long)ns);
}

/* Вариант 4 (по умолчанию): Gray-обход групп по 32 префикса. NB_DENSE самых селективных проверок верхнего слоя считаются для всех 32 префиксов
 * группы ОДНИМ сложением с константой на шаг (см. NbGray в nb_core.h), остальные — только для выживших (обычно <5% префиксов). */
#define NB_DENSE_MAX 10
__constant__ u32 c_g_hl[NB_DENSE_MAX], c_g_hh[NB_DENSE_MAX], c_g_c1[NB_DENSE_MAX], c_g_dl[NB_DENSE_MAX];
__constant__ u32 c_g_delta[NB_DENSE_MAX][31];
template <int NB_DENSE>
__global__ void __launch_bounds__(256) k_scan_gray(u64 idx0, u64 count, int K, u64 *out, u32 *cnt, u32 cap, unsigned long long *surv) {
    __shared__ uint4 sh[NB_MAX_CHK];
    const int n = c_n[K];
    for (int t = threadIdx.x; t < n; t += blockDim.x) { NbChk32 c = c_chk32[t]; sh[t] = make_uint4(c.hl, c.hh, c.c1, c.dl); }
    __syncthreads();
    const u64 end = idx0 + count;
    const u64 gfirst = idx0 >> 5, glast = (end - 1) >> 5;
    const u64 gstride = (u64)gridDim.x * blockDim.x;
    u32 ns = 0;
    for (u64 G = gfirst + (u64)blockIdx.x * blockDim.x + threadIdx.x; G <= glast; G += gstride) {
        const u64 Pg = (G << 5) << K;
        const u32 pl = (u32)Pg, ph = (u32)(Pg >> 32);
        u32 v[NB_DENSE];
#pragma unroll
        for (int c = 0; c < NB_DENSE; c++) v[c] = nb_yh32(pl ^ c_g_hl[c], ph ^ c_g_hh[c]) + c_g_c1[c];
        u32 mask = 0;
#pragma unroll
        for (int s = 0; s < 32; s++) {
            bool ok = v[0] <= c_g_dl[0];
#pragma unroll
            for (int c = 1; c < NB_DENSE; c++) ok &= (v[c] <= c_g_dl[c]);
            mask |= (u32)ok << s;
            if (s < 31) {
#pragma unroll
                for (int c = 0; c < NB_DENSE; c++) v[c] += c_g_delta[c][s];
            }
        }
        if (((G << 5) < idx0) || (((G << 5) + 32) > end)) {       /* граничная группа: оставляем только idx в [idx0, end) */
            u32 vm = 0;
            for (int s = 0; s < 32; s++) { u64 idx = (G << 5) + (u32)(s ^ (s >> 1)); if (idx >= idx0 && idx < end) vm |= 1u << s; }
            mask &= vm;
        }
        while (mask) {
            const int s = __ffs(mask) - 1;
            mask &= mask - 1;
            const u64 P = (((G << 5) + (u32)(s ^ (s >> 1)))) << K;
            const u32 pl2 = (u32)P, ph2 = (u32)(P >> 32);
            bool pass = true;
            for (int j = NB_DENSE; j < n; j++) {
                const uint4 c = sh[j];
                const u32 y = nb_yh32(pl2 ^ c.x, ph2 ^ c.y);
                if (!((u32)(y + c.z) <= c.w)) { pass = false; break; }
            }
            if (pass) {
                ns++;
                if (*(volatile u32 *)cnt >= cap) { mask = 0; G = glast; break; }
                nb_survivor(K, P, out, cnt, cap);
            }
        }
    }
    if (ns) atomicAdd(surv, (unsigned long long)ns);
}

static void launch_scan(int kid, int dense, int blocks, int tpb, u64 idx0, u64 count, int K, u64 *out, u32 *cnt, u32 cap, unsigned long long *surv) {
    switch (kid) {
        case 0: k_scan<<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
        case 1: k_scan_refill<<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
        case 2: k_scan_refill32<<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
        case 3: k_scan_v3<<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
        case 4: switch (dense) {
            case 2: k_scan_gray<2><<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
            case 4: k_scan_gray<4><<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
            case 6: k_scan_gray<6><<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
            case 8: k_scan_gray<8><<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
            default: k_scan_gray<10><<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
        } break;
        default: k_scan_gray<8><<<blocks, tpb>>>(idx0, count, K, out, cnt, cap, surv); break;
    }
}
#endif

/* ============================================================================================================
 *  Обработка кандидатов F (хост): F -> R -> вторая поверхность -> structure seed -> полная проверка
 * ============================================================================================================ */
struct Ctx {
    std::vector<Blk> all, sec;
    u64 prim_hash, sec_hash;
};
struct PostStats { unsigned long long n_cand = 0, n_rev = 0, n_cross = 0, n_seed_rev = 0, n_float_fail = 0; };

/* Этап B (хост): R mod 2^48 (уже прошедшие вторую поверхность) -> structure seed -> полная проверка всех наблюдений */
static void finish_from_R(const std::vector<u64> &Rs, const Ctx &cx, int threads, std::vector<u64> &seeds, PostStats &ps) {
    std::vector<std::vector<u64>> loc(std::max(threads, 1));
    unsigned long long n_seed_rev = 0, n_ff = 0;
#pragma omp parallel for num_threads(threads) schedule(dynamic, 16) reduction(+ : n_seed_rev, n_ff)
    for (long long i = 0; i < (long long)Rs.size(); i++) {
        u64 t0[NB_REV_MAX];
        int m = nb_reverse_nextlong48(Rs[i], t0);
        for (int k = 0; k < m; k++) {
            u64 S = (t0[k] ^ NB_MUL) & NB_MASK48;
            n_seed_rev++;
            if (seed_consistent(S, cx.all)) loc[omp_get_thread_num()].push_back(S);
            else n_ff++;
        }
    }
    ps.n_seed_rev += n_seed_rev; ps.n_float_fail += n_ff;
    for (auto &v : loc) seeds.insert(seeds.end(), v.begin(), v.end());
}

/* Этап A на CPU: F основной поверхности -> R -> проверка второй поверхности; выживших R отдаёт в Rs */
static void stageA_cpu(const std::vector<u64> &Fs, const Ctx &cx, int threads, std::vector<u64> &Rs, PostStats &ps) {
    std::vector<std::vector<u64>> loc(std::max(threads, 1));
    unsigned long long n_rev = 0;
#pragma omp parallel for num_threads(threads) schedule(dynamic, 64) reduction(+ : n_rev)
    for (long long i = 0; i < (long long)Fs.size(); i++) {
        u64 s0s[NB_REV_MAX];
        int n = nb_reverse_nextlong48(Fs[i] & NB_MASK48, s0s);
        for (int a = 0; a < n; a++) {
            n_rev++;
            u64 R48 = ((s0s[a] ^ NB_MUL) ^ cx.prim_hash) & NB_MASK48;       /* R mod 2^48 */
            u64 Fsec = nb_nextlong_state(nb_scramble(cx.sec_hash ^ R48)) & NB_MASK48;
            bool ok = true;
            for (const Blk &b : cx.sec) if (!nb_exact_in(Fsec, b.h, b.lo, b.len)) { ok = false; break; }
            if (ok) loc[omp_get_thread_num()].push_back(R48);
        }
    }
    ps.n_cand += Fs.size(); ps.n_rev += n_rev;
    for (auto &v : loc) { ps.n_cross += v.size(); Rs.insert(Rs.end(), v.begin(), v.end()); }
}
static void process_candidates(const std::vector<u64> &Fs, const Ctx &cx, int threads, std::vector<u64> &seeds, PostStats &ps) {
    std::vector<u64> Rs;
    stageA_cpu(Fs, cx, threads, Rs, ps);
    finish_from_R(Rs, cx, threads, seeds, ps);
}

/* ============================================================================================================
 *  Самопроверка и генерация синтетики
 * ============================================================================================================ */
struct SplitMix { u64 s; explicit SplitMix(u64 x) : s(x) {}
    u64 next() { u64 z = (s += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
    int range(int n) { return (int)(next() % (u64)n); } };

static int java_hash(const char *s) { u32 h = 0; for (; *s; s++) h = h * 31u + (u32)(unsigned char)*s; return (int)h; }

static int selftest(const std::vector<std::string> &vec_files, bool verbose) {
    int fails = 0;
    auto check = [&](bool ok, const char *what) { if (!ok) { fails++; printf("  FAIL: %s\n", what); } else if (verbose) printf("  ok:   %s\n", what); };
    printf("[selftest] константы\n");
    check(((NB_MUL * NB_MINV) & NB_MASK48) == 1, "M * M^-1 == 1 (mod 2^48)");
    check((u64)(i64)java_hash("minecraft:bedrock_roof") == NB_ROOF_HASH, "hashCode(\"minecraft:bedrock_roof\") == 343340730");
    check((u64)(i64)java_hash("minecraft:bedrock_floor") == NB_FLOOR_HASH, "hashCode(\"minecraft:bedrock_floor\") == 2042456806");
    /* пороги */
    {
        const u64 expT[4] = {13421773ULL, 10066330ULL, 6710887ULL, 3355444ULL};     /* ceil(p*2^24), p = .8 .6 .4 .2 */
        bool ok = true;
        for (int i = 0; i < 4; i++) {
            ok &= nb_thr(nb_prob(1 + i, 0)) == expT[i];
            ok &= nb_thr(nb_prob(123 + i, 1)) == expT[i];
        }
        check(ok, "пороги T(y) = ceil(p(y)*2^24) для пола y=1..4 и потолка y=123..126");
        double pf = nb_prob(1, 0), pr = nb_prob(123, 1);
        printf("  p(пол y=1)=%.17g  p(потолок y=123)=%.17g\n", pf, pr);
    }
    /* векторы игры */
    for (const std::string &fn : vec_files) {
        FILE *f = fopen(fn.c_str(), "r");
        if (!f) { printf("  FAIL: нет файла %s\n", fn.c_str()); fails++; continue; }
        char line[512]; int nv = 0, bad = 0, hashes = 0;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "bedrock ", 8)) {
                long long seed; char name[128]; int x, y, z; unsigned bits;
                if (sscanf(line, "bedrock %lld %127s %d %d %d %u", &seed, name, &x, &y, &z, &bits) != 6) { bad++; continue; }
                u64 R = nb_base_R((u64)seed);
                u64 F = nb_factory_F(R, (u64)(i64)java_hash(name));
                float fl = nb_bedrock_float(F, x, y, z);
                u32 fb; memcpy(&fb, &fl, 4);
                if (fb != bits) bad++;
                /* интервальная эквивалентность: state1 >> 24 == k;  k/2^24 == float */
                nv++;
            } else if (!strncmp(line, "floats ", 7)) {     /* формат NbRef (реальный RandomState): floats seed name x y z bits; имя без "minecraft:" */
                long long seed; char name[128]; int x, y, z; unsigned bits;
                if (sscanf(line, "floats %lld %127s %d %d %d %u", &seed, name, &x, &y, &z, &bits) != 6) { bad++; continue; }
                std::string full = std::string("minecraft:") + name;
                u64 R = nb_base_R((u64)seed);
                u64 F = nb_factory_F(R, (u64)(i64)java_hash(full.c_str()));
                float fl = nb_bedrock_float(F, x, y, z);
                u32 fb; memcpy(&fb, &fl, 4);
                if (fb != bits) bad++;
                nv++;
            } else if (!strncmp(line, "hash ", 5)) {
                long long a, b;
                if (sscanf(line, "hash %lld %lld", &a, &b) == 2) { hashes++; if ((u64)a != NB_ROOF_HASH || (u64)b != NB_FLOOR_HASH) bad++; }
            }
        }
        fclose(f);
        char msg[300]; snprintf(msg, sizeof msg, "векторы игры %s: %d значений nextFloat бедрока (побитово), %d hash-строк, расхождений %d", fn.c_str(), nv, hashes, bad);
        check(nv > 0 && bad == 0, msg);
    }
    /* обращение nextLong */
    {
        SplitMix rg(12345);
        int bad = 0, tot = 0, maxn = 0;
        for (int i = 0; i < 20000; i++) {
            u64 s0 = rg.next() & NB_MASK48;
            u64 T = nb_nextlong_state(s0) & NB_MASK48;
            u64 out[NB_REV_MAX]; int n = nb_reverse_nextlong48(T, out);
            bool found = false; for (int k = 0; k < n; k++) { found |= out[k] == s0; if ((nb_nextlong_state(out[k]) & NB_MASK48) != T) bad++; }
            if (!found) bad++;
            tot += n; maxn = std::max(maxn, n);
        }
        char msg[200]; snprintf(msg, sizeof msg, "обращение nextLong: 20000 случайных состояний, среднее число решений %.3f, максимум %d, ошибок %d", tot / 20000.0, maxn, bad);
        check(bad == 0, msg);
    }
    /* интервалы и слои на случайных данных */
    {
        SplitMix rg(777);
        int bad = 0, bad_layer = 0, tot = 0;
        for (int i = 0; i < 200000; i++) {
            u64 S = rg.next() & NB_MASK48;
            u64 R = nb_base_R(S);
            bool paper = (i & 1) != 0;
            u64 F[2] = {nb_factory_F(R, NB_FLOOR_HASH), nb_factory_F(R, NB_ROOF_HASH)};
            int surf = rg.range(2);
            int y = surf == 0 ? 1 + rg.range(4) : 123 + rg.range(4);
            i32 x = (i32)(rg.next() % 60000001ULL) - 30000000, z = (i32)(rg.next() % 60000001ULL) - 30000000;   /* весь диапазон координат мира */
            int hy = paper ? (surf ? 122 : 0) : y;
            float fl = nb_bedrock_float(F[surf], x, hy, z);
            double p = nb_prob(y, surf);
            bool lt = (double)fl < p;
            bool bed = surf == 0 ? lt : !lt;
            Obs o{x, y, z, bed};
            Blk b = make_blk(o, paper);
            tot++;
            if (!nb_exact_in(F[surf] & NB_MASK48, b.h, b.lo, b.len)) bad++;
            /* слои: настоящее F с обнулёнными младшими b битами обязано проходить все слои */
            for (int bb = 0; bb <= 13; bb++) {
                std::vector<Blk> one{b};
                std::vector<NbChk> lay; build_layer(one, bb, lay, nullptr);
                u64 P = F[surf] & NB_MASK48 & ~((1ULL << bb) - 1);
                for (const NbChk &c : lay) if (!nb_chk_pass(P, c)) bad_layer++;
            }
        }
        char msg[200];
        snprintf(msg, sizeof msg, "интервал == float-сравнение на %d случайных блоках (оба режима y), ошибок %d", tot, bad);
        check(bad == 0, msg);
        snprintf(msg, sizeof msg, "слои b=0..13: истинное F (с обнулёнными b младшими битами) проходит все проверки, ошибок %d", bad_layer);
        check(bad_layer == 0, msg);
    }
    /* слой 0 обязан совпадать с точной проверкой на ПРОИЗВОЛЬНЫХ F (в т.ч. ложноположительных) */
    {
        SplitMix rg(4242);
        int bad = 0, tests = 0;
        for (int it = 0; it < 300000; it++) {
            int surf = rg.range(2);
            int y = surf == 0 ? 1 + rg.range(4) : 123 + rg.range(4);
            Obs o{(i32)rg.range(100000) - 50000, y, (i32)rg.range(100000) - 50000, rg.range(2) == 1};
            Blk b = make_blk(o, false);
            std::vector<Blk> one{b}; std::vector<NbChk> lay; build_layer(one, 0, lay, nullptr);
            u64 F = rg.next() & NB_MASK48;
            bool pass = true; for (const NbChk &c : lay) pass &= nb_chk_pass(F, c);
            tests++;
            if (pass != nb_exact_in(F, b.h, b.lo, b.len)) bad++;
        }
        char msg[200]; snprintf(msg, sizeof msg, "слой 0 == точная проверка на %d случайных F (нет ни потерь, ни ложных проходов), ошибок %d", tests, bad);
        check(bad == 0, msg);
    }
    /* 32-битный фильтр верхнего слоя: нет ложноотрицательных относительно точной проверки слоя */
    {
        SplitMix rg(31337);
        long long tests = 0, fn = 0, fp = 0, passes = 0;
        for (int it = 0; it < 3000000; it++) {
            int surf = rg.range(2);
            int y = surf == 0 ? 1 + rg.range(4) : 123 + rg.range(4);
            Obs o{(i32)rg.range(100000) - 50000, y, (i32)rg.range(100000) - 50000, rg.range(2) == 1};
            Blk b = make_blk(o, false);
            int bb = 8 + rg.range(6);
            std::vector<Blk> one{b}; std::vector<NbChk> lay; build_layer(one, bb, lay, nullptr);
            if (lay.empty()) continue;
            u64 P = (rg.next() & NB_MASK48) & ~((1ULL << bb) - 1);
            /* подмешиваем «почти проходящие» P: берём F, удовлетворяющее точно, и обнуляем младшие биты */
            if (it & 1) { u64 Fx = rg.next() & NB_MASK48; for (int k = 0; k < 64 && !nb_exact_in(Fx, b.h, b.lo, b.len); k++) Fx = rg.next() & NB_MASK48; P = Fx & ~((1ULL << bb) - 1); }
            NbChk32 c32 = nb_chk32_of(lay[0]);
            bool ex = nb_chk_pass(P, lay[0]);
            bool f32 = nb_chk32_pass((u32)P, (u32)(P >> 32), c32);
            tests++; passes += ex;
            if (ex && !f32) fn++;
            if (!ex && f32) fp++;
        }
        char msg[320]; snprintf(msg, sizeof msg, "32-битный фильтр: %lld проверок (%lld проходов точной), ложноотрицательных %lld, ложноположительных %lld", tests, passes, fn, fp);
        check(fn == 0, msg);
    }
    /* интервальная арифметика «дуги»: проверка слоя b на полном переборе w для малых b */
    {
        SplitMix rg(99);
        int bad = 0, tests = 0;
        for (int it = 0; it < 2000; it++) {
            int surf = rg.range(2);
            int y = surf == 0 ? 1 + rg.range(4) : 123 + rg.range(4);
            Obs o{(i32)rg.range(100000) - 50000, y, (i32)rg.range(100000) - 50000, rg.range(2) == 1};
            Blk b = make_blk(o, false);
            int bb = 1 + rg.range(12);
            std::vector<Blk> one{b}; std::vector<NbChk> lay; build_layer(one, bb, lay, nullptr);
            u64 P = (rg.next() & NB_MASK48) & ~((1ULL << bb) - 1);
            /* если какое-то F с этим префиксом удовлетворяет наблюдению точно, слой обязан пройти */
            bool any = false;
            for (u64 lowv = 0; lowv < (1ULL << bb) && !any; lowv++) any = nb_exact_in(P | lowv, b.h, b.lo, b.len);
            bool pass = true; for (const NbChk &c : lay) pass &= nb_chk_pass(P, c);
            tests++;
            if (any && !pass) bad++;
        }
        char msg[200]; snprintf(msg, sizeof msg, "слой b — необходимое условие (нет потерянных решений) на %d префиксах, ошибок %d", tests, bad);
        check(bad == 0, msg);
    }
    printf("[selftest] %s (%d провалов)\n", fails ? "ПРОВАЛ" : "OK", fails);
    return fails;
}

static bool nb_value_visible_bedrock(u64 S, i32 x, i32 y, i32 z, bool paper) {       /* одна ячейка по формуле игры */
    int surf; y_valid(y, &surf);
    Obs o{x, y, z, false}; Blk b = make_blk(o, paper);
    u64 R = nb_base_R(S);
    float f = nb_bedrock_float(nb_factory_F(R, surf == 0 ? NB_FLOOR_HASH : NB_ROOF_HASH), x, b.hy, z);
    bool lt = (double)f < b.p;
    return surf == 0 ? lt : !lt;
}

/* генерация синтетических наблюдений: mode 0 — только блоки-бедрок (как собирает игрок), 1 — случайные позиции (bedrock и other) */
static void gen_obs(u64 S, int n, int mode, int area, int yset, double roof_frac, u64 rng_seed, bool paper, FILE *out) {
    SplitMix rg(rng_seed ^ (S * 0x9E3779B97F4A7C15ULL));
    std::map<std::tuple<int,int,int>, int> used;
    int guard = 0;
    while ((int)used.size() < n && guard++ < 100000000) {
        bool roof = (rg.next() >> 11) * (1.0 / 9007199254740992.0) < roof_frac;
        int y;
        if (yset == 0) y = roof ? 123 : 4;                       /* самые информативные слои */
        else if (yset == 1) y = roof ? 123 + rg.range(4) : 1 + rg.range(4);
        else y = roof ? 123 + rg.range(2) * 3 : 1 + rg.range(2) * 3;
        int x = rg.range(2 * area + 1) - area, z = rg.range(2 * area + 1) - area;
        auto key = std::make_tuple(x, y, z);
        if (used.count(key)) continue;
        bool bed = nb_value_visible_bedrock(S, x, y, z, paper);
        if (mode == 0 && !bed) continue;
        used[key] = bed;
        fprintf(out, "%d %d %d %s\n", x, y, z, bed ? "Bedrock" : "Other");
    }
}

/* ============================================================================================================
 *  main
 * ============================================================================================================ */
static void usage() {
    printf(
"crack-nether-bedrock — восстановление 48-битного structure seed по бедроку потолка/пола Незера (26.1/26.2/26.3)\n\n"
"Использование: crack-nether-bedrock [опции] <файл_наблюдений | ->\n"
"  Формат файла: строки 'x y z bedrock|other' (регистр не важен, без типа = bedrock), '#' — комментарий.\n"
"  Значимы y=1..4 (пол) и y=123..126 (потолок); y=0/127 всегда бедрок, y=5..122 — не бедрок: эти строки пропускаются.\n\n"
"Опции:\n"
"  --version V          26.1 | 26.2 | 26.3 (по умолчанию 26.3; арифметика бедрока одинакова — проверка и документация docs/23)\n"
"  --paper1_18          режим Paper < 1.19.2-213 (в позиционный хэш подставляется y=0 (пол) / y=122 (потолок))\n"
"  --cpu                считать на CPU (OpenMP) вместо GPU;  --threads N — число потоков CPU (по умолчанию все)\n"
"  --device N           номер CUDA-устройства (по умолчанию 0)\n"
"  --top-bits K         число неизвестных младших бит на верхнем слое (6..13, по умолчанию 12; 2^(48-K) префиксов)\n"
"  --prefix-start A --prefix-count N   ограничить диапазон индексов префиксов (для тестов/разбиения работы)\n"
"  --first              остановиться после первого подтверждённого seed (проверка после каждого пакета)\n"
"  --bench R            замер ядра: R повторов сканирования диапазона --prefix-count (по умолчанию 2^26), вывод min/медиана и оценка на весь 2^(48-K)\n"
"                       (машина общая: min по многим коротким запускам ближе всего к времени без конкуренции)\n"
"  --kernel N           вариант ядра GPU (0 — простой, 1 — refill, 2 — refill + 32-битный фильтр, 3 — то же + noinline DFS, 4 — Gray-обход групп по 32; по умолчанию 4; см. docs/23)\n"
"  --dense D            число «плотных» проверок ядра 4 (2,4,6,8,10; по умолчанию — наибольшее, не превышающее число проверок верхнего слоя; замеры в docs/23 §5)\n"
"  --force              не отказываться при заведомо недостаточных данных (ожидаемо больше кандидатов, чем буфер)\n"
"  --max-cand N         ёмкость буфера кандидатов на GPU (по умолчанию 2^24)\n"
"  --world-seed         печатать оценку «случайного» world seed (верна ТОЛЬКО если seed выдан игрой: RandomSource.create().nextLong();\n"
"                       для seed, заданных вручную, старшие 16 бит нужно искать другим способом — hashed seed/биомы, см. crack-lift64)\n"
"  --out FILE           записать найденные structure seed в файл (по одному в строке)\n"
"  --check-seed S       не искать: проверить файл наблюдений на согласованность с structure seed S и выйти\n"
"  --filter-seeds FILE  не искать: отфильтровать список seed (по одному в строке, десятичные, знаковые или беззнаковые) по наблюдениям; значим младший 48-бит\n"
"  --gen S N [режим]    сгенерировать N синтетических наблюдений из seed S (stdout). режим: bedrock (только бедрок, по умолчанию) | mixed\n"
"                       доп.: --gen-area R (по умолчанию 40), --gen-y 0|1|2 (0: y=4/123; 1: все y; 2: y=1,4/123,126), --gen-roof F (доля потолка, 0.5), --gen-rng X\n"
"  --selftest [файлы gameref-*.txt]   самопроверка формул (побитово с векторами реальной игры)\n"
"  --quiet / --verbose\n"
"Вывод: на stdout — по одному structure seed (десятичное, 48 бит) в строке; строки, начинающиеся с '#', — пояснения.\n"
"Код возврата: 0 — поиск завершён (число найденных см. в выводе), 2 — ошибка входа, 3 — ошибка CUDA, 4 — данных недостаточно (переполнение/отказ).\n");
}

int main(int argc, char **argv) {
    std::string version = "26.3", in_path, out_path;
    int kernel_id = 4; int dense_override = 0; int bench_reps = 0; bool force = false, paper = false, use_cpu = false, want_ws = false, quiet = false, verbose = false, first = false, do_selftest = false;
    int threads = omp_get_max_threads(), device = 0, K = 12;
    u64 pfx_start = 0, pfx_count = ~0ULL, max_cand = 1ULL << 24;
    bool have_check_seed = false; u64 check_seed = 0; std::string filter_path;
    bool do_gen = false; u64 gen_seed = 0; int gen_n = 0, gen_mode = 0, gen_area = 40, gen_y = 0; double gen_roof = 0.5; u64 gen_rng = 1;
    std::vector<std::string> selftest_files;
    int blocks_override = 0, tpb = 256;
    u64 batch_idx = 1ULL << 30;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](int n) { if (i + n >= argc) { fprintf(stderr, "опция %s требует %d арг.\n", a.c_str(), n); exit(2); } };
        auto u = [&](const char *s) { unsigned long long v; char *e; errno = 0; v = strtoull(s, &e, 0); if (*s == 0 || *e || errno) { fprintf(stderr, "не число: %s\n", s); exit(2); } return (u64)v; };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--version") { need(1); version = argv[++i]; }
        else if (a == "--paper1_18") paper = true;
        else if (a == "--cpu") use_cpu = true;
        else if (a == "--threads") { need(1); threads = (int)u(argv[++i]); }
        else if (a == "--device") { need(1); device = (int)u(argv[++i]); }
        else if (a == "--top-bits") { need(1); K = (int)u(argv[++i]); }
        else if (a == "--prefix-start") { need(1); pfx_start = u(argv[++i]); }
        else if (a == "--prefix-count") { need(1); pfx_count = u(argv[++i]); }
        else if (a == "--first") first = true;
        else if (a == "--force") force = true;
        else if (a == "--dense") { need(1); dense_override = (int)u(argv[++i]); }
        else if (a == "--kernel") { need(1); kernel_id = (int)u(argv[++i]); }
        else if (a == "--bench") { need(1); bench_reps = (int)u(argv[++i]); }
        else if (a == "--max-cand") { need(1); max_cand = u(argv[++i]); }
        else if (a == "--world-seed") want_ws = true;
        else if (a == "--out") { need(1); out_path = argv[++i]; }
        else if (a == "--check-seed") { need(1); have_check_seed = true; check_seed = u(argv[++i]) & NB_MASK48; }
        else if (a == "--filter-seeds") { need(1); filter_path = argv[++i]; }
        else if (a == "--gen") { need(2); do_gen = true; gen_seed = u(argv[++i]); gen_n = (int)u(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') { std::string m = argv[++i]; gen_mode = m == "mixed" ? 1 : 0; } }
        else if (a == "--gen-area") { need(1); gen_area = (int)u(argv[++i]); }
        else if (a == "--gen-y") { need(1); gen_y = (int)u(argv[++i]); }
        else if (a == "--gen-roof") { need(1); gen_roof = atof(argv[++i]); }
        else if (a == "--gen-rng") { need(1); gen_rng = u(argv[++i]); }
        else if (a == "--selftest") { do_selftest = true; while (i + 1 < argc && argv[i + 1][0] != '-') selftest_files.push_back(argv[++i]); }
        else if (a == "--blocks") { need(1); blocks_override = (int)u(argv[++i]); }
        else if (a == "--tpb") { need(1); tpb = (int)u(argv[++i]); }
        else if (a == "--batch-log2") { need(1); batch_idx = 1ULL << u(argv[++i]); }
        else if (a == "--quiet") quiet = true;
        else if (a == "--verbose") verbose = true;
        else if (a.size() > 0 && a[0] == '-' && a != "-") { fprintf(stderr, "неизвестная опция %s (см. --help)\n", a.c_str()); return 2; }
        else in_path = a;
    }
    if (version != "26.1" && version != "26.2" && version != "26.3") {
        fprintf(stderr, "версия %s не поддерживается (26.1, 26.2, 26.3; арифметика бедрока у них побитово одинакова)\n", version.c_str()); return 2;
    }
    if (K < 6 || K > 13) { fprintf(stderr, "--top-bits должен быть в 6..13\n"); return 2; }
    if (threads < 1) threads = 1;
#ifdef NO_CUDA
    (void)device; (void)blocks_override; (void)tpb; (void)kernel_id; (void)dense_override; (void)use_cpu; (void)batch_idx;
#endif

    if (do_selftest) return selftest(selftest_files, verbose) ? 1 : 0;
    if (do_gen) { gen_obs(gen_seed, gen_n, gen_mode, gen_area, gen_y, gen_roof, gen_rng, paper, stdout); return 0; }
    if (in_path.empty()) { usage(); return 2; }

    /* ---- вход ---- */
    std::vector<Obs> obs; ParseStats pst;
    if (!parse_obs(in_path.c_str(), obs, pst, quiet)) return 2;
    std::vector<Blk> all;
    for (const Obs &o : obs) all.push_back(make_blk(o, paper));
    if (all.empty()) { fprintf(stderr, "нет пригодных наблюдений\n"); return 2; }

    if (have_check_seed) {
        int bad = 0; bool ok = seed_consistent(check_seed, all, &bad);
        printf("# structure seed %llu: %s (%d из %zu наблюдений не совпало)\n", (unsigned long long)check_seed, ok ? "СОГЛАСОВАН" : "НЕ согласован", bad, all.size());
        return ok ? 0 : 1;
    }

    if (!filter_path.empty()) {
        FILE *lf = fopen(filter_path.c_str(), "r");
        if (!lf) { fprintf(stderr, "не могу открыть %s\n", filter_path.c_str()); return 2; }
        std::vector<u64> ws; char ln[128];
        while (fgets(ln, sizeof ln, lf)) { char *e; errno = 0; long long v = strtoll(ln, &e, 10); if (e == ln) continue; ws.push_back((u64)v); }
        fclose(lf);
        std::vector<char> okv(ws.size(), 0);
#pragma omp parallel for num_threads(threads) schedule(static)
        for (long long i = 0; i < (long long)ws.size(); i++) okv[i] = seed_consistent(ws[i] & NB_MASK48, all) ? 1 : 0;
        size_t nok = 0;
        for (size_t i = 0; i < ws.size(); i++) if (okv[i]) { printf("%lld\n", (long long)ws[i]); nok++; }
        fprintf(stderr, "# фильтр: из %zu seed подходят %zu (наблюдений %zu)\n", ws.size(), nok, all.size());
        return 0;
    }

    std::vector<Blk> fl, rf;
    for (const Blk &b : all) (b.surf == 0 ? fl : rf).push_back(b);
    double I_fl = info_bits(fl), I_rf = info_bits(rf);
    bool floor_primary = !fl.empty() && (rf.empty() || I_fl > I_rf);            /* основная — более селективная поверхность; при равенстве потолок (как в оригинале) */
    std::vector<Blk> &prim = floor_primary ? fl : rf, &sec = floor_primary ? rf : fl;
    double I_prim = floor_primary ? I_fl : I_rf, I_sec = floor_primary ? I_rf : I_fl;

    Ctx cx; cx.all = all; cx.sec = sec;
    cx.prim_hash = floor_primary ? NB_FLOOR_HASH : NB_ROOF_HASH;
    cx.sec_hash = floor_primary ? NB_ROOF_HASH : NB_FLOOR_HASH;

    Plan pl; build_plan(prim, K, pl);
    const u64 total_idx = 1ULL << (48 - K);
    if (pfx_start > total_idx) pfx_start = total_idx;
    u64 cnt_idx = std::min(pfx_count, total_idx - pfx_start);

    double exp_F = std::pow(2.0, 48.0 - I_prim);
    double exp_false_S = std::pow(2.0, 48.0 - I_prim - I_sec);
    double exp_top = (double)total_idx * pl.survive_frac_top;
    if (!quiet) {
        fprintf(stderr, "# версия %s%s; наблюдений: %zu (строк %d, дубликатов %d, пропущено по y %d, ошибочных %d): пол %zu, потолок %zu\n",
                version.c_str(), paper ? " [Paper<1.19.2-213]" : "", all.size(), pst.lines, pst.dup, pst.skipped_y, pst.bad, fl.size(), rf.size());
        fprintf(stderr, "# информация: пол %.1f бит, потолок %.1f бит, всего %.1f бит (нужно > 48); основная поверхность: %s\n",
                I_fl, I_rf, I_fl + I_rf, floor_primary ? "пол" : "потолок");
        fprintf(stderr, "# ожидаемо: кандидатов F основной поверхности ~%.3g, ложных structure seed ~%.3g, выживших префиксов слоя K=%d: ~%.3g из %llu\n",
                exp_F, exp_false_S, K, exp_top, (unsigned long long)total_idx);
        fprintf(stderr, "# проверок на слой (K..0):");
        for (int b = K; b >= 0; b--) fprintf(stderr, " %d", pl.n[b]);
        fprintf(stderr, "\n");
        if (exp_F > (double)max_cand) fprintf(stderr, "# ВНИМАНИЕ: слишком мало информации в основной поверхности — кандидатов ожидается больше буфера (%llu); результат будет неполным\n", (unsigned long long)max_cand);
        if (I_fl + I_rf < 48.0) fprintf(stderr, "# ВНИМАНИЕ: суммарной информации < 48 бит — однозначный seed не гарантирован (ожидается много ложных)\n");
        if (pl.n[K] == 0) fprintf(stderr, "# ВНИМАНИЕ: на верхнем слое нет ни одной проверки (нужны блоки-бедрок на y=4/123 или другие информативные) — перебор бессмыслен\n");
    }
    if (!force && (exp_F > 2.0 * (double)max_cand || (pl.n[K] == 0 && exp_F > 1e9))) {
        fprintf(stderr, "отказ: информации в основной поверхности недостаточно (ожидаемо ~%.3g кандидатов F, буфер %llu). Добавьте блоки-бедрок на y=4/123\n"
                        "       (и по обеим поверхностям), либо --force / --max-cand.\n", exp_F, (unsigned long long)max_cand);
        return 4;
    }

    /* ---- сканирование ---- */
    std::vector<u64> seeds;       /* найденные structure seed */
    PostStats post; ScanStats sst;
    double t0 = now_s();
    double t_scan = 0, t_post = 0, t_stageA_gpu = 0;
    bool use_gpu = false;
#ifndef NO_CUDA
    int sms = 0; std::string gname;
    if (!use_cpu) {
        int nd = 0;
        if (cudaGetDeviceCount(&nd) == cudaSuccess && nd > device) { CK(cudaSetDevice(device)); use_gpu = true; }
        else { cudaGetLastError(); fprintf(stderr, "# CUDA-устройство недоступно — переключаюсь на CPU\n"); }
    }
    u64 *d_out = nullptr; u32 *d_cnt = nullptr; unsigned long long *d_surv = nullptr;
    u64 *d_R = nullptr; u32 *d_cntR = nullptr; unsigned long long *d_nrev = nullptr;
    const u32 capR = 1u << 22;
    cudaEvent_t ev0, ev1;
    int nblocks = 0;
    int dense_nd = dense_override;
    if (dense_nd == 0) {      /* число «плотных» проверок: замер на 5 наборах (docs/23 §5) — чем больше, тем быстрее до 10 (дальше кэш команд/регистры) */
        const int opts[5] = {2, 4, 6, 8, 10};
        dense_nd = 2;
        for (int oi = 0; oi < 5; oi++) if (opts[oi] <= pl.n[K]) dense_nd = opts[oi];
    }
    if (use_gpu) {
        cudaDeviceProp pr; CK(cudaGetDeviceProperties(&pr, device)); sms = pr.multiProcessorCount; gname = pr.name;
        std::vector<NbChk> hc(NB_MAX_LAYERS * NB_MAX_CHK);
        int hn[NB_MAX_LAYERS] = {0};
        for (int b = 0; b <= K; b++) { hn[b] = pl.n[b]; for (int j = 0; j < pl.n[b]; j++) hc[b * NB_MAX_CHK + j] = pl.chk[b][j]; }
        CK(cudaMemcpyToSymbol(c_chk, hc.data(), sizeof(NbChk) * NB_MAX_LAYERS * NB_MAX_CHK));
        CK(cudaMemcpyToSymbol(c_n, hn, sizeof hn));
        { std::vector<NbChk32> h32(NB_MAX_CHK); for (int j = 0; j < pl.n[K]; j++) h32[j] = nb_chk32_of(pl.chk[K][j]); CK(cudaMemcpyToSymbol(c_chk32, h32.data(), sizeof(NbChk32) * NB_MAX_CHK)); }
        {   /* Gray-таблицы плотного фильтра для первых NB_DENSE проверок верхнего слоя */
            u32 hl[NB_DENSE_MAX], hh[NB_DENSE_MAX], c1[NB_DENSE_MAX], dl[NB_DENSE_MAX], dd[NB_DENSE_MAX][31];
            for (int c = 0; c < NB_DENSE_MAX; c++) {
                NbGray g = c < pl.n[K] ? nb_gray_of(pl.chk[K][c], K) : nb_gray_vacuous();
                hl[c] = g.hl; hh[c] = g.hh; c1[c] = g.c1; dl[c] = g.dl; memcpy(dd[c], g.delta, sizeof g.delta);
            }
            CK(cudaMemcpyToSymbol(c_g_hl, hl, sizeof hl)); CK(cudaMemcpyToSymbol(c_g_hh, hh, sizeof hh)); CK(cudaMemcpyToSymbol(c_g_c1, c1, sizeof c1));
            CK(cudaMemcpyToSymbol(c_g_dl, dl, sizeof dl)); CK(cudaMemcpyToSymbol(c_g_delta, dd, sizeof dd));
        }
        CK(cudaMalloc(&d_out, max_cand * sizeof(u64)));
        CK(cudaMalloc(&d_cnt, sizeof(u32)));
        CK(cudaMalloc(&d_surv, sizeof(unsigned long long)));
        CK(cudaMemset(d_cnt, 0, sizeof(u32))); CK(cudaMemset(d_surv, 0, sizeof(unsigned long long)));
        CK(cudaMalloc(&d_R, (size_t)capR * sizeof(u64))); CK(cudaMalloc(&d_cntR, sizeof(u32))); CK(cudaMalloc(&d_nrev, sizeof(unsigned long long)));
        CK(cudaMemset(d_cntR, 0, sizeof(u32))); CK(cudaMemset(d_nrev, 0, sizeof(unsigned long long)));
        {   /* вторая поверхность: точные проверки (слой 0) в constant memory */
            std::vector<NbChk> sc; build_layer(cx.sec, 0, sc, nullptr);
            if ((int)sc.size() > NB_MAX_SEC) sc.resize(NB_MAX_SEC);
            std::vector<NbChk> hs(NB_MAX_SEC); for (size_t j = 0; j < sc.size(); j++) hs[j] = sc[j];
            int ns = (int)sc.size();
            CK(cudaMemcpyToSymbol(c_sec, hs.data(), sizeof(NbChk) * NB_MAX_SEC)); CK(cudaMemcpyToSymbol(c_nsec, &ns, sizeof ns));
            CK(cudaMemcpyToSymbol(c_prim_hash, &cx.prim_hash, sizeof(u64))); CK(cudaMemcpyToSymbol(c_sec_hash, &cx.sec_hash, sizeof(u64)));
        }
        CK(cudaEventCreate(&ev0)); CK(cudaEventCreate(&ev1));
        nblocks = blocks_override ? blocks_override : sms * (2048 / tpb) * 4;
        if (!quiet) fprintf(stderr, "# GPU: %s, %d SM; сетка %d x %d; пакет %llu префиксов\n", gname.c_str(), sms, nblocks, tpb, (unsigned long long)batch_idx);
    }
#endif
    if (!use_gpu && !quiet) fprintf(stderr, "# CPU: %d потоков (OpenMP)\n", threads);

    if (bench_reps > 0) {
        u64 bc = pfx_count == ~0ULL ? (1ULL << 26) : cnt_idx; if (bc > total_idx - pfx_start) bc = total_idx - pfx_start;
        std::vector<double> ts_ms;
        for (int r = 0; r < bench_reps; r++) {
            double ta = now_s(); float ms = 0;
#ifndef NO_CUDA
            if (use_gpu) {
                CK(cudaMemset(d_cnt, 0, sizeof(u32)));
                CK(cudaEventRecord(ev0));
                launch_scan(kernel_id, dense_nd, nblocks, tpb, pfx_start, bc, K, d_out, d_cnt, (u32)std::min<u64>(max_cand, 0xFFFFFFFFULL), d_surv);
                CK(cudaEventRecord(ev1)); CK(cudaEventSynchronize(ev1)); CK(cudaGetLastError());
                CK(cudaEventElapsedTime(&ms, ev0, ev1));
            } else
#endif
            { std::vector<u64> tmp; ScanStats ss; scan_cpu(pl, pfx_start, bc, threads, tmp, ss); ms = (float)((now_s() - ta) * 1e3); }
            ts_ms.push_back(ms);
        }
        std::sort(ts_ms.begin(), ts_ms.end());
        double mn = ts_ms.front(), med = ts_ms[ts_ms.size() / 2];
        double scale = (double)total_idx / (double)bc;
        printf("bench: %s, %d повторов по %llu префиксов: min %.4f мс, p10 %.4f, медиана %.4f, max %.4f; оценка полного прохода по min: %.3f с (по медиане %.3f с); %.3g префиксов/с\n",
               use_gpu ? "GPU" : "CPU", bench_reps, (unsigned long long)bc, mn, ts_ms[ts_ms.size() / 10], med, ts_ms.back(), mn * scale / 1e3, med * scale / 1e3, (double)bc / (mn / 1e3));
        return 0;
    }

    std::vector<u64> cands;
    u64 done = 0;
    double last_print = now_s();
    bool stop = false;
    while (done < cnt_idx && !stop) {
        u64 n = std::min(batch_idx, cnt_idx - done);
        u64 a = pfx_start + done;
        double ts = now_s();
#ifndef NO_CUDA
        if (use_gpu) {
            CK(cudaEventRecord(ev0));
            launch_scan(kernel_id, dense_nd, nblocks, tpb, a, n, K, d_out, d_cnt, (u32)std::min<u64>(max_cand, 0xFFFFFFFFULL), d_surv);
            CK(cudaEventRecord(ev1)); CK(cudaEventSynchronize(ev1));
            CK(cudaGetLastError());
            u32 c = 0; CK(cudaMemcpy(&c, d_cnt, sizeof c, cudaMemcpyDeviceToHost));
            u32 take = (u32)std::min<u64>(c, max_cand);
            if (c > max_cand) { sst.truncated = true; if (!force) stop = true; }
            sst.cand_total += c;
            t_scan += now_s() - ts;
            if (take) {       /* этап A на GPU: F -> R -> вторая поверхность; выжившие R — на хост */
                double tp = now_s();
                CK(cudaEventRecord(ev0));
                k_stageA<<<sms * 8, 256>>>(d_out, take, d_R, d_cntR, capR, d_nrev);
                CK(cudaEventRecord(ev1)); CK(cudaEventSynchronize(ev1)); CK(cudaGetLastError());
                { float msA = 0; CK(cudaEventElapsedTime(&msA, ev0, ev1)); t_stageA_gpu += msA * 1e-3; }
                u32 cr = 0; CK(cudaMemcpy(&cr, d_cntR, sizeof cr, cudaMemcpyDeviceToHost));
                if (cr > capR) { fprintf(stderr, "# ОШИБКА: слишком много R, прошедших вторую поверхность (%u > %u)\n", cr, capR); cr = capR; stop = true; sst.truncated = true; }
                std::vector<u64> Rs(cr);
                if (cr) CK(cudaMemcpy(Rs.data(), d_R, cr * sizeof(u64), cudaMemcpyDeviceToHost));
                CK(cudaMemset(d_cntR, 0, sizeof(u32)));
                post.n_cand += take; post.n_cross += cr;
                finish_from_R(Rs, cx, threads, seeds, post);
                t_post += now_s() - tp;
                if (first && !seeds.empty()) stop = true;
            }
            CK(cudaMemset(d_cnt, 0, sizeof(u32)));
            ts = now_s();
        } else
#endif
        {
            size_t old = cands.size();
            scan_cpu(pl, a, n, threads, cands, sst);
            sst.cand_total += cands.size() - old;
        }
        if (!use_gpu) t_scan += now_s() - ts;
        done += n;
        if (!use_gpu && first && !cands.empty()) {
            double tp = now_s();
            process_candidates(cands, cx, threads, seeds, post); cands.clear();
            t_post += now_s() - tp;
            if (!seeds.empty()) stop = true;
        }
        if (verbose && now_s() - last_print > 1.0) { last_print = now_s(); fprintf(stderr, "# прогресс: %.1f%% (%.2f с)\n", 100.0 * (double)done / (double)cnt_idx, now_s() - t0); }
    }
#ifndef NO_CUDA
    if (use_gpu) {
        unsigned long long sv = 0; CK(cudaMemcpy(&sv, d_surv, sizeof sv, cudaMemcpyDeviceToHost)); sst.top_survivors = sv;
        unsigned long long nr = 0; CK(cudaMemcpy(&nr, d_nrev, sizeof nr, cudaMemcpyDeviceToHost)); post.n_rev += nr;
        CK(cudaFree(d_out)); CK(cudaFree(d_cnt)); CK(cudaFree(d_surv)); CK(cudaFree(d_R)); CK(cudaFree(d_cntR)); CK(cudaFree(d_nrev));
    }
#endif
    if (!cands.empty()) {
        double tp = now_s();
        process_candidates(cands, cx, threads, seeds, post);
        t_post += now_s() - tp;
    }
    double t_total = now_s() - t0;

    std::sort(seeds.begin(), seeds.end());
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

    FILE *fo = out_path.empty() ? nullptr : fopen(out_path.c_str(), "w");
    if (!out_path.empty() && !fo) { fprintf(stderr, "не могу писать в %s\n", out_path.c_str()); }
    /* «близнецы»: разные S с одним и тем же R mod 2^48 (nextLong не инъективен на 48 бит) дают ИДЕНТИЧНЫЙ бедрок — по нему неотличимы */
    std::map<u64, int> rgroups;
    for (u64 S : seeds) rgroups[nb_base_R(S) & NB_MASK48]++;
    int n_twin_groups = 0; for (auto &g : rgroups) if (g.second > 1) n_twin_groups++;
    for (u64 S : seeds) {
        printf("%llu\n", (unsigned long long)S);
        { u64 r48 = nb_base_R(S) & NB_MASK48; int g = rgroups[r48]; if (g > 1) printf("# близнецы: %d seed с общим R=0x%012llx дают идентичный бедрок (неразличимы по этим данным)\n", g, (unsigned long long)r48); }
        if (fo) fprintf(fo, "%llu\n", (unsigned long long)S);
        if (want_ws) {
            u64 s0s[NB_REV_MAX]; int n = nb_reverse_nextlong48(S, s0s);
            for (int k = 0; k < n; k++)
                printf("# world_seed_if_game_generated(seed=%llu): %lld\n", (unsigned long long)S, (long long)nb_nextlong_state(s0s[k]));
        }
    }
    if (fo) fclose(fo);
    int rc = 0;
    if (sst.truncated && !force) { fprintf(stderr, "# ОШИБКА: буфер кандидатов переполнен (> %llu) — данных недостаточно; результат неполон. Увеличьте --max-cand или добавьте наблюдений.\n", (unsigned long long)max_cand); rc = 4; }
    if (!quiet) {
        fprintf(stderr, "# обработано префиксов: %llu из %llu (%s); прошли слой K: %llu; кандидатов F: %llu%s\n",
                (unsigned long long)done, (unsigned long long)total_idx, use_gpu ? "GPU" : "CPU",
                sst.top_survivors, sst.cand_total, sst.truncated ? " (БУФЕР ПЕРЕПОЛНЕН — часть отброшена!)" : "");
        fprintf(stderr, "# обращение F->R: %llu решений; прошли вторую поверхность: %llu; обращение R->seed: %llu; провал float-проверки: %llu\n",
                post.n_rev, post.n_cross, post.n_seed_rev, post.n_float_fail);
        fprintf(stderr, "# различных R (наборов фабрик бедрока): %zu, групп «близнецов»: %d\n", rgroups.size(), n_twin_groups);
        if (t_stageA_gpu > 0) fprintf(stderr, "# этап A (F->R + вторая поверхность) на GPU: %.4f с; хост (R->seed, полная проверка): остаток пост-обработки\n", t_stageA_gpu);
        fprintf(stderr, "# НАЙДЕНО structure seed: %zu; время: скан %.3f с, пост-обработка %.3f с, всего %.3f с\n", seeds.size(), t_scan, t_post, t_total);
    }
    return rc;
}

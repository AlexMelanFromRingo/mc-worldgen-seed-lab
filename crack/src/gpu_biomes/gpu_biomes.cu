/* gpu_biomes.cu — ядра и хостовая обвязка. Сборка: см. build.sh (nvcc --fmad=false). */
#include "gpu_biomes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#ifdef _OPENMP
#include <omp.h>
#endif

bool gb_progress = false;       /* печатать прогресс длинных поисков в stderr */
int gb_block = 32;              /* потоков в блоке (кратно 32); GB_BLOCK в окружении переопределяет */
static int block_size() { const char *e = getenv("GB_BLOCK"); int b = e ? atoi(e) : gb_block; if (b < 32) b = 32; if (b > 256) b = 256; return (b / 32) * 32; }
template<class K> static void prep_kernel(K k, int B) {
    cudaFuncSetAttribute(k, cudaFuncAttributeMaxDynamicSharedMemorySize, (B / 32) * MCG_TILE_BYTES_PER_WARP);
    cudaFuncSetAttribute(k, cudaFuncAttributePreferredSharedMemoryCarveout, 100);          /* максимум разделяемой памяти (плитки) */
    if (getenv("GB_DEBUG")) { int nb = 0; cudaOccupancyMaxActiveBlocksPerMultiprocessor(&nb, k, B, (B / 32) * MCG_TILE_BYTES_PER_WARP); fprintf(stderr, "[GB_DEBUG] блок %d потоков: %d блоков/SM = %d варпов/SM\n", B, nb, nb * B / 32); }
}
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(3); } } while (0)

/* ===================================== ядра ===================================== */
template<class S>
__global__ void k_points(McgParams P, McgSeedGen G, const i32 *pts, int np, int chunk, u8 *out, i32 *tgout) {
    u64 idx = (u64)blockIdx.x * blockDim.x + threadIdx.x;
    int nch = (np + chunk - 1) / chunk;
    u64 si = idx / (u64)nch; int c = (int)(idx % (u64)nch);
    if (si >= G.n) return;
    extern __shared__ u32 smem[];
    McgTile T; T.t = (u8 *)smem + (threadIdx.x >> 5) * MCG_TILE_BYTES_PER_WARP + (threadIdx.x & 31) * 4;
    int p0 = c * chunk, p1 = min(np, p0 + chunk);
    size_t o = (size_t)si * np + p0;
    S::points(P, mcg_seed_at(G, si), T, pts + 3 * p0, p1 - p0, out + o, tgout ? tgout + o * 6 : nullptr);
}

template<class S>
__global__ void k_search(McgParams P, McgSeedGen G, const McgObs *obs, int nobs, i64 *out, u32 *cnt, u32 maxout, unsigned long long *hist) {
    __shared__ u32 sh[MCG_MAX_OBS + 1];
    extern __shared__ u32 smem[];
    McgTile T; T.t = (u8 *)smem + (threadIdx.x >> 5) * MCG_TILE_BYTES_PER_WARP + (threadIdx.x & 31) * 4;
    for (int i = threadIdx.x; i <= nobs; i += blockDim.x) sh[i] = 0;
    __syncthreads();
    u64 i = (u64)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < G.n) {
        i64 seed = mcg_seed_at(G, i);
        int j = S::check(P, seed, T, obs, nobs);
        atomicAdd(&sh[j], 1u);
        if (j == nobs) { u32 k = atomicAdd(cnt, 1u); if (k < maxout) out[k] = seed; }
    }
    __syncthreads();
    for (int k = threadIdx.x; k <= nobs; k += blockDim.x) if (sh[k]) atomicAdd(&hist[k], (unsigned long long)sh[k]);
}

/* ================================== диспетчер типов ================================== */
#define GB_DISPATCH(ctx, FN, ...)                                                          \
    switch ((ctx).dim * 2 + ((ctx).mode == MC_NOISE_FLOAT ? 1 : 0)) {                      \
        case 0: FN<McgOwS<MC_NOISE_DOUBLE>>(__VA_ARGS__); break;                          \
        case 1: FN<McgOwS<MC_NOISE_FLOAT>>(__VA_ARGS__); break;                            \
        case 2: FN<McgNeS<MC_NOISE_DOUBLE>>(__VA_ARGS__); break;                           \
        case 3: FN<McgNeS<MC_NOISE_FLOAT>>(__VA_ARGS__); break;                            \
        case 4: FN<McgEnS<MC_NOISE_DOUBLE>>(__VA_ARGS__); break;                           \
        default: FN<McgEnS<MC_NOISE_FLOAT>>(__VA_ARGS__); break;                           \
    }

/* ============================= открытие контекста ============================= */
static int build_tree(const McBiomeTree *T, std::vector<McgNode> &out) {
    int n = T->n_nodes;
    std::vector<int> newid(n, -1), order; order.reserve(n);
    out.assign(n, McgNode());
    newid[T->root] = 0; order.push_back(T->root);
    int next = 1;
    for (size_t qi = 0; qi < order.size(); qi++) {
        int o = order[qi]; const McRNode *r = &T->node[o]; McgNode &g = out[qi];
        for (int d = 0; d < MC_RT_DIM; d++) {
            if (r->box.lo[d] < -32768 || r->box.hi[d] > 32767) { fprintf(stderr, "tree: граница не влезает в i16\n"); return -1; }
            g.lo[d] = (i16)r->box.lo[d]; g.hi[d] = (i16)r->box.hi[d];
        }
        if (r->count == 0) { g.first = 0; g.count = 0; g.biome = (u8)r->biome; if (r->biome < 0 || r->biome > 254) return -1; }
        else {
            if (next + r->count > 32767 || r->count > 255) return -1;
            g.first = (i16)next; g.count = (u8)r->count; g.biome = 0xFF;
            for (int c = 0; c < r->count; c++) { int ch = T->child_idx[r->first + c]; newid[ch] = next + c; order.push_back(ch); }
            next += r->count;
        }
    }
    if ((int)order.size() != next) { fprintf(stderr, "tree: не все узлы достижимы (%zu/%d)\n", order.size(), next); return -1; }
    out.resize(next);
    return 0;
}

int gb_open(GbCtx &c, int version, int dim, const char *preset, bool gpu) {
    c.version = version; c.dim = dim;
    if (mcg_host_load(&c.H, version, dim, preset)) return -1;
    c.mode = c.H.mode; strncpy(c.preset, c.H.preset, 15);
    McgParams &P = c.Ph; memset(&P, 0, sizeof P);
    P.version = version; P.dim = dim; P.mode = c.mode;
    lcg_jump_params(262, &P.jm262, &P.ja262);
    lcg_jump_params(MC_END_SKIP, &P.jm_end, &P.ja_end);
    static const char *EN[5] = {"the_end", "end_highlands", "end_midlands", "small_end_islands", "end_barrens"};
    for (int i = 0; i < 5; i++) P.end_ids[i] = mcg_biome_id(EN[i]);
    if (dim == MC_OVERWORLD) {
        const McClimateSpec *S = &c.H.clim;
        const McNoiseSpec *s6[6] = {&S->sp_offset, &S->sp_temp, &S->sp_veg, &S->sp_cont, &S->sp_eros, &S->sp_ridge};
        int acc = 0;
        for (int i = 0; i < 6; i++) { c.sp_host[i] = *s6[i]; P.noff[i] = acc; acc += 2 * s6[i]->n_levels; }
        P.noff[6] = acc;          /* всего экземпляров ImprovedNoise (справочно; на GPU они не хранятся) */
    } else if (dim == MC_NETHER) {
        c.sp_host[0] = c.H.nether_temp; c.sp_host[1] = c.H.nether_veg;
        P.noff[0] = 0; P.noff[1] = 2 * c.sp_host[0].n_levels;
        if (P.noff[1] + 2 * c.sp_host[1].n_levels > MCG_NE_INST) { fprintf(stderr, "Nether: слишком много октав\n"); return -1; }
    }
    if (c.H.tree) { if (build_tree(c.H.tree, c.tree)) return -1; }
    P.clim = &c.H.clim; P.sp = c.sp_host; P.tree = c.tree.empty() ? nullptr : c.tree.data();
    c.Pd = P;
    if (gpu) {
        CK(cudaSetDevice(0));
        CK(cudaFree(0));
        if (dim == MC_OVERWORLD) {
            CK(cudaMalloc(&c.d_clim, sizeof(McClimateSpec))); CK(cudaMemcpy(c.d_clim, &c.H.clim, sizeof(McClimateSpec), cudaMemcpyHostToDevice));
            c.Pd.clim = c.d_clim;
        }
        if (dim != MC_END) {
            CK(cudaMalloc(&c.d_sp, 6 * sizeof(McNoiseSpec))); CK(cudaMemcpy(c.d_sp, c.sp_host, 6 * sizeof(McNoiseSpec), cudaMemcpyHostToDevice));
            c.Pd.sp = c.d_sp;
            CK(cudaMalloc(&c.d_tree, c.tree.size() * sizeof(McgNode)));
            CK(cudaMemcpy(c.d_tree, c.tree.data(), c.tree.size() * sizeof(McgNode), cudaMemcpyHostToDevice));
            c.Pd.tree = c.d_tree;
        }
        c.gpu_ready = true;
    }
    return 0;
}
void gb_set_ties(GbCtx &c, bool on) { c.Ph.ties = on ? 1 : 0; c.Pd.ties = c.Ph.ties; }
void gb_close(GbCtx &c) {
    if (c.d_clim) cudaFree(c.d_clim);
    if (c.d_sp) cudaFree(c.d_sp);
    if (c.d_tree) cudaFree(c.d_tree);
    c.d_clim = nullptr; c.d_sp = nullptr; c.d_tree = nullptr;
    mcg_host_free(&c.H);
}

/* ================================== точки ================================== */
template<class S>
static double points_gpu_impl(GbCtx &c, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out, i32 *out_tg, int chunk) {
    i64 *d_seeds; i32 *d_pts; u8 *d_out; i32 *d_tg = nullptr;
    size_t nout = (size_t)ns * np;
    CK(cudaMalloc(&d_seeds, sizeof(i64) * ns)); CK(cudaMemcpy(d_seeds, seeds, sizeof(i64) * ns, cudaMemcpyHostToDevice));
    CK(cudaMalloc(&d_pts, sizeof(i32) * 3 * np)); CK(cudaMemcpy(d_pts, pts, sizeof(i32) * 3 * np, cudaMemcpyHostToDevice));
    CK(cudaMalloc(&d_out, nout));
    if (out_tg) CK(cudaMalloc(&d_tg, nout * 6 * sizeof(i32)));
    McgSeedGen G; G.kind = 0; G.list = d_seeds; G.base = 0; G.n = (u64)ns;
    int nch = (np + chunk - 1) / chunk;
    u64 nthreads = (u64)ns * nch; int B = block_size();
    prep_kernel(k_points<S>, B);
    cudaEvent_t e0, e1; cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    k_points<S><<<(unsigned)((nthreads + B - 1) / B), B, (B / 32) * MCG_TILE_BYTES_PER_WARP>>>(c.Pd, G, d_pts, np, chunk, d_out, d_tg);
    cudaEventRecord(e1); CK(cudaEventSynchronize(e1)); CK(cudaGetLastError());
    float ms; cudaEventElapsedTime(&ms, e0, e1);
    CK(cudaMemcpy(out, d_out, nout, cudaMemcpyDeviceToHost));
    if (out_tg) CK(cudaMemcpy(out_tg, d_tg, nout * 6 * sizeof(i32), cudaMemcpyDeviceToHost));
    cudaFree(d_seeds); cudaFree(d_pts); cudaFree(d_out); if (d_tg) cudaFree(d_tg);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
    return ms;
}
double gb_points_gpu(GbCtx &c, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out, i32 *out_tg, int chunk) {
    double ms = 0;
    /* кусками по seed'ам, чтобы не упереться в память выходов */
    int per = (int)std::max<size_t>(1, (size_t)(1ull << 28) / ((size_t)np * (out_tg ? 25 : 1)));
    for (int s0 = 0; s0 < ns; s0 += per) {
        int n1 = std::min(per, ns - s0);
        u8 *o = out + (size_t)s0 * np; i32 *t = out_tg ? out_tg + (size_t)s0 * np * 6 : nullptr;
        double m = 0;
        GB_DISPATCH(c, m = points_gpu_impl, c, seeds + s0, n1, pts, np, o, t, chunk);
        ms += m;
    }
    return ms;
}

template<class S>
static void points_cpu_impl(GbCtx &c, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out, i32 *out_tg) {
#pragma omp parallel
    {
        alignas(16) u8 tile[256]; McgTile T; T.t = tile;
#pragma omp for schedule(dynamic, 1)
        for (int si = 0; si < ns; si++) {
            size_t o = (size_t)si * np;
            S::points(c.Ph, seeds[si], T, pts, np, out + o, out_tg ? out_tg + o * 6 : nullptr);
        }
    }
}
double gb_points_cpu(GbCtx &c, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out, i32 *out_tg) {
    auto t0 = std::chrono::steady_clock::now();
    GB_DISPATCH(c, points_cpu_impl, c, seeds, ns, pts, np, out, out_tg);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/* ================================== поиск ================================== */
template<class S>
static void search_gpu_impl(GbCtx &c, const GbSource &src, const std::vector<McgObs> &obs, GbSearch &res, u32 maxfound) {
    int nobs = (int)obs.size();
    McgObs *d_obs; CK(cudaMalloc(&d_obs, sizeof(McgObs) * std::max(1, nobs)));
    CK(cudaMemcpy(d_obs, obs.data(), sizeof(McgObs) * nobs, cudaMemcpyHostToDevice));
    i64 *d_out; CK(cudaMalloc(&d_out, sizeof(i64) * maxfound));
    u32 *d_cnt; CK(cudaMalloc(&d_cnt, sizeof(u32))); CK(cudaMemset(d_cnt, 0, sizeof(u32)));
    unsigned long long *d_hist; CK(cudaMalloc(&d_hist, sizeof(u64) * (nobs + 1))); CK(cudaMemset(d_hist, 0, sizeof(u64) * (nobs + 1)));
    const u64 CH = 1ull << 24;           /* seed'ов на запуск */
    i64 *d_list = nullptr;
    u64 total = src.kind == 1 ? 65536ull : src.n;
    if (src.kind == 0) CK(cudaMalloc(&d_list, sizeof(i64) * std::min<u64>(CH, total)));
    cudaEvent_t e0, e1; cudaEventCreate(&e0); cudaEventCreate(&e1);
    float ms_total = 0;
    int B = block_size();
    prep_kernel(k_search<S>, B);
    { McgSeedGen G0; G0.kind = 2; G0.n = 0; G0.list = nullptr; G0.base = 0;      /* прогрев: загрузка модуля ядра, выделение локальной памяти */
      k_search<S><<<1, B, (B / 32) * MCG_TILE_BYTES_PER_WARP>>>(c.Pd, G0, d_obs, nobs, d_out, d_cnt, maxfound, d_hist);
      CK(cudaDeviceSynchronize()); CK(cudaMemset(d_hist, 0, sizeof(u64) * (nobs + 1))); }
    for (u64 s0 = 0; s0 < total; s0 += CH) {
        u64 n1 = std::min<u64>(CH, total - s0);
        McgSeedGen G; G.kind = src.kind; G.n = n1; G.list = d_list; G.base = src.base;
        if (src.kind == 0) CK(cudaMemcpy(d_list, src.list + s0, sizeof(i64) * n1, cudaMemcpyHostToDevice));
        else if (src.kind == 1) G.base = src.base;       /* (n1 == 65536) */
        else G.base = src.base + s0;
        cudaEventRecord(e0);
        k_search<S><<<(unsigned)((n1 + B - 1) / B), B, (B / 32) * MCG_TILE_BYTES_PER_WARP>>>(c.Pd, G, d_obs, nobs, d_out, d_cnt, maxfound, d_hist);
        cudaEventRecord(e1); CK(cudaEventSynchronize(e1)); CK(cudaGetLastError());
        float ms; cudaEventElapsedTime(&ms, e0, e1); ms_total += ms;
        if (gb_progress && total > 4 * CH) {
            u32 cnt_now; CK(cudaMemcpy(&cnt_now, d_cnt, sizeof(u32), cudaMemcpyDeviceToHost));
            fprintf(stderr, "\r  прогресс: %5.1f%%  (%.3g seed'ов, %.1f с ядра, найдено %u)    ", 100.0 * (s0 + n1) / total, (double)(s0 + n1), ms_total / 1e3, cnt_now); fflush(stderr);
        }
    }
    if (gb_progress && total > 4 * CH) fprintf(stderr, "\n");
    u32 cnt; CK(cudaMemcpy(&cnt, d_cnt, sizeof(u32), cudaMemcpyDeviceToHost));
    std::vector<i64> f(std::min<u32>(cnt, maxfound)); if (!f.empty()) CK(cudaMemcpy(f.data(), d_out, sizeof(i64) * f.size(), cudaMemcpyDeviceToHost));
    std::vector<u64> h(nobs + 1); CK(cudaMemcpy(h.data(), d_hist, sizeof(u64) * (nobs + 1), cudaMemcpyDeviceToHost));
    std::sort(f.begin(), f.end(), [](i64 a, i64 b) { return (u64)a < (u64)b; });
    res.found = f; res.reached = h; res.total = total; res.ms = ms_total;
    cudaFree(d_obs); cudaFree(d_out); cudaFree(d_cnt); cudaFree(d_hist); if (d_list) cudaFree(d_list);
    cudaEventDestroy(e0); cudaEventDestroy(e1);
}

template<class S>
static void search_cpu_impl(GbCtx &c, const GbSource &src, const std::vector<McgObs> &obs, GbSearch &res, u32 maxfound) {
    int nobs = (int)obs.size();
    u64 total = src.kind == 1 ? 65536ull : src.n;
    std::vector<u64> hist(nobs + 1, 0); std::vector<i64> found;
    auto t0 = std::chrono::steady_clock::now();
    McgSeedGen G; G.kind = src.kind; G.list = src.list; G.base = src.base; G.n = total;
#pragma omp parallel
    {
        std::vector<u64> lh(nobs + 1, 0); std::vector<i64> lf; alignas(16) u8 tile[256]; McgTile T; T.t = tile;
#pragma omp for schedule(dynamic, 256)
        for (long long i = 0; i < (long long)total; i++) {
            i64 seed = mcg_seed_at(G, (u64)i);
            int j = S::check(c.Ph, seed, T, obs.data(), nobs);
            lh[j]++;
            if (j == nobs) lf.push_back(seed);
        }
#pragma omp critical
        { for (int j = 0; j <= nobs; j++) hist[j] += lh[j]; found.insert(found.end(), lf.begin(), lf.end()); }
    }
    std::sort(found.begin(), found.end(), [](i64 a, i64 b) { return (u64)a < (u64)b; });
    if (found.size() > maxfound) found.resize(maxfound);
    res.found = found; res.reached = hist; res.total = total;
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int gb_search(GbCtx &c, bool gpu, const GbSource &src, const std::vector<McgObs> &obs, GbSearch &res, u32 maxfound) {
    if ((int)obs.size() > MCG_MAX_OBS) { fprintf(stderr, "слишком много наблюдений (макс %d)\n", MCG_MAX_OBS); return -1; }
    if (gpu) { GB_DISPATCH(c, search_gpu_impl, c, src, obs, res, maxfound); }
    else { GB_DISPATCH(c, search_cpu_impl, c, src, obs, res, maxfound); }
    return 0;
}

void gb_ties_host(GbCtx &c, const i32 *tg, int n, u64 *masks) {
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) {
        i32 t7[MC_RT_DIM]; for (int k = 0; k < 6; k++) t7[k] = tg[(size_t)i * 6 + k]; t7[6] = 0;
        mcg_rt_ties(c.Ph.tree, t7, &masks[(size_t)i * 2]);
    }
}

/* ================================ селективность ================================ */
void gb_estimate_selectivity(GbCtx &c, std::vector<McgObs> &obs, int nsamp, std::vector<double> &p_out) {
    std::vector<i64> seeds(nsamp);
    u64 x = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < nsamp; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; seeds[i] = (i64)(x * 0x2545F4914F6CDD1DULL); }
    int np = (int)obs.size();
    std::vector<i32> pts(3 * np);
    for (int j = 0; j < np; j++) {      /* блоковые координаты -> грубо в кварты (для оценки селективности достаточно) */
        bool b = obs[j].rad & MCG_OBS_BLOCK;
        pts[3 * j] = b ? (obs[j].qx >> 2) : obs[j].qx; pts[3 * j + 1] = b ? (obs[j].qy >> 2) : obs[j].qy; pts[3 * j + 2] = b ? (obs[j].qz >> 2) : obs[j].qz;
    }
    std::vector<u8> out((size_t)nsamp * np);
    if (c.gpu_ready) gb_points_gpu(c, seeds.data(), nsamp, pts.data(), np, out.data(), nullptr, 1);
    else gb_points_cpu(c, seeds.data(), nsamp, pts.data(), np, out.data(), nullptr);
    p_out.assign(np, 0.0);
    for (int j = 0; j < np; j++) {
        int hit = 0;
        for (int s = 0; s < nsamp; s++) hit += mcg_in_mask(&obs[j], out[(size_t)s * np + j]);
        p_out[j] = (hit + 0.5) / (nsamp + 1.0);
    }
}

/* ================================ разбор наблюдений ================================ */
bool gb_parse_obs_line(const char *line, McgObs &o, std::string &err, bool block) {
    memset(&o, 0, sizeof o);
    int qx, qy, qz; char bio[1024]; int n = 0;
    if (sscanf(line, "%d %d %d %1023s%n", &qx, &qy, &qz, bio, &n) != 4) { err = "ожидается: qx qy qz biome[|biome...]"; return false; }
    o.qx = qx; o.qy = qy; o.qz = qz; o.rad = 12 | (block ? MCG_OBS_BLOCK : 0);
    char *save = nullptr; int cnt = 0;
    for (char *tok = strtok_r(bio, "|", &save); tok; tok = strtok_r(nullptr, "|", &save)) {
        int id = mcg_biome_id(tok);
        if (id < 0) { err = std::string("неизвестный биом: ") + tok; return false; }
        o.mask[id >> 6] |= 1ULL << (id & 63); cnt++;
    }
    if (!cnt) { err = "пустое множество биомов"; return false; }
    return true;
}
void gb_finalize_obs(const GbCtx &c, std::vector<McgObs> &obs) {
    for (auto &o : obs) o.rad = (o.rad & MCG_OBS_BLOCK) | ((c.dim == MC_END) ? mcg_end_rad_for_mask(o.mask, c.Ph.end_ids) : 12);
}
std::string gb_mask_str(const McgObs &o) {
    std::string s;
    for (int b = 0; b < mcg_biome_count(); b++) if (mcg_in_mask(&o, b)) { if (!s.empty()) s += "|"; s += mcg_biome_name(b); }
    return s;
}

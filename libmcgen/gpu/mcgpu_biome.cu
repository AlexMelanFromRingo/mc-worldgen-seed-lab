/* mcgpu_biome.cu — сетка биомов на GPU: BiomeManager (зум) → климат клетки (программа density-функций роутера) → R-дерево
 * с теми же правилами «ничьих», что у CPU (world_biome_cell). Результат побитово совпадает с mcgen_biome_at/mcgen_biome_grid.
 * Нить, не сумевшая выдать результат строго как CPU (NaN в климате, длинная цепочка ничьих), пишет 255 — мост пересчитывает
 * такие точки на CPU. */
#include "mcgpu_prog.cuh"

struct BWDev {
    DProg P; DTree T;
    int source, fixed_biome, end_ids[5], newf, min_qy, qh;
    i64 zoom_seed;
};

/* климат в точке (блоки): 6 значений float (temperature, vegetation, continents, erosion, depth, ridges) */
DEVN bool climate6(const BWDev &W, int bx, int by, int bz, float *v) {
    if (W.newf) {
        float val[MCG_MAX_VAL];
        if (!eval_prog_new(W.P, bx, by, bz, val)) return false;
        for (int k = 0; k < 6; k++) v[k] = W.P.root[k] >= 0 ? val[W.P.root[k]] : 0.0f;
    } else {
        double val[MCG_MAX_VAL];
        if (!eval_prog_old(W.P, bx, by, bz, val)) return false;
        for (int k = 0; k < 6; k++) v[k] = W.P.root[k] >= 0 ? (float)val[W.P.root[k]] : 0.0f;
    }
    return true;
}
/* erosion (поле роутера №3) в точке — для End */
DEVN bool erosion_at(const BWDev &W, int bx, int by, int bz, double *h) {
    if (W.newf) {
        float val[MCG_MAX_VAL];
        if (!eval_prog_new(W.P, bx, by, bz, val)) return false;
        *h = (double)val[W.P.root[3]];
    } else {
        double val[MCG_MAX_VAL];
        if (!eval_prog_old(W.P, bx, by, bz, val)) return false;
        *h = val[W.P.root[3]];
    }
    return true;
}
DEV int end_class(const BWDev &W, double h) {
    if (h > 0.25) return W.end_ids[1];
    if (h >= -0.0625) return W.end_ids[2];
    return h < -0.21875 ? W.end_ids[3] : W.end_ids[4];
}
/* TheEndBiomeSource на ячейке (qx,qy,qz); qy уже зажат */
DEVN int end_cell(const BWDev &W, int qx, int qy, int qz, bool *bad) {
    int cx = (qx * 4) >> 4, cz = (qz * 4) >> 4;
    if ((i64)cx * cx + (i64)cz * cz <= 4096LL) return W.end_ids[0];
    double h;
    if (!erosion_at(W, (cx * 2 + 1) * 8, qy * 4, (cz * 2 + 1) * 8, &h)) { *bad = true; return 0; }
    return end_class(W, h);
}

#define CHAIN_CAP 12
struct ChainEnt { i64 tg[7]; u64 d; int l0; };

/* world_biome_cell для мультишумовых источников */
DEVN int multi_cell(const BWDev &W, int qx, int qy, int qz, bool *bad) {
    int qmin = W.min_qy;
    int bxq = qx & ~3, bzq = qz & ~3, ly = qy - qmin;
    int idx = (ly >> 2) * 64 + (qx & 3) * 16 + (ly & 3) * 4 + (qz & 3);
    ChainEnt st[CHAIN_CAP]; int n = 0, base = 0;
    for (;;) {
        int s = idx >> 6, r = idx & 63;
        int cx = bxq + (r >> 4), cy = qmin + s * 4 + ((r >> 2) & 3), cz = bzq + (r & 3);
        float v[6];
        if (!climate6(W, cx * 4, cy * 4, cz * 4, v)) { *bad = true; return 0; }
        ChainEnt c;
        for (int k = 0; k < 6; k++) {
            float f = v[k] * 10000.0f;
            if (!(fabsf(f) < 9.0e18f)) { *bad = true; return 0; }      /* NaN/inf/переполнение: приведение к i64 на CPU и GPU различается */
            c.tg[k] = (i64)f;
        }
        c.tg[6] = 0;
        c.l0 = rt_find(W.T, c.tg);
        c.d = node_dist(W.T.n[c.l0], c.tg);
        if (idx == 0 || !rt_tie(W.T, c.tg, c.d)) { base = c.l0; break; }
        if (n == CHAIN_CAP) { *bad = true; return 0; }
        st[n++] = c; idx--;
    }
    int leaf = base;
    for (int i = n - 1; i >= 0; i--) leaf = node_dist(W.T.n[leaf], st[i].tg) == st[i].d ? leaf : st[i].l0;
    return W.T.n[leaf].biome;
}
DEVN int cell_biome(const BWDev &W, int qx, int qy, int qz, bool *bad) {
    int qmin = W.min_qy;
    if (qy < qmin) qy = qmin;
    if (qy > qmin + W.qh - 1) qy = qmin + W.qh - 1;
    if (W.source == MCG_BS_FIXED) return W.fixed_biome;
    if (W.source == MCG_BS_END) return end_cell(W, qx, qy, qz, bad);
    return multi_cell(W, qx, qy, qz, bad);
}
DEVN int biome_at(const BWDev &W, int x, int y, int z, bool *bad) {
    int qx, qy, qz;
    zoom_cell(W.zoom_seed, x, y, z, &qx, &qy, &qz);
    return cell_biome(W, qx, qy, qz, bad);
}

/* End: таблица биомов по чанкам (для сетки): слой l ∈ {0,1} — qy = qy_lo[l] */
struct EndTab { const unsigned char *t; int cx0, cz0, ncx, ncz, qy0, qy1; };

__global__ void k_biome_grid(BWDev W, int x0, int z0, int nx, int row0, int rows, int step, int y, unsigned char *out, int *nbad, EndTab et) {
    long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= (long long)nx * rows) return;
    int ix = (int)(i % nx), iz = row0 + (int)(i / nx);
    int x_ = x0 + ix * step, z_ = z0 + iz * step;
    bool bad = false; int b;
    if (W.source == MCG_BS_END && et.t) {
        int qx, qy, qz;
        zoom_cell(W.zoom_seed, x_, y, z_, &qx, &qy, &qz);
        int qmin = W.min_qy;
        if (qy < qmin) qy = qmin;
        if (qy > qmin + W.qh - 1) qy = qmin + W.qh - 1;
        int cx = (qx * 4) >> 4, cz = (qz * 4) >> 4;
        if ((i64)cx * cx + (i64)cz * cz <= 4096LL) b = W.end_ids[0];
        else {
            int layer = qy == et.qy0 ? 0 : 1;
            b = et.t[((size_t)layer * et.ncz + (cz - et.cz0)) * et.ncx + (cx - et.cx0)];
        }
    } else b = biome_at(W, x_, y, z_, &bad);
    if (bad) { out[(size_t)iz * nx + ix] = 255; atomicAdd(nbad, 1); } else out[(size_t)iz * nx + ix] = (unsigned char)b;
}
__global__ void k_biome_points(BWDev W, int n, const int *xyz, unsigned char *out, int *nbad) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    bool bad = false;
    int b = biome_at(W, xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2], &bad);
    if (bad) { out[i] = 255; atomicAdd(nbad, 1); } else out[i] = (unsigned char)b;
}
__global__ void k_end_table(BWDev W, unsigned char *tab, int cx0, int cz0, int ncx, int ncz, int qy0, int qy1) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int total = 2 * ncx * ncz;
    if (i >= total) return;
    int layer = i / (ncx * ncz), r = i % (ncx * ncz);
    int cz = cz0 + r / ncx, cx = cx0 + r % ncx;
    int qy = layer == 0 ? qy0 : qy1;
    bool bad = false;
    int b = W.end_ids[0];
    if ((i64)cx * cx + (i64)cz * cz > 4096LL) {
        double h;
        if (!erosion_at(W, (cx * 2 + 1) * 8, qy * 4, (cz * 2 + 1) * 8, &h)) bad = true; else b = end_class(W, h);
    }
    tab[i] = bad ? 255 : (unsigned char)b;
}

struct GBiome {
    McgBiomeWorld W;
    GProg prog; McgTreeNode *tree = nullptr;
    BWDev dev;
};

static bool prog_supported(const McgProg *p) {
    if (p->nnodes > MCG_MAX_VAL) { mcgpu_set_error("программа слишком велика (%d узлов > %d)", p->nnodes, MCG_MAX_VAL); return false; }
    for (int i = 0; i < p->nnodes; i++) {
        int k = p->nodes[i].k;
        if (p->old) { if (k < 0 || k >= MCGO__COUNT) { mcgpu_set_error("неизвестный узел %d", k); return false; } continue; }
        switch (k) {
        case MCGK_INTERP: case MCGK_FTS: case MCGK_LOG: case MCGK_POW_CB: case MCGK_POW_CE: case MCGK_POW: case MCGK_ROUND_INT: case MCGK_ROUND:
            mcgpu_set_error("узел вида %d не поддержан точечным вычислителем GPU", k); return false;
        default: break;
        }
    }
    return true;
}

MCGPU_EXPORT void *mcgpu_biome_new(const McgBiomeWorld *W, char *err, size_t errlen) {
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    if (!mcgpu_ensure_init()) { if (err && errlen) snprintf(err, errlen, "%s", mcgpu_get_error()); return nullptr; }
    GBiome *g = new GBiome();
    g->W = *W;
    bool ok = true;
    if (W->source != MCG_BS_FIXED) {
        ok = W->prog && prog_supported(W->prog) && g->prog.upload(W->prog);
        if (ok && W->source == MCG_BS_MULTI) ok = mcgpu_upload(&g->tree, W->tree, (size_t)W->ntree);
        else if (ok && W->source == MCG_BS_END && (W->prog->root[3] < 0)) { mcgpu_set_error("нет поля erosion"); ok = false; }
        if (ok && W->source == MCG_BS_MULTI) for (int k = 0; k < 6; k++) if (W->prog->root[k] < 0) { mcgpu_set_error("нет поля климата %d", k); ok = false; }
    }
    if (!ok) { if (err && errlen) snprintf(err, errlen, "%s", mcgpu_get_error()); g->prog.release(); cudaFree(g->tree); delete g; return nullptr; }
    BWDev &d = g->dev;
    memset(&d, 0, sizeof d);
    d.P = g->prog.d; d.T.n = g->tree; d.T.root = W->tree_root;
    d.source = W->source; d.fixed_biome = W->fixed_biome; memcpy(d.end_ids, W->end_ids, sizeof d.end_ids);
    d.newf = W->newf; d.min_qy = W->min_qy; d.qh = W->qh; d.zoom_seed = W->zoom_seed;
    return g;
}
MCGPU_EXPORT void mcgpu_biome_free(void *h) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    GBiome *g = (GBiome *)h;
    mcgpu_ensure_init();
    g->prog.release(); cudaFree(g->tree);
    delete g;
}

#define TPB 128
static bool launch_check(const char *what) {
    cudaError_t e = cudaGetLastError();
    if (e == cudaSuccess) e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { mcgpu_set_error("%s: %s", what, cudaGetErrorString(e)); return false; }
    return true;
}

/* сетка nx*nz, шаг step блоков, уровень y; out[iz*nx+ix]; 255 — точка не решена (пересчитать на CPU). Возврат 0 — ок. */
MCGPU_EXPORT int mcgpu_biome_grid(void *h, int x0, int z0, int nx, int nz, int step, int y, unsigned char *out, int *nunres) {
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    if (!h || nx <= 0 || nz <= 0 || step <= 0 || !out) { mcgpu_set_error("неверные аргументы"); return 1; }
    if (!mcgpu_ensure_init()) return 1;
    GBiome *g = (GBiome *)h;
    unsigned char *d_out = nullptr, *d_tab = nullptr; int *d_bad = nullptr;
    size_t total = (size_t)nx * (size_t)nz;
    int rc = 1;
    EndTab et; memset(&et, 0, sizeof et);
    do {
        if (cudaMalloc((void **)&d_out, total) != cudaSuccess) { mcgpu_set_error("cudaMalloc(%zu): нет памяти", total); d_out = nullptr; break; }
        if (cudaMalloc((void **)&d_bad, sizeof(int)) != cudaSuccess) { mcgpu_set_error("cudaMalloc"); d_bad = nullptr; break; }
        cudaMemset(d_bad, 0, sizeof(int));
        if (g->W.source == MCG_BS_END) {
            /* таблица биомов по чанкам: ячейки зума лежат в [(x0-2)>>2, ((xmax-2)>>2)+1] (кварты) */
            long long xmax = (long long)x0 + (long long)(nx - 1) * step, zmax = (long long)z0 + (long long)(nz - 1) * step;
            int qxa = (x0 - 2) >> 2, qxb = (int)((xmax - 2) >> 2) + 1, qza = (z0 - 2) >> 2, qzb = (int)((zmax - 2) >> 2) + 1;
            et.cx0 = qxa >> 2; et.cz0 = qza >> 2; et.ncx = (qxb >> 2) - et.cx0 + 1; et.ncz = (qzb >> 2) - et.cz0 + 1;
            int py = (y - 2) >> 2, lo = g->W.min_qy, hi = g->W.min_qy + g->W.qh - 1;
            et.qy0 = py < lo ? lo : (py > hi ? hi : py);
            int py1 = py + 1; et.qy1 = py1 < lo ? lo : (py1 > hi ? hi : py1);
            size_t tn = (size_t)2 * et.ncx * et.ncz;
            if (tn < (size_t)(1u << 28) && cudaMalloc((void **)&d_tab, tn) == cudaSuccess) {
                int blocks = (int)((tn + TPB - 1) / TPB);
                k_end_table<<<blocks, TPB>>>(g->dev, d_tab, et.cx0, et.cz0, et.ncx, et.ncz, et.qy0, et.qy1);
                if (!launch_check("k_end_table")) break;
                et.t = d_tab;
            } else { d_tab = nullptr; et.t = nullptr; cudaGetLastError(); }
        }
        /* порции по строкам: каждая порция — короткий запуск (не упираемся в TDR на Windows) */
        long long per = 1 << 20; int rows_per = (int)(per / nx); if (rows_per < 1) rows_per = 1;
        bool ok = true;
        for (int r0 = 0; r0 < nz && ok; r0 += rows_per) {
            int rows = nz - r0 < rows_per ? nz - r0 : rows_per;
            long long n = (long long)nx * rows;
            k_biome_grid<<<(unsigned)((n + TPB - 1) / TPB), TPB>>>(g->dev, x0, z0, nx, r0, rows, step, y, d_out, d_bad, et);
            ok = launch_check("k_biome_grid");
        }
        if (!ok) break;
        if (cudaMemcpy(out, d_out, total, cudaMemcpyDeviceToHost) != cudaSuccess) { mcgpu_set_error("cudaMemcpy D2H"); break; }
        int bad = 0; cudaMemcpy(&bad, d_bad, sizeof bad, cudaMemcpyDeviceToHost);
        if (nunres) *nunres = bad;
        rc = 0;
    } while (0);
    cudaFree(d_out); cudaFree(d_bad); cudaFree(d_tab);
    return rc;
}

/* произвольные точки: xyz — 3n int (блоки) */
MCGPU_EXPORT int mcgpu_biome_points(void *h, int n, const int *xyz, unsigned char *out, int *nunres) {
    std::lock_guard<std::mutex> lk(mcgpu_mutex());
    if (!h || n < 0 || !xyz || !out) { mcgpu_set_error("неверные аргументы"); return 1; }
    if (!mcgpu_ensure_init()) return 1;
    GBiome *g = (GBiome *)h;
    int *d_xyz = nullptr, *d_bad = nullptr; unsigned char *d_out = nullptr;
    int rc = 1;
    do {
        if (cudaMalloc((void **)&d_xyz, sizeof(int) * 3 * (size_t)(n ? n : 1)) != cudaSuccess) { mcgpu_set_error("cudaMalloc"); d_xyz = nullptr; break; }
        if (cudaMalloc((void **)&d_out, (size_t)(n ? n : 1)) != cudaSuccess) { mcgpu_set_error("cudaMalloc"); d_out = nullptr; break; }
        if (cudaMalloc((void **)&d_bad, sizeof(int)) != cudaSuccess) { mcgpu_set_error("cudaMalloc"); d_bad = nullptr; break; }
        cudaMemset(d_bad, 0, sizeof(int));
        if (cudaMemcpy(d_xyz, xyz, sizeof(int) * 3 * (size_t)n, cudaMemcpyHostToDevice) != cudaSuccess) { mcgpu_set_error("cudaMemcpy H2D"); break; }
        bool ok = true;
        const int PER = 1 << 20;
        for (int o = 0; o < n && ok; o += PER) {
            int m = n - o < PER ? n - o : PER;
            k_biome_points<<<(m + TPB - 1) / TPB, TPB>>>(g->dev, m, d_xyz + 3 * (size_t)o, d_out + o, d_bad);
            ok = launch_check("k_biome_points");
        }
        if (!ok) break;
        if (cudaMemcpy(out, d_out, (size_t)n, cudaMemcpyDeviceToHost) != cudaSuccess) { mcgpu_set_error("cudaMemcpy D2H"); break; }
        int bad = 0; cudaMemcpy(&bad, d_bad, sizeof bad, cudaMemcpyDeviceToHost);
        if (nunres) *nunres = bad;
        rc = 0;
    } while (0);
    cudaFree(d_xyz); cudaFree(d_out); cudaFree(d_bad);
    return rc;
}

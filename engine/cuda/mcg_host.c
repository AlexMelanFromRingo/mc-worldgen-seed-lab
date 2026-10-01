/* mcg_host.c — см. mcg_host.h. Компилировать: gcc -O2 -ffp-contract=off -fopenmp (без -ffast-math!). */
#include "../mc_data.h"
#include "mcg_host.h"
#ifdef _OPENMP
#include <omp.h>
#endif

int mcg_host_load(McgHost *H, int version, int dim, const char *preset) {
    memset(H, 0, sizeof *H);
    H->version = version; H->dim = dim;
    static McVersionData V;
    if (mc_load_version(&V, version)) return -1;
    H->mode = V.nether_temp.mode;      /* численный режим версии задаёт engine (mc_load_version): DOUBLE 26.1/26.2, FLOAT 26.3 */
    if (dim == MC_END) { strcpy(H->preset, "-"); return 0; }
    if (dim == MC_OVERWORLD) {
        const char *p = (!preset || !strcmp(preset, "normal")) ? "overworld" : preset;
        McClimateSpec *S = mc_preset(&V, p);
        if (!S) { fprintf(stderr, "mcg: нет пресета '%s'\n", p); return -1; }
        H->clim = *S; strncpy(H->preset, p, 15);
        H->tree = mc_load_biome_tree(version, "overworld");
    } else {
        strcpy(H->preset, "-");
        H->nether_temp = V.nether_temp; H->nether_veg = V.nether_veg;
        H->tree = mc_load_biome_tree(version, "nether");
    }
    return H->tree ? 0 : -1;
}
void mcg_host_free(McgHost *H) { free(H->tree); H->tree = NULL; }

int mcg_version_from_name(const char *s) { return mc_version_from_name(s); }
const char *mcg_version_name(int v) { return mc_version_name(v); }
int mcg_biome_count(void) { return MC_BIOME_COUNT; }
const char *mcg_biome_name(int id) { return mc_biome_name(id); }
int mcg_biome_id(const char *name) { return mc_biome_id(name); }

void mcg_ref_points(const McgHost *H, const i64 *seeds, int ns, const i32 *pts, int np, u8 *out_biome, i32 *out_tg) {
    static const char *EN[5] = {"the_end", "end_highlands", "end_midlands", "small_end_islands", "end_barrens"};
    int end_id[5];
    for (int i = 0; i < 5; i++) end_id[i] = mc_biome_id(EN[i]);
#pragma omp parallel
    {
        McClimate *cl = (McClimate *)malloc(sizeof(McClimate));
        McNormal *nt = (McNormal *)malloc(sizeof(McNormal)), *nv = (McNormal *)malloc(sizeof(McNormal));
        McEnd *E = (McEnd *)malloc(sizeof(McEnd));
#pragma omp for schedule(dynamic, 1)
        for (int si = 0; si < ns; si++) {
            i64 seed = seeds[si];
            if (H->dim == MC_OVERWORLD) mc_climate_init_overworld(cl, &H->clim, seed);
            else if (H->dim == MC_NETHER) {
                McLcg r0 = lcg_new(seed), r1 = lcg_new(seed + 1);
                normal_init_legacy(nt, &H->nether_temp, &r0); normal_init_legacy(nv, &H->nether_veg, &r1);
            } else mc_end_init(E, seed);
            for (int p = 0; p < np; p++) {
                int qx = pts[3 * p], qy = pts[3 * p + 1], qz = pts[3 * p + 2];
                size_t o = (size_t)si * np + p;
                if (H->dim == MC_OVERWORLD) {
                    float raw[6];
                    mc_climate_overworld_raw(cl, &H->clim, qx * 4, qy * 4, qz * 4, raw);
                    McTarget t = mc_target_from_raw(raw);
                    out_biome[o] = (u8)mc_biome_find(H->tree, &t);
                    if (out_tg) { out_tg[o * 6] = (i32)t.t; out_tg[o * 6 + 1] = (i32)t.h; out_tg[o * 6 + 2] = (i32)t.c;
                                  out_tg[o * 6 + 3] = (i32)t.e; out_tg[o * 6 + 4] = (i32)t.d; out_tg[o * 6 + 5] = (i32)t.w; }
                } else if (H->dim == MC_NETHER) {
                    int bx = qx * 4, bz = qz * 4, by = qy * 4; float t, h;
                    if (H->mode == MC_NOISE_FLOAT) {
                        t = normal_get_f(nt, &H->nether_temp, bx * 0.25, by * 0.0, bz * 0.25);
                        h = normal_get_f(nv, &H->nether_veg, bx * 0.25, by * 0.0, bz * 0.25);
                    } else {
                        t = (float)normal_get_d(nt, &H->nether_temp, bx * 0.25 + 0.0, by * 0.0 + 0.0, bz * 0.25 + 0.0);
                        h = (float)normal_get_d(nv, &H->nether_veg, bx * 0.25 + 0.0, by * 0.0 + 0.0, bz * 0.25 + 0.0);
                    }
                    McTarget tg = {mc_quantize(t), mc_quantize(h), 0, 0, 0, 0};
                    out_biome[o] = (u8)mc_biome_find(H->tree, &tg);
                    if (out_tg) { out_tg[o * 6] = (i32)tg.t; out_tg[o * 6 + 1] = (i32)tg.h; for (int k = 2; k < 6; k++) out_tg[o * 6 + k] = 0; }
                } else {
                    out_biome[o] = (u8)end_id[mc_end_biome(E, H->mode, qx, qz)];
                    if (out_tg) for (int k = 0; k < 6; k++) out_tg[o * 6 + k] = 0;
                }
            }
        }
        free(cl); free(nt); free(nv); free(E);
    }
}

void mcg_ref_tie_mask(const McgHost *H, const i32 tg[6], u64 out[2]) {
    const McBiomeTree *T = H->tree;
    i64 t7[MC_RT_DIM] = {tg[0], tg[1], tg[2], tg[3], tg[4], tg[5], 0};
    i64 best = 0x7fffffffffffffffLL; out[0] = out[1] = 0;
    for (int i = 0; i < T->n_nodes; i++) {
        const McRNode *n = &T->node[i];
        if (n->count != 0 || n->biome < 0) continue;
        i64 d = mc_rnode_distance(n, t7);
        if (d < best) { best = d; out[0] = out[1] = 0; }
        if (d == best) out[n->biome >> 6] |= 1ULL << (n->biome & 63);
    }
}

/* world.c — McWorld: измерение + пресет + сиды по доменам + тонкие настройки; экземпляры шумов, компиляция роутера. */
#include "mcgen_internal.h"
#include "df_old.h"
#include "surface.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------- домены сидов ----------------
 * climate: шумы, достижимые из полей роутера temperature/vegetation/continents/erosion/depth/ridges
 *          (+ nether/temperature, nether/vegetation по Legacy(seed+0/+1), + острова End);
 * terrain: все остальные именованные шумы, BlendedNoise («minecraft:terrain»), фабрики aquifer/ore.
 * При равных сидах результат побитно равен ванили (RandomState игры).
 */
static void collect_noises(const McGen *g, const Df *f, StrMap *out, int depth);
static void collect_spline(const McGen *g, const DfSpline *s, StrMap *out, int depth) {
    if (!s || s->is_const) return;
    collect_noises(g, s->coord, out, depth);
    for (int i = 0; i < s->n; i++) collect_spline(g, s->val[i], out, depth);
}
static void collect_noises(const McGen *g, const Df *f, StrMap *out, int depth) {
    if (!f || depth > 200) return;
    if (f->t == DF_REF) { collect_noises(g, sm_get(&g->dfs, f->name), out, depth + 1); return; }
    if (f->name && (f->t == DF_NOISE || f->t == DF_SHIFTED_NOISE || f->t == DF_SHIFT_A || f->t == DF_SHIFT_B || f->t == DF_SHIFT || f->t == DF_WEIRD_SCALED))
        sm_put(out, f->name, (void *)1);
    collect_noises(g, f->a, out, depth); collect_noises(g, f->b, out, depth); collect_noises(g, f->c, out, depth); collect_noises(g, f->d, out, depth);
    for (int i = 0; i < f->nlist; i++) collect_noises(g, f->list[i], out, depth);
    collect_spline(g, f->spline, out, depth);
}

static int is_nether_biome_noise(const char *name) { return !strcmp(name, "minecraft:nether/temperature") || !strcmp(name, "minecraft:nether/vegetation"); }

static NoiseInst *noise_slot(McWorld *w, const char *name) {
    NoiseInst *ni = sm_get(&w->noise_inst, name);
    if (!ni) { ni = xcalloc(1, sizeof *ni); ni->name = name; sm_put(&w->noise_inst, name, ni); }
    return ni;
}

const NStack *world_noise_new(McWorld *w, const char *name, char *err, size_t errlen) {
    mutex_lock(w->lock);
    NoiseInst *ni = noise_slot(w, name);
    if (!ni->ready) {
        const NoiseParams *P = sm_get(&w->g->noises, name);
        if (!P) { mutex_unlock(w->lock); set_err(err, errlen, "нет шума %s", name); return NULL; }
        NoiseParams Pt = *P;
        if (is_nether_biome_noise(name)) {
            Rnd r = rnd_legacy_seed(w->seeds.climate + (strstr(name, "temperature") ? 0 : 1));
            nn_new_create_legacy_nether(&ni->ns, &Pt, &r);
        } else {
            int clim = sm_has(&w->climate_noises, name);
            Rnd r = pos_from_hash(clim ? &w->pos_climate : &w->pos_terrain, name);
            nn_new_create(&ni->ns, &Pt, &r);
        }
        ni->ready = 1;
    }
    mutex_unlock(w->lock);
    return &ni->ns;
}
const OldNormal *world_noise_old(McWorld *w, const char *name, char *err, size_t errlen) {
    mutex_lock(w->lock);
    NoiseInst *ni = noise_slot(w, name);
    if (!ni->ready) {
        const NoiseParams *P = sm_get(&w->g->noises, name);
        if (!P) { mutex_unlock(w->lock); set_err(err, errlen, "нет шума %s", name); return NULL; }
        if (is_nether_biome_noise(name)) {
            Rnd r = rnd_legacy_seed(w->seeds.climate + (strstr(name, "temperature") ? 0 : 1));
            old_normal_create(&ni->on, &r, P, 0);
        } else {
            int clim = sm_has(&w->climate_noises, name);
            Rnd r = pos_from_hash(clim ? &w->pos_climate : &w->pos_terrain, name);
            old_normal_create(&ni->on, &r, P, 1);
        }
        ni->ready = 1;
    }
    mutex_unlock(w->lock);
    return &ni->on;
}
Ival world_noise_range(McWorld *w, const char *name) {
    const NoiseParams *P = sm_get(&w->g->noises, name);
    return P ? nn_new_range(P) : iv_inf();
}

/* ---------------- NEnv для компилятора 26.3+ ---------------- */
static const NStack *env_noise(void *ud, const char *name, char *err, size_t errlen) { return world_noise_new(ud, name, err, errlen); }
static const BlendFbm *env_blended(void *ud, const Df *f) {
    McWorld *w = ud;
    for (int i = 0; i < w->nblended; i++) if (df_equal(w->blended_key[i], f)) return w->blended[i];
    if (w->nblended >= 4) return w->blended[0];
    BlendFbm *b = xcalloc(1, sizeof *b);
    /* RandomState.createRandom(BlendedNoise.NOISE_SEED): legacy → Legacy(seed+0), иначе random.fromHashOf("minecraft:terrain") */
    Rnd r = w->ns->legacy_random ? rnd_legacy_seed(w->seeds.terrain + 0) : pos_from_hash(&w->pos_terrain, "minecraft:terrain");
    blended_new_create(b, &r, f->n1.d, f->n4.d, f->n3.d);
    w->blended_key[w->nblended] = f; w->blended[w->nblended++] = b;
    return b;
}
static const GNoise *env_end(void *ud) {
    McWorld *w = ud;
    if (!w->has_end_simplex) {
        Rnd r = rnd_legacy_seed(w->seeds.climate);
        rnd_consume(&r, 17292);
        gn_init(&w->end_simplex, &r, w->g->newf ? 0.0 : 256.0);
        w->has_end_simplex = 1;
    }
    return &w->end_simplex;
}
static const Df *env_ref(void *ud, const char *id) { McWorld *w = ud; return world_df_ref(w, id); }
static void env_scale(void *ud, const char *name, double *mxz, double *my) { world_noise_scale(ud, name, mxz, my); }
static Ival env_range(void *ud, const char *name) { return world_noise_range(ud, name); }

const Df *world_df_lookup(const McWorld *w, const char *id) {
    char *full = df_full_id(id);
    const Df *f = world_df_ref(w, full);
    free(full);
    return f;
}

/* BiomeManager.obfuscateSeed: SHA-256(long LE) → первые 8 байт как long LE */
static void sha256(const u8 *msg, size_t len, u8 out[32]) {
    static const u32 K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    u32 h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    u8 buf[128]; size_t nl = ((len + 9 + 63) / 64) * 64;
    memset(buf, 0, sizeof buf); memcpy(buf, msg, len); buf[len] = 0x80;
    u64 bits = (u64)len * 8; for (int i = 0; i < 8; i++) buf[nl - 1 - i] = (u8)(bits >> (8 * i));
    #define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
    for (size_t off = 0; off < nl; off += 64) {
        u32 W[64];
        for (int i = 0; i < 16; i++) W[i] = ((u32)buf[off+4*i] << 24) | ((u32)buf[off+4*i+1] << 16) | ((u32)buf[off+4*i+2] << 8) | buf[off+4*i+3];
        for (int i = 16; i < 64; i++) { u32 s0 = ROR(W[i-15],7) ^ ROR(W[i-15],18) ^ (W[i-15] >> 3), s1 = ROR(W[i-2],17) ^ ROR(W[i-2],19) ^ (W[i-2] >> 10); W[i] = W[i-16] + s0 + W[i-7] + s1; }
        u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            u32 S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + W[i];
            u32 S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    #undef ROR
    for (int i = 0; i < 8; i++) { out[4*i] = (u8)(h[i] >> 24); out[4*i+1] = (u8)(h[i] >> 16); out[4*i+2] = (u8)(h[i] >> 8); out[4*i+3] = (u8)h[i]; }
}
static i64 obfuscate_seed(i64 seed) {
    u8 m[8], o[32];
    for (int i = 0; i < 8; i++) m[i] = (u8)((u64)seed >> (8 * i));
    sha256(m, 8, o);
    u64 v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | o[i];
    return (i64)v;
}

static void noise_inst_free(void *p) { NoiseInst *ni = p; if (!ni) return; ns_free(&ni->ns); if (ni->on.first.lev) old_normal_free(&ni->on); free(ni); }

int tweaks_apply(McWorld *w, const McTweakValue *tw, int n, char *err, size_t errlen);   /* tweaks.c */
void carvers_world_free(McWorld *w);   /* carver.c */

int mcgen_world_new(McGen *g, const char *dimension, const char *preset, const McSeeds *seeds,
                    const McTweakValue *tweaks, int ntweaks, McWorld **out, char *err, size_t errlen) {
    if (!g || !dimension || !seeds || !out) { set_err(err, errlen, "mcgen_world_new: пустой аргумент"); return MCGEN_E_ARG; }
    *out = NULL;
    int dk;
    const McPreset *p = gen_find_preset(g, dimension, preset, &dk);
    if (!p) { set_err(err, errlen, "нет измерения/пресета %s/%s", dimension, preset ? preset : ""); return MCGEN_E_ARG; }
    const NoiseSettings *ns = sm_get(&g->nsettings, p->settings);
    if (!ns) { set_err(err, errlen, "нет noise_settings %s", p->settings); return MCGEN_E_DATA; }
    McWorld *w = xcalloc(1, sizeof *w);
    w->g = g; w->preset = p; w->ns = ns; w->dim_kind = dk; w->seeds = *seeds;
    w->lock = mutex_new();
    w->min_y = p->min_y; w->height = p->height; w->sea_level = ns->sea_level;
    w->noise_mxz = w->noise_my = w->cave_m = 1.0;
    w->def_fluid = ns->default_fluid;
    w->def_block = ns->default_block >= 0 ? ns->default_block : (dk == 1 ? gen_state_id(g, "minecraft:netherrack") : dk == 2 ? gen_state_id(g, "minecraft:end_stone") : g->st_stone);
    Rnd rc = ns->legacy_random ? rnd_legacy_seed(seeds->climate) : rnd_xoro_seed(seeds->climate);
    Rnd rt = ns->legacy_random ? rnd_legacy_seed(seeds->terrain) : rnd_xoro_seed(seeds->terrain);
    w->pos_climate = rnd_fork_positional(&rc);
    w->pos_terrain = rnd_fork_positional(&rt);
    {   /* getOrCreateRandomFactory("minecraft:aquifer"/"minecraft:ore") = random.fromHashOf(name).forkPositional() */
        Rnd a = pos_from_hash(&w->pos_terrain, "minecraft:aquifer"); w->aquifer_pos = rnd_fork_positional(&a);
        Rnd o = pos_from_hash(&w->pos_terrain, "minecraft:ore"); w->ore_pos = rnd_fork_positional(&o);
    }
    for (int k = RF_TEMPERATURE; k <= RF_RIDGES; k++) collect_noises(g, ns->rf[k], &w->climate_noises, 0);
    w->biome_zoom_seed = obfuscate_seed(seeds->climate);
    w->tweak = xcalloc((size_t)(g->ntweaks ? g->ntweaks : 1), sizeof(double));
    for (int i = 0; i < g->ntweaks; i++) w->tweak[i] = g->tweaks[i].def;
    if (tweaks_apply(w, tweaks, ntweaks, err, errlen)) { mcgen_world_free(w); return MCGEN_E_ARG; }
    tweaks_prepare(w);
    int rcode = MCGEN_OK;
    if (g->newf) {
        NEnv env = { w, env_noise, env_blended, env_end, env_ref, env_range, g->version >= V26_4, env_scale };
        w->nc = nc_new(&env);
        for (int k = 0; k < RF__COUNT && !rcode; k++) {
            if (!ns->rf[k]) continue;
            w->s_rf[k] = nc_get(w->nc, world_rf(w, k), err, errlen);
            if (!w->s_rf[k]) rcode = MCGEN_E_DATA;
        }
        for (int k = 0; k < AQ__COUNT && !rcode && ns->has_aquifers; k++) {
            w->s_aq[k] = nc_get(w->nc, world_aq(w, k), err, errlen);
            if (!w->s_aq[k]) rcode = MCGEN_E_DATA;
        }
        if (!rcode && veins_init(w, err, errlen)) rcode = MCGEN_E_DATA;
    } else {
        w->old = old_wire_new(w, err, errlen);
        if (!w->old) rcode = MCGEN_E_DATA;
    }
    if (!rcode && surface_world_init(w, err, errlen)) rcode = MCGEN_E_DATA;   /* стадия SURFACE: правила и шумы (surface.c) */
    if (rcode) { mcgen_world_free(w); return rcode; }
    *out = w;
    return MCGEN_OK;
}

void mcgen_world_free(McWorld *w) {
    if (!w) return;
    features_world_free(w);
    carvers_world_free(w);
    surface_world_free(w);
    veins_free(w);
    nc_free(w->nc);
    tweaks_release(w);
    if (w->old) old_wire_free(w->old);
    for (int i = 0; i < w->nblended; i++) { ns_free(&w->blended[i]->min_lim); ns_free(&w->blended[i]->max_lim); ns_free(&w->blended[i]->main); free(w->blended[i]); }
    sm_free(&w->noise_inst, noise_inst_free);
    sm_free(&w->climate_noises, NULL);
    mutex_free(w->lock);
    free(w->tweak);
    free(w);
}
int mcgen_world_min_y(const McWorld *w) { return w ? w->min_y : 0; }
int mcgen_world_height(const McWorld *w) { return w ? w->height : 0; }
int mcgen_world_sea_level(const McWorld *w) { return w ? w->sea_level : 0; }

/* G1: значение зарегистрированной density-функции в точках, без кэшей (как RandomState.sampleBlockValueUncached) */
static const char *RF_NAMES[RF__COUNT] = { "temperature", "vegetation", "continents", "erosion", "depth", "ridges", "final_density",
    "chunk_surface_level", "preliminary_surface_level", "barrier", "fluid_level_floodedness", "fluid_level_spread", "lava", "vein_toggle", "vein_ridged", "vein_gap" };
static const char *AQ_NAMES[AQ__COUNT] = { "barrier", "fluid_level_floodedness", "fluid_level_spread", "lava", "exclusion", "surface_level" };
/* id: зарегистрированная функция, «rf:<поле роутера>» или «aq:<поле aquifers>» (26.3+) */
int world_df_point(McWorld *w, const char *id, int n, const int *xyz, double *out, char *err, size_t errlen) {
    const Df *f = NULL;
    if (!strncmp(id, "rf:", 3)) { for (int k = 0; k < RF__COUNT; k++) if (!strcmp(id + 3, RF_NAMES[k])) f = world_rf(w, k); }
    else if (!strncmp(id, "aq:", 3)) { for (int k = 0; k < AQ__COUNT; k++) if (!strcmp(id + 3, AQ_NAMES[k])) f = world_aq(w, k); }
    else f = world_df_lookup(w, id);
    if (!f) { set_err(err, errlen, "нет density_function %s", id); return MCGEN_E_ARG; }
    if (w->g->newf) {
        const S *s = nc_get(w->nc, f, err, errlen);
        if (!s) return MCGEN_E_DATA;
        SCtx *x = sctx_new(w->nc, 0);
        for (int i = 0; i < n; i++) out[i] = (double)s_value(x, s, xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]);
        sctx_free(x);
        return MCGEN_OK;
    }
    return old_df_point(w->old, f, n, xyz, out, err, errlen);
}

/* pack.c — McGen: чтение pack-каталога (датапак + отчёты генератора данных), реестры, измерения и пресеты. */
#include "mcgen_internal.h"
#include <stdio.h>
#include <stdlib.h>

const McTweakInfo *tweaks_table(int *n);   /* tweaks.c */

const char *mcgen_version(void) { return "libmcgen " MCGEN_SEMVER " abi 1"; }
int mcgen_abi_version(void) { return MCGEN_ABI_VERSION; }

static int parse_version(const char *v) {
    if (!v) return -1;
    if (!strcmp(v, "26.1")) return V26_1;
    if (!strcmp(v, "26.2")) return V26_2;
    if (!strcmp(v, "26.3")) return V26_3;
    if (!strncmp(v, "26.4", 4)) return V26_4;
    /* более новые 26.x — поведение последней известной версии (данные читаются из pack) */
    if (!strncmp(v, "26.", 3) && atoi(v + 3) >= 4) return V26_4;
    return -1;
}

/* ---------------- блоки ---------------- */
static int load_blocks(McGen *g, char *err, size_t errlen) {
    char *path = xsprintf("%s/reports/blocks.json", g->pack);
    JsDoc *d = js_parse_file(path, err, errlen); free(path);
    if (!d) return MCGEN_E_IO;
    Js *root = js_root(d);
    int maxid = -1;
    for (int i = 0; i < root->n; i++) {
        Js *st = js_get(root->items[i], "states");
        for (int k = 0; st && k < st->n; k++) { int id = js_int(js_get(st->items[k], "id"), -1); if (id > maxid) maxid = id; }
    }
    if (maxid < 0 || maxid > 65534) { set_err(err, errlen, "blocks.json: плохие id (%d)", maxid); js_free(d); return MCGEN_E_DATA; }
    g->nstates = maxid + 1;
    g->state_names = xcalloc((size_t)g->nstates, sizeof(char *));
    g->state_block = xcalloc((size_t)g->nstates, sizeof(u16));
    g->nblocks = root->n; g->blk_water = g->blk_lava = -1;
    for (int i = 0; i < root->n; i++) {
        const char *bname = root->keys[i];
        if (!strcmp(bname, "minecraft:water")) g->blk_water = i;
        if (!strcmp(bname, "minecraft:lava")) g->blk_lava = i;
        Js *st = js_get(root->items[i], "states");
        for (int k = 0; st && k < st->n; k++) {
            Js *s = st->items[k];
            int id = js_int(js_get(s, "id"), -1);
            if (id >= 0) g->state_block[id] = (u16)i;
            Js *pr = js_get(s, "properties");
            StrBuf b = {0};
            sb_puts(&b, bname);
            if (js_is_obj(pr) && pr->n > 0) {
                sb_putc(&b, '[');
                for (int p = 0; p < pr->n; p++) { if (p) sb_putc(&b, ','); sb_printf(&b, "%s=%s", pr->keys[p], js_str(pr->items[p], "")); }
                sb_putc(&b, ']');
            }
            char *nm = sb_take(&b);
            if (id >= 0 && !g->state_names[id]) g->state_names[id] = nm; else free(nm);
            sm_put(&g->state_ids, g->state_names[id], (void *)(intptr_t)(id + 1));
            if (js_bool(js_get(s, "default"), 0)) sm_put(&g->state_ids, bname, (void *)(intptr_t)(id + 1));
        }
    }
    for (int i = 0; i < g->nstates; i++) if (!g->state_names[i]) g->state_names[i] = xstrdup("minecraft:air");
    js_free(d);
    g->st_air = gen_state_id(g, "minecraft:air");
    g->st_stone = gen_state_id(g, "minecraft:stone");
    g->st_water = gen_state_id(g, "minecraft:water");
    g->st_lava = gen_state_id(g, "minecraft:lava");
    g->st_cave_air = gen_state_id(g, "minecraft:cave_air");
    if (g->st_air < 0 || g->st_water < 0 || g->st_lava < 0) { set_err(err, errlen, "blocks.json: нет air/water/lava"); return MCGEN_E_DATA; }
    return 0;
}

int gen_state_id(const McGen *g, const char *name) {
    char *full = NULL;
    if (!strchr(name, ':') || (strchr(name, '[') && strchr(name, ':') > strchr(name, '['))) { full = xsprintf("minecraft:%s", name); name = full; }
    intptr_t v = (intptr_t)sm_get(&g->state_ids, name);
    if (!v && strchr(name, '[')) {
        /* свойства в другом порядке/частично: сопоставление по множеству пар */
        char *base = xstrndup(name, (size_t)(strchr(name, '[') - name));
        intptr_t def = (intptr_t)sm_get(&g->state_ids, base);
        if (def) {
            /* перебор состояний того же блока с совпадением указанных свойств */
            size_t bl = strlen(base);
            const char *want = strchr(name, '[') + 1;
            for (int id = 0; id < g->nstates && !v; id++) {
                const char *sn = g->state_names[id];
                if (strncmp(sn, base, bl) || (sn[bl] != '[' && sn[bl] != 0)) continue;
                int ok = 1;
                const char *p = want;
                while (*p && *p != ']' && ok) {
                    const char *e = p; while (*e && *e != ',' && *e != ']') e++;
                    char *kv = xstrndup(p, (size_t)(e - p));
                    char *pat1 = xsprintf("[%s,", kv), *pat2 = xsprintf(",%s,", kv), *pat3 = xsprintf(",%s]", kv), *pat4 = xsprintf("[%s]", kv);
                    if (!strstr(sn, pat1) && !strstr(sn, pat2) && !strstr(sn, pat3) && !strstr(sn, pat4)) ok = 0;
                    free(kv); free(pat1); free(pat2); free(pat3); free(pat4);
                    p = *e == ',' ? e + 1 : e;
                }
                if (ok) v = id + 1;
            }
        }
        free(base);
    }
    free(full);
    return v ? (int)v - 1 : -1;
}
int gen_state_from_json(const McGen *g, const Js *v) {
    if (js_is_str(v)) return gen_state_id(g, v->s);
    if (!js_is_obj(v)) return -1;
    const char *nm = js_str(js_get(v, "Name"), NULL);
    if (!nm) return -1;
    Js *pr = js_get(v, "Properties");
    if (!js_is_obj(pr) || pr->n == 0) return gen_state_id(g, nm);
    StrBuf b = {0}; sb_puts(&b, nm); sb_putc(&b, '[');
    for (int i = 0; i < pr->n; i++) { if (i) sb_putc(&b, ','); sb_printf(&b, "%s=%s", pr->keys[i], js_str(pr->items[i], "")); }
    sb_putc(&b, ']');
    char *s = sb_take(&b); int id = gen_state_id(g, s); free(s); return id;
}

/* ---------------- биомы ---------------- */
static int load_biomes(McGen *g, char *err, size_t errlen) {
    char *dir = xsprintf("%s/data/minecraft/worldgen/biome", g->pack);
    char **files; int n = list_dir_recursive(dir, ".json", &files); free(dir);
    if (n <= 0) { set_err(err, errlen, "нет worldgen/biome в %s", g->pack); return MCGEN_E_DATA; }
    if (n > 255) { set_err(err, errlen, "биомов больше 255 (%d)", n); free_str_list(files, n); return MCGEN_E_DATA; }
    g->nbiomes = n; g->biome_names = xcalloc((size_t)n, sizeof(char *));
    for (int i = 0; i < n; i++) {
        files[i][strlen(files[i]) - 5] = 0;
        g->biome_names[i] = xsprintf("minecraft:%s", files[i]);
        sm_put(&g->biome_ids, g->biome_names[i], (void *)(intptr_t)(i + 1));
    }
    free_str_list(files, n);
    return 0;
}
int gen_biome_id(const McGen *g, const char *name) {
    char *full = strchr(name, ':') ? NULL : xsprintf("minecraft:%s", name);
    intptr_t v = (intptr_t)sm_get(&g->biome_ids, full ? full : name);
    free(full);
    return v ? (int)v - 1 : -1;
}

/* ---------------- шумы и функции ---------------- */
static int load_noises(McGen *g, char *err, size_t errlen) {
    char *dir = xsprintf("%s/data/minecraft/worldgen/noise", g->pack);
    char **files; int n = list_dir_recursive(dir, ".json", &files);
    int rc = 0;
    for (int i = 0; i < n && !rc; i++) {
        char *path = xsprintf("%s/%s", dir, files[i]);
        JsDoc *d = js_parse_file(path, err, errlen); free(path);
        if (!d) { rc = MCGEN_E_DATA; break; }
        NoiseParams *P = xcalloc(1, sizeof *P);
        char e2[200];
        if (noise_params_parse(js_root(d), g->newf, P, e2, sizeof e2)) { set_err(err, errlen, "noise/%s: %s", files[i], e2); free(P); rc = MCGEN_E_DATA; }
        else { files[i][strlen(files[i]) - 5] = 0; char *id = xsprintf("minecraft:%s", files[i]); sm_put(&g->noises, id, P); free(id); }
        js_free(d);
    }
    free_str_list(files, n); free(dir);
    if (!rc && n == 0) { set_err(err, errlen, "нет worldgen/noise"); rc = MCGEN_E_DATA; }
    return rc;
}
static int load_dfs(McGen *g, char *err, size_t errlen) {
    char *dir = xsprintf("%s/data/minecraft/worldgen/density_function", g->pack);
    char **files; int n = list_dir_recursive(dir, ".json", &files);
    int rc = 0;
    for (int i = 0; i < n && !rc; i++) {
        char *path = xsprintf("%s/%s", dir, files[i]);
        JsDoc *d = js_parse_file(path, err, errlen); free(path);
        if (!d) { rc = MCGEN_E_DATA; break; }
        char e2[300];
        Df *f = df_parse(js_root(d), g->newf, e2, sizeof e2);
        if (!f) { set_err(err, errlen, "density_function/%s: %s", files[i], e2); rc = MCGEN_E_DATA; }
        else { files[i][strlen(files[i]) - 5] = 0; char *id = xsprintf("minecraft:%s", files[i]); sm_put(&g->dfs, id, f); free(id); }
        js_free(d);
    }
    free_str_list(files, n); free(dir);
    return rc;
}

static const char *RF_KEYS[RF__COUNT] = {
    "temperature", "vegetation", "continents", "erosion", "depth", "ridges", "final_density",
    "chunk_surface_level", "preliminary_surface_level",
    "barrier", "fluid_level_floodedness", "fluid_level_spread", "lava", "vein_toggle", "vein_ridged", "vein_gap"
};
static const char *AQ_KEYS[AQ__COUNT] = { "barrier", "fluid_level_floodedness", "fluid_level_spread", "lava", "exclusion", "surface_level" };

static void ns_free_cb(void *p) {
    NoiseSettings *s = p; if (!s) return;
    for (int i = 0; i < RF__COUNT; i++) df_free(s->rf[i]);
    for (int i = 0; i < AQ__COUNT; i++) df_free(s->aq[i]);
    js_free(s->doc); free(s->id); free(s);
}
static int load_noise_settings(McGen *g, char *err, size_t errlen) {
    char *dir = xsprintf("%s/data/minecraft/worldgen/noise_settings", g->pack);
    char **files; int n = list_dir_recursive(dir, ".json", &files);
    int rc = 0;
    for (int i = 0; i < n && !rc; i++) {
        char *path = xsprintf("%s/%s", dir, files[i]);
        JsDoc *d = js_parse_file(path, err, errlen); free(path);
        if (!d) { rc = MCGEN_E_DATA; break; }
        Js *r = js_root(d);
        files[i][strlen(files[i]) - 5] = 0;
        NoiseSettings *s = xcalloc(1, sizeof *s);
        s->id = xsprintf("minecraft:%s", files[i]);
        s->doc = d;
        Js *nz = js_get(r, "noise");
        s->min_y = js_int(js_get(nz, "min_y"), 0); s->height = js_int(js_get(nz, "height"), 0);
        s->size_h = js_int(js_get(nz, "size_horizontal"), 1); s->size_v = js_int(js_get(nz, "size_vertical"), 2);
        s->sea_level = js_int(js_get(r, "sea_level"), 63);
        s->legacy_random = js_bool(js_get(r, "legacy_random_source"), 0);
        s->aquifers_enabled = js_bool(js_get(r, "aquifers_enabled"), 0);
        s->ore_veins_enabled = js_bool(js_get(r, "ore_veins_enabled"), 0);
        Js *db = js_get(r, "default_block");
        /* 26.4-snapshot-2: default_block убран из noise_settings — в коде игры камень/незерак/эндерняк по умолчанию */
        s->default_block = db ? gen_state_from_json(g, db) : -1;
        s->default_fluid = gen_state_from_json(g, js_get(r, "default_fluid"));
        if (s->default_fluid < 0) { set_err(err, errlen, "noise_settings/%s: default_fluid", files[i]); rc = MCGEN_E_DATA; ns_free_cb(s); break; }
        Js *router = js_get(r, "noise_router");
        char e2[300];
        for (int k = 0; k < RF__COUNT && !rc; k++) {
            Js *v = js_get(router, RF_KEYS[k]);
            if (!v) continue;
            s->rf[k] = df_parse(v, g->newf, e2, sizeof e2);
            if (!s->rf[k]) { set_err(err, errlen, "noise_settings/%s.noise_router.%s: %s", files[i], RF_KEYS[k], e2); rc = MCGEN_E_DATA; }
        }
        Js *aq = js_get(r, "aquifers");
        if (js_is_obj(aq) && !rc) {
            s->has_aquifers = 1;
            for (int k = 0; k < AQ__COUNT && !rc; k++) {
                Js *v = js_get(aq, AQ_KEYS[k]);
                if (!v) { set_err(err, errlen, "noise_settings/%s.aquifers: нет %s", files[i], AQ_KEYS[k]); rc = MCGEN_E_DATA; break; }
                s->aq[k] = df_parse(v, 1, e2, sizeof e2);
                if (!s->aq[k]) { set_err(err, errlen, "noise_settings/%s.aquifers.%s: %s", files[i], AQ_KEYS[k], e2); rc = MCGEN_E_DATA; }
            }
        }
        if (!s->rf[RF_FINAL_DENSITY] && !rc) { set_err(err, errlen, "noise_settings/%s: нет final_density", files[i]); rc = MCGEN_E_DATA; }
        if (rc) { ns_free_cb(s); break; }
        sm_put(&g->nsettings, s->id, s);
    }
    free_str_list(files, n); free(dir);
    return rc;
}

/* ---------------- измерения и пресеты ---------------- */
static int dim_type_dims(McGen *g, const char *id, int *min_y, int *height, int *fast_lava) {
    const char *nm = strchr(id, ':') ? strchr(id, ':') + 1 : id;
    char *path = xsprintf("%s/data/minecraft/dimension_type/%s.json", g->pack, nm);
    JsDoc *d = js_parse_file(path, NULL, 0); free(path);
    if (!d) return -1;
    Js *r = js_root(d);
    *min_y = js_int(js_get(r, "min_y"), 0); *height = js_int(js_get(r, "height"), 256);
    *fast_lava = js_bool(js_get(js_get(r, "attributes"), "minecraft:gameplay/fast_lava"), 0) || js_bool(js_get(r, "ultrawarm"), 0);
    js_free(d); return 0;
}
static void add_preset(McGen *g, int dk, const char *name, const char *settings, int bs, int fixed, const char *dim_type) {
    McDim *D = &g->dims[dk];
    for (int i = 0; i < D->n; i++)   /* одинаковые (настройки, источник биомов) — один пресет (первое имя) */
        if (!strcmp(D->p[i].settings, settings) && D->p[i].biome_source == bs && D->p[i].fixed_biome == fixed && !strcmp(D->p[i].dim_type, dim_type)) return;
    for (int i = 0; i < D->n; i++) if (!strcmp(D->p[i].name, name)) return;
    D->p = xrealloc(D->p, sizeof(McPreset) * (size_t)(D->n + 1));
    McPreset *p = &D->p[D->n++];
    memset(p, 0, sizeof *p);
    p->name = xstrdup(name); p->settings = xstrdup(settings); p->biome_source = bs; p->fixed_biome = fixed; p->dim_type = xstrdup(dim_type);
    if (dim_type_dims(g, dim_type, &p->min_y, &p->height, &p->fast_lava)) { p->min_y = 0; p->height = 256; }
}
static int cmp_preset_name(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    if (!strcmp(x, "normal.json")) return -1;
    if (!strcmp(y, "normal.json")) return 1;
    return strcmp(x, y);
}
static int load_presets(McGen *g, char *err, size_t errlen) {
    static const char *DN[3] = { "minecraft:overworld", "minecraft:the_nether", "minecraft:the_end" };
    g->ndims = 3;
    for (int i = 0; i < 3; i++) { g->dims[i].name = xstrdup(DN[i]); g->dims[i].kind = i; }
    char *dir = xsprintf("%s/data/minecraft/worldgen/world_preset", g->pack);
    char **files; int n = list_dir_recursive(dir, ".json", &files);
    qsort(files, (size_t)n, sizeof(char *), cmp_preset_name);
    for (int i = 0; i < n; i++) {
        char *path = xsprintf("%s/%s", dir, files[i]);
        JsDoc *d = js_parse_file(path, NULL, 0); free(path);
        if (!d) continue;
        files[i][strlen(files[i]) - 5] = 0;
        Js *dims = js_get(js_root(d), "dimensions");
        for (int k = 0; dims && k < dims->n; k++) {
            int dk = -1; for (int q = 0; q < 3; q++) if (!strcmp(dims->keys[k], DN[q])) dk = q;
            if (dk < 0) continue;
            Js *stem = dims->items[k], *gen = js_get(stem, "generator");
            const char *gt = js_str(js_get(gen, "type"), "");
            if (strcmp(gt, "minecraft:noise") && strcmp(gt, "noise")) continue;
            const char *st = js_str(js_get(gen, "settings"), NULL);
            if (!st) continue;   /* встроенные настройки в пресете — не поддерживаются */
            Js *bsrc = js_get(gen, "biome_source");
            const char *bt = js_str(js_get(bsrc, "type"), "");
            int bs = -1, fixed = -1;
            if (strstr(bt, "multi_noise")) {
                const char *pr = js_str(js_get(bsrc, "preset"), "");
                bs = strstr(pr, "nether") ? BS_MULTI_NETHER : BS_MULTI_OVERWORLD;
            } else if (strstr(bt, "the_end")) bs = BS_THE_END;
            else if (strstr(bt, "fixed")) { bs = BS_FIXED; fixed = gen_biome_id(g, js_str(js_get(bsrc, "biome"), "")); if (fixed < 0) bs = -1; }
            if (bs < 0) continue;
            char *sid = df_full_id(st);
            char *dtid = df_full_id(js_str(js_get(stem, "type"), DN[dk]));
            add_preset(g, dk, files[i], sid, bs, fixed, dtid);
            free(sid); free(dtid);
        }
        js_free(d);
    }
    free_str_list(files, n); free(dir);
    /* noise_settings, на которые не ссылается ни один пресет (caves, floating_islands): мир Overworld с этим рельефом */
    for (int i = 0; i < g->nsettings.cap; i++) {
        if (!g->nsettings.e[i].key) continue;
        const char *sid = g->nsettings.e[i].key;
        int used = 0;
        for (int dk = 0; dk < 3; dk++) for (int p = 0; p < g->dims[dk].n; p++) if (!strcmp(g->dims[dk].p[p].settings, sid)) used = 1;
        if (used) continue;
        const char *nm = strchr(sid, ':') + 1;
        char *dtpath = xsprintf("%s/data/minecraft/dimension_type/overworld_%s.json", g->pack, nm);
        char *dt = file_exists(dtpath) ? xsprintf("minecraft:overworld_%s", nm) : xstrdup("minecraft:overworld");
        free(dtpath);
        add_preset(g, 0, nm, sid, BS_MULTI_OVERWORLD, -1, dt);
        free(dt);
    }
    for (int dk = 0; dk < 3; dk++) if (g->dims[dk].n == 0) { set_err(err, errlen, "нет пресетов для %s", DN[dk]); return MCGEN_E_DATA; }
    return 0;
}

const McPreset *gen_find_preset(const McGen *g, const char *dim, const char *preset, int *dim_kind) {
    int dk = -1;
    for (int i = 0; i < g->ndims; i++) {
        const char *n = g->dims[i].name;
        if (!strcmp(n, dim) || !strcmp(n + 10, dim)) dk = i;
    }
    if (dk < 0) {
        if (!strcmp(dim, "overworld")) dk = 0; else if (!strcmp(dim, "nether")) dk = 1; else if (!strcmp(dim, "end")) dk = 2;
    }
    if (dk < 0) return NULL;
    if (dim_kind) *dim_kind = dk;
    const McDim *D = &g->dims[dk];
    if (!preset || !*preset) return &D->p[0];
    const char *pn = strchr(preset, ':') ? strchr(preset, ':') + 1 : preset;
    for (int i = 0; i < D->n; i++) if (!strcmp(D->p[i].name, pn)) return &D->p[i];
    /* имя мирового пресета, совпавшего по настройкам с другим (например nether в large_biomes) */
    char *path = xsprintf("%s/data/minecraft/worldgen/world_preset/%s.json", g->pack, pn);
    JsDoc *d = js_parse_file(path, NULL, 0); free(path);
    const McPreset *found = NULL;
    if (d) {
        Js *stem = js_get(js_get(js_root(d), "dimensions"), D->name);
        const char *st = js_str(js_get(js_get(stem, "generator"), "settings"), NULL);
        if (st) { char *sid = df_full_id(st); for (int i = 0; i < D->n && !found; i++) if (!strcmp(D->p[i].settings, sid)) found = &D->p[i]; free(sid); }
        js_free(d);
    }
    return found;
}

/* ---------------- теги блоков (data/minecraft/tags/block) ---------------- */
static int tag_resolve(const McGen *g, const char *name, u8 *out, int depth) {
    if (depth > 32) return -1;
    char *full = df_full_id(name);
    char *path = xsprintf("%s/data/%.*s/tags/block/%s.json", g->pack, (int)(strchr(full, ':') - full), full, strchr(full, ':') + 1);
    free(full);
    JsDoc *d = js_parse_file(path, NULL, 0); free(path);
    if (!d) return -1;
    Js *vals = js_get(js_root(d), "values");
    for (int i = 0; vals && i < vals->n; i++) {
        const Js *v = vals->items[i];
        const char *id = js_is_str(v) ? v->s : js_str(js_get(v, "id"), NULL);
        if (!id) continue;
        if (id[0] == '#') tag_resolve(g, id + 1, out, depth + 1);
        else {
            int st = gen_state_id(g, id);
            if (st >= 0) out[g->state_block[st]] = 1;
        }
    }
    js_free(d);
    return 0;
}
/* массив [nblocks] (1 — блок в теге); кэшируется в McGen; потокобезопасно после первого вызова для тега */
const u8 *gen_block_tag(const McGen *gc, const char *name) {
    McGen *g = (McGen *)gc;
    mutex_lock(g->lock);
    u8 *t = sm_get(&g->tags, name);
    if (!t) {
        t = xcalloc((size_t)(g->nblocks ? g->nblocks : 1), 1);
        tag_resolve(g, name, t, 0);
        sm_put(&g->tags, name, t);
    }
    mutex_unlock(g->lock);
    return t;
}

/* ---------------- открыть/закрыть ---------------- */
static void df_free_cb(void *p) { df_free(p); }
int mcgen_open(const char *pack_dir, const char *version, McGen **out, char *err, size_t errlen) {
    if (!pack_dir || !out) { set_err(err, errlen, "mcgen_open: пустой аргумент"); return MCGEN_E_ARG; }
    *out = NULL;
    int v = parse_version(version);
    if (v < 0) { set_err(err, errlen, "неизвестная версия '%s' (26.1|26.2|26.3|26.4-snapshot-2)", version ? version : ""); return MCGEN_E_VERSION; }
    McGen *g = xcalloc(1, sizeof *g);
    g->version = v; g->newf = v >= V26_3; g->pack = xstrdup(pack_dir);
    g->lock = mutex_new();
    int rc;
    if ((rc = load_blocks(g, err, errlen)) || (rc = load_biomes(g, err, errlen)) || (rc = load_noises(g, err, errlen)) ||
        (rc = load_dfs(g, err, errlen)) || (rc = load_noise_settings(g, err, errlen)) || (rc = load_presets(g, err, errlen)) ||
        (rc = biome_params_build(g, err, errlen))) {
        mcgen_close(g); return rc;
    }
    g->tweaks = tweaks_table(&g->ntweaks);
    gen_compute_state_classes(g);
    *out = g;
    return MCGEN_OK;
}
void mcgen_close(McGen *g) {
    if (!g) return;
    for (int i = 0; i < g->nstates; i++) free(g->state_names[i]);
    free(g->state_names); sm_free(&g->state_ids, NULL);
    for (int i = 0; i < g->nbiomes; i++) free(g->biome_names[i]);
    free(g->biome_names); sm_free(&g->biome_ids, NULL);
    sm_free(&g->noises, free); sm_free(&g->dfs, df_free_cb); sm_free(&g->nsettings, ns_free_cb);
    for (int d = 0; d < 3; d++) {
        for (int i = 0; i < g->dims[d].n; i++) { free(g->dims[d].p[i].name); free(g->dims[d].p[i].settings); free(g->dims[d].p[i].dim_type); }
        free(g->dims[d].p); free(g->dims[d].name);
    }
    biome_params_free(g);
    bs_free(g->bs_tab);
    free(g->state_cls); free(g->state_block);
    sm_free(&g->tags, free);
    if (g->lock) mutex_free(g->lock);
    free(g->pack); free(g);
}

int mcgen_dimension_count(const McGen *g) { return g ? g->ndims : 0; }
const char *mcgen_dimension_name(const McGen *g, int i) { return (g && i >= 0 && i < g->ndims) ? g->dims[i].name : NULL; }
int mcgen_preset_count(const McGen *g, const char *dimension) {
    int dk; const McPreset *p = g ? gen_find_preset(g, dimension, NULL, &dk) : NULL;
    return p ? g->dims[dk].n : 0;
}
const char *mcgen_preset_name(const McGen *g, const char *dimension, int i) {
    int dk; const McPreset *p = g ? gen_find_preset(g, dimension, NULL, &dk) : NULL;
    if (!p || i < 0 || i >= g->dims[dk].n) return NULL;
    return g->dims[dk].p[i].name;
}
int mcgen_block_state_count(const McGen *g) { return g ? g->nstates : 0; }
const char *mcgen_block_state_name(const McGen *g, int id) { return (g && id >= 0 && id < g->nstates) ? g->state_names[id] : NULL; }
int mcgen_block_state_from_name(const McGen *g, const char *name) { return (g && name) ? gen_state_id(g, name) : -1; }
int mcgen_biome_count(const McGen *g) { return g ? g->nbiomes : 0; }
const char *mcgen_biome_name(const McGen *g, int id) { return (g && id >= 0 && id < g->nbiomes) ? g->biome_names[id] : NULL; }
int mcgen_tweak_count(const McGen *g) { return g ? g->ntweaks : 0; }
const McTweakInfo *mcgen_tweak_info(const McGen *g, int i) { return (g && i >= 0 && i < g->ntweaks) ? &g->tweaks[i] : NULL; }

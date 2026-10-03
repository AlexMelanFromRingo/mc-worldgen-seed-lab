/* blockstate.c — см. blockstate.h */
#include "blockstate.h"
#include <stdio.h>
#include <stdlib.h>

int hm_type_from_name(const char *s) {
    static const char *N[HM__COUNT] = { "WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG", "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES" };
    if (!s) return -1;
    if (!strncmp(s, "minecraft:", 10)) s += 10;
    for (int i = 0; i < HM__COUNT; i++) if (!strcmp(s, N[i])) return i;
    return -1;
}

static const char *js_scalar_text(const Js *v, char *buf, size_t n) {
    if (!v) return NULL;
    if (v->t == JS_STR) return v->s;
    if (v->t == JS_BOOL) return v->d != 0.0 ? "true" : "false";
    if (v->t == JS_NUM) { if (v->s) return v->s; snprintf(buf, n, "%d", (int)v->d); return buf; }
    return NULL;
}

/* ---------------- загрузка blocks.json ---------------- */
static int load_blocks(BsTab *t, const McGen *g) {
    char *path = xsprintf("%s/reports/blocks.json", g->pack);
    JsDoc *d = js_parse_file(path, NULL, 0); free(path);
    if (!d) return -1;
    Js *root = js_root(d);
    t->nblocks = root->n; t->blk = xcalloc((size_t)root->n, sizeof(BsBlock));
    for (int i = 0; i < root->n; i++) {
        BsBlock *b = &t->blk[i];
        b->name = xstrdup(root->keys[i]);
        sm_put(&t->block_ids, b->name, (void *)(intptr_t)(i + 1));
        Js *props = js_get(root->items[i], "properties");
        b->nprops = js_is_obj(props) ? props->n : 0;
        if (b->nprops) {
            b->pname = xcalloc((size_t)b->nprops, sizeof(char *)); b->pn = xcalloc((size_t)b->nprops, sizeof(int));
            b->pval = xcalloc((size_t)b->nprops, sizeof(char **)); b->stride = xcalloc((size_t)b->nprops, sizeof(int));
            for (int p = 0; p < b->nprops; p++) {
                b->pname[p] = xstrdup(props->keys[p]);
                Js *vals = props->items[p];
                b->pn[p] = vals->n; b->pval[p] = xcalloc((size_t)(vals->n ? vals->n : 1), sizeof(char *));
                for (int k = 0; k < vals->n; k++) { char buf[32]; const char *s = js_scalar_text(vals->items[k], buf, sizeof buf); b->pval[p][k] = xstrdup(s ? s : ""); }
            }
            int st = 1;
            for (int p = b->nprops - 1; p >= 0; p--) { b->stride[p] = st; st *= b->pn[p] ? b->pn[p] : 1; }
        }
        Js *sts = js_get(root->items[i], "states");
        int first = 1 << 30, last = -1, def = -1;
        for (int k = 0; sts && k < sts->n; k++) {
            int id = js_int(js_get(sts->items[k], "id"), -1);
            if (id < first) first = id;
            if (id > last) last = id;
            if (js_bool(js_get(sts->items[k], "default"), 0)) def = id;
        }
        b->first = last >= 0 ? first : 0; b->count = last >= 0 ? last - first + 1 : 0;
        b->def = def >= 0 ? def : b->first;
        /* проверка: id = first + Σ idx·stride */
        b->radix_ok = (sts && sts->n == b->count);
        for (int k = 0; sts && k < sts->n && b->radix_ok; k++) {
            int id = js_int(js_get(sts->items[k], "id"), -1);
            Js *pr = js_get(sts->items[k], "properties");
            int want = b->first;
            for (int p = 0; p < b->nprops; p++) {
                char buf[32]; const char *vs = pr ? js_scalar_text(js_get(pr, b->pname[p]), buf, sizeof buf) : NULL;
                int vi = -1;
                for (int q = 0; vs && q < b->pn[p]; q++) if (!strcmp(b->pval[p][q], vs)) { vi = q; break; }
                if (vi < 0) { b->radix_ok = 0; break; }
                want += vi * b->stride[p];
            }
            if (want != id) b->radix_ok = 0;
        }
    }
    js_free(d);
    return 0;
}

/* ---------------- block_flags.json или эвристика ---------------- */
static int load_flags(BsTab *t, const McGen *g) {
    char *path = xsprintf("%s/reports/block_flags.json", g->pack);
    JsDoc *d = js_parse_file(path, NULL, 0); free(path);
    if (!d) return 0;
    Js *root = js_root(d);
    Js *fl = js_get(root, "flags"), *fu = js_get(root, "fluid"), *st = js_get(root, "sturdy"), *bl = js_get(root, "blocks");
    int ok = js_is_arr(fl) && js_is_arr(fu) && js_is_arr(st) && fl->n == t->nstates && fu->n == t->nstates && st->n == t->nstates;
    if (ok) {
        for (int i = 0; i < t->nstates; i++) {
            t->flags[i] = (u32)(i64)fl->items[i]->d; t->fluid[i] = (u16)(i32)fu->items[i]->d; t->sturdy[i] = (u8)(i32)st->items[i]->d;
        }
        for (int i = 0; bl && i < bl->n; i++) {
            int bi = bs_block_index(t, js_str(js_get(bl->items[i], "name"), ""));
            if (bi < 0) continue;
            t->blk[bi].cls = xstrdup(js_str(js_get(bl->items[i], "cls"), ""));
            StrBuf sb = {0}; sb_putc(&sb, '|'); sb_puts(&sb, t->blk[bi].cls); sb_putc(&sb, '|');
            Js *sup = js_get(bl->items[i], "sup");
            for (int k = 0; sup && k < sup->n; k++) { sb_puts(&sb, js_str(sup->items[k], "")); sb_putc(&sb, '|'); }
            t->blk[bi].chain = sb_take(&sb);
        }
        t->exported = 1;
    }
    js_free(d);
    return ok;
}

/* эвристика (без block_flags.json): грубо, только чтобы стадия работала */
static void heuristic_flags(BsTab *t, const McGen *g) {
    static const char *REPL[] = { "grass", "fern", "dead_bush", "vine", "seagrass", "snow[layers=1", "fire", "light[", "structure_void", "hanging_roots",
                                  "glow_lichen", "sculk_vein", "short_", "tall_", "bush", "moss_carpet", NULL };
    for (int i = 0; i < t->nstates; i++) {
        const char *n = g->state_names[i]; const char *b = strchr(n, ':') ? strchr(n, ':') + 1 : n;
        u32 f = 0;
        int air = gen_is_air(g, i);
        int liq = !strncmp(b, "water", 5) || !strncmp(b, "lava", 4) || !strncmp(b, "bubble_column", 13);
        if (air) f |= BSF_AIR | BSF_REPLACEABLE | BSF_SKY;
        if (liq) f |= BSF_LIQUID | BSF_REPLACEABLE;
        for (int k = 0; REPL[k] && !air; k++) if (strstr(b, REPL[k]) && strcmp(b, "grass_block")) f |= BSF_REPLACEABLE;
        int motion = (g->state_cls[i] & 2) != 0;
        if (motion) f |= BSF_MOTION;
        if (motion && !strstr(n, "waterlogged=true")) f |= BSF_SOLID | BSF_OCCLUDE;
        if (strstr(b, "_leaves")) f |= BSF_LEAVES;
        t->flags[i] = f;
        u16 fl = 0;
        if (!strncmp(b, "water", 5)) { int lv = 0; const char *p = strstr(n, "level="); if (p) lv = atoi(p + 6); fl = (u16)((lv == 0 ? FL_WATER : FL_FLOWING_WATER) * 256 + (lv == 0 ? 8 : (lv & 7 ? 8 - (lv & 7) : 8)) * 16 + (lv >= 8 ? 8 : 0) + (lv == 0 ? 4 : 0)); }
        else if (!strncmp(b, "lava", 4)) { int lv = 0; const char *p = strstr(n, "level="); if (p) lv = atoi(p + 6); fl = (u16)((lv == 0 ? FL_LAVA : FL_FLOWING_LAVA) * 256 + (lv == 0 ? 8 : (lv & 7 ? 8 - (lv & 7) : 8)) * 16 + (lv >= 8 ? 8 : 0) + (lv == 0 ? 4 : 0)); }
        else if (strstr(n, "waterlogged=true") || !strncmp(b, "kelp", 4) || !strncmp(b, "seagrass", 8) || !strncmp(b, "tall_seagrass", 13) || !strncmp(b, "bubble_column", 13))
            fl = (u16)(FL_WATER * 256 + 8 * 16 + 4);
        t->fluid[i] = fl;
        t->sturdy[i] = (f & BSF_SOLID_RENDER || (f & BSF_SOLID && !(f & BSF_LEAVES))) ? 63 : 0;
        if (f & BSF_SOLID) t->sturdy[i] = 63;
    }
}

static void build_hmcls(BsTab *t, const McGen *g) {
    const u8 *tm = g->newf ? gen_block_tag(g, "minecraft:blocks_motion_in_heightmap") : NULL;
    const u8 *tn = g->newf ? gen_block_tag(g, "minecraft:blocks_motion_in_heightmap_no_leaves") : NULL;
    int tags_ok = 0;
    for (int b = 0; tm && b < g->nblocks && !tags_ok; b++) tags_ok = tm[b];
    t->hmcls = xcalloc((size_t)t->nstates, 1);
    for (int i = 0; i < t->nstates; i++) {
        u32 f = t->flags[i];
        int air = (f & BSF_AIR) != 0;
        int fluid = BS_FL_TYPE(t->fluid[i]) != FL_NONE;
        int motion, motion_nl;
        if (tags_ok) { int b = g->state_block[i]; motion = tm[b]; motion_nl = tn[b]; }
        else {
            motion = (f & BSF_MOTION) != 0;
            if (!t->exported) motion = (g->state_cls[i] & 2) != 0;
            motion_nl = motion && !(f & BSF_LEAVES);
        }
        u8 c = 0;
        if (!air) c |= (1 << HM_WORLD_SURFACE_WG) | (1 << HM_WORLD_SURFACE);
        if (motion) c |= (1 << HM_OCEAN_FLOOR_WG) | (1 << HM_OCEAN_FLOOR);
        if (motion || fluid) c |= 1 << HM_MOTION_BLOCKING;
        if (motion_nl || fluid) c |= 1 << HM_MOTION_BLOCKING_NO_LEAVES;
        t->hmcls[i] = c;
    }
}

const BsTab *bs_get(const McGen *gc) {
    McGen *g = (McGen *)gc;
    mutex_lock(g->lock);
    BsTab *have = g->bs_tab;
    mutex_unlock(g->lock);
    if (have) return have;
    /* строим без блокировки McGen (gen_block_tag берёт её сам); при гонке лишняя таблица освобождается */
    BsTab *t = xcalloc(1, sizeof *t);
    t->g = g; t->nstates = g->nstates;
    t->flags = xcalloc((size_t)t->nstates, sizeof(u32)); t->fluid = xcalloc((size_t)t->nstates, sizeof(u16)); t->sturdy = xcalloc((size_t)t->nstates, 1);
    if (load_blocks(t, g) != 0) { fprintf(stderr, "libmcgen: нет/плохой reports/blocks.json\n"); }
    if (!load_flags(t, g)) {
        if (getenv("MCGEN_FEATURES_DEBUG")) fprintf(stderr, "libmcgen: нет reports/block_flags.json (python3 libmcgen/tests/g5_blockflags.py) — эвристика\n");
        heuristic_flags(t, g);
    }
    t->st_air = g->st_air; t->st_cave_air = g->st_cave_air; t->st_void_air = gen_state_id(g, "minecraft:void_air");
    build_hmcls(t, g);
    mutex_lock(g->lock);
    if (g->bs_tab) { bs_free(t); t = g->bs_tab; } else g->bs_tab = t;
    mutex_unlock(g->lock);
    return t;
}

void bs_free(void *tab) {
    BsTab *t = tab;
    if (!t) return;
    for (int i = 0; i < t->nblocks; i++) {
        BsBlock *b = &t->blk[i];
        free(b->name); free(b->cls); free(b->chain);
        for (int p = 0; p < b->nprops; p++) { free(b->pname[p]); for (int k = 0; k < b->pn[p]; k++) free(b->pval[p][k]); free(b->pval[p]); }
        free(b->pname); free(b->pn); free(b->pval); free(b->stride);
    }
    free(t->blk); sm_free(&t->block_ids, NULL);
    free(t->flags); free(t->fluid); free(t->sturdy); free(t->hmcls); free(t);
}

int bs_block_index(const BsTab *t, const char *name) {
    char *full = NULL;
    if (!strchr(name, ':')) { full = xsprintf("minecraft:%s", name); name = full; }
    intptr_t v = (intptr_t)sm_get(&t->block_ids, name);
    free(full);
    return v ? (int)v - 1 : -1;
}

int bs_prop_index(const BsTab *t, int blk, const char *prop) {
    const BsBlock *b = &t->blk[blk];
    for (int p = 0; p < b->nprops; p++) if (!strcmp(b->pname[p], prop)) return p;
    return -1;
}
int bs_has_prop(const BsTab *t, int st, const char *prop) { return bs_prop_index(t, t->g->state_block[st], prop) >= 0; }

static int value_index(const BsBlock *b, int p, int st) {
    if (b->radix_ok) return ((st - b->first) / b->stride[p]) % b->pn[p];
    /* общий случай: по имени состояния */
    return -1;
}
int bs_get_prop(const BsTab *t, int st, const char *prop, const char **val) {
    int blk = t->g->state_block[st]; const BsBlock *b = &t->blk[blk];
    int p = bs_prop_index(t, blk, prop);
    if (p < 0) return 0;
    int vi = value_index(b, p, st);
    if (vi < 0) {
        /* без радикса: ищем «prop=значение» в имени состояния */
        const char *n = t->g->state_names[st]; char key[80]; snprintf(key, sizeof key, "%s=", prop);
        const char *s = strstr(n, key);
        if (!s) return 0;
        s += strlen(key);
        for (int q = 0; q < b->pn[p]; q++) { size_t l = strlen(b->pval[p][q]); if (!strncmp(s, b->pval[p][q], l) && (s[l] == ',' || s[l] == ']')) { vi = q; break; } }
        if (vi < 0) return 0;
    }
    if (val) *val = b->pval[p][vi];
    return 1;
}
int bs_with(const BsTab *t, int st, const char *prop, const char *val) {
    int blk = t->g->state_block[st]; const BsBlock *b = &t->blk[blk];
    int p = bs_prop_index(t, blk, prop);
    if (p < 0) return -1;
    int nv = -1;
    for (int q = 0; q < b->pn[p]; q++) if (!strcmp(b->pval[p][q], val)) { nv = q; break; }
    if (nv < 0) return -1;
    if (b->radix_ok) { int cur = value_index(b, p, st); return st + (nv - cur) * b->stride[p]; }
    /* общий случай: перебор состояний блока с теми же остальными значениями */
    const char *cur = NULL; bs_get_prop(t, st, prop, &cur);
    for (int id = b->first; id < b->first + b->count; id++) {
        int ok = 1;
        for (int q = 0; q < b->nprops && ok; q++) {
            const char *a = NULL, *c = NULL; bs_get_prop(t, id, b->pname[q], &a); bs_get_prop(t, st, b->pname[q], &c);
            if (q == p) ok = a && !strcmp(a, val); else ok = a && c && !strcmp(a, c);
        }
        if (ok) return id;
    }
    return -1;
}
int bs_try_with(const BsTab *t, int st, const char *prop, const char *val) { int r = bs_with(t, st, prop, val); return r < 0 ? st : r; }
int bs_flag(const BsTab *t, int st, int bit) { return st >= 0 && st < t->nstates && (t->flags[st] & (u32)bit) != 0; }

int bs_from_json(const BsTab *t, const Js *v) {
    if (!v) return -1;
    if (js_is_str(v)) return gen_state_id(t->g, v->s);
    if (!js_is_obj(v)) return -1;
    const char *nm = js_str(js_get(v, "Name"), NULL);
    if (!nm) nm = js_str(js_get(v, "id"), NULL);
    if (!nm) nm = js_str(js_get(v, "name"), NULL);
    if (!nm) return -1;
    int blk = bs_block_index(t, nm);
    if (blk < 0) return -1;
    int st = t->blk[blk].def;
    Js *pr = js_get(v, "Properties"); if (!pr) pr = js_get(v, "properties");
    for (int i = 0; js_is_obj(pr) && i < pr->n; i++) {
        char buf[32]; const char *vs = js_scalar_text(pr->items[i], buf, sizeof buf);
        if (!vs) return -1;
        int ns = bs_with(t, st, pr->keys[i], vs);
        if (ns < 0) return -1;
        st = ns;
    }
    return st;
}

int bs_fluid_block_from_json(const BsTab *t, const Js *v) {
    const char *id = js_is_str(v) ? v->s : js_str(js_get(v, "id"), js_str(js_get(v, "Name"), NULL));
    if (!id) return -1;
    if (!strncmp(id, "minecraft:", 10)) id += 10;
    int water = !strcmp(id, "water") || !strcmp(id, "flowing_water"), lava = !strcmp(id, "lava") || !strcmp(id, "flowing_lava");
    if (!water && !lava) return -1;
    int source = !strncmp(id, "flowing_", 8) ? 0 : 1;
    int amount = 8, falling = 0;
    Js *pr = js_is_obj(v) ? (js_get(v, "properties") ? js_get(v, "properties") : js_get(v, "Properties")) : NULL;
    for (int i = 0; js_is_obj(pr) && i < pr->n; i++) {
        char buf[32]; const char *vs = js_scalar_text(pr->items[i], buf, sizeof buf); if (!vs) continue;
        if (!strcmp(pr->keys[i], "falling")) falling = !strcmp(vs, "true");
        else if (!strcmp(pr->keys[i], "level")) amount = atoi(vs);
    }
    if (source) amount = 8;
    int level = 8 - (amount < 8 ? amount : 8) + (falling ? 8 : 0);
    int blk = bs_block_index(t, water ? "minecraft:water" : "minecraft:lava");
    char lv[16]; snprintf(lv, sizeof lv, "%d", level);
    return blk < 0 ? -1 : bs_with(t, t->blk[blk].def, "level", lv);
}

int bs_is_block(const BsTab *t, int st, const char *name) { int b = bs_block_index(t, name); return b >= 0 && t->g->state_block[st] == b; }

u8 *bs_block_set_from_json(const BsTab *t, const Js *v) {
    u8 *out = xcalloc((size_t)(t->nblocks ? t->nblocks : 1), 1);
    const Js *items[1]; int n = 0; const Js *const *arr = items;
    if (js_is_arr(v)) { arr = (const Js *const *)v->items; n = v->n; } else { items[0] = v; n = 1; }
    for (int i = 0; i < n; i++) {
        const Js *e = arr[i];
        const char *s = js_is_str(e) ? e->s : js_str(js_get(e, "id"), NULL);
        if (!s) { free(out); return NULL; }
        if (s[0] == '#') { const u8 *tg = gen_block_tag(t->g, s + 1); for (int b = 0; b < t->nblocks; b++) if (tg[b]) out[b] = 1; }
        else { int b = bs_block_index(t, s); if (b < 0) { free(out); return NULL; } out[b] = 1; }
    }
    return out;
}

int bs_is_a(const BsTab *t, int st, const char *cls) {
    const BsBlock *b = &t->blk[t->g->state_block[st]];
    if (!b->chain) return 0;
    char key[96]; snprintf(key, sizeof key, "|%s|", cls);
    return strstr(b->chain, key) != NULL;
}

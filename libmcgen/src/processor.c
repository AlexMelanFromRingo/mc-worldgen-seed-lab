/* processor.c — StructureProcessor: разбор processor_list из датапака и применение к блокам шаблона (см. processor.h). */
#include "structure.h"
#include "processor.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- RuleTest / PosRuleTest */
enum { RT_ALWAYS, RT_BLOCK, RT_STATE, RT_RAND_BLOCK, RT_RAND_STATE, RT_TAG, RT_ALL, RT_ANY, RT_NOT };
typedef struct RuleTestN {
    int type; int block, state; float prob; const u8 *tag;
    int nkid; struct RuleTestN **kid;
} RuleTestN;
enum { PT_ALWAYS, PT_LINEAR, PT_AXIS };
typedef struct { int type; float min_chance, max_chance; int min_dist, max_dist, axis; } PosTestN;
typedef struct { RuleTestN *in, *loc; PosTestN pos; int out_state; } RuleN;

enum { PK_NOP, PK_RULE, PK_PROTECTED, PK_BLOCK_ROT, PK_CAPPED, PK_BLOCK_IGNORE, PK_JIGSAW_REPL, PK_GRAVITY, PK_UNIMPL, PK_BLOCK_AGE, PK_BLACKSTONE, PK_LAVA_SUB };
struct Proc {
    int kind;
    int nrules; RuleN *rules;           /* rule */
    u8 *mask; int has_mask;             /* protected_blocks / block_rot / block_ignore */
    float integrity;                    /* block_rot */
    Proc *delegate; int lim_min, lim_max;   /* capped: IntProvider constant/uniform */
    int hm, offset;                     /* gravity */
    float mossiness;                    /* block_age */
};

typedef struct ProcStore { McMutex *lock; StrMap lists; PtrVec all_lists; Proc *builtin[PB__COUNT]; PtrVec gravity; PtrVec procs; } ProcStore;

static ProcStore *store_of(McWorld *w) {
    StructWorld *sw = structures_world_get(w);
    return sw ? (ProcStore *)sw->procs : NULL;
}
void *processors_store_new(void) { ProcStore *s = xcalloc(1, sizeof *s); s->lock = mutex_new(); return s; }

static void rt_free(RuleTestN *r) { if (!r) return; for (int i = 0; i < r->nkid; i++) rt_free(r->kid[i]); free(r->kid); free(r); }
static void proc_free(Proc *p) {
    if (!p) return;
    for (int i = 0; i < p->nrules; i++) { rt_free(p->rules[i].in); rt_free(p->rules[i].loc); }
    free(p->rules); free(p->mask); proc_free(p->delegate); free(p);
}
static void list_free(void *v) { ProcList *l = v; if (!l) return; for (int i = 0; i < l->n; i++) proc_free(l->p[i]); free(l->p); free(l->id); free(l); }
void processors_world_free(McWorld *w, void *procs) {
    (void)w; ProcStore *s = procs; if (!s) return;
    sm_free(&s->lists, list_free);
    for (int i = 0; i < PB__COUNT; i++) proc_free(s->builtin[i]);
    for (int i = 0; i < s->gravity.n; i++) proc_free(s->gravity.v[i]);
    for (int i = 0; i < s->procs.n; i++) proc_free(s->procs.v[i]);
    pv_free(&s->gravity); pv_free(&s->all_lists); pv_free(&s->procs);
    mutex_free(s->lock); free(s);
}

/* ---------------------------------------------------------------- разбор */
static const char *strip_ns(const char *s) { return !strncmp(s, "minecraft:", 10) ? s + 10 : s; }

static RuleTestN *parse_rt(const McGen *g, const BsTab *bs, const Js *v) {
    if (!js_is_obj(v)) return NULL;
    const char *t = strip_ns(js_str(js_get(v, "predicate_type"), ""));
    RuleTestN *r = xcalloc(1, sizeof *r);
    r->block = r->state = -1;
    if (!strcmp(t, "always_true")) r->type = RT_ALWAYS;
    else if (!strcmp(t, "block_match") || !strcmp(t, "random_block_match")) {
        r->type = t[0] == 'r' ? RT_RAND_BLOCK : RT_BLOCK;
        r->block = bs_block_index(bs, js_str(js_get(v, "block"), ""));
        r->prob = js_numf(js_get(v, "probability"), 1.0f);
    } else if (!strcmp(t, "blockstate_match") || !strcmp(t, "random_blockstate_match")) {
        r->type = t[0] == 'r' ? RT_RAND_STATE : RT_STATE;
        r->state = bs_from_json(bs, js_get(v, "block_state"));
        r->prob = js_numf(js_get(v, "probability"), 1.0f);
    } else if (!strcmp(t, "tag_match")) {
        r->type = RT_TAG; r->tag = gen_block_tag(g, js_str(js_get(v, "tag"), ""));
    } else if (!strcmp(t, "all_of") || !strcmp(t, "any_of")) {
        r->type = t[1] == 'l' ? RT_ALL : RT_ANY;
        const Js *a = js_get(v, "predicates");
        for (int i = 0; js_is_arr(a) && i < a->n; i++) { r->kid = xrealloc(r->kid, (size_t)(r->nkid + 1) * sizeof *r->kid); r->kid[r->nkid++] = parse_rt(g, bs, a->items[i]); }
    } else if (!strcmp(t, "not")) {
        r->type = RT_NOT; r->kid = xcalloc(1, sizeof *r->kid); r->kid[0] = parse_rt(g, bs, js_get(v, "predicate")); r->nkid = 1;
    } else { free(r); return NULL; }
    return r;
}
static int parse_pos(const Js *v, PosTestN *p) {
    memset(p, 0, sizeof *p);
    if (!v) { p->type = PT_ALWAYS; return 1; }
    const char *t = strip_ns(js_str(js_get(v, "predicate_type"), ""));
    if (!strcmp(t, "always_true")) { p->type = PT_ALWAYS; return 1; }
    p->min_chance = js_numf(js_get(v, "min_chance"), 0.0f); p->max_chance = js_numf(js_get(v, "max_chance"), 0.0f);
    p->min_dist = js_int(js_get(v, "min_dist"), 0); p->max_dist = js_int(js_get(v, "max_dist"), 0);
    if (!strcmp(t, "linear_pos")) { p->type = PT_LINEAR; return 1; }
    if (!strcmp(t, "axis_aligned_linear_pos")) {
        p->type = PT_AXIS; const char *a = js_str(js_get(v, "axis"), "y");
        p->axis = a[0] == 'x' ? 0 : a[0] == 'z' ? 2 : 1; return 1;
    }
    return 0;
}

static Proc *parse_proc(McWorld *w, const Js *v, const McGen *g, const BsTab *bs) {
    if (!js_is_obj(v)) return NULL;
    const char *t = strip_ns(js_str(js_get(v, "processor_type"), ""));
    Proc *p = xcalloc(1, sizeof *p);
    if (!strcmp(t, "rule")) {
        p->kind = PK_RULE;
        const Js *rules = js_get(v, "rules");
        for (int i = 0; js_is_arr(rules) && i < rules->n; i++) {
            const Js *r = rules->items[i];
            RuleN rn; memset(&rn, 0, sizeof rn);
            rn.in = parse_rt(g, bs, js_get(r, "input_predicate")); rn.loc = parse_rt(g, bs, js_get(r, "location_predicate"));
            if (!rn.in || !rn.loc || !parse_pos(js_get(r, "position_predicate"), &rn.pos)) { rt_free(rn.in); rt_free(rn.loc); p->kind = PK_UNIMPL; continue; }
            rn.out_state = bs_from_json(bs, js_get(r, "output_state"));
            if (rn.out_state < 0) rn.out_state = g->st_air;
            p->rules = xrealloc(p->rules, (size_t)(p->nrules + 1) * sizeof *p->rules); p->rules[p->nrules++] = rn;
        }
    } else if (!strcmp(t, "nop")) p->kind = PK_NOP;
    else if (!strcmp(t, "protected_blocks")) { p->kind = PK_PROTECTED; p->mask = bs_block_set_from_json(bs, js_get(v, "value")); p->has_mask = p->mask != NULL; }
    else if (!strcmp(t, "block_rot")) {
        p->kind = PK_BLOCK_ROT; p->integrity = js_numf(js_get(v, "integrity"), 1.0f);
        const Js *rb = js_get(v, "rottable_blocks"); if (rb) { p->mask = bs_block_set_from_json(bs, rb); p->has_mask = p->mask != NULL; }
    } else if (!strcmp(t, "block_ignore")) {
        p->kind = PK_BLOCK_IGNORE; const Js *bl = js_get(v, "blocks");
        p->mask = xcalloc((size_t)(bs->nblocks ? bs->nblocks : 1), 1); p->has_mask = 1;
        for (int i = 0; js_is_arr(bl) && i < bl->n; i++) { int st = bs_from_json(bs, bl->items[i]); if (st >= 0) p->mask[g->state_block[st]] = 1; }
    } else if (!strcmp(t, "gravity")) {
        p->kind = PK_GRAVITY; p->hm = hm_type_from_name(js_str(js_get(v, "heightmap"), "WORLD_SURFACE_WG")); if (p->hm < 0) p->hm = HM_WORLD_SURFACE_WG;
        p->offset = js_int(js_get(v, "offset"), 0);
    } else if (!strcmp(t, "capped")) {
        p->kind = PK_CAPPED; p->delegate = parse_proc(w, js_get(v, "delegate"), g, bs);
        const Js *lim = js_get(v, "limit");
        if (js_is_num(lim)) p->lim_min = p->lim_max = js_int(lim, 0);
        else if (js_is_obj(lim)) {
            const char *lt = strip_ns(js_str(js_get(lim, "type"), "constant"));
            if (!strcmp(lt, "uniform")) { p->lim_min = js_int(js_get(lim, "min_inclusive"), 0); p->lim_max = js_int(js_get(lim, "max_inclusive"), 0); }
            else p->lim_min = p->lim_max = js_int(js_get(lim, "value"), 0);
        }
        if (!p->delegate) p->kind = PK_UNIMPL;
    } else p->kind = PK_UNIMPL;       /* block_age, lava_submerged_block, blackstone_replace… — не используются данными 26.3 */
    return p;
}

static ProcList *parse_list_json(McWorld *w, const Js *v) {
    const McGen *g = w->g; const BsTab *bs = bs_get(g);
    const Js *arr = js_is_arr(v) ? v : js_get(v, "processors");
    if (!js_is_arr(arr)) return NULL;
    ProcList *l = xcalloc(1, sizeof *l);
    for (int i = 0; i < arr->n; i++) {
        Proc *p = parse_proc(w, arr->items[i], g, bs);
        if (!p) continue;
        l->p = xrealloc(l->p, (size_t)(l->n + 1) * sizeof *l->p); l->p[l->n++] = p;
    }
    return l;
}

const ProcList *proclist_empty(void) { static ProcList e; return &e; }

const ProcList *proclist_get(McWorld *w, const char *id) {
    ProcStore *s = store_of(w); if (!s) return NULL;
    char full[256]; if (!strchr(id, ':')) snprintf(full, sizeof full, "minecraft:%s", id); else snprintf(full, sizeof full, "%s", id);
    mutex_lock(s->lock);
    ProcList *l = sm_get(&s->lists, full);
    if (!l && !sm_has(&s->lists, full)) {
        const char *c = strchr(full, ':');
        char path[1024]; snprintf(path, sizeof path, "%s/data/%.*s/worldgen/processor_list/%s.json", w->g->pack, (int)(c - full), full, c + 1);
        char err[128];
        JsDoc *d = js_parse_file(path, err, sizeof err);
        if (d) { l = parse_list_json(w, js_root(d)); l->id = xstrdup(full); /* JsDoc не освобождается раньше: правила копируют всё нужное */ js_free(d); }
        sm_put(&s->lists, full, l);
    }
    mutex_unlock(s->lock);
    return l;
}
const ProcList *proclist_from_json(McWorld *w, const Js *v) {
    if (js_is_str(v)) return proclist_get(w, v->s);
    ProcStore *s = store_of(w); if (!s) return NULL;
    mutex_lock(s->lock);
    ProcList *l = parse_list_json(w, v);
    if (l) pv_push(&s->all_lists, l);
    mutex_unlock(s->lock);
    return l;
}

const Proc *proc_builtin(McWorld *w, int which) {
    ProcStore *s = store_of(w); if (!s) return NULL;
    mutex_lock(s->lock);
    if (!s->builtin[which]) {
        Proc *p = xcalloc(1, sizeof *p); const BsTab *bs = bs_get(w->g);
        p->mask = xcalloc((size_t)(bs->nblocks ? bs->nblocks : 1), 1); p->has_mask = 1;
        if (which == PB_JIGSAW_REPLACEMENT) { p->kind = PK_JIGSAW_REPL; p->offset = bs_block_index(bs, "minecraft:jigsaw"); }
        else {
            p->kind = PK_BLOCK_IGNORE;
            int sb = bs_block_index(bs, "minecraft:structure_block"), air = bs_block_index(bs, "minecraft:air");
            if (which != PB_AIR && sb >= 0) p->mask[sb] = 1;
            if (which != PB_STRUCTURE_BLOCK && air >= 0) p->mask[air] = 1;
        }
        s->builtin[which] = p;
    }
    const Proc *r = s->builtin[which];
    mutex_unlock(s->lock);
    return r;
}
static const Proc *simple_proc(McWorld *w, int kind, float moss) {
    ProcStore *s = store_of(w); if (!s) return NULL;
    mutex_lock(s->lock);
    for (int i = 0; i < s->procs.n; i++) { Proc *p = s->procs.v[i]; if (p->kind == kind && p->mossiness == moss) { mutex_unlock(s->lock); return p; } }
    Proc *p = xcalloc(1, sizeof *p); p->kind = kind; p->mossiness = moss; pv_push(&s->procs, p);
    mutex_unlock(s->lock);
    return p;
}
const Proc *proc_block_age(McWorld *w, float mossiness) { return simple_proc(w, PK_BLOCK_AGE, mossiness); }
const Proc *proc_blackstone_replace(McWorld *w) { return simple_proc(w, PK_BLACKSTONE, 0.0f); }
const Proc *proc_lava_submerged(McWorld *w) { return simple_proc(w, PK_LAVA_SUB, 0.0f); }
const Proc *proc_gravity(McWorld *w, int hm, int offset) {
    ProcStore *s = store_of(w); if (!s) return NULL;
    mutex_lock(s->lock);
    for (int i = 0; i < s->gravity.n; i++) { Proc *p = s->gravity.v[i]; if (p->hm == hm && p->offset == offset) { mutex_unlock(s->lock); return p; } }
    Proc *p = xcalloc(1, sizeof *p); p->kind = PK_GRAVITY; p->hm = hm; p->offset = offset; pv_push(&s->gravity, p);
    mutex_unlock(s->lock);
    return p;
}

/* ---------------------------------------------------------------- применение */
static int world_state(PEnv *e, int x, int y, int z) { return e->fc ? fc_get(e->fc, x, y, z) : e->w->g->st_air; }

static int rt_test(const RuleTestN *r, const McGen *g, int state, RS *rnd) {
    switch (r->type) {
    case RT_ALWAYS: return 1;
    case RT_BLOCK: return gen_is_block(g, state, r->block);
    case RT_STATE: return state == r->state;
    case RT_RAND_BLOCK: return gen_is_block(g, state, r->block) && rs_float(rnd) < r->prob;
    case RT_RAND_STATE: return state == r->state && rs_float(rnd) < r->prob;
    case RT_TAG: return r->tag && g->state_block[state] < g->nblocks && r->tag[g->state_block[state]];
    case RT_ALL: for (int i = 0; i < r->nkid; i++) if (!rt_test(r->kid[i], g, state, rnd)) return 0; return 1;
    case RT_ANY: for (int i = 0; i < r->nkid; i++) if (rt_test(r->kid[i], g, state, rnd)) return 1; return 0;
    case RT_NOT: return !rt_test(r->kid[0], g, state, rnd);
    }
    return 0;
}
static float clamped_lerpf(float t, float a, float b) { return t < 0.0f ? a : (t > 1.0f ? b : a + t * (b - a)); }
static int pos_test(const PosTestN *p, const TInfo *cur, PEnv *e, RS *rnd) {
    switch (p->type) {
    case PT_ALWAYS: return 1;
    case PT_LINEAR: {
        int dist = abs(cur->x - e->ref_x) + abs(cur->y - e->ref_y) + abs(cur->z - e->ref_z);
        float rn = rs_float(rnd);
        return rn <= clamped_lerpf(((float)dist - (float)p->min_dist) / ((float)p->max_dist - (float)p->min_dist), p->min_chance, p->max_chance);
    }
    default: {
        float xd = p->axis == 0 ? (float)abs(cur->x - e->ref_x) : 0.0f;
        float yd = p->axis == 1 ? (float)abs(cur->y - e->ref_y) : 0.0f;
        float zd = p->axis == 2 ? (float)abs(cur->z - e->ref_z) : 0.0f;
        int dist = (int)(xd + yd + zd);
        float rn = rs_float(rnd);
        return rn <= clamped_lerpf(((float)dist - (float)p->min_dist) / ((float)p->max_dist - (float)p->min_dist), p->min_chance, p->max_chance);
    }
    }
}


/* BlockState.withPropertiesOf: свойства old, которые есть у нового блока */
static int with_props_of(const BsTab *bs, int new_blk, int old_state) {
    int st = bs->blk[new_blk].def;
    const BsBlock *ob = &bs->blk[bs->g->state_block[old_state]];
    for (int i = 0; i < ob->nprops; i++) {
        const char *v; if (!bs_get_prop(bs, old_state, ob->pname[i], &v)) continue;
        int ns = bs_with(bs, st, ob->pname[i], v); if (ns >= 0) st = ns;
    }
    return st;
}
static int blk_idx(PEnv *e, const char *n) { return bs_block_index(e->bs, n); }
static int tag_has(const McGen *g, const char *tag, int blk) { const u8 *t = gen_block_tag(g, tag); return t && t[blk]; }
static int random_facing_stairs(PEnv *e, RS *r, const char *blk) {
    static const char *HD[4] = { "north", "east", "south", "west" };
    int st = e->bs->blk[blk_idx(e, blk)].def;
    st = bs_with(e->bs, st, "facing", HD[rs_bound(r, 4)]);
    return bs_with(e->bs, st, "half", rs_bound(r, 2) == 0 ? "top" : "bottom");       /* Half.values(): TOP, BOTTOM */
}
static int block_age(const Proc *p, PEnv *e, const TInfo *cur) {
    const McGen *g = e->w->g; const BsTab *bs = e->bs;
    RS r; rs_seed_lcg(&r, mth_get_seed(cur->x, cur->y, cur->z));
    int st = cur->state, b = g->state_block[st], ns = -1;
    if (b == blk_idx(e, "minecraft:stone_bricks") || b == blk_idx(e, "minecraft:stone") || b == blk_idx(e, "minecraft:chiseled_stone_bricks")) {
        if (rs_float(&r) >= 0.5f) return -1;
        int non[2] = { bs->blk[blk_idx(e, "minecraft:cracked_stone_bricks")].def, 0 }; non[1] = random_facing_stairs(e, &r, "minecraft:stone_brick_stairs");
        int mos[2] = { bs->blk[blk_idx(e, "minecraft:mossy_stone_bricks")].def, 0 }; mos[1] = random_facing_stairs(e, &r, "minecraft:mossy_stone_brick_stairs");
        const int *arr = rs_float(&r) < p->mossiness ? mos : non;
        return arr[rs_bound(&r, 2)];
    } else if (tag_has(g, "minecraft:stairs", b)) {
        if (rs_float(&r) >= 0.5f) return -1;
        int mos[2] = { with_props_of(bs, blk_idx(e, "minecraft:mossy_stone_brick_stairs"), st), bs->blk[blk_idx(e, "minecraft:mossy_stone_brick_slab")].def };
        int non[2] = { bs->blk[blk_idx(e, "minecraft:stone_slab")].def, bs->blk[blk_idx(e, "minecraft:stone_brick_slab")].def };
        const int *arr = rs_float(&r) < p->mossiness ? mos : non;
        return arr[rs_bound(&r, 2)];
    } else if (tag_has(g, "minecraft:slabs", b)) { if (rs_float(&r) < p->mossiness) ns = with_props_of(bs, blk_idx(e, "minecraft:mossy_stone_brick_slab"), st); }
    else if (tag_has(g, "minecraft:walls", b)) { if (rs_float(&r) < p->mossiness) ns = with_props_of(bs, blk_idx(e, "minecraft:mossy_stone_brick_wall"), st); }
    else if (b == blk_idx(e, "minecraft:obsidian")) { if (rs_float(&r) < 0.15f) ns = bs->blk[blk_idx(e, "minecraft:crying_obsidian")].def; }
    return ns;
}
static int blackstone_state(PEnv *e, int st) {
    static const char *M[][2] = {
        {"cobblestone","blackstone"},{"mossy_cobblestone","blackstone"},{"stone","polished_blackstone"},{"stone_bricks","polished_blackstone_bricks"},{"mossy_stone_bricks","polished_blackstone_bricks"},
        {"cobblestone_stairs","blackstone_stairs"},{"mossy_cobblestone_stairs","blackstone_stairs"},{"stone_stairs","polished_blackstone_stairs"},{"stone_brick_stairs","polished_blackstone_brick_stairs"},
        {"mossy_stone_brick_stairs","polished_blackstone_brick_stairs"},{"cobblestone_slab","blackstone_slab"},{"mossy_cobblestone_slab","blackstone_slab"},{"smooth_stone_slab","polished_blackstone_slab"},
        {"stone_slab","polished_blackstone_slab"},{"stone_brick_slab","polished_blackstone_brick_slab"},{"mossy_stone_brick_slab","polished_blackstone_brick_slab"},{"stone_brick_wall","polished_blackstone_brick_wall"},
        {"mossy_stone_brick_wall","polished_blackstone_brick_wall"},{"cobblestone_wall","blackstone_wall"},{"mossy_cobblestone_wall","blackstone_wall"},{"chiseled_stone_bricks","chiseled_polished_blackstone"},
        {"cracked_stone_bricks","cracked_polished_blackstone_bricks"},{"iron_bars","iron_chain"},{NULL,NULL} };
    int b = e->w->g->state_block[st];
    for (int i = 0; M[i][0]; i++) {
        char a[64]; snprintf(a, sizeof a, "minecraft:%s", M[i][0]);
        if (b != blk_idx(e, a)) continue;
        snprintf(a, sizeof a, "minecraft:%s", M[i][1]);
        int nb = blk_idx(e, a); if (nb < 0) return -1;
        int ns = e->bs->blk[nb].def; const char *v;
        if (bs_get_prop(e->bs, st, "facing", &v)) { int r = bs_with(e->bs, ns, "facing", v); if (r >= 0) ns = r; }
        if (bs_get_prop(e->bs, st, "half", &v)) { int r = bs_with(e->bs, ns, "half", v); if (r >= 0) ns = r; }
        if (bs_get_prop(e->bs, st, "type", &v)) { int r = bs_with(e->bs, ns, "type", v); if (r >= 0) ns = r; }
        return ns;
    }
    return -1;
}

int proc_whole_piece(const Proc *p) { return p->kind == PK_CAPPED; }

int proc_block(const Proc *p, PEnv *e, const TInfo *orig, TInfo *cur) {
    const McGen *g = e->w->g;
    switch (p->kind) {
    case PK_RULE: {
        RS rnd; rs_seed_lcg(&rnd, mth_get_seed(cur->x, cur->y, cur->z));
        for (int i = 0; i < p->nrules; i++) {
            const RuleN *r = &p->rules[i];
            if (rt_test(r->in, g, cur->state, &rnd) && rt_test(r->loc, g, world_state(e, cur->x, cur->y, cur->z), &rnd) && pos_test(&r->pos, cur, e, &rnd)) {
                cur->state = r->out_state; return 1;       /* getOutputTag: Passthrough — NBT сохраняется */
            }
        }
        return 1;
    }
    case PK_PROTECTED: {
        int st = world_state(e, cur->x, cur->y, cur->z);
        return !(p->has_mask && g->state_block[st] < e->bs->nblocks && p->mask[g->state_block[st]]);
    }
    case PK_BLOCK_ROT: {
        RS own; RS *rr = (RS *)e->rnd;                      /* settings.getRandom(pos): общий ГСЧ, если он задан (фичи), иначе LCG по позиции */
        if (!rr) { rs_seed_lcg(&own, mth_get_seed(cur->x, cur->y, cur->z)); rr = &own; }
        int rottable = !p->has_mask || p->mask[g->state_block[cur->state]];
        if (rottable && !(rs_float(rr) <= p->integrity)) return 0;
        return 1;
    }
    case PK_BLOCK_IGNORE: return !p->mask[g->state_block[cur->state]];
    case PK_JIGSAW_REPL: {
        if (g->state_block[cur->state] != p->offset) return 1;
        if (!cur->nbt) return 1;
        const char *fs = nbt_str(nbt_get(cur->nbt, "final_state"), "minecraft:air");
        int st = gen_state_id(g, fs);
        if (st < 0) return 0;
        int sv = bs_block_index(e->bs, "minecraft:structure_void");
        if (sv >= 0 && g->state_block[st] == sv) return 0;
        cur->state = st; cur->nbt = NULL; return 1;
    }
    case PK_GRAVITY: {
        int h;       /* level.getHeight(WORLD_SURFACE_WG…): карты окна фич W8 (WG-карты 26.3 праймятся при первом запросе); без окна — по заполнению шумом */
        if (e->fc) h = structure_height(e->fc, p->hm, cur->x, cur->z) + p->offset;
        else if (e->w->g->newf && (p->hm == HM_WORLD_SURFACE_WG || p->hm == HM_OCEAN_FLOOR_WG)) h = structure_height_wg(e->w, p->hm, cur->x, cur->z) + p->offset;
        else h = p->offset;
        cur->y = h + orig->y;
        return 1;
    }
    case PK_BLOCK_AGE: { int ns = block_age(p, e, cur); if (ns >= 0) cur->state = ns; return 1; }
    case PK_BLACKSTONE: { int ns = blackstone_state(e, cur->state); if (ns >= 0) cur->state = ns; return 1; }
    case PK_LAVA_SUB: {
        int was_lava = e->bs->g->state_block[world_state(e, cur->x, cur->y, cur->z)] == e->w->g->blk_lava;
        if (was_lava && !(e->bs->flags[cur->state] & BSF_FULL_COLL)) { int lava = bs_block_index(e->bs, "minecraft:lava"); cur->state = e->bs->blk[lava].def; }
        return 1;
    }
    default: return 1;
    }
}

void proc_finalize(const Proc *p, PEnv *e, const TInfo *orig, TInfo *proc, int n) {
    if (p->kind != PK_CAPPED || p->lim_max == 0 || n <= 0) return;
    /* RandomSource.createThreadLocalInstance(seed).forkPositional().at(position) */
    RS base; rs_seed_lcg(&base, e->level_seed);
    i64 fork_seed = rs_long(&base);
    RS rnd; rs_seed_lcg(&rnd, mth_get_seed(e->pos_x, e->pos_y, e->pos_z) ^ fork_seed);
    int limit = p->lim_min >= p->lim_max ? p->lim_min : rs_bound(&rnd, p->lim_max - p->lim_min + 1) + p->lim_min;   /* UniformInt.sample = Mth.randomBetweenInclusive */
    int max_replace = limit < n ? limit : n;
    if (max_replace < 1) return;
    int *idx = xmalloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = n; i > 1; i--) { int sw = rs_bound(&rnd, i); int t = idx[i - 1]; idx[i - 1] = idx[sw]; idx[sw] = t; }
    int replaced = 0;
    for (int k = 0; k < n && replaced < max_replace; k++) {
        int i = idx[k];
        TInfo cur = proc[i];
        if (!proc_block(p->delegate, e, &orig[i], &cur)) continue;     /* null: не «изменён» */
        if (cur.x != proc[i].x || cur.y != proc[i].y || cur.z != proc[i].z || cur.state != proc[i].state || cur.nbt != proc[i].nbt) { replaced++; proc[i] = cur; }
    }
    free(idx);
}

/* feature_bpred.c — BlockPredicate (levelgen/blockpredicates) и RuleTest (templatesystem) + BlockState.canSurvive для нужных классов. */
#include "feature.h"
#include "feature_veg.h"
#include <stdio.h>
#include <stdlib.h>

/* ====================================================================== BlockPredicate */
enum { BP_TRUE, BP_BLOCKS, BP_TAG, BP_FLUIDS, BP_SURVIVE, BP_SOLID, BP_REPLACEABLE, BP_STURDY, BP_ALL, BP_ANY, BP_NOT, BP_INSIDE, BP_HEIGHT,
       BP_VOLUME, BP_BIOMES, BP_UNOBSTRUCTED, BP_BELOW_HM };
struct BPred {
    int kind;
    int ox, oy, oz;
    const u8 *set;            /* BP_BLOCKS / BP_TAG: u8[nblocks] */
    u32 fluids;               /* BP_FLUIDS: маска типов жидкости */
    int state;                /* BP_SURVIVE */
    int dir;                  /* BP_STURDY */
    int n; BPred **kids;      /* ALL / ANY (NOT: kids[0]; VOLUME: kids[0]) */
    VAnchor amin, amax;       /* BP_HEIGHT */
    int mx, my, mz;           /* BP_VOLUME: max (min = ox,oy,oz) */
    u8 *biomes;               /* BP_BIOMES: u8[nbiomes] */
};

static int parse_offset(FParse *p, const Js *v, int *x, int *y, int *z) {
    *x = *y = *z = 0;
    if (!v) return 1;
    if (!js_is_arr(v) || v->n != 3) return fp_fail(p, "offset: ожидался массив из 3 чисел");
    *x = js_int(v->items[0], 0); *y = js_int(v->items[1], 0); *z = js_int(v->items[2], 0);
    return 1;
}

static u32 fluid_set_from_json(FParse *p, const Js *v) {
    u32 m = 0;
    const Js *items[1]; int n; const Js *const *arr;
    if (js_is_arr(v)) { arr = (const Js *const *)v->items; n = v->n; } else { items[0] = v; arr = items; n = 1; }
    for (int i = 0; i < n; i++) {
        const char *s = js_is_str(arr[i]) ? arr[i]->s : NULL;
        if (!s) { fp_fail(p, "matching_fluids: ожидалась строка"); return 0; }
        int tag = s[0] == '#'; if (tag) s++;
        if (!strncmp(s, "minecraft:", 10)) s += 10;
        if (!strcmp(s, "water")) m |= tag ? (1u << FL_WATER) | (1u << FL_FLOWING_WATER) : (1u << FL_WATER);
        else if (!strcmp(s, "flowing_water")) m |= 1u << FL_FLOWING_WATER;
        else if (!strcmp(s, "lava")) m |= tag ? (1u << FL_LAVA) | (1u << FL_FLOWING_LAVA) : (1u << FL_LAVA);
        else if (!strcmp(s, "flowing_lava")) m |= 1u << FL_FLOWING_LAVA;
        else if (!strcmp(s, "empty")) m |= 1u << FL_NONE;
        else { fp_fail(p, "matching_fluids: неизвестная жидкость %s", s); return 0; }
    }
    return m;
}

u8 *fp_blockset(FParse *p, const Js *v) {
    u8 *t = bs_block_set_from_json(p->bs, v);
    if (!t) return NULL;
    u8 *r = fp_alloc(p, (size_t)(p->bs->nblocks ? p->bs->nblocks : 1));
    memcpy(r, t, (size_t)p->bs->nblocks); free(t);
    return r;
}
BPred *bpred_true(FParse *p) { BPred *b = fp_alloc(p, sizeof *b); b->kind = BP_TRUE; return b; }

BPred *fp_bpred(FParse *p, const Js *v) {
    if (!js_is_obj(v)) { fp_fail(p, "BlockPredicate: ожидался объект (получено %s)", !v ? "ничего" : v->t == JS_STR ? v->s : v->t == JS_ARR ? "массив" : "другое"); return NULL; }
    const char *t = js_str(js_get(v, "type"), NULL);
    if (!t) { fp_fail(p, "BlockPredicate: нет type"); return NULL; }
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    BPred *b = fp_alloc(p, sizeof *b);
    if (!strcmp(t, "true")) { b->kind = BP_TRUE; return b; }
    if (!strcmp(t, "all_of") || !strcmp(t, "any_of")) {
        b->kind = t[0] == 'a' && t[1] == 'l' ? BP_ALL : BP_ANY;
        Js *l = js_get(v, "predicates");
        if (!js_is_arr(l)) { fp_fail(p, "BlockPredicate %s: нет predicates", t); return NULL; }
        b->n = l->n; b->kids = fp_alloc(p, sizeof(BPred *) * (size_t)(l->n ? l->n : 1));
        for (int i = 0; i < l->n; i++) { b->kids[i] = fp_bpred(p, l->items[i]); if (!b->kids[i]) return NULL; }
        return b;
    }
    if (!strcmp(t, "not")) {
        b->kind = BP_NOT; b->n = 1; b->kids = fp_alloc(p, sizeof(BPred *));
        b->kids[0] = fp_bpred(p, js_get(v, "predicate")); return b->kids[0] ? b : NULL;
    }
    if (!strcmp(t, "height_range")) {
        b->kind = BP_HEIGHT;
        if (!fp_vanchor(p, js_get(v, "min_inclusive"), &b->amin) || !fp_vanchor(p, js_get(v, "max_inclusive"), &b->amax)) return NULL;
        return b;
    }
    if (!strcmp(t, "matching_biomes")) {
        b->kind = BP_BIOMES; b->biomes = fp_alloc(p, (size_t)(p->g->nbiomes ? p->g->nbiomes : 1));
        Js *l = js_get(v, "biomes");
        const Js *items[1]; int n; const Js *const *arr;
        if (js_is_arr(l)) { arr = (const Js *const *)l->items; n = l->n; } else { items[0] = l; arr = items; n = 1; }
        for (int i = 0; i < n; i++) {
            const char *s = js_is_str(arr[i]) ? arr[i]->s : NULL;
            if (!s || s[0] == '#') { fp_fail(p, "matching_biomes: теги биомов не поддержаны"); return NULL; }
            int id = gen_biome_id(p->g, s); if (id < 0) { fp_fail(p, "matching_biomes: нет биома %s", s); return NULL; }
            b->biomes[id] = 1;
        }
        return b;
    }
    if (!strcmp(t, "below_heightmap")) {      /* 26.4+ */
        b->kind = BP_BELOW_HM; b->dir = hm_type_from_name(js_str(js_get(v, "heightmap"), NULL));
        return b->dir >= 0 ? b : (fp_fail(p, "below_heightmap: плохая карта"), NULL);
    }
    if (!strcmp(t, "unobstructed")) { b->kind = BP_UNOBSTRUCTED; return parse_offset(p, js_get(v, "offset"), &b->ox, &b->oy, &b->oz) ? b : NULL; }
    if (!strcmp(t, "volume_match")) {
        b->kind = BP_VOLUME; b->n = 1; b->kids = fp_alloc(p, sizeof(BPred *));
        if (!parse_offset(p, js_get(v, "min"), &b->ox, &b->oy, &b->oz) || !parse_offset(p, js_get(v, "max"), &b->mx, &b->my, &b->mz)) return NULL;
        { const Js *m = js_get(v, "predicate"); if (!m) m = js_get(v, "match"); b->kids[0] = fp_bpred(p, m); }
        return b->kids[0] ? b : NULL;
    }
    if (!parse_offset(p, js_get(v, "offset"), &b->ox, &b->oy, &b->oz)) return NULL;
    if (!strcmp(t, "matching_blocks")) {
        b->kind = BP_BLOCKS; b->set = fp_blockset(p, js_get(v, "blocks"));
        if (!b->set) { fp_fail(p, "matching_blocks: плохие blocks"); return NULL; }
    }
    else if (!strcmp(t, "matching_block_tag")) {
        b->kind = BP_TAG; const char *tg = js_str(js_get(v, "tag"), NULL);
        if (!tg) { fp_fail(p, "matching_block_tag: нет tag"); return NULL; }
        if (tg[0] == '#') tg++;
        b->set = gen_block_tag(p->g, tg);
    }
    else if (!strcmp(t, "matching_fluids")) { b->kind = BP_FLUIDS; b->fluids = fluid_set_from_json(p, js_get(v, "fluids")); if (!fp_ok(p)) return NULL; }
    else if (!strcmp(t, "would_survive")) {
        b->kind = BP_SURVIVE; b->state = bs_from_json(p->bs, js_get(v, "state"));
        if (b->state < 0) { fp_fail(p, "would_survive: плохое state"); return NULL; }
    }
    else if (!strcmp(t, "solid")) b->kind = BP_SOLID;
    else if (!strcmp(t, "replaceable")) b->kind = BP_REPLACEABLE;
    else if (!strcmp(t, "has_sturdy_face")) {
        b->kind = BP_STURDY; b->dir = dir_from_name(js_str(js_get(v, "direction"), NULL));
        if (b->dir < 0) { fp_fail(p, "has_sturdy_face: плохое direction"); return NULL; }
    }
    else if (!strcmp(t, "inside_world_bounds")) b->kind = BP_INSIDE;
    else { fp_fail(p, "BlockPredicate: неизвестный тип %s", t); return NULL; }
    return b;
}

int bpred_test(FCtx *c, const BPred *b, int x, int y, int z) {
    switch (b->kind) {
    case BP_TRUE: return 1;
    case BP_ALL: for (int i = 0; i < b->n; i++) if (!bpred_test(c, b->kids[i], x, y, z)) return 0; return 1;
    case BP_ANY: for (int i = 0; i < b->n; i++) if (bpred_test(c, b->kids[i], x, y, z)) return 1; return 0;
    case BP_NOT: return !bpred_test(c, b->kids[0], x, y, z);
    case BP_HEIGHT: return y >= vanchor_resolve(&b->amin, c) && y <= vanchor_resolve(&b->amax, c);
    case BP_BIOMES: return b->biomes[fc_biome(c, x, y, z)];
    case BP_UNOBSTRUCTED: return 1;
    case BP_BELOW_HM: return y < fc_height(c, b->dir, x, z);
    case BP_INSIDE: return !fc_outside(c, y + b->oy);
    case BP_VOLUME: {
        for (int ox = b->ox; ox <= b->mx; ox++) for (int oz = b->oz; oz <= b->mz; oz++) for (int oy = b->oy; oy <= b->my; oy++)
            if (!bpred_test(c, b->kids[0], x + ox, y + oy, z + oz)) return 0;
        return 1;
    }
    default: break;
    }
    int px = x + b->ox, py = y + b->oy, pz = z + b->oz;
    int st = fc_get(c, px, py, pz);
    switch (b->kind) {
    case BP_BLOCKS: case BP_TAG: return b->set[c->g->state_block[st]] != 0;
    case BP_FLUIDS: return (b->fluids >> BS_FL_TYPE(c->bs->fluid[st])) & 1;
    case BP_SURVIVE: return block_can_survive(c, b->state, px, py, pz);
    case BP_SOLID: return (c->bs->flags[st] & BSF_SOLID) != 0;
    case BP_REPLACEABLE: return (c->bs->flags[st] & BSF_REPLACEABLE) != 0;
    case BP_STURDY: return (c->bs->sturdy[st] >> b->dir) & 1;
    }
    return 0;
}

/* ====================================================================== RuleTest */
enum { RT_ALWAYS, RT_BLOCK, RT_STATE, RT_TAG, RT_RBLOCK, RT_RSTATE, RT_HEIGHT, RT_ALL, RT_ANY, RT_NOT };
struct RuleTest {
    int kind;
    int block, state; const u8 *tag; float prob;
    int hmin, hmax;
    int n; RuleTest **kids;
};
RuleTest *fp_ruletest(FParse *p, const Js *v) {
    if (!js_is_obj(v)) { fp_fail(p, "RuleTest: ожидался объект"); return NULL; }
    const char *t = js_str(js_get(v, "predicate_type"), NULL);
    if (!t) { fp_fail(p, "RuleTest: нет predicate_type"); return NULL; }
    if (!strncmp(t, "minecraft:", 10)) t += 10;
    RuleTest *r = fp_alloc(p, sizeof *r);
    if (!strcmp(t, "always_true")) { r->kind = RT_ALWAYS; return r; }
    if (!strcmp(t, "tag_match")) {
        r->kind = RT_TAG; const char *tg = js_str(js_get(v, "tag"), NULL);
        if (!tg) { fp_fail(p, "tag_match: нет tag"); return NULL; }
        r->tag = gen_block_tag(p->g, tg[0] == '#' ? tg + 1 : tg); return r;
    }
    if (!strcmp(t, "block_match") || !strcmp(t, "random_block_match")) {
        r->kind = t[0] == 'r' ? RT_RBLOCK : RT_BLOCK;
        const char *b = js_str(js_get(v, "block"), NULL); r->block = b ? bs_block_index(p->bs, b) : -1;
        if (r->block < 0) { fp_fail(p, "%s: плохой block", t); return NULL; }
        r->prob = js_numf(js_get(v, "probability"), 0); return r;
    }
    if (!strcmp(t, "blockstate_match") || !strcmp(t, "random_blockstate_match")) {
        r->kind = t[0] == 'r' ? RT_RSTATE : RT_STATE;
        Js *bs = js_get(v, "block_state"); r->state = bs ? bs_from_json(p->bs, bs) : -1;
        if (r->state < 0) { fp_fail(p, "%s: плохой block_state", t); return NULL; }
        r->prob = js_numf(js_get(v, "probability"), 0); return r;
    }
    if (!strcmp(t, "height_match")) { r->kind = RT_HEIGHT; r->hmin = js_int(js_get(v, "min_inclusive"), 0); r->hmax = js_int(js_get(v, "max_inclusive"), 0); return r; }
    if (!strcmp(t, "all_of") || !strcmp(t, "any_of")) {
        r->kind = t[1] == 'l' ? RT_ALL : RT_ANY;
        Js *l = js_get(v, "rules");
        if (!js_is_arr(l)) { fp_fail(p, "%s: нет rules", t); return NULL; }
        r->n = l->n; r->kids = fp_alloc(p, sizeof(RuleTest *) * (size_t)(l->n ? l->n : 1));
        for (int i = 0; i < l->n; i++) { r->kids[i] = fp_ruletest(p, l->items[i]); if (!r->kids[i]) return NULL; }
        return r;
    }
    if (!strcmp(t, "not")) {
        r->kind = RT_NOT; r->n = 1; r->kids = fp_alloc(p, sizeof(RuleTest *));
        r->kids[0] = fp_ruletest(p, js_get(v, "rule")); return r->kids[0] ? r : NULL;
    }
    fp_fail(p, "RuleTest: неизвестный тип %s", t);
    return NULL;
}
int ruletest_test(FCtx *c, const RuleTest *r, int st, int x, int y, int z) {
    switch (r->kind) {
    case RT_ALWAYS: return 1;
    case RT_BLOCK: return c->g->state_block[st] == r->block;
    case RT_STATE: return st == r->state;
    case RT_TAG: return r->tag[c->g->state_block[st]] != 0;
    case RT_RBLOCK: return c->g->state_block[st] == r->block && frnd_float(c->rnd) < r->prob;
    case RT_RSTATE: return st == r->state && frnd_float(c->rnd) < r->prob;
    case RT_HEIGHT: return r->hmin <= y && y <= r->hmax;
    case RT_ALL: for (int i = 0; i < r->n; i++) if (!ruletest_test(c, r->kids[i], st, x, y, z)) return 0; return 1;
    case RT_ANY: for (int i = 0; i < r->n; i++) if (ruletest_test(c, r->kids[i], st, x, y, z)) return 1; return 0;
    case RT_NOT: return !ruletest_test(c, r->kids[0], st, x, y, z);
    }
    return 0;
}

/* ====================================================================== BlockState.canSurvive
 * Потомки VegetationBlock: основание — тег поддержки класса/блока (BlockBehaviour: mayPlaceOn). Нестандартные классы (грибы, морская трава и огурцы,
 * кувшинка, листовая подстилка, посевы со светом, дриплиф, пропагулы) разобраны в feature_veg.c и ниже. */
static const struct { const char *cls; const char *tag; } SURV_CLS[] = {
    { "NetherSproutsBlock", "minecraft:supports_nether_sprouts" }, { "NetherWartBlock", "minecraft:supports_nether_wart" },
    { "AzaleaBlock", "minecraft:supports_azalea" }, { "DryVegetationBlock", "minecraft:supports_dry_vegetation" },
    { "WitherRoseBlock", "minecraft:supports_wither_rose" }, { NULL, NULL } };
static const struct { const char *block; const char *tag; } SURV_BLK[] = {          /* NetherRootsBlock / NetherFungusBlock — тег по блоку */
    { "minecraft:crimson_roots", "minecraft:supports_crimson_roots" }, { "minecraft:warped_roots", "minecraft:supports_warped_roots" },
    { "minecraft:crimson_fungus", "minecraft:supports_crimson_fungus" }, { "minecraft:warped_fungus", "minecraft:supports_warped_fungus" }, { NULL, NULL } };
static const char *PLAIN_VEG[] = { "SaplingBlock", "FlowerBlock", "TallGrassBlock", "FernBlock", "TallFlowerBlock", "BushBlock", "FireflyBushBlock", "EyeblossomBlock",
                                   "FlowerBedBlock", "ShortDryGrassBlock", "TallDryGrassBlock", NULL };
int veg_survive(FCtx *c, int state, int x, int y, int z, int *res);     /* feature_veg.c */
int block_can_survive(FCtx *c, int st, int x, int y, int z) {
    const BsTab *bs = c->bs;
    const BsBlock *bb = &bs->blk[c->g->state_block[st]];
    if (!bb->cls) return 1;
    { int vr; if (veg_survive(c, st, x, y, z, &vr)) return vr; }       /* классы растений группы W10 (feature_veg.c) */
    /* дриплифы (SmallDripleafBlock / BigDripleafBlock / BigDripleafStemBlock.canSurvive): без этих правил растения «выживали где угодно» и
     * ставились на глубинный сланец; в игре опора — теги supports_small_dripleaf / supports_big_dripleaf (+ вода-источник над опорой у малого) */
    if (!strcmp(bb->cls, "SmallDripleafBlock") || !strcmp(bb->cls, "BigDripleafBlock") || !strcmp(bb->cls, "BigDripleafStemBlock")) {
        static _Thread_local const McGen *dg; static _Thread_local const u8 *t_small, *t_big, *t_veg;
        if (dg != c->g) { dg = c->g; t_small = gen_block_tag(c->g, "minecraft:supports_small_dripleaf"); t_big = gen_block_tag(c->g, "minecraft:supports_big_dripleaf");
                          t_veg = gen_block_tag(c->g, "minecraft:supports_vegetation"); }
        int blk = c->g->state_block[st], below = fc_get(c, x, y - 1, z), bblk = c->g->state_block[below];
        if (!strcmp(bb->cls, "SmallDripleafBlock")) {
            const char *half = NULL; bs_get_prop(bs, st, "half", &half);
            if (half && !strcmp(half, "upper")) {                       /* DoublePlantBlock: снизу — нижняя половина того же блока */
                const char *bh = NULL; return bblk == blk && bs_get_prop(bs, below, "half", &bh) && !strcmp(bh, "lower");
            }
            int here = fc_get(c, x, y, z), fl = bs->fluid[here];
            /* mayPlaceOn(below, pos=below): is(SUPPORTS_SMALL_DRIPLEAF) || fluid(pos.above()).isSourceOfType(WATER) && BushBlock.mayPlaceOn(SUPPORTS_VEGETATION) */
            return t_small[bblk] != 0 || (BS_FL_TYPE(fl) == FL_WATER && BS_FL_SOURCE(fl) && t_veg && t_veg[bblk] != 0);
        }
        if (!strcmp(bb->cls, "BigDripleafBlock"))
            return bblk == blk || !strcmp(bs->blk[bblk].name, "minecraft:big_dripleaf_stem") || t_big[bblk] != 0;
        { int above = fc_get(c, x, y + 1, z), ablk = c->g->state_block[above];                      /* BigDripleafStemBlock */
          return (bblk == blk || t_big[bblk] != 0) && (ablk == blk || !strcmp(bs->blk[ablk].name, "minecraft:big_dripleaf")); }
    }
    /* MangrovePropaguleBlock.canSurvive: висящий — над ним тег supports_hanging_mangrove_propagule; обычный — под ним supports_mangrove_propagule (supports_vegetation + глина).
     * Без правила попытки «мангрового дерева» шли и по кронам (внизу листва): would_survive пропускал всё, сервер такие попытки отбрасывает */
    if (!strcmp(bb->cls, "MangrovePropaguleBlock")) {
        const u8 *t_ground = veg_tag(c, "minecraft:supports_mangrove_propagule"), *t_hang = veg_tag(c, "minecraft:supports_hanging_mangrove_propagule");
        const char *h = NULL;
        if (bs_get_prop(bs, st, "hanging", &h) && h && !strcmp(h, "true")) return t_hang[c->g->state_block[fc_get(c, x, y + 1, z)]] != 0;
        return t_ground[c->g->state_block[fc_get(c, x, y - 1, z)]] != 0;
    }
    const char *tag = NULL;
    if (bs_is_a(bs, st, "DoublePlantBlock")) {
        const char *half = NULL; bs_get_prop(bs, st, "half", &half);
        if (half && !strcmp(half, "upper")) {          /* верхняя половина: снизу — нижняя половина того же блока */
            int below = fc_get(c, x, y - 1, z);
            return c->g->state_block[below] == c->g->state_block[st] && bs_get_prop(bs, below, "half", &half) && !strcmp(half, "lower");
        }
    }
    for (int i = 0; SURV_CLS[i].cls && !tag; i++) if (!strcmp(bb->cls, SURV_CLS[i].cls)) tag = SURV_CLS[i].tag;
    for (int i = 0; SURV_BLK[i].block && !tag; i++) if (!strcmp(bb->name, SURV_BLK[i].block)) tag = SURV_BLK[i].tag;
    for (int i = 0; PLAIN_VEG[i] && !tag; i++) if (!strcmp(bb->cls, PLAIN_VEG[i])) tag = "minecraft:supports_vegetation";
    if (!tag && (!strcmp(bb->cls, "DoublePlantBlock"))) tag = "minecraft:supports_vegetation";
    if (!tag) return 1;
    static _Thread_local const McGen *cg; static _Thread_local const char *ck[16]; static _Thread_local const u8 *cv[16]; static _Thread_local int cn;
    if (cg != c->g) { cg = c->g; cn = 0; }
    const u8 *t = NULL;
    for (int i = 0; i < cn; i++) if (ck[i] == tag) { t = cv[i]; break; }          /* теги — строковые литералы: ключ по указателю */
    if (!t) { t = gen_block_tag(c->g, tag); if (cn < 16) { ck[cn] = tag; cv[cn++] = t; } }
    int below = fc_get(c, x, y - 1, z);
    return t[c->g->state_block[below]] != 0;
}

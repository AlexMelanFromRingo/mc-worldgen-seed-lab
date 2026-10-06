/* fluidpp.c — «пост-обработка» жидкостей при переходе чанка в статус FULL (LevelChunk.postProcessGeneration).
 *
 * Генерация помечает позиции жидкостей (ProtoChunk.markPosForPostProcessing): aquifer — где соседние водоёмы
 * разных уровней («shouldScheduleFluidUpdate»), поверхность и карверы — где ставят жидкость. Когда чанк становится
 * FULL, игра один раз вызывает FluidState.tick для каждой помеченной позиции (по секциям снизу вверх, в порядке
 * пометки): FlowingFluid.tick → getNewLiquid/spread/spreadToSides/getSpread/getSlopeDistance — это растекание на один
 * шаг, включая правило «бесконечной воды» (два источника рядом + твёрдое под → новый источник). Запланированные
 * дальнейшие тики не выполняются (их нет в сохранённом мире), но мгновенные реакции выполняются: лава рядом с водой
 * становится обсидианом/булыжником (LiquidBlock.shouldSpreadLiquid в onPlace/neighborChanged), лава, стекающая в
 * воду, — камнем (LavaFluid.spreadTo).
 *
 * Классы блоков для стадий рельефа: воздух, вода, лава, «полные» твёрдые блоки. Формы неполных блоков (ступени,
 * плиты, растения…) — задача стадий декораций; здесь всё непрозрачное считается полным блоком.
 */
#include "mcgen_internal.h"
#include "blockstate.h"
#include "fluidpp.h"
#include <stdlib.h>
#include <stdio.h>

/* ---------------- классификация состояний ---------------- */
enum { FT_NONE = 0, FT_WATER = 1, FT_LAVA = 2 };
typedef struct { u8 type, source, amount, falling; } FS;   /* FluidState */

typedef struct {
    const McGen *g;
    const struct BsTab *bs;        /* флаги состояний блоков: форма столкновения/опорная грань (при block_flags.json), иначе всё непрозрачное — «полный» блок */
    int water[16], lava[16];       /* состояния water[level=i], lava[level=i] */
    int obsidian, cobblestone, stone;
    const u8 *washed;              /* тег washed_away_by_fluids (26.3+) */
    u8 *legacy_hold;               /* 26.1/26.2: canHoldAnyFluid по коду игры (тега ещё нет), по состояниям */
    u8 *container;                 /* LiquidBlockContainer: свойство waterlogged, ламинария, морская трава (canHoldAnyFluid = да; пускают только источник воды) */
    u8 *no_liquid;                 /* canPlaceLiquid всегда нет: ламинария, морская трава, двойная плита */
} FTab;

static FTab *ftab_get(const McGen *g) {
    static _Thread_local FTab *cache; static _Thread_local const McGen *owner;
    if (owner == g && cache) return cache;
    FTab *t = xcalloc(1, sizeof *t);
    t->g = g; t->bs = bs_get(g);
    for (int i = 0; i < 16; i++) {
        char n[64];
        snprintf(n, sizeof n, "minecraft:water[level=%d]", i); t->water[i] = gen_state_id(g, n);
        snprintf(n, sizeof n, "minecraft:lava[level=%d]", i); t->lava[i] = gen_state_id(g, n);
    }
    t->obsidian = gen_state_id(g, "minecraft:obsidian");
    t->cobblestone = gen_state_id(g, "minecraft:cobblestone");
    t->stone = gen_state_id(g, "minecraft:stone");
    t->washed = gen_block_tag(g, "minecraft:washed_away_by_fluids");
    t->container = xcalloc((size_t)g->nstates, 1); t->no_liquid = xcalloc((size_t)g->nstates, 1);
    for (int st = 0; st < g->nstates; st++) {
        const char *n = g->state_names[st];
        int plant = strstr(n, "minecraft:kelp") == n || strstr(n, "minecraft:seagrass") == n || strstr(n, "minecraft:tall_seagrass") == n;
        t->container[st] = (u8)(plant || strstr(n, "waterlogged=") != NULL);
        t->no_liquid[st] = (u8)(plant || (strstr(n, "_slab[") && strstr(n, "type=double")));
    }
    if (!g->newf) {
        /* 26.1/26.2 FlowingFluid.canHoldAnyFluid: LiquidBlockContainer → да; blocksMotion → нет; иначе нет только у дверей,
         * табличек, лестницы, тростника, пузырькового столба, порталов, шлюза Края, structure_void */
        static const char *EXCL[] = { "door[", "sign[", "minecraft:ladder[", "minecraft:sugar_cane[", "minecraft:bubble_column[",
                                      "minecraft:nether_portal[", "minecraft:end_portal", "minecraft:end_gateway", "minecraft:structure_void" };
        t->legacy_hold = xcalloc((size_t)g->nstates, 1);
        for (int st = 0; st < g->nstates; st++) {
            const char *n = g->state_names[st];
            char nb[256]; snprintf(nb, sizeof nb, "%s[", n);   /* состояние без свойств — тоже с «[» для сравнения */
            int container = strstr(n, "waterlogged=") || strstr(n, "minecraft:kelp") || strstr(n, "seagrass");
            int hold = 0;
            if (container) hold = 1;
            else if (!(g->state_cls[st] & 2)) {          /* CL_MOTION: blocksMotion */
                hold = 1;
                for (size_t k = 0; k < sizeof EXCL / sizeof *EXCL; k++) if (strstr(nb, EXCL[k])) hold = 0;
                if (strstr(n, "_door") && strchr(n, '[')) hold = 0;
            }
            t->legacy_hold[st] = (u8)hold;
        }
    }
    cache = t; owner = g;
    return t;
}
static FS fluid_of(const FTab *t, int st) {
    FS f = { FT_NONE, 0, 0, 0 };
    const McGen *g = t->g;
    if (st < 0) return f;
    int blk = g->state_block[st];
    if (blk == g->blk_water || blk == g->blk_lava) {
        int base = blk == g->blk_water ? t->water[0] : t->lava[0];
        int level = st - base;
        if (level < 0 || level > 15) {   /* на всякий случай — поиск */
            const int *arr = blk == g->blk_water ? t->water : t->lava;
            for (level = 0; level < 16 && arr[level] != st; level++) ;
        }
        f.type = blk == g->blk_water ? FT_WATER : FT_LAVA;
        if (level == 0) { f.source = 1; f.amount = 8; }
        else if (level < 8) { f.amount = (u8)(8 - level); }
        else { f.amount = 8; f.falling = 1; }
        return f;
    }
    if (t->bs->exported) {          /* BlockState.getFluidState() из настоящих классов игры (waterlogged, морская трава, ламинария, пузырьки…) */
        u16 fl = t->bs->fluid[st]; int ty = BS_FL_TYPE(fl);
        if (ty == FL_WATER || ty == FL_FLOWING_WATER) f.type = FT_WATER; else if (ty == FL_LAVA || ty == FL_FLOWING_LAVA) f.type = FT_LAVA; else return f;
        f.source = (u8)BS_FL_SOURCE(fl); f.amount = (u8)BS_FL_AMOUNT(fl); f.falling = (u8)BS_FL_FALLING(fl);
        return f;
    }
    if (g->state_cls[st] & 4) { f.type = FT_WATER; f.source = 1; f.amount = 8; }   /* waterlogged и т. п. */
    return f;
}
static int legacy_block(const FTab *t, FS f) {
    int lvl = f.source ? 0 : 8 - (f.amount < 8 ? f.amount : 8) + (f.falling ? 8 : 0);
    return f.type == FT_WATER ? t->water[lvl] : t->lava[lvl];
}
static inline int fs_eq(FS a, FS b) { return a.type == b.type && a.source == b.source && a.amount == b.amount && a.falling == b.falling; }
static inline int is_air(const FTab *t, int st) { return gen_is_air(t->g, st); }
/* форма коллизии: полный блок / пусто (воздух, жидкости) */
static inline int full_shape(const FTab *t, int st) {
    if (is_air(t, st)) return 0;
    int blk = t->g->state_block[st];
    if (blk == t->g->blk_water || blk == t->g->blk_lava) return 0;
    return 1;
}
/* грань блока в направлении dir (0 вниз, 1 вверх, 2 N, 3 S, 4 W, 5 E) закрывает клетку целиком: полный блок столкновения или полная опорная грань (приближение
 * Shapes.mergedFaceOccludes: пустые формы — растения, лишайник, ковры сбоку — жидкость пропускают). Без block_flags.json — прежнее «всё непрозрачное полное». */
static inline int face_full(const FTab *t, int st, int dir) {
    if (!full_shape(t, st)) return 0;
    const struct BsTab *bs = t->bs;
    if (!bs->exported) return 1;
    return ((bs->flags[st] & BSF_FULL_COLL) != 0) || ((bs->sturdy[st] >> dir) & 1);
}
/* BSF_SOLID: BlockState.isSolid() — для правила «бесконечной воды» (твёрдое под клеткой) */
static inline int is_solid(const FTab *t, int st) {
    if (!full_shape(t, st)) return 0;
    return t->bs->exported ? (t->bs->flags[st] & BSF_SOLID) != 0 : 1;
}
/* FlowingFluid.canHoldSpecificFluid: LiquidBlockContainer.canPlaceLiquid(…, newFluid) — SimpleWaterloggedBlock: type == Fluids.WATER (ИСТОЧНИК воды, не текущая вода и не лава);
 * ламинария/морская трава/двойная плита — нет. nf — новое состояние жидкости клетки */
static inline int can_hold_specific(const FTab *t, int st, FS nf) {
    if (!t->container[st]) return 1;
    return !t->no_liquid[st] && nf.type == FT_WATER && nf.source;
}
static inline int can_hold_any(const FTab *t, int st) {
    if (t->container[st]) return 1;
    if (t->legacy_hold) return t->legacy_hold[st];
    return t->washed && t->washed[t->g->state_block[st]];
}

/* ---------------- мир ---------------- */
typedef struct { FluidWorld *w; const FTab *t; int ft; } Ctx;   /* ft — тип текущей жидкости */
static inline int G(Ctx *c, int x, int y, int z) { return c->w->get(c->w->ud, x, y, z); }
static const int HX[4] = { 0, 1, 0, -1 }, HZ[4] = { -1, 0, 1, 0 };   /* Direction.Plane.HORIZONTAL: N, E, S, W */
static inline int opposite_h(int h) { return (h + 2) & 3; }

static int same_fluid(int a, int b) { return a == b && a != FT_NONE; }
static int drop_off(Ctx *c) { return c->ft == FT_WATER ? 1 : (c->w->fast_lava ? 1 : 2); }
static int slope_dist(Ctx *c) { return c->ft == FT_WATER ? 4 : (c->w->fast_lava ? 4 : 2); }
static int can_convert(Ctx *c) { return c->ft == FT_WATER ? c->w->water_conversion : c->w->lava_conversion; }

static void set_and_update(Ctx *c, int x, int y, int z, int st);

/* LiquidBlock.shouldSpreadLiquid (для лавы): вода над или сбоку → обсидиан/булыжник */
static int lava_check(Ctx *c, int x, int y, int z) {
    int st = G(c, x, y, z);
    FS f = fluid_of(c->t, st);
    if (c->t->g->state_block[st] != c->t->g->blk_lava) return 0;
    static const int NX[5] = { 0, 0, 0, -1, 1 }, NY[5] = { 1, 0, 0, 0, 0 }, NZ[5] = { 0, -1, 1, 0, 0 };   /* UP, N, S, W, E */
    for (int i = 0; i < 5; i++) {
        FS n = fluid_of(c->t, G(c, x + NX[i], y + NY[i], z + NZ[i]));
        if (n.type == FT_WATER) {
            set_and_update(c, x, y, z, f.source ? c->t->obsidian : c->t->cobblestone);
            return 1;
        }
    }
    return 0;
}
/* Level.setBlock(pos, state, 3): onPlace нового блока, затем neighborChanged у 6 соседей (W, E, D, U, N, S) */
static void set_and_update(Ctx *c, int x, int y, int z, int st) {
    int old = G(c, x, y, z);
    if (old == st) return;
    c->w->set(c->w->ud, x, y, z, st);
    lava_check(c, x, y, z);
    if (c->w->neighbors_update) c->w->neighbors_update(c->w, x, y, z);
    static const int UX[6] = { -1, 1, 0, 0, 0, 0 }, UY[6] = { 0, 0, -1, 1, 0, 0 }, UZ[6] = { 0, 0, 0, 0, -1, 1 };
    for (int i = 0; i < 6; i++) lava_check(c, x + UX[i], y + UY[i], z + UZ[i]);
}

static const int HDIR[4] = { 2, 5, 3, 4 };      /* горизонтальное h (N, E, S, W) → номер грани (DOWN 0, UP 1, N 2, S 3, W 4, E 5) */
static inline int opp_dir(int d) { return d ^ 1; }
/* FlowingFluid.canPassThroughWall(direction, from, to): !mergedFaceOccludes(from.collision, to.collision, direction) */
static int can_pass_wall(Ctx *c, int src_st, int tgt_st, int dir) { return !face_full(c->t, src_st, dir) && !face_full(c->t, tgt_st, opp_dir(dir)); }
static int is_source_of_type(Ctx *c, FS f) { return f.type == c->ft && f.source; }
static int can_maybe_pass(Ctx *c, int src_st, int tgt_st, int dir) {
    FS tf = fluid_of(c->t, tgt_st);
    return !is_source_of_type(c, tf) && can_hold_any(c->t, tgt_st) && can_pass_wall(c, src_st, tgt_st, dir);
}
static int is_water_hole(Ctx *c, int top_st, int bot_st) {
    if (!can_pass_wall(c, top_st, bot_st, 0)) return 0;
    FS bf = fluid_of(c->t, bot_st);
    return same_fluid(bf.type, c->ft) ? 1 : (can_hold_any(c->t, bot_st) && !c->t->container[bot_st]);   /* canHoldFluid(…, getFlowing()): контейнеры принимают только источник воды */
}
static FS new_liquid(Ctx *c, int x, int y, int z, int st) {
    int highest = 0, sources = 0;
    for (int h = 0; h < 4; h++) {
        int ns = G(c, x + HX[h], y, z + HZ[h]);
        FS nf = fluid_of(c->t, ns);
        if (same_fluid(nf.type, c->ft) && can_pass_wall(c, st, ns, HDIR[h])) {
            if (nf.source) sources++;
            if (nf.amount > highest) highest = nf.amount;
        }
    }
    FS r = { FT_NONE, 0, 0, 0 };
    if (sources >= 2 && can_convert(c)) {
        int bs = G(c, x, y - 1, z);
        FS bf = fluid_of(c->t, bs);
        if (is_solid(c->t, bs) || is_source_of_type(c, bf)) { r.type = (u8)c->ft; r.source = 1; r.amount = 8; return r; }
    }
    int as = G(c, x, y + 1, z);
    FS af = fluid_of(c->t, as);
    if (af.type != FT_NONE && same_fluid(af.type, c->ft) && can_pass_wall(c, st, as, 1)) { r.type = (u8)c->ft; r.amount = 8; r.falling = 1; return r; }
    int amount = highest - drop_off(c);
    if (amount <= 0) return r;
    r.type = (u8)c->ft; r.amount = (u8)amount;
    return r;
}

/* SpreadContext: кэш по (x−ox, z−oz) — y в ключ не входит (как в игре) */
typedef struct { int keys[512]; int vals[512]; u8 used[512]; } KMap;
static int km_find(KMap *m, int key, int *slot) {
    int i = (key * 2654435761u) >> 23 & 511;
    while (m->used[i] && m->keys[i] != key) i = (i + 1) & 511;
    *slot = i; return m->used[i];
}
typedef struct { int ox, oz; KMap st, hole; } Spread;
static int sc_state(Ctx *c, Spread *s, int x, int y, int z) {
    int key = (((x - s->ox + 128) & 0xFF) << 8) | ((z - s->oz + 128) & 0xFF), slot;
    if (km_find(&s->st, key, &slot)) return s->st.vals[slot];
    int v = G(c, x, y, z);
    s->st.used[slot] = 1; s->st.keys[slot] = key; s->st.vals[slot] = v;
    return v;
}
static int sc_hole(Ctx *c, Spread *s, int x, int y, int z) {
    int key = (((x - s->ox + 128) & 0xFF) << 8) | ((z - s->oz + 128) & 0xFF), slot;
    if (km_find(&s->hole, key, &slot)) return s->hole.vals[slot];
    int st = sc_state(c, s, x, y, z);
    int bs = G(c, x, y - 1, z);
    int v = is_water_hole(c, st, bs);
    km_find(&s->hole, key, &slot);
    s->hole.used[slot] = 1; s->hole.keys[slot] = key; s->hole.vals[slot] = v;
    return v;
}
static int slope_distance(Ctx *c, Spread *s, int x, int y, int z, int pass, int from_h, int st) {
    int lowest = 1000;
    for (int h = 0; h < 4; h++) {
        if (h == from_h) continue;
        int tx = x + HX[h], tz = z + HZ[h];
        int ts = sc_state(c, s, tx, y, tz);
        if (can_maybe_pass(c, st, ts, HDIR[h]) && !c->t->container[ts]) {   /* canPassThrough: canHoldSpecificFluid(getFlowing()) — контейнеры (type == Fluids.WATER) не пускают */
            if (sc_hole(c, s, tx, y, tz)) return pass;
            if (pass < slope_dist(c)) {
                int v = slope_distance(c, s, tx, y, tz, pass + 1, opposite_h(h), ts);
                if (v < lowest) lowest = v;
            }
        }
    }
    return lowest;
}
/* Fluid.canBeReplacedWith для жидкости target_at в (x,y,z) */
static int can_be_replaced(Ctx *c, FS target_at, int x, int y, int z, int new_type, int dir_down) {
    if (target_at.type == FT_NONE) return 1;
    if (target_at.type == FT_WATER) return dir_down && new_type != FT_WATER;
    /* лава: getHeight ≥ 0.44444445 (та же жидкость сверху → 1.0, иначе amount/9) и заменяет вода */
    FS above = fluid_of(c->t, G(c, x, y + 1, z));
    float h = same_fluid(above.type, target_at.type) ? 1.0f : (float)target_at.amount / 9.0f;
    return h >= 0.44444445f && new_type == FT_WATER;
}
static void spread_to(Ctx *c, int x, int y, int z, int st, int dir_down, FS target) {
    if (c->ft == FT_LAVA && dir_down) {
        FS at = fluid_of(c->t, st);
        if (at.type == FT_WATER) {
            if (c->t->g->state_block[st] == c->t->g->blk_water) set_and_update(c, x, y, z, c->t->stone);
            return;
        }
    }
    if (c->t->container[st]) {                  /* LiquidBlockContainer.placeLiquid: SimpleWaterloggedBlock — waterlogged = true (только источник воды), без замены блока */
        if (target.type == FT_WATER && target.source && !c->t->no_liquid[st]) {
            const char *wl = NULL;
            if (bs_get_prop(c->t->bs, st, "waterlogged", &wl) && wl && wl[0] == 'f') {
                int ns = bs_with(c->t->bs, st, "waterlogged", "true");
                if (ns >= 0) set_and_update(c, x, y, z, ns);
            }
        }
        return;
    }
    set_and_update(c, x, y, z, legacy_block(c->t, target));
}
static void spread_to_sides(Ctx *c, int x, int y, int z, FS f, int st) {
    int nb = f.amount - drop_off(c);
    if (f.falling) nb = 7;
    if (nb <= 0) return;
    /* getSpread: обход N, E, S, W; результат — EnumMap (порядок ординалов N, S, W, E) */
    int lowest = 1000; int has[4] = {0}; FS nf[4];
    Spread *s = NULL; Spread sp;
    for (int h = 0; h < 4; h++) {
        int tx = x + HX[h], tz = z + HZ[h];
        int ts = G(c, tx, y, tz);
        FS tf = fluid_of(c->t, ts);
        if (!can_maybe_pass(c, st, ts, HDIR[h])) continue;
        FS newf = new_liquid(c, tx, y, tz, ts);
        if (!can_hold_specific(c->t, ts, newf)) continue;          /* canHoldSpecificFluid(testPos, newFluid.getType()) */
        if (!s) { memset(&sp, 0, sizeof sp); sp.ox = x; sp.oz = z; s = &sp; }
        int dist = sc_hole(c, s, tx, y, tz) ? 0 : slope_distance(c, s, tx, y, tz, 1, opposite_h(h), ts);
        if (dist < lowest) memset(has, 0, sizeof has);
        if (dist <= lowest) {
            if (can_be_replaced(c, tf, tx, y, tz, newf.type, 0)) { has[h] = 1; nf[h] = newf; }
            lowest = dist;
        }
    }
    static const int ORD[4] = { 0, 2, 3, 1 };   /* N, S, W, E (Direction.ordinal) */
    for (int k = 0; k < 4; k++) {
        int h = ORD[k];
        if (!has[h]) continue;
        int tx = x + HX[h], tz = z + HZ[h];
        spread_to(c, tx, y, tz, G(c, tx, y, tz), 0, nf[h]);
    }
}
static int source_neighbors(Ctx *c, int x, int y, int z) {
    int n = 0;
    for (int h = 0; h < 4; h++) if (is_source_of_type(c, fluid_of(c->t, G(c, x + HX[h], y, z + HZ[h])))) n++;
    return n;
}
static void spread(Ctx *c, int x, int y, int z, int st, FS f) {
    if (f.type == FT_NONE) return;
    int bs = G(c, x, y - 1, z);
    FS bf = fluid_of(c->t, bs);
    if (can_maybe_pass(c, st, bs, 0)) {
        FS nb = new_liquid(c, x, y - 1, z, bs);
        if (can_be_replaced(c, bf, x, y - 1, z, nb.type, 1) && can_hold_specific(c->t, bs, nb)) {
            spread_to(c, x, y - 1, z, bs, 1, nb);
            if (source_neighbors(c, x, y, z) >= 3) spread_to_sides(c, x, y, z, f, st);
            return;
        }
    }
    if (f.source || !is_water_hole(c, st, bs)) spread_to_sides(c, x, y, z, f, st);
}

/* FlowingFluid.tick */
void fluidpp_tick(FluidWorld *w, int x, int y, int z) {
    Ctx c; c.w = w; c.t = ftab_get(w->g);
    int st = G(&c, x, y, z);
    FS f = fluid_of(c.t, st);
    if (f.type == FT_NONE) return;
    c.ft = f.type;
    if (!f.source) {
        FS nf = new_liquid(&c, x, y, z, G(&c, x, y, z));
        if (nf.type == FT_NONE) { f = nf; st = w->g->st_air; set_and_update(&c, x, y, z, st); }
        else if (!fs_eq(nf, f)) { f = nf; st = legacy_block(c.t, f); set_and_update(&c, x, y, z, st); }
    }
    spread(&c, x, y, z, st, f);
}

/* ---------------- пометки ---------------- */
void ppmarks_clear(PPMarks *m) { for (int i = 0; i < m->nsec; i++) m->n[i] = 0; }
void ppmarks_add(PPMarks *m, int sec, int lx, int ly, int lz) {
    if (sec < 0) return;
    if (sec >= m->nsec) {
        int ns = sec + 1;
        m->pos = xrealloc(m->pos, sizeof(u16 *) * (size_t)ns); m->n = xrealloc(m->n, sizeof(int) * (size_t)ns); m->cap = xrealloc(m->cap, sizeof(int) * (size_t)ns);
        for (int i = m->nsec; i < ns; i++) { m->pos[i] = NULL; m->n[i] = 0; m->cap[i] = 0; }
        m->nsec = ns;
    }
    if (m->n[sec] == m->cap[sec]) { m->cap[sec] = m->cap[sec] ? m->cap[sec] * 2 : 16; m->pos[sec] = xrealloc(m->pos[sec], sizeof(u16) * (size_t)m->cap[sec]); }
    m->pos[sec][m->n[sec]++] = (u16)((lx & 15) | ((ly & 15) << 4) | ((lz & 15) << 8));
}
void ppmarks_free(PPMarks *m) { for (int i = 0; i < m->nsec; i++) free(m->pos[i]); free(m->pos); free(m->n); free(m->cap); memset(m, 0, sizeof *m); }
void ppmarks_copy(PPMarks *dst, const PPMarks *src) {
    ppmarks_clear(dst);
    for (int s = 0; s < src->nsec; s++) for (int i = 0; i < src->n[s]; i++) {
        u16 p = src->pos[s][i];
        ppmarks_add(dst, s, p & 15, (p >> 4) & 15, (p >> 8) & 15);
    }
}
/* LevelChunk.postProcessGeneration: секции снизу вверх, позиции в порядке пометки */
void fluidpp_chunk(FluidWorld *w, const PPMarks *m, int cx, int cz, int min_y) {
    for (int s = 0; s < m->nsec; s++) for (int i = 0; i < m->n[s]; i++) {
        u16 p = m->pos[s][i];
        int x = cx * 16 + (p & 15), y = min_y + s * 16 + ((p >> 4) & 15), z = cz * 16 + ((p >> 8) & 15);
        fluidpp_tick(w, x, y, z);
        if (w->shape_update) w->shape_update(w, x, y, z);
    }
}

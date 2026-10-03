/* structures/igloo.c — IglooStructure/IglooPieces (Java-кодированная постройка из шаблонов igloo/{top,middle,bottom}).
 * Старт: чанк-угол, y = 90, случайный поворот; с вероятностью 1/2 под иглу строится подвал (bottom + (depth-1) × middle).
 * Часть — TemplateStructurePiece: при рисовании шаблон сдвигается по высоте к WORLD_SURFACE_WG у входа (height - 91), маркер "chest" превращается
 * в воздух (сундук ниже получает loot table: random.nextLong()), а у верхней части люк заменяется снежным блоком, если под ним не пусто и не лестница. */
#include "../structure_piece.h"
#include <stdio.h>
#include <stdlib.h>

extern const PieceVT PIECE_IGLOO;
enum { IG_TOP = 0, IG_MIDDLE = 1, IG_BOTTOM = 2 };
static const char *IG_NAME[3] = { "minecraft:igloo/top", "minecraft:igloo/middle", "minecraft:igloo/bottom" };
static const int IG_PIVOT[3][3] = { {3, 5, 5}, {1, 3, 1}, {3, 6, 7} };
static const int IG_OFFSET[3][3] = { {0, 0, 0}, {2, -3, 4}, {0, -3, -2} };

typedef struct IGData { const Template *t; int loc, rot; int tx, ty, tz; } IGData;

static void ig_free(void *v) { free(v); }

static void ig_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    IGData *d = p->data;
    McWorld *w = c->w;
    const int px = IG_PIVOT[d->loc][0], pz = IG_PIVOT[d->loc][2];
    /* entrancePos = templatePosition + calculateRelativePosition(settings, (3 - offset.x, 0, -offset.z)) */
    int ex, ez;
    tpl_transform(3 - IG_OFFSET[d->loc][0], -IG_OFFSET[d->loc][2], MIR_NONE, d->rot, px, pz, &ex, &ez);
    int height = sp_height(c, HM_WORLD_SURFACE_WG, d->tx + ex, d->tz + ez);
    int ty = d->ty + height - 90 - 1;
    /* TemplateStructurePiece.postProcess */
    p->bb = tpl_bounding_box(d->t, d->tx, ty, d->tz, d->rot, MIR_NONE, px, pz);
    TSettings s; tsettings_init(&s);
    const Proc *procs[1] = { proc_builtin(w, PB_STRUCTURE_BLOCK) };
    s.rot = d->rot; s.mir = MIR_NONE; s.px = px; s.pz = pz; s.bounds = &c->chunk; s.procs = procs; s.nprocs = 1;
    s.waterlog = 0;                                                        /* LiquidSettings.IGNORE_WATERLOGGING */
    if (template_place(c->fc, w, d->t, d->tx, ty, d->tz, rx, ry, rz, &s, w->seeds.structures, 2)) {
        const TPal *pal = tpl_palette_at(d->t, d->tx, ty, d->tz);
        static _Thread_local const BsTab *kb; static _Thread_local int sblock, chest;
        const BsTab *bs = c->sw->bs;
        if (kb != bs) { sblock = bs_block_index(bs, "minecraft:structure_block"); chest = bs_block_index(bs, "minecraft:chest"); kb = bs; }
        for (int i = 0; pal && i < pal->nb; i++) {                         /* filterBlocks(STRUCTURE_BLOCK) → handleDataMarker */
            const TInfo *b = &pal->b[i];
            if (w->g->state_block[b->state] != sblock || !b->nbt) continue;
            int wx, wz; tpl_transform(b->x, b->z, MIR_NONE, d->rot, px, pz, &wx, &wz);
            wx += d->tx; wz += d->tz; int wy = b->y + ty;
            if (!bb_inside(&c->chunk, wx, wy, wz)) continue;
            const char *mode = nbt_str(nbt_get(b->nbt, "mode"), "");
            if (strcmp(mode, "DATA")) continue;
            const char *meta = nbt_str(nbt_get(b->nbt, "metadata"), "");
            if (strcmp(meta, "chest")) continue;
            sp_set_world(c, wx, wy, wz, sp_air(c));
            if (bb_inside(&c->chunk, wx, wy - 1, wz) && w->g->state_block[sp_get_world(c, wx, wy - 1, wz)] == chest) (void)rs_long(c->rs);   /* setLootTable(IGLOO_CHEST, random.nextLong()) */
        }
    }
    if (d->loc == IG_TOP) {
        int tx, tz; tpl_transform(3, 5, MIR_NONE, d->rot, px, pz, &tx, &tz);
        tx += d->tx; tz += d->tz; int tyy = ty;
        if (bb_inside(&c->chunk, tx, tyy, tz)) {
            int below = sp_get_world(c, tx, tyy - 1, tz);
            static _Thread_local const BsTab *kb2; static _Thread_local int ladder;
            const BsTab *bs = c->sw->bs;
            if (kb2 != bs) { ladder = bs_block_index(bs, "minecraft:ladder"); kb2 = bs; }
            if (!gen_is_air(w->g, below) && w->g->state_block[below] != ladder) sp_set_world(c, tx, tyy, tz, sp_st(c, "minecraft:snow_block"));
        }
    }
}

static void ig_dump(const StPiece *p, StrBuf *o) {
    const IGData *d = p->data;
    sb_printf(o, ",\"tpl\":\"%s\",\"tpos\":[%d,%d,%d],\"trot\":%d", IG_NAME[d->loc], d->tx, d->ty, d->tz, d->rot);
}

const PieceVT PIECE_IGLOO = { "minecraft:iglu", ig_post, NULL, ig_free, ig_dump, NULL, NULL };

/* ---------------------------------------------------------------- структура */
static void *ig_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) { (void)sw; (void)cfg; (void)err; (void)errlen; return xcalloc(1, 1); }
static int ig_find(GenCtx *c, const void *cfg, Stub *out) {
    (void)cfg;
    if (!gen_could_exist_on_chunk_center(c)) return 0;               /* onTopOfChunkCenter(WORLD_SURFACE_WG) */
    int bx = c->cx * 16 + 8, bz = c->cz * 16 + 8;
    out->x = bx; out->z = bz; out->y = gen_first_occupied_height(c, bx, bz, HM_WORLD_SURFACE_WG); out->state = NULL;
    return 1;
}
static StPiece *ig_piece(GenCtx *c, int loc, int x, int y, int z, int rot, int depth) {
    const Template *t = template_get(c->w, IG_NAME[loc]);
    if (!t) return NULL;
    IGData *d = xcalloc(1, sizeof *d);
    d->t = t; d->loc = loc; d->rot = rot;
    d->tx = x + IG_OFFSET[loc][0]; d->ty = y + IG_OFFSET[loc][1] - depth; d->tz = z + IG_OFFSET[loc][2];
    BB bb = tpl_bounding_box(t, d->tx, d->ty, d->tz, rot, MIR_NONE, IG_PIVOT[loc][0], IG_PIVOT[loc][2]);
    StPiece *p = piece_new(&PIECE_IGLOO, bb, dir_to_2d(DIR_NORTH), 0, d);
    p->rot = rot; p->mir = MIR_NONE;
    return p;
}
static int ig_build(GenCtx *c, const void *cfg, Stub *stub, PieceVec *out) {
    (void)cfg; (void)stub;
    int x = c->cx * 16, y = 90, z = c->cz * 16;
    int rot = rs_bound(&c->rs, 4);                                    /* Rotation.getRandom */
    if (rs_double(&c->rs) < 0.5) {
        int depth = rs_bound(&c->rs, 8) + 4;
        StPiece *p = ig_piece(c, IG_BOTTOM, x, y, z, rot, depth * 3);
        if (!p) return 0;
        pvec_push(out, p);
        for (int i = 0; i < depth - 1; i++) {
            p = ig_piece(c, IG_MIDDLE, x, y, z, rot, i * 3);
            if (!p) return 0;
            pvec_push(out, p);
        }
    }
    StPiece *p = ig_piece(c, IG_TOP, x, y, z, rot, 0);
    if (!p) return 0;
    pvec_push(out, p);
    return 1;
}
static void ig_free_cfg(void *p) { free(p); }
const StructType STRUCT_IGLOO = { "minecraft:igloo", ig_parse, ig_find, ig_build, NULL, ig_free_cfg, NULL };
void structures_register_igloo(void) { structure_register_type(&STRUCT_IGLOO); piece_register_type(&PIECE_IGLOO); }

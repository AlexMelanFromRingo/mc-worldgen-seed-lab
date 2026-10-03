/* structures/nether_fossil.c — NetherFossilStructure/NetherFossilPieces (minecraft:nether_fossil; часть minecraft:nefos).
 * Точка: случайная в чанке, высота из HeightProvider, затем вниз по колонке заполнения шумом (getBaseColumn) до воздуха над
 * песком душ/прочной верхней гранью (выше уровня моря). Шаблон nether_fossils/fossil_1..14, BlockIgnore STRUCTURE_AND_AIR.
 * Рисование: chunkBB.encapsulate(bb окаменелости) — каждая задетая часть рисует окаменелость целиком (в окне записи), затем
 * «высохший гаст» по позиционному ГСЧ от центра bb. */
#include "shipwreck_tpl.h"
#include <stdio.h>
#include <stdlib.h>

/* ChunkGenerator.getBaseColumn (structure.c; слабая ссылка — пока функции нет в каркасе, постройка не создаётся) */
extern int gen_base_column(GenCtx *c, int x, int z, int *out, int *y0) __attribute__((weak));

extern const PieceVT PIECE_NETHER_FOSSIL;
typedef struct NFData { TplPiece tp; } NFData;

static void nf_free(void *v) { free(v); }
static void nf_move(StPiece *p, int dx, int dy, int dz) { NFData *d = p->data; tplp_move(&d->tp, dx, dy, dz); }
static void nf_dump(const StPiece *p, StrBuf *o) { const NFData *d = p->data; tplp_dump(&d->tp, o); }

static void nf_post(StCtx *c, StPiece *p, int rx, int ry, int rz) {
    NFData *d = p->data;
    BB fb = tplp_bb(&d->tp);
    c->chunk = bb_union(c->chunk, fb);                               /* chunkBB.encapsulate(fossilBB): общий chunkBB шага */
    tplp_post(c, p, &d->tp, &c->chunk, rx, ry, rz, NULL, NULL);
    /* placeDriedGhast */
    RS pr = sp_seed_positional(c, bb_cx(&fb), bb_cy(&fb), bb_cz(&fb));
    if (rs_float(&pr) < 0.5f) {
        int x = fb.x0 + rs_bound(&pr, bb_xspan(&fb));
        int y = fb.y0;
        int z = fb.z0 + rs_bound(&pr, bb_zspan(&fb));
        if ((c->sw->bs->flags[fc_get(c->fc, x, y, z)] & BSF_AIR) && bb_inside(&c->chunk, x, y, z)) {
            int st = bsx_rotate(c->sw->bs, sp_st(c, "minecraft:dried_ghast"), rs_bound(&pr, 4));
            fc_set(c->fc, x, y, z, st, 2);
        }
    }
}
const PieceVT PIECE_NETHER_FOSSIL = { "minecraft:nefos", nf_post, nf_move, nf_free, nf_dump, NULL, NULL };

/* ---------------------------------------------------------------- структура */
typedef struct NFCfg { HProv *height; } NFCfg;
static void *nf_parse(StructWorld *sw, const Js *cfg, char *err, size_t errlen) {
    (void)sw;
    NFCfg *c = xcalloc(1, sizeof *c);
    c->height = hprov_parse(js_get(cfg, "height"), err, errlen);
    if (!c->height) { free(c); return NULL; }
    return c;
}
typedef struct NFStub { int rot, idx; } NFStub;
static int nf_find(GenCtx *c, const void *cfgp, Stub *out) {
    const NFCfg *cfg = cfgp;
    if (!gen_base_column) {
        static int warned; if (!warned) { warned = 1; fprintf(stderr, "nether_fossil: в каркасе нет gen_base_column — постройка пропущена\n"); }
        return 0;
    }
    int bx = c->cx * 16 + rs_bound(&c->rs, 16);
    int bz = c->cz * 16 + rs_bound(&c->rs, 16);
    int sea = c->w->sea_level;
    int y = hprov_sample(cfg->height, &c->rs, c->gen_min_y, c->gen_depth, sea);
    int *col = xmalloc((size_t)(c->height > 0 ? c->height : 1) * sizeof(int)); int y0 = 0;
    int n = gen_base_column(c, bx, bz, col, &y0);
    const BsTab *bs = c->bs; int air = c->w->g->st_air;
#define COL(yy) (((yy) >= y0 && (yy) - y0 < n) ? col[(yy) - y0] : air)
    int soul = bs_block_index(bs, "minecraft:soul_sand");
    while (y > sea) {
        int cur = COL(y);
        int below = COL(y - 1); y--;
        if ((bs->flags[cur] & BSF_AIR) && (c->w->g->state_block[below] == soul || ((bs->sturdy[below] >> DIR_UP) & 1))) break;
    }
#undef COL
    free(col);
    if (y <= sea) return 0;
    out->x = bx; out->y = y; out->z = bz; out->state = NULL;
    return 1;
}
static int nf_build(GenCtx *c, const void *cfgp, Stub *stub, PieceVec *out) {
    (void)cfgp;
    int rot = rs_bound(&c->rs, 4);
    int idx = rs_bound(&c->rs, 14);
    char loc[96]; snprintf(loc, sizeof loc, "minecraft:nether_fossils/fossil_%d", idx + 1);
    NFData *d = xcalloc(1, sizeof *d);
    if (!tplp_init(c->w, &d->tp, loc, loc, stub->x, stub->y, stub->z, rot, MIR_NONE, 0, 0)) { free(d); return 0; }
    tplp_add_proc(&d->tp, proc_builtin(c->w, PB_STRUCTURE_AND_AIR));
    StPiece *p = piece_new(&PIECE_NETHER_FOSSIL, tplp_bb(&d->tp), -1, 0, d);
    sp_set_orientation(p, DIR_NORTH);
    pvec_push(out, p);
    return 1;
}
static void nf_free_cfg(void *p) { NFCfg *c = p; if (c) { hprov_free(c->height); free(c); } }
const StructType STRUCT_NETHER_FOSSIL = { "minecraft:nether_fossil", nf_parse, nf_find, nf_build, NULL, nf_free_cfg, NULL };

void structures_register_nether_fossil(void) { structure_register_type(&STRUCT_NETHER_FOSSIL); piece_register_type(&PIECE_NETHER_FOSSIL); }

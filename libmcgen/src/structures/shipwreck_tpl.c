/* structures/shipwreck_tpl.c — общая часть шаблонных частей (TemplateStructurePiece): см. shipwreck_tpl.h.
 * Запись блоков — общий template_place (StructureTemplate.placeInWorld: процессоры, заливка, knownShape == false → обновление форм,
 * nextLong() на контейнеры с NBT через TSettings.rnd); здесь — TemplateStructurePiece.postProcess вокруг него: bounding box,
 * маркеры данных (structure_block mode=DATA → handleDataMarker) и jigsaw-блоки (final_state). */
#include "shipwreck_tpl.h"
#include "../blockstate.h"
#include <stdio.h>
#include <stdlib.h>

int tplp_init(McWorld *w, TplPiece *tp, const char *loc, const char *name, int x, int y, int z, int rot, int mir, int px, int pz) {
    memset(tp, 0, sizeof *tp);
    tp->t = template_get(w, loc);
    snprintf(tp->name, sizeof tp->name, "%s", name);
    tp->x = x; tp->y = y; tp->z = z; tp->rot = rot; tp->mir = mir; tp->px = px; tp->pz = pz;
    tp->waterlog = 1; tp->known_shape = 0;
    return tp->t != NULL;
}
void tplp_add_proc(TplPiece *tp, const Proc *p) { if (p && tp->nprocs < 8) tp->procs[tp->nprocs++] = p; }

BB tplp_bb(const TplPiece *tp) {
    if (!tp->t) return bb_make(tp->x, tp->y, tp->z, tp->x, tp->y, tp->z);
    return tpl_bounding_box(tp->t, tp->x, tp->y, tp->z, tp->rot, tp->mir, tp->px, tp->pz);
}

void tplp_connected(const TplPiece *parent, int ox, int oy, int oz, const TplPiece *child, int *dx, int *dy, int *dz) {
    int ax, az, bx, bz;
    tpl_transform(ox, oz, parent->mir, parent->rot, parent->px, parent->pz, &ax, &az);
    tpl_transform(0, 0, child->mir, child->rot, child->px, child->pz, &bx, &bz);
    *dx = ax - bx; *dy = oy; *dz = az - bz;
}

int tplp_is_container(StCtx *c, int st) {
    const BsTab *bs = c->sw->bs;
    return bs_is_a(bs, st, "ChestBlock") || bs_is_a(bs, st, "BarrelBlock") || bs_is_a(bs, st, "DispenserBlock") || bs_is_a(bs, st, "HopperBlock")
        || bs_is_a(bs, st, "ShulkerBoxBlock") || bs_is_a(bs, st, "DecoratedPotBlock") || bs_is_a(bs, st, "CrafterBlock");
}
void tplp_loot(StCtx *c, int x, int y, int z) {
    if (!fc_chunk(c->fc, x, z) || fc_outside(c->fc, y)) return;
    if (tplp_is_container(c, fc_get(c->fc, x, y, z))) (void)rs_long(c->rs);
}

int tplp_post(StCtx *c, StPiece *p, TplPiece *tp, const BB *bounds, int rx, int ry, int rz, TplMarkerFn marker, void *ud) {
    McWorld *w = c->w; const BsTab *bs = c->sw->bs; FCtx *fc = c->fc; const McGen *g = bs->g;
    const Template *t = tp->t;
    p->bb = tplp_bb(tp);
    if (!t) return 0;
    const TPal *pal = tpl_palette_at(t, tp->x, tp->y, tp->z);
    if (!pal || pal->nb == 0 || t->sx < 1 || t->sy < 1 || t->sz < 1) return 0;
    TSettings s; tsettings_init(&s);
    s.rot = tp->rot; s.mir = tp->mir; s.px = tp->px; s.pz = tp->pz; s.bounds = bounds; s.waterlog = tp->waterlog;
    s.procs = tp->procs; s.nprocs = tp->nprocs;
    s.known_shape = tp->known_shape; s.rnd = c->rs;                  /* placeInWorld(…, random, 2): nextLong() на контейнеры с NBT */
    if (!template_place(fc, w, t, tp->x, tp->y, tp->z, rx, ry, rz, &s, w->seeds.structures, 2)) return 0;
    /* маркеры данных: filterBlocks(templatePosition, settings, STRUCTURE_BLOCK) */
    int sblk = bs_block_index(bs, "minecraft:structure_block"), jblk = bs_block_index(bs, "minecraft:jigsaw");
    for (int i = 0; i < pal->nb; i++) {
        const TInfo *b = &pal->b[i];
        if (g->state_block[b->state] != sblk || !b->nbt) continue;
        int wx, wz; tpl_transform(b->x, b->z, tp->mir, tp->rot, tp->px, tp->pz, &wx, &wz);
        wx += tp->x; wz += tp->z; int wy = b->y + tp->y;
        if (bounds && !bb_inside(bounds, wx, wy, wz)) continue;
        const char *mode = nbt_str(nbt_get(b->nbt, "mode"), "");
        if (strcmp(mode, "DATA") != 0) continue;
        if (marker) marker(c, p, ud, nbt_str(nbt_get(b->nbt, "metadata"), ""), wx, wy, wz, bounds);
    }
    /* jigsaw-блоки: setBlockAndUpdate(pos, final_state) */
    for (int i = 0; i < pal->nb; i++) {
        const TInfo *b = &pal->b[i];
        if (g->state_block[b->state] != jblk || !b->nbt) continue;
        int wx, wz; tpl_transform(b->x, b->z, tp->mir, tp->rot, tp->px, tp->pz, &wx, &wz);
        wx += tp->x; wz += tp->z; int wy = b->y + tp->y;
        if (bounds && !bb_inside(bounds, wx, wy, wz)) continue;
        int st = gen_state_id(g, nbt_str(nbt_get(b->nbt, "final_state"), "minecraft:air"));
        fc_set(fc, wx, wy, wz, st >= 0 ? st : g->st_air, 3);
    }
    return 1;
}

void tplp_dump(const TplPiece *tp, StrBuf *o) {
    sb_printf(o, ",\"tpl\":\"%s\",\"tp\":[%d,%d,%d],\"trot\":\"%s\",\"tmir\":%d", tp->name, tp->x, tp->y, tp->z, TPLP_ROTN[tp->rot & 3], tp->mir);
}

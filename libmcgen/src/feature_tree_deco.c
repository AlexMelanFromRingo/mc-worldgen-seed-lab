/* feature_tree_deco.c — TreeDecorator'ы (украшатели деревьев) и updateShapeAtEdge (StructureTemplate) для краёв дерева (поток W11). */
#include "feature_tree.h"
#include <stdio.h>
#include <stdlib.h>

static inline int blk_of(const FCtx *c, int st) { return c->g->state_block[st]; }
static const int HZ_DX[4] = { 0, 1, 0, -1 }, HZ_DZ[4] = { -1, 0, 1, 0 };          /* Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST */
static const int HZ_DIR[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };
static inline int opp(int d) { return d ^ 1; }                                       /* Direction.getOpposite для порядка DOWN,UP,NORTH,SOUTH,WEST,EAST */

/* ====================================================================== разбор */
static const char *tname(const char *t) { return !strncmp(t, "minecraft:", 10) ? t + 10 : t; }
static float fld(const Js *v, const char *k) { return js_numf(js_get(v, k), 0.0f); }

int tree_parse_decorators(FParse *p, const Js *arr, TreeDeco **out, int *n) {
    *out = NULL; *n = 0;
    if (!arr) return 1;
    if (!js_is_arr(arr)) return fp_fail(p, "decorators: ожидался массив");
    if (arr->n == 0) return 1;
    TreeDeco *l = fp_alloc(p, sizeof(TreeDeco) * (size_t)arr->n);
    for (int i = 0; i < arr->n; i++) {
        const Js *v = arr->items[i]; TreeDeco *d = &l[i];
        const char *t = js_str(js_get(v, "type"), NULL);
        if (!t) return fp_fail(p, "decorator: нет type");
        t = tname(t);
        if (!strcmp(t, "beehive")) { d->type = TD_BEEHIVE; d->prob = fld(v, "probability"); }
        else if (!strcmp(t, "cocoa")) { d->type = TD_COCOA; d->prob = fld(v, "probability"); }
        else if (!strcmp(t, "leave_vine")) { d->type = TD_LEAVE_VINE; d->prob = fld(v, "probability"); }
        else if (!strcmp(t, "trunk_vine")) d->type = TD_TRUNK_VINE;
        else if (!strcmp(t, "creaking_heart")) { d->type = TD_CREAKING_HEART; d->prob = fld(v, "probability"); }
        else if (!strcmp(t, "shelf_mushroom")) { d->type = TD_SHELF_MUSHROOM; d->prob = fld(v, "probability"); }
        else if (!strcmp(t, "alter_ground")) { d->type = TD_ALTER_GROUND; if (!(d->prov = fp_bsprov(p, js_get(v, "provider")))) return 0; }
        else if (!strcmp(t, "attached_to_leaves") || !strcmp(t, "attached_to_logs")) {
            d->type = !strcmp(t, "attached_to_leaves") ? TD_ATTACHED_LEAVES : TD_ATTACHED_LOGS;
            d->prob = fld(v, "probability");
            if (!(d->prov = fp_bsprov(p, js_get(v, "block_provider")))) return 0;
            const Js *ds = js_get(v, "directions");
            if (!js_is_arr(ds) || ds->n == 0 || ds->n > 6) return fp_fail(p, "directions");
            d->ndirs = ds->n;
            for (int k = 0; k < ds->n; k++) { d->dirs[k] = dir_from_name(js_str(ds->items[k], "")); if (d->dirs[k] < 0) return fp_fail(p, "direction"); }
            if (d->type == TD_ATTACHED_LEAVES) {
                d->excl_xz = js_int(js_get(v, "exclusion_radius_xz"), 0); d->excl_y = js_int(js_get(v, "exclusion_radius_y"), 0);
                d->required_empty = js_int(js_get(v, "required_empty_blocks"), 1);
            }
        } else if (!strcmp(t, "place_on_ground")) {
            d->type = TD_PLACE_ON_GROUND;
            d->tries = js_int(js_get(v, "tries"), 128); d->radius = js_int(js_get(v, "radius"), 2); d->height = js_int(js_get(v, "height"), 1);
            if (!(d->prov = fp_bsprov(p, js_get(v, "block_state_provider")))) return 0;
        } else if (!strcmp(t, "pale_moss")) {
            d->type = TD_PALE_MOSS;
            d->leaves_prob = fld(v, "leaves_probability"); d->trunk_prob = fld(v, "trunk_probability"); d->ground_prob = fld(v, "ground_probability");
            Js name = { JS_STR, 0, "minecraft:pale_moss_patch", 0, NULL, NULL };
            int un0 = p->nunimpl; (void)un0;
            d->moss_patch = fp_feature(p, &name);                    /* PALE_MOSS_PATCH (vegetation_patch: может быть не реализована) */
            if (!d->moss_patch) { p->err[0] = 0; }
        } else return fp_fail(p, "tree decorator: неизвестный тип %s", t);
    }
    *out = l; *n = arr->n;
    return 1;
}

/* ====================================================================== вспомогательные */
static inline int ctx_air(const TreeCtx *x, int px, int py, int pz) { return fc_is_air(x->c, fc_get(x->c, px, py, pz)); }
static inline void ctx_set(TreeCtx *x, int px, int py, int pz, int st) {
    if (x->tr) tr_set_decor(x->tr, px, py, pz, st); else fc_set(x->c, px, py, pz, st, 19);
}
static int block_state_named(const FCtx *c, const char *name) { int b = bs_block_index(c->bs, name); return b < 0 ? -1 : c->bs->blk[b].def; }
static int vine_face_state(const FCtx *c, int dir /* грань, на которой висит лоза */) {
    static const char *N[6] = { NULL, "up", "north", "south", "west", "east" };
    int v = block_state_named(c, "minecraft:vine");
    return v < 0 ? -1 : bs_with(c->bs, v, N[dir], "true");
}
static void place_vine(TreeCtx *x, int px, int py, int pz, int face) {
    int st = vine_face_state(x->c, face);
    if (st >= 0) ctx_set(x, px, py, pz, st);
}
static void shuffle_list(BList *l, FRnd *r) {              /* Util.shuffle */
    for (int i = l->n; i > 1; i--) { int j = frnd_int_bound(r, i); BPos t = l->a[i - 1]; l->a[i - 1] = l->a[j]; l->a[j] = t; }
}
static void copy_list(BList *dst, const BList *src) { blist_reset(dst); for (int i = 0; i < src->n; i++) blist_push(dst, src->a[i].x, src->a[i].y, src->a[i].z); }
/* TreeFeature.getLowestTrunkOrRootOfTree */
static void lowest_trunk_or_root(const TreeCtx *x, BList *out) {
    blist_reset(out);
    if (x->roots.n == 0) { for (int i = 0; i < x->logs.n; i++) blist_push(out, x->logs.a[i].x, x->logs.a[i].y, x->logs.a[i].z); }
    else if (x->logs.n > 0 && x->roots.a[0].y == x->logs.a[0].y) {
        for (int i = 0; i < x->logs.n; i++) blist_push(out, x->logs.a[i].x, x->logs.a[i].y, x->logs.a[i].z);
        for (int i = 0; i < x->roots.n; i++) blist_push(out, x->roots.a[i].x, x->roots.a[i].y, x->roots.a[i].z);
    } else for (int i = 0; i < x->roots.n; i++) blist_push(out, x->roots.a[i].x, x->roots.a[i].y, x->roots.a[i].z);
}

/* ====================================================================== украшатели */
static void deco_beehive(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r; FCtx *c = x->c;
    if (x->logs.n == 0) return;
    if (!(frnd_float(r) < d->prob)) return;
    int hive_y;
    if (x->leaves.n > 0) { int a = x->leaves.a[0].y - 1, b = x->logs.a[0].y + 1; hive_y = a > b ? a : b; }
    else { int a = x->logs.a[0].y + 1 + frnd_int_bound(r, 3), b = x->logs.a[x->logs.n - 1].y; hive_y = a < b ? a : b; }
    BList pl = {0};
    static const int SPAWN[3] = { DIR_EAST, DIR_SOUTH, DIR_WEST };            /* HORIZONTAL без NORTH (противоположное WORLDGEN_FACING=SOUTH) */
    for (int i = 0; i < x->logs.n; i++) if (x->logs.a[i].y == hive_y)
        for (int k = 0; k < 3; k++) blist_push(&pl, x->logs.a[i].x + DIR_DX[SPAWN[k]], hive_y, x->logs.a[i].z + DIR_DZ[SPAWN[k]]);
    if (pl.n > 0) {
        shuffle_list(&pl, r);
        for (int i = 0; i < pl.n; i++) {
            int px = pl.a[i].x, pz = pl.a[i].z;
            if (ctx_air(x, px, hive_y, pz) && ctx_air(x, px, hive_y, pz + 1)) {          /* WORLDGEN_FACING = SOUTH */
                int nest = block_state_named(c, "minecraft:bee_nest");
                if (nest >= 0) { int s2 = bs_with(c->bs, nest, "facing", "south"); if (s2 >= 0) nest = s2; ctx_set(x, px, hive_y, pz, nest); }
                int bees = 2 + frnd_int_bound(r, 2);                                      /* BlockEntity улья создаётся при записи блока */
                for (int b = 0; b < bees; b++) frnd_int_bound(r, 599);
                break;
            }
        }
    }
    free(pl.a);
}
static void deco_cocoa(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r; FCtx *c = x->c;
    if (!(frnd_float(r) < d->prob)) return;
    if (x->logs.n == 0) return;
    int tree_y = x->logs.a[0].y;
    int cocoa = block_state_named(c, "minecraft:cocoa");
    static const char *FN[6] = { NULL, NULL, "north", "south", "west", "east" };
    for (int i = 0; i < x->logs.n; i++) {
        if (x->logs.a[i].y - tree_y > 2) continue;
        for (int k = 0; k < 4; k++) {
            int dir = HZ_DIR[k];
            if (frnd_float(r) <= 0.25f) {
                int o = opp(dir);
                int px = x->logs.a[i].x + DIR_DX[o], py = x->logs.a[i].y, pz = x->logs.a[i].z + DIR_DZ[o];
                if (ctx_air(x, px, py, pz)) {
                    char age[4]; age[0] = (char)('0' + frnd_int_bound(r, 3)); age[1] = 0;
                    int st = cocoa;
                    if (st >= 0) { int s1 = bs_with(c->bs, st, "age", age); if (s1 >= 0) st = s1; int s2 = bs_with(c->bs, st, "facing", FN[dir]); if (s2 >= 0) st = s2; }
                    ctx_set(x, px, py, pz, st);
                }
            }
        }
    }
}
static void deco_leave_vine(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r;
    static const int NB[4] = { DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH };
    for (int i = 0; i < x->leaves.n; i++) {
        for (int k = 0; k < 4; k++) {
            if (frnd_float(r) < d->prob) {
                int px = x->leaves.a[i].x + DIR_DX[NB[k]], py = x->leaves.a[i].y, pz = x->leaves.a[i].z + DIR_DZ[NB[k]];
                if (ctx_air(x, px, py, pz)) {
                    int face = opp(NB[k]);
                    place_vine(x, px, py, pz, face);
                    int max_dir = 4;
                    for (int by = py - 1; ctx_air(x, px, by, pz) && max_dir > 0; max_dir--) { place_vine(x, px, by, pz, face); by--; }
                }
            }
        }
    }
}
static void deco_trunk_vine(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r;
    static const int NB[4] = { DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH };
    for (int i = 0; i < x->logs.n; i++) {
        for (int k = 0; k < 4; k++) {
            if (frnd_int_bound(r, 3) > 0) {
                int px = x->logs.a[i].x + DIR_DX[NB[k]], py = x->logs.a[i].y, pz = x->logs.a[i].z + DIR_DZ[NB[k]];
                if (ctx_air(x, px, py, pz)) place_vine(x, px, py, pz, opp(NB[k]));
            }
        }
    }
}
static void alter_place_at(TreeCtx *x, const TreeDeco *d, int px, int py, int pz) {
    FCtx *c = x->c;
    for (int dy = 2; dy >= -3; dy--) {
        int cy = py + dy;
        int st = bsprov_optional(c, d->prov, px, cy, pz);
        if (st >= 0) { ctx_set(x, px, cy, pz, st); break; }
        if (!ctx_air(x, px, cy, pz) && dy < 0) break;
    }
}
static void alter_circle(TreeCtx *x, const TreeDeco *d, int px, int py, int pz) {
    for (int xx = -2; xx <= 2; xx++) for (int zz = -2; zz <= 2; zz++)
        if (abs(xx) != 2 || abs(zz) != 2) alter_place_at(x, d, px + xx, py, pz + zz);
}
static void deco_alter_ground(TreeCtx *x, const TreeDeco *d) {
    BList l = {0}; lowest_trunk_or_root(x, &l);
    if (l.n > 0) {
        int min_y = l.a[0].y;
        for (int i = 0; i < l.n; i++) if (l.a[i].y == min_y) {
            int px = l.a[i].x, pz = l.a[i].z;
            alter_circle(x, d, px - 1, min_y, pz - 1);
            alter_circle(x, d, px + 2, min_y, pz - 1);
            alter_circle(x, d, px - 1, min_y, pz + 2);
            alter_circle(x, d, px + 2, min_y, pz + 2);
            for (int k = 0; k < 5; k++) {
                int pl = frnd_int_bound(x->r, 64), xx = pl % 8, zz = pl / 8;
                if (xx == 0 || xx == 7 || zz == 0 || zz == 7) alter_circle(x, d, px - 3 + xx, min_y, pz - 3 + zz);
            }
        }
    }
    free(l.a);
}
static void deco_attached_leaves(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r; FCtx *c = x->c;
    JSet black; jset_init(&black);
    BList sh = {0}; copy_list(&sh, &x->leaves); shuffle_list(&sh, r);
    for (int i = 0; i < sh.n; i++) {
        int dir = d->dirs[frnd_int_bound(r, d->ndirs)];
        int px = sh.a[i].x + DIR_DX[dir], py = sh.a[i].y + DIR_DY[dir], pz = sh.a[i].z + DIR_DZ[dir];
        if (jset_contains(&black, px, py, pz)) continue;
        if (!(frnd_float(r) < d->prob)) continue;
        int ok = 1;
        for (int k = 1; k <= d->required_empty; k++)
            if (!ctx_air(x, sh.a[i].x + DIR_DX[dir] * k, sh.a[i].y + DIR_DY[dir] * k, sh.a[i].z + DIR_DZ[dir] * k)) { ok = 0; break; }
        if (!ok) continue;
        for (int ax = px - d->excl_xz; ax <= px + d->excl_xz; ax++) for (int ay = py - d->excl_y; ay <= py + d->excl_y; ay++)
            for (int az = pz - d->excl_xz; az <= pz + d->excl_xz; az++) jset_add(&black, ax, ay, az);
        ctx_set(x, px, py, pz, bsprov_state(c, d->prov, px, py, pz));
    }
    free(sh.a); jset_free(&black);
}
static void deco_attached_logs(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r; FCtx *c = x->c;
    BList sh = {0}; copy_list(&sh, &x->logs); shuffle_list(&sh, r);
    for (int i = 0; i < sh.n; i++) {
        int dir = d->dirs[frnd_int_bound(r, d->ndirs)];
        int px = sh.a[i].x + DIR_DX[dir], py = sh.a[i].y + DIR_DY[dir], pz = sh.a[i].z + DIR_DZ[dir];
        if (frnd_float(r) <= d->prob && ctx_air(x, px, py, pz)) ctx_set(x, px, py, pz, bsprov_state(c, d->prov, px, py, pz));
    }
    free(sh.a);
}
static void deco_place_on_ground(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r; FCtx *c = x->c;
    BList l = {0}; lowest_trunk_or_root(x, &l);
    if (l.n > 0) {
        int min_y = l.a[0].y, minx = l.a[0].x, maxx = l.a[0].x, minz = l.a[0].z, maxz = l.a[0].z;
        for (int i = 0; i < l.n; i++) if (l.a[i].y == min_y) {
            if (l.a[i].x < minx) minx = l.a[i].x; if (l.a[i].x > maxx) maxx = l.a[i].x;
            if (l.a[i].z < minz) minz = l.a[i].z; if (l.a[i].z > maxz) maxz = l.a[i].z;
        }
        int bx0 = minx - d->radius, bx1 = maxx + d->radius, by0 = min_y - d->height, by1 = min_y + d->height, bz0 = minz - d->radius, bz1 = maxz + d->radius;
        int vine = bs_block_index(c->bs, "minecraft:vine");
        if (getenv("MCGEN_TRACE_TREE")) { int tx2, tz2; if (sscanf(getenv("MCGEN_TRACE_TREE"), "%d,%d", &tx2, &tz2) == 2 && l.a[0].x == tx2 && l.a[0].z == tz2) { fprintf(stderr, "POGINFO n=%d min_y=%d box x[%d,%d] y[%d,%d] z[%d,%d] radius=%d height=%d tries=%d\n", l.n, min_y, bx0, bx1, by0, by1, bz0, bz1, d->radius, d->height, d->tries); for (int q = 0; q < l.n && q < 4; q++) fprintf(stderr, "POGINFO  l[%d]=(%d,%d,%d)\n", q, l.a[q].x, l.a[q].y, l.a[q].z); } }
        int trace = 0; { static int tx, tz, tr = -1, all = 0; if (tr < 0) { const char *e = getenv("MCGEN_TRACE_TREE"); all = (e && !strcmp(e, "all")); tr = (e && (all || sscanf(e, "%d,%d", &tx, &tz) == 2)); }
                         trace = tr && (all || (l.a[0].x == tx && l.a[0].z == tz)); }
        static _Thread_local int dctr; if (trace) fprintf(stderr, "POGBEGIN chunk(%d,%d) tree(%d,%d,%d) deco#%d\n", c->ccx, c->ccz, l.a[0].x, l.a[0].y, l.a[0].z, dctr++);
        for (int i = 0; i < d->tries; i++) {
            int px = frnd_between(r, bx0, bx1);
            int py = frnd_between(r, by0, by1);
            int pz = frnd_between(r, bz0, bz1);
            int above = fc_get(c, px, py + 1, pz);
            int why = 0;
            if (!(fc_is_air(c, above) || blk_of(c, above) == vine)) why = 1;
            else if (!(c->bs->flags[fc_get(c, px, py, pz)] & BSF_SOLID_RENDER)) why = 2;
            else if (!(fc_height(c, HM_MOTION_BLOCKING_NO_LEAVES, px, pz) <= py + 1)) why = 3;
            if (trace) fprintf(stderr, "POG try %d (%d,%d,%d) %s why=%d hm=%d\n", i, px, py, pz, mcgen_block_state_name(c->g, fc_get(c, px, py, pz)), why, fc_height(c, HM_MOTION_BLOCKING_NO_LEAVES, px, pz));
            { static int hx, hy, hz, hr = -1; if (hr < 0) { const char *e = getenv("MCGEN_HACK_SKIP"); hr = (e && sscanf(e, "%d,%d,%d", &hx, &hy, &hz) == 3); }
              if (hr && px == hx && py + 1 == hy && pz == hz && !why) why = 9; }                  /* отладка: эксперимент «игра здесь не ставит» */
            if (why) continue;
            { int stt = bsprov_state(c, d->prov, px, py + 1, pz); if (trace) fprintf(stderr, "POGPLACE chunk(%d,%d) try %d (%d,%d,%d) %s\n", c->ccx, c->ccz, i, px, py + 1, pz, mcgen_block_state_name(c->g, stt)); ctx_set(x, px, py + 1, pz, stt); }
        }
    }
    free(l.a);
}
static void pale_hanger(TreeCtx *x, int px, int py, int pz) {
    FCtx *c = x->c; FRnd *r = x->r;
    int moss = block_state_named(c, "minecraft:pale_hanging_moss");
    if (moss < 0) return;
    int tip_f = bs_with(c->bs, moss, "tip", "false"), tip_t = bs_with(c->bs, moss, "tip", "true");
    while (ctx_air(x, px, py - 1, pz) && !(frnd_float(r) < 0.5f)) { ctx_set(x, px, py, pz, tip_f >= 0 ? tip_f : moss); py--; }
    ctx_set(x, px, py, pz, tip_t >= 0 ? tip_t : moss);
}
static void deco_pale_moss(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r; FCtx *c = x->c;
    BList sh = {0}; copy_list(&sh, &x->logs); shuffle_list(&sh, r);
    if (sh.n > 0) {
        int mi = 0;
        for (int i = 1; i < sh.n; i++) if (sh.a[i].y < sh.a[mi].y) mi = i;
        BPos org = sh.a[mi];
        if (frnd_float(r) < d->ground_prob) { if (d->moss_patch) feat_place(c, d->moss_patch, org.x, org.y + 1, org.z); }
        for (int i = 0; i < x->logs.n; i++)
            if (frnd_float(r) < d->trunk_prob) { int px = x->logs.a[i].x, py = x->logs.a[i].y - 1, pz = x->logs.a[i].z; if (ctx_air(x, px, py, pz)) pale_hanger(x, px, py, pz); }
        for (int i = 0; i < x->leaves.n; i++)
            if (frnd_float(r) < d->leaves_prob) { int px = x->leaves.a[i].x, py = x->leaves.a[i].y - 1, pz = x->leaves.a[i].z; if (ctx_air(x, px, py, pz)) pale_hanger(x, px, py, pz); }
    }
    free(sh.a);
}
static void deco_creaking_heart(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r; FCtx *c = x->c;
    if (x->logs.n == 0) return;
    if (!(frnd_float(r) < d->prob)) return;
    BList sh = {0}; copy_list(&sh, &x->logs); shuffle_list(&sh, r);
    for (int i = 0; i < sh.n; i++) {
        int all = 1;
        for (int dd = 0; dd < 6 && all; dd++) all = x->tags->logs[blk_of(c, fc_get(c, sh.a[i].x + DIR_DX[dd], sh.a[i].y + DIR_DY[dd], sh.a[i].z + DIR_DZ[dd]))] != 0;
        if (all) {
            int st = block_state_named(c, "minecraft:creaking_heart");
            if (st >= 0) {
                int s1 = bs_with(c->bs, st, "creaking_heart_state", "dormant"); if (s1 >= 0) st = s1;
                int s2 = bs_with(c->bs, st, "natural", "true"); if (s2 >= 0) st = s2;
                ctx_set(x, sh.a[i].x, sh.a[i].y, sh.a[i].z, st);
            }
            break;
        }
    }
    free(sh.a);
}
/* ---- shelf_mushroom (26.3+) ---- */
static int shelf_replaceable(const TreeCtx *x, int px, int py, int pz) {
    FCtx *c = x->c;
    if (!(c->bs->flags[fc_get(c, px, py, pz)] & BSF_REPLACEABLE)) return 0;
    int w = bs_block_index(c->bs, "minecraft:water");
    static const int OX[5] = { 0, 1, -1, 0, 0 }, OZ[5] = { 0, 0, 0, -1, 1 };
    for (int k = 0; k < 5; k++) if (blk_of(c, fc_get(c, px + OX[k], py, pz + OZ[k])) == w) return 0;
    return 1;
}
static int shelf_at(const TreeCtx *x, int px, int py, int pz) {
    int b = bs_block_index(x->c->bs, "minecraft:shelf_mushroom");
    return blk_of(x->c, fc_get(x->c, px, py, pz)) == b;
}
static int shelf_adjacent(const TreeCtx *x, int px, int py, int pz) {
    for (int k = 0; k < 4; k++) if (shelf_at(x, px + HZ_DX[k], py, pz + HZ_DZ[k])) return 1;
    return 0;
}
static void shelf_place(TreeCtx *x, int px, int py, int pz, int facing) {
    static const char *FN[6] = { NULL, NULL, "north", "south", "west", "east" };
    FCtx *c = x->c;
    int age = frnd_int_bound(x->r, 2);
    int st = block_state_named(c, "minecraft:shelf_mushroom");
    if (st >= 0) { int s1 = bs_with(c->bs, st, "age", age ? "1" : "0"); if (s1 >= 0) st = s1; int s2 = bs_with(c->bs, st, "facing", FN[facing]); if (s2 >= 0) st = s2; }
    ctx_set(x, px, py, pz, st);
}
static void deco_shelf(TreeCtx *x, const TreeDeco *d) {
    FRnd *r = x->r;
    if (!(frnd_float(r) < d->prob)) return;
    if (x->logs.n == 0) return;
    if (x->logs.a[0].y == x->logs.a[x->logs.n - 1].y) {          /* упавший ствол */
        int dirs[2];
        if (x->logs.a[0].x != x->logs.a[x->logs.n - 1].x) { dirs[0] = DIR_NORTH; dirs[1] = DIR_SOUTH; } else { dirs[0] = DIR_EAST; dirs[1] = DIR_WEST; }
        for (int i = 0; i < x->logs.n; i++) for (int k = 0; k < 2; k++) {
            if (frnd_float(r) > 0.25f) continue;
            int px = x->logs.a[i].x + DIR_DX[dirs[k]], py = x->logs.a[i].y, pz = x->logs.a[i].z + DIR_DZ[dirs[k]];
            if (shelf_replaceable(x, px, py, pz) && !shelf_adjacent(x, px, py, pz) && !shelf_adjacent(x, x->logs.a[i].x, x->logs.a[i].y, x->logs.a[i].z))
                shelf_place(x, px, py, pz, dirs[k]);
        }
    } else {
        int first = HZ_DIR[frnd_int_bound(r, 4)];
        int k0 = 0; for (int k = 0; k < 4; k++) if (HZ_DIR[k] == first) k0 = k;
        int dirs[2] = { first, HZ_DIR[(k0 + 1) & 3] };               /* getClockWise */
        int base_y = x->logs.a[0].y;
        for (int i = 0; i < x->logs.n; i++) {
            int dy = x->logs.a[i].y - base_y;
            if (!(dy >= 1 && dy <= 4)) continue;
            for (int k = 0; k < 2; k++) {
                if (frnd_float(r) > 0.25f) continue;
                int px = x->logs.a[i].x + DIR_DX[dirs[k]], py = x->logs.a[i].y, pz = x->logs.a[i].z + DIR_DZ[dirs[k]];
                if (!shelf_replaceable(x, px, py, pz)) continue;
                if (shelf_at(x, px, py - 1, pz)) continue;
                shelf_place(x, px, py, pz, dirs[k]);
                break;
            }
        }
    }
}

void deco_run(TreeCtx *x, const TreeDeco *d) {
    switch (d->type) {
    case TD_BEEHIVE: deco_beehive(x, d); break;
    case TD_COCOA: deco_cocoa(x, d); break;
    case TD_LEAVE_VINE: deco_leave_vine(x, d); break;
    case TD_TRUNK_VINE: deco_trunk_vine(x, d); break;
    case TD_ALTER_GROUND: deco_alter_ground(x, d); break;
    case TD_ATTACHED_LEAVES: deco_attached_leaves(x, d); break;
    case TD_ATTACHED_LOGS: deco_attached_logs(x, d); break;
    case TD_PLACE_ON_GROUND: deco_place_on_ground(x, d); break;
    case TD_PALE_MOSS: deco_pale_moss(x, d); break;
    case TD_CREAKING_HEART: deco_creaking_heart(x, d); break;
    case TD_SHELF_MUSHROOM: deco_shelf(x, d); break;
    }
}

/* ====================================================================== StructureTemplate.updateShapeAtEdge: updateShape блоков на границе формы */
static int prop_true(const BsTab *bs, int st, const char *name) {
    const char *v = NULL; return bs_get_prop(bs, st, name, &v) && v && v[0] == 't';
}
/* MultifaceBlock.canAttachTo: у соседа грань напротив направления полная (опорная форма или форма столкновения) */
static int can_attach_to(const FCtx *c, int nst, int dir_to_neighbour) {
    return ((c->bs->sturdy[nst] >> opp(dir_to_neighbour)) & 1) || (c->bs->flags[nst] & BSF_FULL_COLL);
}
static int is_vine(const FCtx *c, int st, int vine_blk) { return blk_of(c, st) == vine_blk; }
static int vine_updated(FCtx *c, int vine_blk, int st, int x, int y, int z) {
    const BsTab *bs = c->bs;
    static const char *N[4] = { "north", "east", "south", "west" };
    if (prop_true(bs, st, "up")) { int s2 = bs_with(bs, st, "up", can_attach_to(c, fc_get(c, x, y + 1, z), DIR_DOWN) ? "true" : "false"); if (s2 >= 0) st = s2; }
    int above = -1;
    for (int k = 0; k < 4; k++) {
        if (!prop_true(bs, st, N[k])) continue;
        int dir = HZ_DIR[k];
        int can = can_attach_to(c, fc_get(c, x + DIR_DX[dir], y, z + DIR_DZ[dir]), dir);
        if (!can) { if (above < 0) above = fc_get(c, x, y + 1, z); can = is_vine(c, above, vine_blk) && prop_true(bs, above, N[k]); }
        else can = 1;
        if (!can) { if (above < 0) above = fc_get(c, x, y + 1, z); can = is_vine(c, above, vine_blk) && prop_true(bs, above, N[k]); }
        int s2 = bs_with(bs, st, N[k], can ? "true" : "false"); if (s2 >= 0) st = s2;
    }
    return st;
}
static int has_faces(const BsTab *bs, int st) {
    return prop_true(bs, st, "north") || prop_true(bs, st, "east") || prop_true(bs, st, "south") || prop_true(bs, st, "west") || prop_true(bs, st, "up");
}
static int facing_dir(const BsTab *bs, int st) {
    const char *v = NULL;
    if (!bs_get_prop(bs, st, "facing", &v) || !v) return -1;
    return !strcmp(v, "north") ? DIR_NORTH : !strcmp(v, "south") ? DIR_SOUTH : !strcmp(v, "west") ? DIR_WEST : !strcmp(v, "east") ? DIR_EAST : -1;
}
typedef struct UpdCls { int vine, cocoa, shelf; const u8 *supports_cocoa; } UpdCls;
static int upd_shape(FCtx *c, const UpdCls *u, int st, int x, int y, int z, int d) {
    int b = blk_of(c, st);
    if (b == u->vine) {
        if (d == DIR_DOWN) return st;
        int ns = vine_updated(c, u->vine, st, x, y, z);
        return has_faces(c->bs, ns) ? ns : c->st_air;
    }
    if (b == u->cocoa) {
        int f = facing_dir(c->bs, st);
        if (d == f && !u->supports_cocoa[blk_of(c, fc_get(c, x + DIR_DX[f], y, z + DIR_DZ[f]))]) return c->st_air;
        return st;
    }
    if (b == u->shelf) {
        int f = facing_dir(c->bs, st);
        if (f >= 0 && d == opp(f)) {
            int o = opp(f);
            int sup = fc_get(c, x + DIR_DX[o], y, z + DIR_DZ[o]);
            if (!((c->bs->sturdy[sup] >> f) & 1)) return c->st_air;
        }
        return st;
    }
    return st;
}
int structure_update_shape_fc(FCtx *fc, int st, int x, int y, int z, int dir, int nst);      /* structure_post.c */
static const UpdCls *upd_classes(FCtx *c) {
    static _Thread_local struct { const McGen *g; UpdCls u; } cache;
    if (cache.g != c->g) {
        cache.g = c->g;
        cache.u.vine = bs_block_index(c->bs, "minecraft:vine"); cache.u.cocoa = bs_block_index(c->bs, "minecraft:cocoa");
        cache.u.shelf = bs_block_index(c->bs, "minecraft:shelf_mushroom");
        cache.u.supports_cocoa = gen_block_tag(c->g, "minecraft:supports_cocoa");
    }
    return &cache.u;
}
/* BlockState.updateShape для лозы, какао и грибов-полок (пост-обработка помеченных позиций, structure_post.c): новое состояние либо st */
int tree_upd_shape(FCtx *c, int st, int x, int y, int z, int d) { return upd_shape(c, upd_classes(c), st, x, y, z, d); }
void tree_update_shape_at_edge(FCtx *c, int minx, int miny, int minz, int sx, int sy, int sz, const u8 *shape) {
    const UpdCls *u = upd_classes(c);
    { static int tx, ty, tz, tr = -1;      /* отладка: MCGEN_TRACE_POS="x,y,z" — деревья, у которых форма (±1) охватывает клетку */
      if (tr < 0) { const char *e = getenv("MCGEN_TRACE_POS"); tr = (e && sscanf(e, "%d,%d,%d", &tx, &ty, &tz) == 3) ? 1 : 0; }
      if (tr && tx >= minx - 1 && tx <= minx + sx && ty >= miny - 1 && ty <= miny + sy && tz >= minz - 1 && tz <= minz + sz)
          fprintf(stderr, "TREEEDGE chunk(%d,%d) shape min(%d,%d,%d) size(%d,%d,%d)\n", c->ccx, c->ccz, minx, miny, minz, sx, sy, sz); }
    #define FULL(x_, y_, z_) (shape[(((size_t)(x_) * sy) + (size_t)(y_)) * sz + (size_t)(z_)])
    #define CONSUME(dir_, x_, y_, z_) do { \
        int px = minx + (x_), py = miny + (y_), pz = minz + (z_); \
        int nx = px + DIR_DX[dir_], ny = py + DIR_DY[dir_], nz = pz + DIR_DZ[dir_]; \
        int st = fc_get(c, px, py, pz), nst = fc_get(c, nx, ny, nz); \
        int ns = upd_shape(c, u, st, px, py, pz, dir_); \
        if (ns == st) { int r_ = structure_update_shape_fc(c, st, px, py, pz, dir_, nst); if (r_ >= 0) ns = r_; } \
        if (ns != st) { fc_set(c, px, py, pz, ns, 2); st = ns; } \
        int nn = upd_shape(c, u, nst, nx, ny, nz, opp(dir_)); \
        if (nn == nst) { int r_ = structure_update_shape_fc(c, nst, nx, ny, nz, opp(dir_), st); if (r_ >= 0) nn = r_; } \
        if (nn != nst) fc_set(c, nx, ny, nz, nn, 2); \
    } while (0)
    /* быстрый выход: в мире нет блоков интересных классов рядом — upd_shape ничего не меняет; обход граней оставлен точным */
    for (int a = 0; a < sx; a++) for (int b = 0; b < sy; b++) {          /* NONE: вдоль Z */
        int last = 0;
        for (int cc = 0; cc <= sz; cc++) {
            int full = cc != sz && FULL(a, b, cc);
            if (!last && full) CONSUME(DIR_NORTH, a, b, cc);
            if (last && !full) CONSUME(DIR_SOUTH, a, b, cc - 1);
            last = full;
        }
    }
    for (int a = 0; a < sz; a++) for (int b = 0; b < sx; b++) {          /* FORWARD: вдоль Y; X = b, Y = c, Z = a */
        int last = 0;
        for (int cc = 0; cc <= sy; cc++) {
            int full = cc != sy && FULL(b, cc, a);
            if (!last && full) CONSUME(DIR_DOWN, b, cc, a);
            if (last && !full) CONSUME(DIR_UP, b, cc - 1, a);
            last = full;
        }
    }
    for (int a = 0; a < sy; a++) for (int b = 0; b < sz; b++) {          /* BACKWARD: вдоль X; X = c, Y = a, Z = b */
        int last = 0;
        for (int cc = 0; cc <= sx; cc++) {
            int full = cc != sx && FULL(cc, a, b);
            if (!last && full) CONSUME(DIR_WEST, cc, a, b);
            if (last && !full) CONSUME(DIR_EAST, cc - 1, a, b);
            last = full;
        }
    }
    #undef FULL
    #undef CONSUME
}

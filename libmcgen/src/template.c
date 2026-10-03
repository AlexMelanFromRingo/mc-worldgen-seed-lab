/* template.c — шаблоны построек: загрузка NBT, палитры, повороты/отражения состояний, размещение (см. template.h). */
#include "template.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- хранилище */
typedef struct TStore { McMutex *lock; StrMap map; u16 *cache[3 * 4]; int n_cache_states; u8 *rc, *lbc; } TStore;
void *templates_store_new(void) { TStore *s = xcalloc(1, sizeof *s); s->lock = mutex_new(); return s; }

static void tpl_free(void *v) {
    Template *t = v; if (!t) return;
    for (int p = 0; p < t->npal; p++) {
        free(t->pal[p].b);
        for (int j = 0; j < t->pal[p].nj; j++) { free((void *)t->pal[p].j[j].final_state); }
        free(t->pal[p].j);
    }
    free(t->pal); nbt_free(t->doc); free(t->id); free(t);
}
void templates_world_free(void *store) {
    TStore *s = store; if (!s) return;
    sm_free(&s->map, tpl_free);
    for (int i = 0; i < 12; i++) free(s->cache[i]);
    free(s->rc); free(s->lbc);
    mutex_free(s->lock); free(s);
}

static TStore *store_of(McWorld *w) { StructWorld *sw = structures_world_get(w); return sw ? (TStore *)sw->templates : NULL; }

/* ---------------------------------------------------------------- состояния: поворот и отражение */
static int dir_of_name(const char *s) {
    switch (s[0]) {
    case 'n': return DIR_NORTH; case 'e': return DIR_EAST; case 's': return DIR_SOUTH; case 'w': return DIR_WEST;
    case 'u': return DIR_UP; case 'd': return DIR_DOWN; default: return -1;
    }
}
static const char *DNAME[6] = { "down", "up", "north", "south", "west", "east" };
static int get_s(const BsTab *t, int st, const char *p, const char **v) { return bs_get_prop(t, st, p, v); }
static int with_s(const BsTab *t, int st, const char *p, const char *v) { int r = bs_with(t, st, p, v); return r < 0 ? st : r; }

/* направление (по часовой на rot поворотов) */
static int mir_dir(int d, int mir) {          /* Mirror.mirror(Direction) */
    if (mir == MIR_FRONT_BACK && (d == DIR_EAST || d == DIR_WEST)) return dir_opp(d);
    if (mir == MIR_LEFT_RIGHT && (d == DIR_NORTH || d == DIR_SOUTH)) return dir_opp(d);
    return d;
}
static int mir_rot(int d, int mir) {          /* Mirror.getRotation(Direction): 0 или 2 (CW180) */
    return mir_dir(d, mir) != d ? ROT_CW180 : ROT_NONE;
}

/* форма рельсов/лестниц: геометрически */
static const char *RAIL_NAMES[10] = { "north_south", "east_west", "ascending_east", "ascending_west", "ascending_north", "ascending_south",
                                      "south_east", "south_west", "north_west", "north_east" };
static int rail_map(const char *shape, int rot, int mir) {
    /* возвращает индекс RAIL_NAMES или −1 */
    int a = -1, b = -1, asc = 0;
    if (!strncmp(shape, "ascending_", 10)) { asc = 1; a = dir_of_name(shape + 10); }
    else { char tmp[16]; snprintf(tmp, sizeof tmp, "%s", shape); char *u = strchr(tmp, '_'); if (!u) return -1; *u = 0; a = dir_of_name(tmp); b = dir_of_name(u + 1); }
    a = mir_dir(a, mir); a = dir_rotate(a, rot);
    if (asc) { for (int i = 2; i < 6; i++) { char buf[24]; snprintf(buf, sizeof buf, "ascending_%s", DNAME[a]); if (!strcmp(buf, RAIL_NAMES[i])) return i; } return -1; }
    b = mir_dir(b, mir); b = dir_rotate(b, rot);
    int ns = (a == DIR_NORTH || a == DIR_SOUTH), ew = (a == DIR_EAST || a == DIR_WEST);
    int ns2 = (b == DIR_NORTH || b == DIR_SOUTH), ew2 = (b == DIR_EAST || b == DIR_WEST);
    if (ns && ns2) return 0;
    if (ew && ew2) return 1;
    int sn = ns ? a : b, e = ns ? b : a;     /* sn — north/south, e — east/west */
    if (sn == DIR_SOUTH) return e == DIR_EAST ? 6 : 7;
    return e == DIR_WEST ? 8 : 9;
}

static const char *stairs_mirror_shape(const char *shape, int mir, int *flip) {
    /* StairBlock.mirror: flip=1 — повернуть на 180 */
    *flip = 0;
    return shape;
    (void)mir;
}

static int rotate_raw(const BsTab *t, int st, int rot) {
    if (rot == ROT_NONE) return st;
    const char *v;
    if (get_s(t, st, "facing", &v)) { int d = dir_of_name(v); if (d >= 0) { int nd = dir_rotate(d, rot); if (nd != d) st = with_s(t, st, "facing", DNAME[nd]); } }
    if ((rot & 1) && get_s(t, st, "axis", &v) && v[0] != 'y') st = with_s(t, st, "axis", v[0] == 'x' ? "z" : "x");
    if (get_s(t, st, "rotation", &v) && (v[0] >= '0' && v[0] <= '9')) { char b[8]; snprintf(b, sizeof b, "%d", (atoi(v) + rot * 4) % 16); st = with_s(t, st, "rotation", b); }
    if (get_s(t, st, "north", &v)) {
        const char *vn, *ve, *vs, *vw;
        if (get_s(t, st, "east", &ve) && get_s(t, st, "south", &vs) && get_s(t, st, "west", &vw) && !bs_is_a(t, st, "PipeBlock")) {
            char old[4][24]; vn = v;
            snprintf(old[0], 24, "%s", vn); snprintf(old[1], 24, "%s", ve); snprintf(old[2], 24, "%s", vs); snprintf(old[3], 24, "%s", vw);
            static const int sd[4] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST }; static const char *sn[4] = { "north", "east", "south", "west" };
            char nw[4][24];
            for (int i = 0; i < 4; i++) { int nd = dir_rotate(sd[i], rot); for (int k = 0; k < 4; k++) if (sd[k] == nd) snprintf(nw[k], 24, "%s", old[i]); }
            for (int k = 0; k < 4; k++) st = with_s(t, st, sn[k], nw[k]);
        }
    }
    if (get_s(t, st, "shape", &v)) {
        if (strstr(v, "north_south") || strstr(v, "east_west") || !strncmp(v, "ascending_", 10) || !strcmp(v, "south_east") || !strcmp(v, "south_west") || !strcmp(v, "north_west") || !strcmp(v, "north_east")) {
            int r = rail_map(v, rot, MIR_NONE); if (r >= 0) st = with_s(t, st, "shape", RAIL_NAMES[r]);
        }
    }
    if (get_s(t, st, "orientation", &v) && strchr(v, '_')) {
        char tmp[24]; snprintf(tmp, sizeof tmp, "%s", v); char *u = strchr(tmp, '_'); *u = 0;
        int f = dir_of_name(tmp), tp = dir_of_name(u + 1);
        if (f >= 0 && tp >= 0) { char nb[32]; snprintf(nb, sizeof nb, "%s_%s", DNAME[dir_rotate(f, rot)], DNAME[dir_rotate(tp, rot)]); st = with_s(t, st, "orientation", nb); }
    }
    return st;
}

static int mirror_raw(const BsTab *t, int st, int mir) {
    if (mir == MIR_NONE) return st;
    const char *v;
    int is_stair = get_s(t, st, "shape", &v) && (!strncmp(v, "inner_", 6) || !strncmp(v, "outer_", 6) || !strcmp(v, "straight")) && bs_is_a(t, st, "StairBlock");
    if (is_stair) {
        const char *fv; if (!get_s(t, st, "facing", &fv)) return st;
        int d = dir_of_name(fv); int axis_z = (d == DIR_NORTH || d == DIR_SOUTH);
        char shape[24]; snprintf(shape, sizeof shape, "%s", v);
        int hit = 0; const char *ns = shape;
        if (mir == MIR_LEFT_RIGHT && axis_z) {
            hit = 1;
            if (!strcmp(shape, "outer_left")) ns = "outer_right"; else if (!strcmp(shape, "inner_right")) ns = "inner_left";
            else if (!strcmp(shape, "inner_left")) ns = "inner_right"; else if (!strcmp(shape, "outer_right")) ns = "outer_left";
        } else if (mir == MIR_FRONT_BACK && !axis_z) {
            hit = 1;
            if (!strcmp(shape, "outer_left")) ns = "outer_right"; else if (!strcmp(shape, "outer_right")) ns = "outer_left";
            /* straight, inner_right, inner_left — форма не меняется */
        }
        if (!hit) return st;
        st = with_s(t, st, "facing", DNAME[dir_opp(d)]);
        return with_s(t, st, "shape", ns);
    }
    if (get_s(t, st, "facing", &v)) {
        int d = dir_of_name(v);
        if (d >= 0 && mir_rot(d, mir)) st = with_s(t, st, "facing", DNAME[dir_opp(d)]);
        if (get_s(t, st, "hinge", &v) && bs_is_a(t, st, "DoorBlock")) st = with_s(t, st, "hinge", !strcmp(v, "left") ? "right" : "left");
    }
    if (get_s(t, st, "rotation", &v) && (v[0] >= '0' && v[0] <= '9')) {
        int r = atoi(v), half = 8, c = r > half ? r - 16 : r;
        int nr = mir == MIR_LEFT_RIGHT ? (half - c + 16) % 16 : (16 - c) % 16;
        char b[8]; snprintf(b, sizeof b, "%d", nr); st = with_s(t, st, "rotation", b);
    }
    if (get_s(t, st, "north", &v)) {
        const char *ve, *vs, *vw;
        if (get_s(t, st, "east", &ve) && get_s(t, st, "south", &vs) && get_s(t, st, "west", &vw) && !bs_is_a(t, st, "PipeBlock")) {
            char a[2][24], b[2][24];
            if (mir == MIR_LEFT_RIGHT) { snprintf(a[0], 24, "%s", v); snprintf(a[1], 24, "%s", vs); st = with_s(t, st, "north", a[1]); st = with_s(t, st, "south", a[0]); }
            else { snprintf(b[0], 24, "%s", ve); snprintf(b[1], 24, "%s", vw); st = with_s(t, st, "east", b[1]); st = with_s(t, st, "west", b[0]); }
        }
    }
    if (get_s(t, st, "shape", &v) && (strstr(v, "north_south") || strstr(v, "east_west") || !strncmp(v, "ascending_", 10) || !strcmp(v, "south_east") || !strcmp(v, "south_west") || !strcmp(v, "north_west") || !strcmp(v, "north_east"))) {
        int r = rail_map(v, ROT_NONE, mir); if (r >= 0) st = with_s(t, st, "shape", RAIL_NAMES[r]);
    }
    if (get_s(t, st, "orientation", &v) && strchr(v, '_')) {
        char tmp[24]; snprintf(tmp, sizeof tmp, "%s", v); char *u = strchr(tmp, '_'); *u = 0;
        int f = dir_of_name(tmp), tp = dir_of_name(u + 1);
        if (f >= 0 && tp >= 0) { char nb[32]; snprintf(nb, sizeof nb, "%s_%s", DNAME[mir_dir(f, mir)], DNAME[mir_dir(tp, mir)]); st = with_s(t, st, "orientation", nb); }
    }
    return st;
}

/* кэш (mir*4 + rot): u16 на состояние, 0xFFFF — не вычислено; гонки безвредны (значение детерминировано) */
static u16 *tcache(const BsTab *bs, int k) {
    TStore *s = NULL; (void)s;
    static _Thread_local const BsTab *last_bs; static _Thread_local u16 *last[12];
    (void)last_bs; (void)last; (void)bs; (void)k;
    return NULL;
}
static u16 *state_cache(const BsTab *bs, int kind /* 0 rot, 1 mir */, int amount) {
    /* таблицы хранятся в McGen через BsTab: используем таблицу в хранилище первого мира… проще: статический реестр по BsTab */
    static McMutex *lk; static const BsTab *keys[8]; static u16 *tabs[8][12]; static int nk;
    if (!lk) { lk = mutex_new(); }
    mutex_lock(lk);
    int ki = -1; for (int i = 0; i < nk; i++) if (keys[i] == bs) { ki = i; break; }
    if (ki < 0 && nk < 8) { ki = nk++; keys[ki] = bs; }
    u16 *tab = NULL;
    if (ki >= 0) {
        int slot = kind * 4 + amount;
        if (!tabs[ki][slot]) { tabs[ki][slot] = xmalloc(sizeof(u16) * (size_t)bs->nstates); memset(tabs[ki][slot], 0xFF, sizeof(u16) * (size_t)bs->nstates); }
        tab = tabs[ki][slot];
    }
    mutex_unlock(lk);
    (void)tcache; (void)stairs_mirror_shape;
    return tab;
}
int bsx_rotate(const BsTab *bs, int st, int rot) {
    if (rot == ROT_NONE || st < 0) return st;
    u16 *tab = state_cache(bs, 0, rot);
    if (!tab) return rotate_raw(bs, st, rot);
    if (tab[st] == 0xFFFF) tab[st] = (u16)rotate_raw(bs, st, rot);
    return tab[st];
}
int bsx_mirror(const BsTab *bs, int st, int mir) {
    if (mir == MIR_NONE || st < 0) return st;
    u16 *tab = state_cache(bs, 1, mir);
    if (!tab) return mirror_raw(bs, st, mir);
    if (tab[st] == 0xFFFF) tab[st] = (u16)mirror_raw(bs, st, mir);
    return tab[st];
}

/* ---------------------------------------------------------------- геометрия */
void tpl_transform(int x, int z, int mir, int rot, int px, int pz, int *ox, int *oz) {
    if (mir == MIR_LEFT_RIGHT) z = -z; else if (mir == MIR_FRONT_BACK) x = -x;
    switch (rot) {
    case ROT_CCW90: *ox = px - pz + z; *oz = px + pz - x; break;
    case ROT_CW90: *ox = px + pz - z; *oz = pz - px + x; break;
    case ROT_CW180: *ox = px + px - x; *oz = pz + pz - z; break;
    default: *ox = x; *oz = z; break;
    }
}
BB tpl_bounding_box(const Template *t, int x, int y, int z, int rot, int mir, int px, int pz) {
    int ax, az, bx, bz;
    tpl_transform(0, 0, mir, rot, px, pz, &ax, &az);
    tpl_transform(t->sx - 1, t->sz - 1, mir, rot, px, pz, &bx, &bz);
    BB b = bb_from_corners(ax, 0, az, bx, t->sy - 1, bz);
    return bb_moved(b, x, y, z);
}
void tpl_size(const Template *t, int rot, int *sx, int *sy, int *sz) {
    if (rot == ROT_CW90 || rot == ROT_CCW90) { *sx = t->sz; *sz = t->sx; } else { *sx = t->sx; *sz = t->sz; }
    *sy = t->sy;
}
const TPal *tpl_palette_at(const Template *t, int x, int y, int z) {
    if (t->npal == 0) return NULL;
    if (t->npal == 1) return &t->pal[0];
    RS r; rs_seed_lcg(&r, mth_get_seed(x, y, z));
    return &t->pal[rs_bound(&r, t->npal)];
}

/* ---------------------------------------------------------------- загрузка */
static int pal_state(McWorld *w, const BsTab *bs, const Nbt *e) {
    const char *name = nbt_str(nbt_get(e, "Name"), NULL);       /* 26.1/26.2: Name/Properties; 26.3+: id/properties */
    if (!name) name = nbt_str(nbt_get(e, "id"), NULL);
    if (!name) return w->g->st_air;
    int blk = bs_block_index(bs, name);
    if (blk < 0) return w->g->st_air;
    int st = bs->blk[blk].def;
    const Nbt *pr = nbt_get(e, "Properties"); if (!pr) pr = nbt_get(e, "properties");
    if (pr && pr->type == NBT_COMPOUND) {
        for (int i = 0; i < pr->n; i++) {
            const Nbt *kv = &pr->v.kids[i];
            if (kv->type != NBT_STRING) continue;
            int ns = bs_with(bs, st, kv->name, kv->v.s);
            if (ns >= 0) st = ns;
        }
    }
    return st;
}

static int cmp_yxz(const void *a, const void *b) {
    const TInfo *p = a, *q = b;
    if (p->y != q->y) return p->y < q->y ? -1 : 1;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    if (p->z != q->z) return p->z < q->z ? -1 : 1;
    return 0;
}

static void build_palette(McWorld *w, const BsTab *bs, const Nbt *palette_list, const Nbt *blocks, TPal *out, int jig_blk) {
    int np = palette_list ? palette_list->n : 0;
    int *ps = xmalloc((size_t)(np ? np : 1) * sizeof(int));
    for (int i = 0; i < np; i++) ps[i] = pal_state(w, bs, nbt_at(palette_list, i));
    int nb = blocks ? blocks->n : 0;
    TInfo *full = xmalloc((size_t)(nb ? nb : 1) * sizeof(TInfo)), *other = xmalloc((size_t)(nb ? nb : 1) * sizeof(TInfo)), *ents = xmalloc((size_t)(nb ? nb : 1) * sizeof(TInfo));
    int nf = 0, no = 0, ne = 0;
    for (int i = 0; i < nb; i++) {
        const Nbt *b = nbt_at(blocks, i);
        const Nbt *pos = nbt_get(b, "pos");
        TInfo ti;
        ti.x = pos ? (int)nbt_int(nbt_at(pos, 0), 0) : 0; ti.y = pos ? (int)nbt_int(nbt_at(pos, 1), 0) : 0; ti.z = pos ? (int)nbt_int(nbt_at(pos, 2), 0) : 0;
        int si = (int)nbt_int(nbt_get(b, "state"), 0);
        ti.state = (si >= 0 && si < np) ? ps[si] : w->g->st_air;    /* SimplePalette.stateFor: нет записи → воздух */
        const Nbt *nb_ = nbt_get(b, "nbt");
        ti.nbt = (nb_ && nb_->type == NBT_COMPOUND) ? nb_ : NULL;
        if (ti.nbt) ents[ne++] = ti;
        else if (bs->flags[ti.state] & BSF_FULL_COLL) full[nf++] = ti;
        else other[no++] = ti;
    }
    qsort(full, (size_t)nf, sizeof(TInfo), cmp_yxz); qsort(other, (size_t)no, sizeof(TInfo), cmp_yxz); qsort(ents, (size_t)ne, sizeof(TInfo), cmp_yxz);
    out->nb = nf + no + ne; out->b = xmalloc((size_t)(out->nb ? out->nb : 1) * sizeof(TInfo));
    memcpy(out->b, full, (size_t)nf * sizeof(TInfo)); memcpy(out->b + nf, other, (size_t)no * sizeof(TInfo)); memcpy(out->b + nf + no, ents, (size_t)ne * sizeof(TInfo));
    free(full); free(other); free(ents); free(ps);
    /* jigsaw-блоки в порядке палитры */
    int nj = 0; for (int i = 0; i < out->nb; i++) if (w->g->state_block[out->b[i].state] == jig_blk) nj++;
    out->nj = nj; out->j = xcalloc((size_t)(nj ? nj : 1), sizeof(TJig));
    int k = 0;
    for (int i = 0; i < out->nb; i++) {
        const TInfo *b = &out->b[i];
        if (w->g->state_block[b->state] != jig_blk) continue;
        TJig *j = &out->j[k++];
        j->x = b->x; j->y = b->y; j->z = b->z; j->state = b->state;
        const char *orient = NULL; bs_get_prop(bs, b->state, "orientation", &orient);
        const Nbt *nb_ = b->nbt;
        const char *joint = nbt_str(nbt_get(nb_, "joint"), NULL);
        if (joint) j->rollable = !strcmp(joint, "rollable");
        else j->rollable = orient && (strncmp(orient, "up_", 3) == 0 || strncmp(orient, "down_", 5) == 0);   /* getDefaultJointType: фронт не горизонтален → rollable */
        j->name = nbt_str(nbt_get(nb_, "name"), "minecraft:empty");
        j->pool = nbt_str(nbt_get(nb_, "pool"), "minecraft:empty");
        j->target = nbt_str(nbt_get(nb_, "target"), "minecraft:empty");
        j->final_state = xstrdup(nbt_str(nbt_get(nb_, "final_state"), "minecraft:air"));
        j->place_prio = (int)nbt_int(nbt_get(nb_, "placement_priority"), 0);
        j->sel_prio = (int)nbt_int(nbt_get(nb_, "selection_priority"), 0);
    }
}

const Template *template_get(McWorld *w, const char *id) {
    TStore *s = store_of(w); if (!s) return NULL;
    char full[256]; if (!strchr(id, ':')) snprintf(full, sizeof full, "minecraft:%s", id); else snprintf(full, sizeof full, "%s", id);
    mutex_lock(s->lock);
    Template *t = sm_get(&s->map, full);
    if (!t && !sm_has(&s->map, full)) {
        const char *c = strchr(full, ':');
        char path[1024]; snprintf(path, sizeof path, "%s/data/%.*s/structure/%s.nbt", w->g->pack, (int)(c - full), full, c + 1);
        char err[128];
        NbtDoc *d = nbt_read_file(path, err, sizeof err);
        if (d) {
            const BsTab *bs = bs_get(w->g);
            const Nbt *root = nbt_root(d);
            t = xcalloc(1, sizeof *t); t->id = xstrdup(full); t->doc = d;
            const Nbt *sz = nbt_get(root, "size");
            t->sx = sz ? (int)nbt_int(nbt_at(sz, 0), 0) : 0; t->sy = sz ? (int)nbt_int(nbt_at(sz, 1), 0) : 0; t->sz = sz ? (int)nbt_int(nbt_at(sz, 2), 0) : 0;
            const Nbt *blocks = nbt_get(root, "blocks");
            const Nbt *pl = nbt_get(root, "palettes");
            int jig = bs_block_index(bs, "minecraft:jigsaw");
            if (pl && pl->type == NBT_LIST) {
                t->npal = pl->n; t->pal = xcalloc((size_t)(t->npal ? t->npal : 1), sizeof(TPal));
                for (int p = 0; p < t->npal; p++) build_palette(w, bs, nbt_at(pl, p), blocks, &t->pal[p], jig);
            } else {
                t->npal = 1; t->pal = xcalloc(1, sizeof(TPal));
                build_palette(w, bs, nbt_get(root, "palette"), blocks, &t->pal[0], jig);
            }
        }
        sm_put(&s->map, full, t);
    }
    mutex_unlock(s->lock);
    return t;
}

/* ---------------------------------------------------------------- размещение */
int template_process(FCtx *fc, McWorld *w, const Template *t, int x, int y, int z, int rx, int ry, int rz,
                     const TSettings *s, i64 level_seed, TInfo **out, TInfo **orig_out) {
    const BsTab *bs = bs_get(w->g);
    const TPal *pal = tpl_palette_at(t, x, y, z);
    *out = NULL; if (orig_out) *orig_out = NULL;
    if (!pal) return 0;
    int whole = 0;
    for (int i = 0; i < s->nprocs; i++) if (proc_whole_piece(s->procs[i])) { whole = 1; break; }
    TInfo *po = xmalloc((size_t)(pal->nb ? pal->nb : 1) * sizeof(TInfo)), *oo = xmalloc((size_t)(pal->nb ? pal->nb : 1) * sizeof(TInfo));
    int n = 0;
    PEnv env; memset(&env, 0, sizeof env);
    env.w = w; env.bs = bs; env.fc = fc; env.pos_x = x; env.pos_y = y; env.pos_z = z; env.ref_x = rx; env.ref_y = ry; env.ref_z = rz; env.level_seed = level_seed;
    for (int i = 0; i < pal->nb; i++) {
        const TInfo *b = &pal->b[i];
        int wx, wz; tpl_transform(b->x, b->z, s->mir, s->rot, s->px, s->pz, &wx, &wz);
        wx += x; wz += z; int wy = b->y + y;
        if (!whole && s->bounds && !bb_inside(s->bounds, wx, s->bounds->y0, wz)) continue;   /* chunkBb.isInside(pos) — по x/z; y проверяется при записи */
        TInfo cur = { wx, wy, wz, b->state, b->nbt };
        TInfo orig = *b;
        int keep = 1;
        for (int k = 0; k < s->nprocs && keep; k++) keep = proc_block(s->procs[k], &env, &orig, &cur);
        if (!keep) continue;
        po[n] = cur; oo[n] = orig; n++;
    }
    for (int k = 0; k < s->nprocs; k++) proc_finalize(s->procs[k], &env, oo, po, n);
    *out = po; if (orig_out) *orig_out = oo; else free(oo);
    return n;
}

/* instanceof LiquidBlockContainer: блоки со свойством waterlogged (SimpleWaterloggedBlock) + келп/морская трава */
static const u8 *liquid_container_blocks(McWorld *w) {
    TStore *s = store_of(w); const BsTab *bs = bs_get(w->g);
    mutex_lock(s->lock);
    if (!s->lbc) {
        u8 *m = xcalloc((size_t)(bs->nblocks ? bs->nblocks : 1), 1);
        for (int b = 0; b < bs->nblocks; b++) {
            const BsBlock *bb = &bs->blk[b];
            for (int p = 0; p < bb->nprops; p++) if (!strcmp(bb->pname[p], "waterlogged")) { m[b] = 1; break; }
        }
        static const char *K[] = { "minecraft:kelp", "minecraft:kelp_plant", "minecraft:seagrass", "minecraft:tall_seagrass", NULL };
        for (int k = 0; K[k]; k++) { int b = bs_block_index(bs, K[k]); if (b >= 0) m[b] = 1; }
        s->lbc = m;
    }
    const u8 *r = s->lbc;
    mutex_unlock(s->lock);
    return r;
}
/* блок-сущность — RandomizableContainer (ChestBlockEntity, BarrelBlockEntity, DispenserBlockEntity, HopperBlockEntity, ShulkerBoxBlockEntity, CrafterBlockEntity, DecoratedPotBlockEntity) */
static const u8 *randomizable_blocks(McWorld *w) {
    TStore *s = store_of(w); const BsTab *bs = bs_get(w->g);
    mutex_lock(s->lock);
    if (!s->rc) {
        u8 *m = xcalloc((size_t)(bs->nblocks ? bs->nblocks : 1), 1);
        static const char *C[] = { "ChestBlock", "BarrelBlock", "DispenserBlock", "HopperBlock", "ShulkerBoxBlock", "CrafterBlock", "DecoratedPotBlock", NULL };
        for (int b = 0; b < bs->nblocks; b++) {
            const BsBlock *bb = &bs->blk[b]; if (!bb->chain) continue;
            for (int k = 0; C[k]; k++) { char key[64]; snprintf(key, sizeof key, "|%s|", C[k]); if (strstr(bb->chain, key)) { m[b] = 1; break; } }
        }
        s->rc = m;
    }
    const u8 *r = s->rc;
    mutex_unlock(s->lock);
    return r;
}

int template_place(FCtx *fc, McWorld *w, const Template *t, int x, int y, int z, int rx, int ry, int rz,
                   const TSettings *s, i64 level_seed, int flags) {
    const BsTab *bs = bs_get(w->g);
    const u8 *rand_blk = s->rnd ? randomizable_blocks(w) : NULL;
    const u8 *lbc = liquid_container_blocks(w);
    const TPal *pal = tpl_palette_at(t, x, y, z);
    if (!pal || pal->nb == 0 || t->sx < 1 || t->sy < 1 || t->sz < 1) return 0;
    TInfo *list = NULL;
    int n = template_process(fc, w, t, x, y, z, rx, ry, rz, s, level_seed, &list, NULL);
    int *placed = s->known_shape ? NULL : xmalloc((size_t)(n ? n : 1) * 3 * sizeof(int)); int nplaced = 0;
    /* места, где жидкость была до записи → waterlogging */
    int *tofill = NULL, nfill = 0; int *locked = NULL, nlocked = 0;
    if (s->waterlog) { tofill = xmalloc((size_t)(n ? n : 1) * 3 * sizeof(int)); locked = xmalloc((size_t)(n ? n : 1) * 3 * sizeof(int)); }
    for (int i = 0; i < n; i++) {
        TInfo *b = &list[i];
        if (s->bounds && !bb_inside(s->bounds, b->x, b->y, b->z)) continue;
        int prev = 0;
        if (s->waterlog) prev = bs->fluid[fc_get(fc, b->x, b->y, b->z)];
        int st = bsx_rotate(bs, bsx_mirror(bs, b->state, s->mir), s->rot);
        if (!fc_set(fc, b->x, b->y, b->z, st, flags)) continue;
        if (placed) { placed[3 * nplaced] = b->x; placed[3 * nplaced + 1] = b->y; placed[3 * nplaced + 2] = b->z; nplaced++; }
        if (rand_blk && b->nbt && rand_blk[w->g->state_block[st]]) (void)rs_long(s->rnd);       /* blockInfo.nbt.putLong("LootTableSeed", random.nextLong()) */
        if (s->waterlog) {
            int nf = bs->fluid[st];
            if (BS_FL_TYPE(nf) != FL_NONE && BS_FL_SOURCE(nf)) { locked[nlocked * 3] = b->x; locked[nlocked * 3 + 1] = b->y; locked[nlocked * 3 + 2] = b->z; nlocked++; }
            else if (lbc[w->g->state_block[st]]) {          /* instanceof LiquidBlockContainer: placeLiquid(level, pos, state, previousFluidState) */
                const char *wv;
                if (bs_get_prop(bs, st, "waterlogged", &wv) && !strcmp(wv, "false") && BS_FL_TYPE(prev) == FL_WATER) {
                    int ns = bs_with(bs, st, "waterlogged", "true");
                    if (ns >= 0) fc_set(fc, b->x, b->y, b->z, ns, 3);
                }
                if (!(BS_FL_TYPE(prev) != FL_NONE && BS_FL_SOURCE(prev))) { tofill[nfill * 3] = b->x; tofill[nfill * 3 + 1] = b->y; tofill[nfill * 3 + 2] = b->z; nfill++; }   /* previousFluidState не источник (в т. ч. пусто) */
            }
        }
    }
    /* заливка: соседи UP, NORTH, EAST, SOUTH, WEST; первый источник (любой жидкости), не входящий в lockedFluids; элемент снимается при любом найденном источнике */
    if (nfill) {
        static const int D[5][3] = { {0,1,0}, {0,0,-1}, {1,0,0}, {0,0,1}, {-1,0,0} };
        int filled = 1;
        while (filled && nfill > 0) {
            filled = 0;
            for (int i = 0; i < nfill;) {
                int px = tofill[i * 3], py = tofill[i * 3 + 1], pz = tofill[i * 3 + 2];
                int fl = bs->fluid[fc_get(fc, px, py, pz)];
                int src = BS_FL_TYPE(fl) != FL_NONE && BS_FL_SOURCE(fl), stype = src ? BS_FL_TYPE(fl) : FL_NONE;
                for (int d = 0; d < 5 && !src; d++) {
                    int nx = px + D[d][0], ny = py + D[d][1], nz = pz + D[d][2];
                    int nf = bs->fluid[fc_get(fc, nx, ny, nz)];
                    if (BS_FL_TYPE(nf) != FL_NONE && BS_FL_SOURCE(nf)) {
                        int lk = 0; for (int k = 0; k < nlocked; k++) if (locked[k * 3] == nx && locked[k * 3 + 1] == ny && locked[k * 3 + 2] == nz) { lk = 1; break; }
                        if (!lk) { src = 1; stype = BS_FL_TYPE(nf); }
                    }
                }
                if (src) {
                    int st = fc_get(fc, px, py, pz); const char *wv;
                    if (lbc[w->g->state_block[st]]) {
                        if (stype == FL_WATER && bs_get_prop(bs, st, "waterlogged", &wv) && !strcmp(wv, "false")) { int ns = bs_with(bs, st, "waterlogged", "true"); if (ns >= 0) fc_set(fc, px, py, pz, ns, 3); }
                        filled = 1;
                        memmove(&tofill[i * 3], &tofill[(i + 1) * 3], (size_t)(nfill - i - 1) * 3 * sizeof(int)); nfill--;
                        continue;
                    }
                }
                i++;
            }
        }
    }
    if (placed && nplaced) structure_update_after_template(fc, placed, nplaced, flags);
    free(placed);
    free(list); free(tofill); free(locked);
    return 1;
}

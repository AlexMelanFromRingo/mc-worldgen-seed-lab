/* feature_veg_hash.c — порядок обхода java.util.HashSet<BlockPos> (HashMap): точное моделирование таблицы корзин, растущей по правилам putVal/resize,
 * включая деревья-корзины (treeifyBin при цепочке ≥ 9 и ёмкости ≥ 64; красно-чёрное дерево, moveRootToFront, split при resize, untreeify ≤ 6).
 *
 * Зачем: порядок обхода множества позиций (VegetationPatchFeature.surface и т. п.) определяет, в каком порядке тратится ГСЧ — отличие в одной позиции
 * сдвигает все последующие выборки. Хэш BlockPos = Vec3i.hashCode = (y + z·31)·31 + x; spread = h ^ (h >>> 16).
 * Допущения: ключи различны; при равных (после spread) хэшах в дереве порядок определяет System.identityHashCode (недетерминирован) — берём «влево». */
#include "feature_veg.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct HNode { int h; int key; int next, prev, parent, left, right; int red; } HNode;   /* индексы узлов, −1 = null */
typedef struct HMap { HNode *nd; int n; int *tab; int cap; int size; int thr; int *tree; /* tree[j]: корзина j — дерево */ } HMap;

#define NIL (-1)
static HNode *N(HMap *m, int i) { return &m->nd[i]; }

/* ---------------- красно-чёрное дерево (TreeNode) ---------------- */
static int rotate_left(HMap *m, int root, int p) {
    int r, pp, rl;
    if (p != NIL && (r = N(m, p)->right) != NIL) {
        if ((rl = N(m, p)->right = N(m, r)->left) != NIL) N(m, rl)->parent = p;
        if ((pp = N(m, r)->parent = N(m, p)->parent) == NIL) { root = r; N(m, r)->red = 0; }
        else if (N(m, pp)->left == p) N(m, pp)->left = r; else N(m, pp)->right = r;
        N(m, r)->left = p; N(m, p)->parent = r;
    }
    return root;
}
static int rotate_right(HMap *m, int root, int p) {
    int l, pp, lr;
    if (p != NIL && (l = N(m, p)->left) != NIL) {
        if ((lr = N(m, p)->left = N(m, l)->right) != NIL) N(m, lr)->parent = p;
        if ((pp = N(m, l)->parent = N(m, p)->parent) == NIL) { root = l; N(m, l)->red = 0; }
        else if (N(m, pp)->right == p) N(m, pp)->right = l; else N(m, pp)->left = l;
        N(m, l)->right = p; N(m, p)->parent = l;
    }
    return root;
}
static int balance_insertion(HMap *m, int root, int x) {
    N(m, x)->red = 1;
    for (int xp, xpp, xppl, xppr;;) {
        if ((xp = N(m, x)->parent) == NIL) { N(m, x)->red = 0; return x; }
        else if (!N(m, xp)->red || (xpp = N(m, xp)->parent) == NIL) return root;
        if (xp == (xppl = N(m, xpp)->left)) {
            if ((xppr = N(m, xpp)->right) != NIL && N(m, xppr)->red) {
                N(m, xppr)->red = 0; N(m, xp)->red = 0; N(m, xpp)->red = 1; x = xpp;
            } else {
                if (x == N(m, xp)->right) { root = rotate_left(m, root, x = xp); xpp = (xp = N(m, x)->parent) == NIL ? NIL : N(m, xp)->parent; }
                if (xp != NIL) {
                    N(m, xp)->red = 0;
                    if (xpp != NIL) { N(m, xpp)->red = 1; root = rotate_right(m, root, xpp); }
                }
            }
        } else {
            if (xppl != NIL && N(m, xppl)->red) {
                N(m, xppl)->red = 0; N(m, xp)->red = 0; N(m, xpp)->red = 1; x = xpp;
            } else {
                if (x == N(m, xp)->left) { root = rotate_right(m, root, x = xp); xpp = (xp = N(m, x)->parent) == NIL ? NIL : N(m, xp)->parent; }
                if (xp != NIL) {
                    N(m, xp)->red = 0;
                    if (xpp != NIL) { N(m, xpp)->red = 1; root = rotate_left(m, root, xpp); }
                }
            }
        }
    }
}
static void move_root_to_front(HMap *m, int root) {
    if (root == NIL || m->cap <= 0) return;
    int index = (m->cap - 1) & N(m, root)->h;
    int first = m->tab[index];
    if (root != first) {
        m->tab[index] = root;
        int rp = N(m, root)->prev, rn = N(m, root)->next;
        if (rn != NIL) N(m, rn)->prev = rp;
        if (rp != NIL) N(m, rp)->next = rn;
        if (first != NIL) N(m, first)->prev = root;
        N(m, root)->next = first; N(m, root)->prev = NIL;
    }
}
static inline int cmp_hash(int ph, int h) { return ph > h ? -1 : (ph < h ? 1 : 0); }   /* signed int, как в Java */
/* TreeNode.treeify: строит дерево из цепочки, начиная с узла first (порядок — по next), корень выносится в начало корзины */
static void treeify(HMap *m, int first) {
    int root = NIL;
    for (int x = first, next; x != NIL; x = next) {
        next = N(m, x)->next;
        N(m, x)->left = N(m, x)->right = NIL;
        if (root == NIL) { N(m, x)->parent = NIL; N(m, x)->red = 0; root = x; }
        else {
            int h = N(m, x)->h;
            for (int p = root;;) {
                int ph = N(m, p)->h, dir = cmp_hash(ph, h);
                if (dir == 0) dir = -1;                              /* tieBreakOrder: identityHashCode — недетерминирован */
                int xp = p;
                p = dir <= 0 ? N(m, p)->left : N(m, p)->right;
                if (p == NIL) {
                    N(m, x)->parent = xp;
                    if (dir <= 0) N(m, xp)->left = x; else N(m, xp)->right = x;
                    root = balance_insertion(m, root, x);
                    break;
                }
            }
        }
    }
    move_root_to_front(m, root);
}
static int tree_root_of(HMap *m, int x) { for (;;) { int p = N(m, x)->parent; if (p == NIL) return x; x = p; } }
static void put_tree_val(HMap *m, int first, int x_new) {
    int h = N(m, x_new)->h;
    int root = N(m, first)->parent != NIL ? tree_root_of(m, first) : first;
    for (int p = root;;) {
        int ph = N(m, p)->h, dir = cmp_hash(ph, h);
        if (dir == 0) dir = -1;
        int xp = p;
        p = dir <= 0 ? N(m, p)->left : N(m, p)->right;
        if (p == NIL) {
            int xpn = N(m, xp)->next;
            HNode *x = N(m, x_new);
            x->next = xpn; x->left = x->right = NIL;
            if (dir <= 0) N(m, xp)->left = x_new; else N(m, xp)->right = x_new;
            N(m, xp)->next = x_new; x->parent = x->prev = xp;
            if (xpn != NIL) N(m, xpn)->prev = x_new;
            move_root_to_front(m, balance_insertion(m, root, x_new));
            return;
        }
    }
}

/* ---------------- resize ---------------- */
static void resize(HMap *m) {
    int oldcap = m->cap, newcap = oldcap == 0 ? 16 : oldcap * 2;
    int *oldtab = m->tab; int *oldtree = m->tree;
    m->tab = malloc(sizeof(int) * (size_t)newcap); m->tree = calloc((size_t)newcap, sizeof(int));
    for (int i = 0; i < newcap; i++) m->tab[i] = NIL;
    m->cap = newcap; m->thr = newcap * 3 / 4;
    for (int j = 0; j < oldcap; j++) {
        int e = oldtab[j];
        if (e == NIL) continue;
        if (N(m, e)->next == NIL && !oldtree[j]) { m->tab[N(m, e)->h & (newcap - 1)] = e; continue; }
        /* разбиение на lo/hi с сохранением порядка по next (и для обычных цепочек, и для деревьев) */
        int lo_head = NIL, lo_tail = NIL, hi_head = NIL, hi_tail = NIL, lc = 0, hc = 0;
        for (int nx; e != NIL; e = nx) {
            nx = N(m, e)->next; N(m, e)->next = NIL;
            if ((N(m, e)->h & oldcap) == 0) {
                if (oldtree[j]) N(m, e)->prev = lo_tail;
                if (lo_tail == NIL) lo_head = e; else N(m, lo_tail)->next = e;
                lo_tail = e; lc++;
            } else {
                if (oldtree[j]) N(m, e)->prev = hi_tail;
                if (hi_tail == NIL) hi_head = e; else N(m, hi_tail)->next = e;
                hi_tail = e; hc++;
            }
        }
        if (!oldtree[j]) {
            if (lo_head != NIL) m->tab[j] = lo_head;
            if (hi_head != NIL) m->tab[j + oldcap] = hi_head;
            continue;
        }
        if (lo_head != NIL) {
            if (lc <= 6) { m->tab[j] = lo_head; }                                   /* untreeify: порядок по next сохраняется */
            else { m->tab[j] = lo_head; m->tree[j] = 1; if (hi_head != NIL) treeify(m, lo_head); }
        }
        if (hi_head != NIL) {
            if (hc <= 6) { m->tab[j + oldcap] = hi_head; }
            else { m->tab[j + oldcap] = hi_head; m->tree[j + oldcap] = 1; if (lo_head != NIL) treeify(m, hi_head); }
        }
    }
    free(oldtab); free(oldtree);
}

static void put(HMap *m, int node) {
    if (m->cap == 0) resize(m);
    int idx = (m->cap - 1) & N(m, node)->h;
    N(m, node)->next = N(m, node)->prev = N(m, node)->parent = N(m, node)->left = N(m, node)->right = NIL;
    int first = m->tab[idx];
    if (first == NIL) m->tab[idx] = node;
    else if (m->tree[idx]) put_tree_val(m, first, node);
    else {
        int p = first;
        for (int bin = 0;; bin++) {
            int e = N(m, p)->next;
            if (e == NIL) {
                N(m, p)->next = node;
                if (bin >= 7) {                                                    /* treeifyBin */
                    if (m->cap < 64) resize(m);
                    else {
                        int h0 = N(m, node)->h, i2 = (m->cap - 1) & h0;
                        int pr = NIL;
                        for (int q = m->tab[i2]; q != NIL; q = N(m, q)->next) { N(m, q)->prev = pr; pr = q; }     /* replacementTreeNode: prev/next цепочки */
                        m->tree[i2] = 1; treeify(m, m->tab[i2]);
                    }
                }
                break;
            }
            p = e;
        }
    }
    if (++m->size > m->thr) resize(m);
}

void veg_hashset_order(VPos *a, int n) {
    if (n < 2) return;
    if (getenv("MCGEN_VEG_ORDER")) return;               /* отладка: порядок вставки */
    HMap m; memset(&m, 0, sizeof m);
    m.nd = malloc(sizeof(HNode) * (size_t)n); m.n = n;
    for (int i = 0; i < n; i++) {
        u32 x = (u32)(((u32)a[i].y + (u32)a[i].z * 31u) * 31u + (u32)a[i].x);
        m.nd[i].h = (int)(x ^ (x >> 16)); m.nd[i].key = i;
    }
    for (int i = 0; i < n; i++) put(&m, i);
    VPos *tmp = malloc(sizeof(VPos) * (size_t)n); int k = 0;
    for (int j = 0; j < m.cap; j++) for (int e = m.tab[j]; e != NIL; e = N(&m, e)->next) tmp[k++] = a[N(&m, e)->key];
    if (k == n) memcpy(a, tmp, sizeof(VPos) * (size_t)n);
    free(tmp); free(m.nd); free(m.tab); free(m.tree);
}

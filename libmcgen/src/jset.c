/* jset.c — java.util.HashSet<BlockPos> (HashMap, JDK 17–25) буквально: хэш BlockPos.hashCode() = (y + z·31)·31 + x, spread h ^ (h >>> 16),
 * таблица корзин 16 → удвоение при size > 0,75·ёмкость, разделение корзин по (hash & oldCap) с сохранением порядка, treeifyBin при цепочке ≥ 9
 * (при ёмкости < 64 вместо дерева — resize), деревья-корзины (TreeNode: красно-чёрное дерево по хэшу, moveRootToFront, split при resize, untreeify ≤ 6),
 * iterator().next() + remove() (removeTreeNode при movable = false).
 *
 * Порядок обхода (цепочки next) определяет, в каком порядке тратится ГСЧ украшателей и обходится updateLeaves, поэтому он нужен точно.
 * Допущение: при равных (после spread) хэшах разных ключей порядок в дереве определял бы System.identityHashCode (недетерминирован) — берём «влево»;
 * для позиций внутри одного дерева/заплатки полные хэши совпасть не могут (Δx + 31Δy + 961Δz = 0 требует разброса ≥ 31 по оси).
 * Проверка против настоящей JDK: libmcgen/tests/g5_jset/run.sh (add / pop-first, в том числе плотные наборы с деревьями-корзинами). */
#include "feature_tree.h"
#include <stdlib.h>
#include <string.h>

#define NIL (-1)
#define P(i) (s->nodes[i])

static inline int jhash(int x, int y, int z) {
    unsigned h = (unsigned)(((unsigned)y + (unsigned)z * 31u) * 31u + (unsigned)x);
    return (int)(h ^ (h >> 16));      /* HashMap.hash(): h ^ (h >>> 16) от BlockPos.hashCode() */
}

void jset_init(JSet *s) { memset(s, 0, sizeof *s); s->free = -1; jset_clear(s); }
void jset_clear(JSet *s) {
    if (!s->tab) { s->tcap = 16; s->tab = xmalloc(sizeof(int) * 16); }
    s->tsize = 16; s->thr = 12; s->size = 0; s->nn = 0; s->free = -1; s->lo = 16; s->treeified = 0;
    for (int i = 0; i < 16; i++) s->tab[i] = -1;
}
void jset_free(JSet *s) { free(s->nodes); free(s->tab); memset(s, 0, sizeof *s); }

static int jnew(JSet *s, int x, int y, int z, int hash) {
    int i;
    if (s->free >= 0) { i = s->free; s->free = s->nodes[i].next; }
    else {
        if (s->nn == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->nodes = realloc(s->nodes, sizeof(JNode) * (size_t)s->cap); if (!s->nodes) abort(); }
        i = s->nn++;
    }
    JNode *n = &s->nodes[i]; n->x = x; n->y = y; n->z = z; n->hash = hash;
    n->next = n->prev = n->parent = n->left = n->right = NIL; n->red = 0; n->istree = 0;
    return i;
}

/* ---------------- красно-чёрное дерево (HashMap.TreeNode) ---------------- */
static int rotate_left(JSet *s, int root, int p) {
    int r, pp, rl;
    if (p != NIL && (r = P(p).right) != NIL) {
        if ((rl = P(p).right = P(r).left) != NIL) P(rl).parent = p;
        if ((pp = P(r).parent = P(p).parent) == NIL) { root = r; P(r).red = 0; }
        else if (P(pp).left == p) P(pp).left = r; else P(pp).right = r;
        P(r).left = p; P(p).parent = r;
    }
    return root;
}
static int rotate_right(JSet *s, int root, int p) {
    int l, pp, lr;
    if (p != NIL && (l = P(p).left) != NIL) {
        if ((lr = P(p).left = P(l).right) != NIL) P(lr).parent = p;
        if ((pp = P(l).parent = P(p).parent) == NIL) { root = l; P(l).red = 0; }
        else if (P(pp).right == p) P(pp).right = l; else P(pp).left = l;
        P(l).right = p; P(p).parent = l;
    }
    return root;
}
static int balance_insertion(JSet *s, int root, int x) {
    P(x).red = 1;
    for (int xp, xpp, xppl, xppr;;) {
        if ((xp = P(x).parent) == NIL) { P(x).red = 0; return x; }
        else if (!P(xp).red || (xpp = P(xp).parent) == NIL) return root;
        if (xp == (xppl = P(xpp).left)) {
            if ((xppr = P(xpp).right) != NIL && P(xppr).red) {
                P(xppr).red = 0; P(xp).red = 0; P(xpp).red = 1; x = xpp;
            } else {
                if (x == P(xp).right) { root = rotate_left(s, root, x = xp); xpp = (xp = P(x).parent) == NIL ? NIL : P(xp).parent; }
                if (xp != NIL) {
                    P(xp).red = 0;
                    if (xpp != NIL) { P(xpp).red = 1; root = rotate_right(s, root, xpp); }
                }
            }
        } else {
            if (xppl != NIL && P(xppl).red) {
                P(xppl).red = 0; P(xp).red = 0; P(xpp).red = 1; x = xpp;
            } else {
                if (x == P(xp).left) { root = rotate_right(s, root, x = xp); xpp = (xp = P(x).parent) == NIL ? NIL : P(xp).parent; }
                if (xp != NIL) {
                    P(xp).red = 0;
                    if (xpp != NIL) { P(xpp).red = 1; root = rotate_left(s, root, xpp); }
                }
            }
        }
    }
}
static int balance_deletion(JSet *s, int root, int x) {
    for (int xp, xpl, xpr;;) {
        if (x == NIL || x == root) return root;
        if ((xp = P(x).parent) == NIL) { P(x).red = 0; return x; }
        if (P(x).red) { P(x).red = 0; return root; }
        if ((xpl = P(xp).left) == x) {
            if ((xpr = P(xp).right) != NIL && P(xpr).red) {
                P(xpr).red = 0; P(xp).red = 1;
                root = rotate_left(s, root, xp);
                xpr = (xp = P(x).parent) == NIL ? NIL : P(xp).right;
            }
            if (xpr == NIL) x = xp;
            else {
                int sl = P(xpr).left, sr = P(xpr).right;
                if ((sr == NIL || !P(sr).red) && (sl == NIL || !P(sl).red)) { P(xpr).red = 1; x = xp; }
                else {
                    if (sr == NIL || !P(sr).red) {
                        if (sl != NIL) P(sl).red = 0;
                        P(xpr).red = 1;
                        root = rotate_right(s, root, xpr);
                        xpr = (xp = P(x).parent) == NIL ? NIL : P(xp).right;
                    }
                    if (xpr != NIL) {
                        P(xpr).red = xp == NIL ? 0 : P(xp).red;
                        if ((sr = P(xpr).right) != NIL) P(sr).red = 0;
                    }
                    if (xp != NIL) { P(xp).red = 0; root = rotate_left(s, root, xp); }
                    x = root;
                }
            }
        } else {
            if (xpl != NIL && P(xpl).red) {
                P(xpl).red = 0; P(xp).red = 1;
                root = rotate_right(s, root, xp);
                xpl = (xp = P(x).parent) == NIL ? NIL : P(xp).left;
            }
            if (xpl == NIL) x = xp;
            else {
                int sl = P(xpl).left, sr = P(xpl).right;
                if ((sl == NIL || !P(sl).red) && (sr == NIL || !P(sr).red)) { P(xpl).red = 1; x = xp; }
                else {
                    if (sl == NIL || !P(sl).red) {
                        if (sr != NIL) P(sr).red = 0;
                        P(xpl).red = 1;
                        root = rotate_left(s, root, xpl);
                        xpl = (xp = P(x).parent) == NIL ? NIL : P(xp).left;
                    }
                    if (xpl != NIL) {
                        P(xpl).red = xp == NIL ? 0 : P(xp).red;
                        if ((sl = P(xpl).left) != NIL) P(sl).red = 0;
                    }
                    if (xp != NIL) { P(xp).red = 0; root = rotate_right(s, root, xp); }
                    x = root;
                }
            }
        }
    }
}
static void move_root_to_front(JSet *s, int root) {
    if (root == NIL || s->tsize <= 0) return;
    int index = (s->tsize - 1) & P(root).hash;
    int first = s->tab[index];
    if (root != first) {
        s->tab[index] = root;
        int rp = P(root).prev, rn = P(root).next;
        if (rn != NIL) P(rn).prev = rp;
        if (rp != NIL) P(rp).next = rn;
        if (first != NIL) P(first).prev = root;
        P(root).next = first; P(root).prev = NIL;
    }
}
static inline int cmp_hash(int ph, int h) { return ph > h ? -1 : (ph < h ? 1 : 0); }   /* int со знаком, как в Java */
/* TreeNode.treeify: строит дерево из цепочки, начиная с узла first (порядок — по next), корень выносится в начало корзины */
static void treeify(JSet *s, int first) {
    int root = NIL;
    for (int x = first, next; x != NIL; x = next) {
        next = P(x).next;
        P(x).left = P(x).right = NIL;
        if (root == NIL) { P(x).parent = NIL; P(x).red = 0; root = x; }
        else {
            int h = P(x).hash;
            for (int p = root;;) {
                int ph = P(p).hash, dir = cmp_hash(ph, h);
                if (dir == 0) dir = -1;                                  /* tieBreakOrder: identityHashCode — недетерминирован */
                int xp = p;
                p = dir <= 0 ? P(p).left : P(p).right;
                if (p == NIL) {
                    P(x).parent = xp;
                    if (dir <= 0) P(xp).left = x; else P(xp).right = x;
                    root = balance_insertion(s, root, x);
                    break;
                }
            }
        }
    }
    move_root_to_front(s, root);
}
static int tree_root_of(JSet *s, int x) { for (;;) { int p = P(x).parent; if (p == NIL) return x; x = p; } }
static void untreeify(JSet *s, int first) { for (int e = first; e != NIL; e = P(e).next) P(e).istree = 0; }   /* порядок по next сохраняется */
/* TreeNode.putTreeVal для нового узла xn (ключ отсутствует) */
static void put_tree_val(JSet *s, int first, int xn) {
    int h = P(xn).hash;
    int root = P(first).parent != NIL ? tree_root_of(s, first) : first;
    for (int p = root;;) {
        int ph = P(p).hash, dir = cmp_hash(ph, h);
        if (dir == 0) dir = -1;
        int xp = p;
        p = dir <= 0 ? P(p).left : P(p).right;
        if (p == NIL) {
            int xpn = P(xp).next;
            P(xn).next = xpn; P(xn).left = P(xn).right = NIL; P(xn).istree = 1;
            if (dir <= 0) P(xp).left = xn; else P(xp).right = xn;
            P(xp).next = xn; P(xn).parent = P(xn).prev = xp;
            if (xpn != NIL) P(xpn).prev = xn;
            move_root_to_front(s, balance_insertion(s, root, xn));
            return;
        }
    }
}
/* TreeNode.removeTreeNode при movable = false (так удаляет Iterator.remove): без untreeify и moveRootToFront */
static void remove_tree_node(JSet *s, int self, int index) {
    int first = s->tab[index], root = first;
    int succ = P(self).next, pred = P(self).prev;
    if (pred == NIL) s->tab[index] = first = succ; else P(pred).next = succ;
    if (succ != NIL) P(succ).prev = pred;
    if (first == NIL) return;
    if (P(root).parent != NIL) root = tree_root_of(s, root);
    int pl = P(self).left, pr = P(self).right, replacement;
    if (pl != NIL && pr != NIL) {
        int sc = pr, sl;
        while ((sl = P(sc).left) != NIL) sc = sl;
        int c = P(sc).red; P(sc).red = P(self).red; P(self).red = (unsigned char)c;
        int sr = P(sc).right, pp = P(self).parent;
        if (sc == pr) { P(self).parent = sc; P(sc).right = self; }
        else {
            int sp = P(sc).parent;
            P(self).parent = sp;
            if (sp != NIL) { if (sc == P(sp).left) P(sp).left = self; else P(sp).right = self; }
            P(sc).right = pr; if (pr != NIL) P(pr).parent = sc;
        }
        P(self).left = NIL;
        P(self).right = sr; if (sr != NIL) P(sr).parent = self;
        P(sc).left = pl; if (pl != NIL) P(pl).parent = sc;
        P(sc).parent = pp;
        if (pp == NIL) root = sc; else if (self == P(pp).left) P(pp).left = sc; else P(pp).right = sc;
        replacement = sr != NIL ? sr : self;
    } else if (pl != NIL) replacement = pl;
    else if (pr != NIL) replacement = pr;
    else replacement = self;
    if (replacement != self) {
        int pp2 = P(self).parent; P(replacement).parent = pp2;
        if (pp2 == NIL) { root = replacement; P(replacement).red = 0; }
        else if (self == P(pp2).left) P(pp2).left = replacement; else P(pp2).right = replacement;
        P(self).parent = P(self).right = P(self).left = NIL;
    }
    if (!P(self).red) balance_deletion(s, root, replacement);
    if (replacement == self) {
        int pp3 = P(self).parent; P(self).parent = NIL;
        if (pp3 != NIL) { if (self == P(pp3).left) P(pp3).left = NIL; else if (self == P(pp3).right) P(pp3).right = NIL; }
    }
}

/* ---------------- resize ---------------- */
static void jresize(JSet *s) {
    int oc = s->tsize, nc = oc * 2;
    if (nc > s->tcap) { s->tcap = nc; s->tab = realloc(s->tab, sizeof(int) * (size_t)nc); if (!s->tab) abort(); }
    s->tsize = nc;                                                         /* moveRootToFront внутри treeify индексирует по новой таблице */
    for (int j = 0; j < oc; j++) {
        int e = s->tab[j];
        s->tab[j] = s->tab[j + oc] = NIL;
        if (e == NIL) continue;
        if (P(e).next == NIL) { s->tab[P(e).hash & (nc - 1)] = e; continue; }
        int tree = P(e).istree;
        int lh = NIL, lt = NIL, hh = NIL, ht = NIL, lc = 0, hc = 0;
        for (int nx; e != NIL; e = nx) {
            nx = P(e).next; P(e).next = NIL;
            if ((P(e).hash & oc) == 0) { if (tree) P(e).prev = lt; if (lt == NIL) lh = e; else P(lt).next = e; lt = e; lc++; }
            else { if (tree) P(e).prev = ht; if (ht == NIL) hh = e; else P(ht).next = e; ht = e; hc++; }
        }
        if (!tree) { s->tab[j] = lh; s->tab[j + oc] = hh; continue; }
        if (lh != NIL) {
            if (lc <= 6) { untreeify(s, lh); s->tab[j] = lh; }              /* UNTREEIFY_THRESHOLD */
            else { s->tab[j] = lh; if (hh != NIL) treeify(s, lh); }          /* иначе дерево уцелело целиком */
        }
        if (hh != NIL) {
            if (hc <= 6) { untreeify(s, hh); s->tab[j + oc] = hh; }
            else { s->tab[j + oc] = hh; if (lh != NIL) treeify(s, hh); }
        }
    }
    s->thr *= 2; s->lo = 0;
}

int jset_add(JSet *s, int x, int y, int z) {
    int hash = jhash(x, y, z), idx = hash & (s->tsize - 1);
    int p = s->tab[idx];
    if (p < 0) { int n = jnew(s, x, y, z, hash); s->tab[idx] = n; }
    else {
        int len = 0, last = p;
        for (int e = p; e >= 0; e = P(e).next) {
            const JNode *n = &P(e);
            if (n->hash == hash && n->x == x && n->y == y && n->z == z) return 0;
            len++; last = e;
        }
        int nn = jnew(s, x, y, z, hash);
        if (P(p).istree) put_tree_val(s, p, nn);
        else {
            P(last).next = nn;
            if (len >= 8) {                    /* binCount >= TREEIFY_THRESHOLD − 1 → treeifyBin */
                if (s->tsize < 64) jresize(s);
                else {                         /* replacementTreeNode для всей цепочки + treeify */
                    int prev = NIL;
                    for (int e = s->tab[idx]; e != NIL; e = P(e).next) { P(e).prev = prev; P(e).istree = 1; prev = e; }
                    treeify(s, s->tab[idx]);
                    s->treeified++;
                }
            }
        }
    }
    if (idx < s->lo) s->lo = idx;
    if (++s->size > s->thr) jresize(s);
    return 1;
}
int jset_contains(const JSet *s, int x, int y, int z) {
    int hash = jhash(x, y, z);
    for (int e = s->tab[hash & (s->tsize - 1)]; e >= 0; e = s->nodes[e].next) {
        const JNode *n = &s->nodes[e];
        if (n->hash == hash && n->x == x && n->y == y && n->z == z) return 1;
    }
    return 0;
}
int jset_pop_first(JSet *s, BPos *out) {
    if (s->size == 0) return 0;
    int b = s->lo;
    while (b < s->tsize && s->tab[b] < 0) b++;
    if (b >= s->tsize) { s->size = 0; return 0; }
    s->lo = b;
    int e = s->tab[b]; JNode *n = &s->nodes[e];
    out->x = n->x; out->y = n->y; out->z = n->z;
    if (n->istree) remove_tree_node(s, e, b); else s->tab[b] = n->next;
    n = &s->nodes[e];
    n->next = s->free; s->free = e; s->size--;
    return 1;
}

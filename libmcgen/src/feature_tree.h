/* feature_tree.h — деревья (TreeFeature), стволы, листва, украшатели, корни, упавшие деревья: внутренний интерфейс группы фич W11.
 *
 * Файлы группы:
 *   feature_tree.c          TreeFeature.place (двухфазность: блоки + updateLeaves), разбор конфигурации, Java-HashSet<BlockPos>, fallen_tree, регистрация
 *   feature_tree_placers.c  TrunkPlacer'ы (straight, forking, giant, mega_jungle, dark_oak, fancy, bending, upwards_branching, cherry, poplar),
 *                           FoliagePlacer'ы, RootPlacer (mangrove), FeatureSize
 *   feature_tree_deco.c     TreeDecorator'ы (beehive, cocoa, leave_vine, trunk_vine, alter_ground, attached_to_leaves/logs, place_on_ground, pale_moss,
 *                           creaking_heart, shelf_mushroom) и updateShape краёв дерева
 *   feature_mushroom.c      huge_red_mushroom, huge_brown_mushroom, huge_fungus, root_system (азалия)
 *
 * ВАЖНО (бит-в-бит с игрой): TreeFeature хранит позиции в HashSet<BlockPos>; порядок обхода множеств определяет порядок вызовов ГСЧ в украшателях
 * (листья/стволы идут в ObjectArrayList(set), затем стабильная сортировка по Y) и порядок обработки BFS в updateLeaves — поэтому множества
 * воспроизведены точно по алгоритму java.util.HashMap (JSet).
 */
#ifndef MCGEN_FEATURE_TREE_H
#define MCGEN_FEATURE_TREE_H
#include "feature.h"

/* ====================================================================== java.util.HashSet<BlockPos> */
typedef struct JNode { int x, y, z, hash, next, prev, parent, left, right; unsigned char red, istree; } JNode;   /* prev/parent/left/right/red — TreeNode, istree — узел в дереве-корзине */
typedef struct JSet {
    JNode *nodes; int nn, cap;      /* пул узлов (nn — выдано, free — список освобождённых) */
    int free;                       /* индекс головы списка свободных узлов или -1 */
    int *tab; int tsize, tcap;      /* таблица корзин: индекс первого узла цепочки или -1 */
    int size, thr;
    int lo;                         /* нижняя граница индекса первой непустой корзины (ускорение итератора) */
    long treeified;                 /* сколько раз цепочка ≥ 9 при таблице ≥ 64 превратилась в дерево-корзину (HashMap.treeifyBin) */
} JSet;
typedef struct BPos { int x, y, z; } BPos;
typedef struct BList { BPos *a; int n, cap; } BList;
void jset_init(JSet *s);
void jset_clear(JSet *s);               /* новый HashSet (ёмкость 16) без освобождения памяти */
void jset_free(JSet *s);
int jset_add(JSet *s, int x, int y, int z);                 /* 1 — добавлен */
int jset_contains(const JSet *s, int x, int y, int z);
int jset_pop_first(JSet *s, BPos *out);                     /* iterator().next() + remove(): 0, если пусто */
void jset_to_list(const JSet *s, BList *out);               /* порядок обхода итератора */
void blist_reset(BList *l);
void blist_push(BList *l, int x, int y, int z);
void blist_free(BList *l);
void blist_sort_by_y(BList *l);                              /* стабильная сортировка по Y (Comparator.comparingInt(Vec3i::getY)) */

/* ====================================================================== теги блоков, нужные деревьям */
typedef struct TreeTags {
    const u8 *replaceable_by_trees, *logs, *leaves, *replaceable_by_mushrooms, *prevents_leaf_decay;
    int vine;                                                /* индекс блока minecraft:vine */
} TreeTags;
const TreeTags *tree_tags(FParse *p);                       /* в арене разбора */

/* ====================================================================== конфигурация */
enum { FS_TWO, FS_THREE };
typedef struct FeatSize { int type, limit, upper_limit, lower, middle, upper, min_clipped; } FeatSize;   /* min_clipped −1 — нет */
int featsize_at(const FeatSize *fs, int tree_height, int yo);

enum { TP_STRAIGHT, TP_FORKING, TP_GIANT, TP_MEGA_JUNGLE, TP_DARK_OAK, TP_FANCY, TP_BENDING, TP_UPWARDS, TP_CHERRY, TP_POPLAR };
typedef struct TrunkCfg {
    int type, base_height, rand_a, rand_b;
    /* bending */ int min_height_for_leaves; IntProv *bend_length;
    /* upwards_branching */ IntProv *extra_branch_steps, *extra_branch_length; float place_branch_per_log; u8 *can_grow_through;
    /* cherry */ IntProv *branch_count, *branch_horizontal_length, *branch_end_offset; int bs_min, bs_max;   /* branch_start_offset_from_top: UniformInt [bs_min, bs_max] */
    /* poplar */ IntProv *trunk_height_above_branches, *branch_amount;
    /* straight (26.4+) */ IntProv *trunk_width;                /* NULL — 1 */
} TrunkCfg;

enum { FP_BLOB, FP_SPRUCE, FP_PINE, FP_ACACIA, FP_BUSH, FP_FANCY, FP_MEGA_JUNGLE, FP_MEGA_PINE, FP_DARK_OAK, FP_RANDOM_SPREAD, FP_CHERRY, FP_POPLAR };
typedef struct FoliageCfg {
    int type;
    IntProv *radius, *offset;
    int height;                           /* blob, bush, fancy, mega_jungle: фиксированная высота */
    IntProv *height_ip;                   /* pine: height; spruce: trunk_height; mega_pine: crown_height; random_spread: foliage_height; cherry/poplar: height */
    int leaf_attempts;                    /* random_spread */
    float wide_bottom_hole, corner_hole, hanging, hanging_ext, side_hole;   /* cherry / poplar */
} FoliageCfg;

typedef struct RootCfg {
    IntProv *trunk_offset_y; BSProv *root_provider;
    int has_above; BSProv *above_provider; float above_chance;
    /* mangrove */ u8 *can_grow_through, *muddy_roots_in; BSProv *muddy_provider; int max_width, max_length; float skew;
} RootCfg;

enum { TD_BEEHIVE, TD_COCOA, TD_LEAVE_VINE, TD_TRUNK_VINE, TD_ALTER_GROUND, TD_ATTACHED_LEAVES, TD_ATTACHED_LOGS, TD_PLACE_ON_GROUND, TD_PALE_MOSS,
       TD_CREAKING_HEART, TD_SHELF_MUSHROOM };
typedef struct TreeDeco {
    int type;
    float prob;                          /* beehive, cocoa, leave_vine, creaking_heart, shelf_mushroom, attached*: probability */
    BSProv *prov;                        /* alter_ground: provider; attached*: block_provider; place_on_ground: block_state_provider */
    int excl_xz, excl_y, required_empty; /* attached_to_leaves */
    int ndirs; int dirs[6];              /* attached*: directions */
    int tries, radius, height;           /* place_on_ground */
    float leaves_prob, trunk_prob, ground_prob;   /* pale_moss */
    Feat *moss_patch;                    /* pale_moss: PALE_MOSS_PATCH (может быть не реализована) */
} TreeDeco;

typedef struct TreeCfg {
    BSProv *trunk_prov, *foliage_prov, *below_prov;
    int ignore_vines;
    FeatSize size;
    TrunkCfg tp;
    FoliageCfg fp;
    RootCfg *root;
    int ndeco; TreeDeco *deco;
    const TreeTags *tags;
} TreeCfg;

typedef struct FallenCfg {
    BSProv *trunk_prov; IntProv *log_length;
    int nstump, nlog; TreeDeco *stump, *log;
    const TreeTags *tags;
} FallenCfg;

/* ====================================================================== выполнение */
typedef struct FoliageAtt { int x, y, z, radius_off, height_off, size_x, size_z; } FoliageAtt;
typedef struct AttList { FoliageAtt *a; int n, cap; FoliageAtt inl[48]; } AttList;
void att_init(AttList *l);
void att_push(AttList *l, int x, int y, int z, int radius_off, int height_off, int size_x, int size_z);
void att_free(AttList *l);

typedef struct TreeRun {
    FCtx *c; FRnd *r; const TreeCfg *t; const TreeTags *tags;
    JSet *roots, *trunks, *foliage, *decor;
} TreeRun;

/* состояние / проверки уровня */
int tr_valid_pos(const TreeRun *tr, int x, int y, int z);       /* TreeFeature.validTreePos */
int tr_is_free(const TreeRun *tr, int x, int y, int z);         /* TrunkPlacer.isFree (с учётом can_grow_through у upwards_branching) */
int tr_is_air_or_leaves(const TreeRun *tr, int x, int y, int z);
int tree_state_axis(const BsTab *bs, int st, int axis);         /* trySetValue(AXIS, ось 0=X 1=Y 2=Z) */
void tr_set_trunk(TreeRun *tr, int x, int y, int z, int st);
void tr_set_root(TreeRun *tr, int x, int y, int z, int st);
void tr_set_foliage(TreeRun *tr, int x, int y, int z, int st);
void tr_set_decor(TreeRun *tr, int x, int y, int z, int st);

/* TrunkPlacer (feature_tree_placers.c) */
int tp_tree_height(const TrunkCfg *tp, FRnd *r);
void tp_place_trunk(TreeRun *tr, int tree_height, int ox, int oy, int oz, AttList *out);
int fs_min_clipped(const FeatSize *fs);
/* FoliagePlacer */
int fp_foliage_height(const FoliageCfg *fp, FRnd *r, int tree_height);
int fp_foliage_radius(const FoliageCfg *fp, FRnd *r, int trunk_height);
void fp_create_foliage(TreeRun *tr, int tree_height, const FoliageAtt *att, int foliage_height, int leaf_radius);
/* RootPlacer */
int root_place(TreeRun *tr, int ox, int oy, int oz, int tox, int toy, int toz);
int root_trunk_origin(const RootCfg *rc, FRnd *r, int *y);        /* getTrunkOrigin: смещение по Y */

/* разбор */
int tree_parse_trunk(FParse *p, const Js *v, TrunkCfg *tp);
int tree_parse_foliage(FParse *p, const Js *v, FoliageCfg *fp);
RootCfg *tree_parse_root(FParse *p, const Js *v);
int tree_parse_decorators(FParse *p, const Js *arr, TreeDeco **out, int *n);

/* украшатели (feature_tree_deco.c) */
typedef struct TreeCtx {              /* TreeDecorator.Context */
    FCtx *c; FRnd *r; const TreeTags *tags;
    BList logs, leaves, roots;
    TreeRun *tr;                      /* NULL: сеттер без учёта decorations (fallen_tree) */
} TreeCtx;
void deco_run(TreeCtx *x, const TreeDeco *d);
void tree_update_shape_at_edge(FCtx *c, int minx, int miny, int minz, int sx, int sy, int sz, const u8 *shape);

#endif

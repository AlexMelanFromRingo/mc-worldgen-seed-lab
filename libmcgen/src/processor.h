/* processor.h — StructureProcessor (templatesystem): правила замены блоков при размещении шаблона (поток W9).
 *
 * Поддержано: rule (RuleTest: always_true, block_match, blockstate_match, random_block_match, random_blockstate_match, tag_match,
 * all_of/any_of/not; PosRuleTest: always_true, linear_pos, axis_aligned_linear_pos), protected_blocks, block_rot, capped,
 * block_ignore, nop, jigsaw_replacement (встроен), gravity (проекция terrain_matching), block_age, lava_submerged_block, blackstone_replace.
 * Порядок и расход ГСЧ — как у игры: RandomSource.create(Mth.getSeed(pos)) = LegacyRandomSource на блок.
 */
#ifndef MCGEN_PROCESSOR_H
#define MCGEN_PROCESSOR_H
#include "feature.h"
#include "nbt.h"

/* StructureTemplate.StructureBlockInfo: позиция, состояние (id libmcgen), NBT (указывает в документ шаблона; NULL — нет) */
typedef struct TInfo { int x, y, z; int state; const Nbt *nbt; } TInfo;

typedef struct Proc Proc;
typedef struct ProcList { char *id; int n; Proc **p; } ProcList;

/* окружение обработки (аргументы StructureProcessor.processBlock) */
typedef struct PEnv {
    McWorld *w; const BsTab *bs; FCtx *fc;       /* fc может быть NULL — тогда чтения мира дают воздух */
    int pos_x, pos_y, pos_z;                     /* targetPosition (позиция шаблона) */
    int ref_x, ref_y, ref_z;                     /* referencePos */
    i64 level_seed;                              /* ServerLevel.getSeed (для capped) */
    void *rnd;                                   /* RS* — StructurePlaceSettings.random (фичи fossil/template): getRandom(pos) отдаёт его, а не LCG по позиции; NULL — по позиции */
} PEnv;

const ProcList *proclist_get(McWorld *w, const char *id);               /* worldgen/processor_list/<id>.json (кэш в мире); NULL если нет */
const ProcList *proclist_from_json(McWorld *w, const Js *v);              /* строка-id | {"processors":[…]} | […] */
const ProcList *proclist_empty(void);
/* встроенные процессоры (StructurePlaceSettings) */
enum { PB_STRUCTURE_BLOCK = 0, PB_STRUCTURE_AND_AIR, PB_AIR, PB_JIGSAW_REPLACEMENT, PB__COUNT };
const Proc *proc_builtin(McWorld *w, int which);
const Proc *proc_gravity(McWorld *w, int heightmap_type, int offset);    /* Projection.TERRAIN_MATCHING */
/* процессоры, которые постройки создают в коде (RuinedPortal, OceanRuin): */
const Proc *proc_block_age(McWorld *w, float mossiness);                  /* BlockAgeProcessor */
const Proc *proc_blackstone_replace(McWorld *w);                          /* BlackstoneReplaceProcessor */
const Proc *proc_lava_submerged(McWorld *w);                              /* LavaSubmergedBlockProcessor */

/* processBlock: 1 — блок остался (cur изменён на месте), 0 — убран (null) */
int proc_block(const Proc *p, PEnv *e, const TInfo *orig, TInfo *cur);
int proc_whole_piece(const Proc *p);                                      /* evaluatesEntirePieceState */
/* finalizeProcessing: original/processed — параллельные массивы длины n (processed меняется на месте) */
void proc_finalize(const Proc *p, PEnv *e, const TInfo *orig, TInfo *proc, int n);

void *processors_store_new(void);
void processors_world_free(McWorld *w, void *procs);
#endif

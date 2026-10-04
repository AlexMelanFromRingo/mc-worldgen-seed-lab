/* feature_sched.c — порядок шагов FEATURES «как у настоящего сервера»: дискретная модель планировщика генерации чанков 26.3
 * (ChunkMap + ChunkGenerationTask + ChunkTaskDispatcher + ChunkPyramid), порт tools/gt/sched_sim.py. Подробно — docs/blender/nondeterminism.md §2.1.
 *
 *  - Для каждого чанка в пределах 2 от запрошенной области создаётся задача до статуса FULL (область и два кольца) либо INITIALIZE_LIGHT (третье кольцо);
 *    уровень очереди = 31 + расстояние до области (уровень билета forceload).
 *  - Задача (ChunkGenerationTask.runUntilWait) идёт по слоям статусов; на каждом слое для всех чанков радиуса (x внешний, z внутренний) вызывается applyStep:
 *    первый дошедший до (чанк, статус) выполняет шаг, остальные ждут его future.
 *  - STRUCTURE_* и FEATURES — синхронно в последовательном исполнителе; BIOMES/TERRAIN/свет — асинхронно (в модели задержка фиксирована: порядок от неё не зависит).
 *  - Очередь: меньший уровень первым, внутри уровня — порядок вставки ключа чанка; готовая снова задача уходит в хвост.
 * Порядок создания задач в игре определяет обход ReferenceOpenHashSet (идентификационные хэши, случайный); модель берёт порядок (x, z) — он точнее случайных (см. документ).
 * Сам порядок игры недетерминирован (между прогонами различается у ≈5 % пар соседних чанков); модель воспроизводит его с расхождением ≈9 % против 19 % у обхода «x, затем z»,
 * а суммарное расхождение с эталонами G5 ниже на ≈20 % (Nether seed 12345: 26,9 → 6,3 тыс. блоков). */
#include "feature.h"

enum { S_EMPTY, S_STRUCT_STARTS, S_STRUCT_REFS, S_BIOMES, S_TERRAIN, S_FEATURES, S_INIT_LIGHT, S_LIGHT, S_SPAWN, S_FULL, S_N };
/* ChunkStep.getAccumulatedRadiusOf(статус) для целей FULL и INITIALIZE_LIGHT (порт ChunkStep.Builder; проверка — tools/gt/sched_sim.py --print-pyramid) */
static const int RAD_FULL[S_N] = { 11, 11, 3, 3, 2, 1, 1, 0, 0, 0 };
static const int RAD_INIT[S_N] = { 10, 10, 2, 2, 1, 0, 0, 0, 0, 0 };
#define EXTRA 14                          /* кольцо сетки вокруг области: максимальный радиус слоя (11) + запас */
#define ASYNC_MS 40.0
#define FEAT_MS 27.0
#define NQ 8

typedef struct STask { int cx, cz, target, sched, cancel, nlayer, caplayer; int *layer; } STask;     /* layer: индекс чанка * 16 + статус */
typedef struct SChunk { int level, started, done, inq, task, npend, cappend, qnext; int *pend; } SChunk;
typedef struct WNode { int task, next; } WNode;
typedef struct Ev { double t; unsigned long seq; int chunk, status; } Ev;

typedef struct Sim {
    int x0, z0, nx, nz;
    SChunk *ch;
    STask *tasks; int ntasks, captasks;
    int *whead; WNode *wpool; int nw, capw;      /* ожидающие (чанк, статус): односвязные списки задач */
    Ev *heap; int nev, capev; unsigned long seq;
    int qhead[NQ], qtail[NQ];
    double now;
    int *order; int nord, capord;
} Sim;

static inline int sidx(const Sim *s, int x, int z) { return (z - s->z0) * s->nx + (x - s->x0); }
static inline int sin_(const Sim *s, int x, int z) { return x >= s->x0 && x < s->x0 + s->nx && z >= s->z0 && z < s->z0 + s->nz; }
static inline int ev_less(const Ev *a, const Ev *b) { return a->t < b->t || (a->t == b->t && a->seq < b->seq); }

static void ev_push(Sim *s, double t, int chunk, int status) {
    if (s->nev == s->capev) { s->capev = s->capev ? s->capev * 2 : 1024; s->heap = xrealloc(s->heap, sizeof(Ev) * (size_t)s->capev); }
    Ev e = { t, s->seq++, chunk, status };
    int i = s->nev++;
    while (i > 0) { int p = (i - 1) / 2; if (!ev_less(&e, &s->heap[p])) break; s->heap[i] = s->heap[p]; i = p; }
    s->heap[i] = e;
}
static Ev ev_pop(Sim *s) {
    Ev top = s->heap[0], e = s->heap[--s->nev];
    int i = 0;
    while (s->nev > 0) {
        int l = 2 * i + 1, r = l + 1, m = -1;
        if (l < s->nev && ev_less(&s->heap[l], &e)) m = l;
        if (r < s->nev && ev_less(&s->heap[r], m < 0 ? &e : &s->heap[l])) m = r;
        if (m < 0) break;
        s->heap[i] = s->heap[m]; i = m;
    }
    if (s->nev > 0) s->heap[i] = e;
    return top;
}

static void submit(Sim *s, int ti) {
    STask *t = &s->tasks[ti]; int ci = sidx(s, t->cx, t->cz); SChunk *c = &s->ch[ci];
    if (c->npend == c->cappend) { c->cappend = c->cappend ? c->cappend * 2 : 4; c->pend = xrealloc(c->pend, sizeof(int) * (size_t)c->cappend); }
    c->pend[c->npend++] = ti;
    if (!c->inq) {                                   /* ключ чанка в очереди своего уровня: порядок вставки сохраняется, пока ключ жив */
        int q = c->level - 31; if (q < 0) q = 0; if (q >= NQ) q = NQ - 1;
        c->inq = 1; c->qnext = -1;
        if (s->qtail[q] >= 0) s->ch[s->qtail[q]].qnext = ci; else s->qhead[q] = ci;
        s->qtail[q] = ci;
    }
}

static void complete(Sim *s, int ci, int status) {
    s->ch[ci].done |= 1 << status;
    int key = ci * S_N + status, n = s->whead[key];
    s->whead[key] = -1;
    while (n >= 0) { int nx = s->wpool[n].next, ti = s->wpool[n].task; submit(s, ti); n = nx; }      /* список ожидающих хранится в порядке добавления (см. wait_add) */
}
static void wait_add(Sim *s, int ci, int status, int ti) {
    if (s->nw == s->capw) { s->capw = s->capw ? s->capw * 2 : 4096; s->wpool = xrealloc(s->wpool, sizeof(WNode) * (size_t)s->capw); }
    int key = ci * S_N + status, n = s->nw++;
    s->wpool[n].task = ti; s->wpool[n].next = -1;
    if (s->whead[key] < 0) s->whead[key] = n;
    else { int p = s->whead[key]; while (s->wpool[p].next >= 0) p = s->wpool[p].next; s->wpool[p].next = n; }       /* в хвост (списки короткие) */
}

/* выполняет шаг (чанк, статус): синхронный — возвращает затраченное время исполнителя; асинхронный — планирует завершение, 0 */
static double exec_step(Sim *s, int ci, int status) {
    if (status == S_EMPTY || status == S_STRUCT_STARTS || status == S_STRUCT_REFS) { complete(s, ci, status); return 0.2; }
    if (status == S_FEATURES) {
        if (s->nord == s->capord) { s->capord = s->capord ? s->capord * 2 : 1024; s->order = xrealloc(s->order, sizeof(int) * (size_t)s->capord); }
        s->order[s->nord++] = ci; complete(s, ci, status); return FEAT_MS;
    }
    double base = ASYNC_MS * ((status == S_INIT_LIGHT || status == S_LIGHT || status == S_FULL || status == S_SPAWN) ? 0.05 : (status == S_BIOMES ? 0.1 : 1.0));
    ev_push(s, s->now + base, ci, status);
    return 0.0;
}

static double run_until_wait(Sim *s, int ti) {
    double spent = 0.0;
    for (;;) {
        STask *t = &s->tasks[ti];
        while (t->nlayer) {                                           /* waitForScheduledLayer: последний future слоя */
            int e = t->layer[t->nlayer - 1], ci = e >> 4, st = e & 15;
            if (!((s->ch[ci].done >> st) & 1)) { wait_add(s, ci, st, ti); return spent; }
            t->nlayer--;
        }
        if (t->cancel || t->sched == t->target) return spent;
        int nxt = t->sched < 0 ? 0 : t->sched + 1;
        int r = (t->target == S_FULL ? RAD_FULL : RAD_INIT)[nxt];
        for (int x = t->cx - r; x <= t->cx + r; x++) for (int z = t->cz - r; z <= t->cz + r; z++) {
            if (!sin_(s, x, z)) continue;
            int ci = sidx(s, x, z); SChunk *c = &s->ch[ci];
            if (!((c->started >> nxt) & 1)) { c->started |= 1 << nxt; spent += exec_step(s, ci, nxt); t = &s->tasks[ti]; }
            if (!((c->done >> nxt) & 1)) {
                if (t->nlayer == t->caplayer) { t->caplayer = t->caplayer ? t->caplayer * 2 : 64; t->layer = xrealloc(t->layer, sizeof(int) * (size_t)t->caplayer); }
                t->layer[t->nlayer++] = ci * 16 + nxt;
            }
        }
        t->sched = nxt;
    }
}

/* Порядок шагов FEATURES для запрошенной области [ax0..ax1]×[az0..az1] (чанки, включительно): массив пар (cx, cz) длиной n (освобождает вызывающий) */
int features_sched_order(int ax0, int az0, int ax1, int az1, int **out_xz, int *n_out) {
    Sim s; memset(&s, 0, sizeof s);
    s.x0 = ax0 - EXTRA; s.z0 = az0 - EXTRA; s.nx = ax1 - ax0 + 1 + 2 * EXTRA; s.nz = az1 - az0 + 1 + 2 * EXTRA;
    size_t N = (size_t)s.nx * s.nz;
    s.ch = xcalloc(N, sizeof(SChunk)); s.whead = xmalloc(sizeof(int) * N * S_N);
    for (size_t i = 0; i < N * S_N; i++) s.whead[i] = -1;
    for (int q = 0; q < NQ; q++) s.qhead[q] = s.qtail[q] = -1;
    for (int z = s.z0; z < s.z0 + s.nz; z++) for (int x = s.x0; x < s.x0 + s.nx; x++) {
        int dx = x < ax0 ? ax0 - x : (x > ax1 ? x - ax1 : 0), dz = z < az0 ? az0 - z : (z > az1 ? z - az1 : 0);
        SChunk *c = &s.ch[sidx(&s, x, z)]; c->level = 31 + (dx > dz ? dx : dz); c->task = -1; c->qnext = -1;
    }
    /* создание задач: доступные держатели (расстояние ≤ 2) в порядке (x, z); для каждого окно 3×3 (z внешний): центр — FULL, соседи — INITIALIZE_LIGHT; более высокий статус заменяет задачу */
    for (int hx = ax0 - 2; hx <= ax1 + 2; hx++) for (int hz = az0 - 2; hz <= az1 + 2; hz++)
        for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
            int ci = sidx(&s, hx + dx, hz + dz); SChunk *c = &s.ch[ci];
            int st = (dx == 0 && dz == 0) ? S_FULL : S_INIT_LIGHT;
            if (c->task < 0 || st > s.tasks[c->task].target) {
                if (c->task >= 0) s.tasks[c->task].cancel = 1;
                if (s.ntasks == s.captasks) { s.captasks = s.captasks ? s.captasks * 2 : 1024; s.tasks = xrealloc(s.tasks, sizeof(STask) * (size_t)s.captasks); }
                STask *t = &s.tasks[s.ntasks]; memset(t, 0, sizeof *t); t->cx = hx + dx; t->cz = hz + dz; t->target = st; t->sched = -1;
                c->task = s.ntasks++;
            }
        }
    for (int ti = 0; ti < s.ntasks; ti++) submit(&s, ti);
    for (;;) {
        while (s.nev > 0 && s.heap[0].t <= s.now) { Ev e = ev_pop(&s); complete(&s, e.chunk, e.status); }
        int q = 0; while (q < NQ && s.qhead[q] < 0) q++;
        if (q == NQ) { if (s.nev == 0) break; s.now = s.heap[0].t; continue; }
        int ci = s.qhead[q]; SChunk *c = &s.ch[ci];
        s.qhead[q] = c->qnext; if (s.qhead[q] < 0) s.qtail[q] = -1;
        c->inq = 0; c->qnext = -1;
        int cnt = c->npend; int *lst = xmalloc(sizeof(int) * (size_t)cnt); memcpy(lst, c->pend, sizeof(int) * (size_t)cnt); c->npend = 0;
        for (int k = 0; k < cnt; k++) s.now += run_until_wait(&s, lst[k]);
        free(lst);
    }
    int *out = xmalloc(sizeof(int) * 2 * (size_t)(s.nord ? s.nord : 1));
    for (int i = 0; i < s.nord; i++) { int ci = s.order[i]; out[2 * i] = s.x0 + ci % s.nx; out[2 * i + 1] = s.z0 + ci / s.nx; }
    *out_xz = out; *n_out = s.nord;
    for (size_t i = 0; i < N; i++) free(s.ch[i].pend);
    for (int i = 0; i < s.ntasks; i++) free(s.tasks[i].layer);
    free(s.ch); free(s.tasks); free(s.whead); free(s.wpool); free(s.heap); free(s.order);
    return 0;
}

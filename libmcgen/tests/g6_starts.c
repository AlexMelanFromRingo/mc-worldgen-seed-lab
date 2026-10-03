/* g6_starts.c — вывод стартов построек libmcgen по чанкам (JSON, по строке на старт) для сверки с сохранёнными стартами эталонных миров
 * (tests/g6_starts.py). Использование:
 *   g6_starts <pack> <версия> <dim> <seed> <cx0> <cz0> <nx> <nz> [--sets id,id]
 * Внутренние структуры: линкуется с libmcgen.a. */
#include "structure.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 9) { fprintf(stderr, "usage: g6_starts pack version dim seed cx0 cz0 nx nz\n"); return 2; }
    McGen *g; char err[512];
    if (mcgen_open(argv[1], argv[2], &g, err, sizeof err)) { fprintf(stderr, "open: %s\n", err); return 1; }
    McSeeds sd = mcgen_seeds_unified(strtoll(argv[4], NULL, 10));
    McWorld *w;
    if (mcgen_world_new(g, argv[3], "normal", &sd, NULL, 0, &w, err, sizeof err)) { fprintf(stderr, "world: %s\n", err); return 1; }
    int cx0 = atoi(argv[5]), cz0 = atoi(argv[6]), nx = atoi(argv[7]), nz = atoi(argv[8]);
    StructWorld *sw = structures_world_get(w);
    if (!sw) { fprintf(stderr, "structures: нет\n"); return 1; }
    double t0 = now_sec();
    for (int cz = cz0; cz < cz0 + nz; cz++) for (int cx = cx0; cx < cx0 + nx; cx++) {
        StStart **st; int n = structure_starts_at(sw, cx, cz, &st);
        for (int i = 0; i < n; i++) {
            StrBuf o; memset(&o, 0, sizeof o);
            sb_printf(&o, "{\"id\":\"%s\",\"cx\":%d,\"cz\":%d,\"bb\":[%d,%d,%d,%d,%d,%d],\"pieces\":[", st[i]->def->id, cx, cz, st[i]->bb.x0, st[i]->bb.y0, st[i]->bb.z0, st[i]->bb.x1, st[i]->bb.y1, st[i]->bb.z1);
            for (int k = 0; k < st[i]->n; k++) {
                const StPiece *p = st[i]->pieces[k];
                sb_printf(&o, "%s{\"id\":\"%s\",\"bb\":[%d,%d,%d,%d,%d,%d],\"o\":%d,\"gd\":%d", k ? "," : "", p->vt->id, p->bb.x0, p->bb.y0, p->bb.z0, p->bb.x1, p->bb.y1, p->bb.z1, p->orient, p->depth);
                if (p->vt->dump) p->vt->dump(p, &o);
                sb_puts(&o, "}");
            }
            sb_puts(&o, "]}");
            puts(sb_take(&o));
        }
    }
    fprintf(stderr, "starts: %ld valid / %ld tried, %.2f s\n", sw->n_starts_valid, sw->n_starts_tried, now_sec() - t0);
    mcgen_world_free(w); mcgen_close(g);
    return 0;
}

/* schedule.c — разбор файла расписания .mcsched (см. schedule.h). */
#include "schedule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sched_free(McSchedule *s) {
    if (!s) return;
    free(s->fx); free(s->fz); free(s->fmask); free(s->px); free(s->pz); free(s);
}

McSchedule *sched_load(const char *path, char *err, size_t errlen) {
    FILE *fp = mc_fopen(path, "r");
    if (!fp) { if (err && errlen) snprintf(err, errlen, "не удалось открыть расписание %s", path); return NULL; }
    McSchedule *s = xcalloc(1, sizeof *s); s->dim_kind = -1;
    int capf = 0, capp = 0;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        int x, z; unsigned m = 0;
        if (line[0] == '#') {
            char d[32];
            if (sscanf(line, "# dim %31s", d) == 1) s->dim_kind = !strcmp(d, "overworld") ? 0 : !strcmp(d, "the_nether") ? 1 : !strcmp(d, "the_end") ? 2 : -1;
            continue;
        }
        if (line[0] == 'F' && sscanf(line + 1, "%d %d %x", &x, &z, &m) >= 2) {
            if (s->nf == capf) { capf = capf ? capf * 2 : 1024; s->fx = xrealloc(s->fx, sizeof(int) * (size_t)capf); s->fz = xrealloc(s->fz, sizeof(int) * (size_t)capf); s->fmask = xrealloc(s->fmask, sizeof(unsigned) * (size_t)capf); }
            s->fx[s->nf] = x; s->fz[s->nf] = z; s->fmask[s->nf] = m; s->nf++;
        } else if (line[0] == 'P' && sscanf(line + 1, "%d %d", &x, &z) == 2) {
            if (s->np == capp) { capp = capp ? capp * 2 : 1024; s->px = xrealloc(s->px, sizeof(int) * (size_t)capp); s->pz = xrealloc(s->pz, sizeof(int) * (size_t)capp); }
            s->px[s->np] = x; s->pz[s->np] = z; s->np++;
        }
    }
    fclose(fp);
    if (s->nf == 0 && s->np == 0) { if (err && errlen) snprintf(err, errlen, "в файле %s нет строк F/P (формат .mcsched)", path); sched_free(s); return NULL; }
    return s;
}

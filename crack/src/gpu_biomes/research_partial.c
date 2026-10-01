/* research_partial — исследовательская заметка (п.4): можно ли отсекать seed-кандидаты по «усечённому» шуму continentalness
 * (первые m низкочастотных октав, без сдвига offset), инициализируя лишь 2*m таблиц вместо 46?
 * Меряем: ошибку e = c_full - c_partial (по случайным seed'ам/точкам), доли прохождения предфильтра
 *   c_partial in [lo - M, hi + M], где [lo,hi] — оболочка диапазонов continentalness ВСЕХ листьев таблицы биомов из множества B,
 * M = выбранный квантиль |e| (эвристика, НЕ гарантия: реальные тай-брейки/границы листьев могут её нарушить).
 *   research_partial <26.1|26.2|26.3> [N seed'ов=4000] [точек на seed=8]
 */
#include "../../../engine/mc_data.h"
static u64 rs = 0x9E3779B97F4A7C15ULL;
static u64 rnd64(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs * 0x2545F4914F6CDD1DULL; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
int main(int argc, char **argv) {
    int ver = mc_version_from_name(argv[1]); int NS = argc > 2 ? atoi(argv[2]) : 4000, NP = argc > 3 ? atoi(argv[3]) : 8;
    static McVersionData V; if (mc_load_version(&V, ver)) return 1;
    McClimateSpec *S = mc_preset(&V, "overworld");
    McBiomeTree *T = mc_load_biome_tree(ver, "overworld");
    int dbl = S->mode == MC_NOISE_DOUBLE;
    static McClimate cl; static McNormal part[6];
    int M[5] = {1, 2, 3, 4, 5};
    int nsamp = NS * NP;
    double *err[5]; for (int i = 0; i < 5; i++) err[i] = malloc(sizeof(double) * nsamp);
    double *cfull = malloc(sizeof(double) * nsamp);
    double *cpart[5]; for (int i = 0; i < 5; i++) cpart[i] = malloc(sizeof(double) * nsamp);
    int *bio = malloc(sizeof(int) * nsamp);
    static McNormal tmp;
    int k = 0;
    for (int si = 0; si < NS; si++) {
        i64 seed = (i64)rnd64();
        mc_climate_init_overworld(&cl, S, seed);
        McXoro root = xoro_from_long_seed(seed); McXoroPos pf = xoro_fork_positional(&root);
        McNoiseSpec sp[5]; McNormal pn[5];
        for (int i = 0; i < 5; i++) {
            sp[i] = S->sp_cont; if (M[i] < sp[i].n_levels) sp[i].n_levels = M[i];
            McXoro r = xoro_from_hash(&pf, S->hl[3], S->hh[3]);
            normal_init_xoro(&pn[i], &sp[i], &r);
        }
        for (int p = 0; p < NP; p++, k++) {
            int qx = (int)(rnd64() % 40001) - 20000, qz = (int)(rnd64() % 40001) - 20000, qy = 16;
            int bx = qx * 4, bz = qz * 4;
            float raw[6]; mc_climate_overworld_raw(&cl, S, bx, qy * 4, bz, raw);
            McTarget t = mc_target_from_raw(raw); bio[k] = mc_biome_find(T, &t);
            double x, z;
            if (dbl) {
                double sx = normal_get_d(&cl.offset, &S->sp_offset, bx * 0.25, 0.0, bz * 0.25) * 4.0;
                double sz = normal_get_d(&cl.offset, &S->sp_offset, bz * 0.25, bx * 0.25, 0.0) * 4.0;
                x = bx * 0.25 + sx; z = bz * 0.25 + sz;
            } else {
                float sx = normal_get_f(&cl.offset, &S->sp_offset, bx * 0.25, 0.0, bz * 0.25) * 4.0f;
                float sz = normal_get_f(&cl.offset, &S->sp_offset, bz * 0.25, bx * 0.25, 0.0) * 4.0f;
                x = bx * 0.25 + (double)sx; z = bz * 0.25 + (double)sz;
            }
            double cf = normal_get(&cl.cont, &S->sp_cont, x, 0.0, z);
            cfull[k] = cf;
            for (int i = 0; i < 5; i++) {
                double cp = normal_get(&pn[i], &sp[i], bx * 0.25, 0.0, bz * 0.25);      /* без сдвига! */
                cpart[i][k] = cp; err[i][k] = fabs(cf - cp);
            }
        }
    }
    printf("версия %s (%s), %d seed'ов x %d точек = %d выборок; continentalness: уровней всего %d (таблиц 2*%d=%d из 46 у всех шумов)\n", argv[1], dbl ? "DOUBLE" : "FLOAT", NS, NP, nsamp, S->sp_cont.n_levels, S->sp_cont.n_levels, 2 * S->sp_cont.n_levels);
    printf("Ошибка |c_full - c_partial(m уровней, без сдвига)|:\n  m  таблиц  доля от 46   медиана    p99      p99.9    max\n");
    for (int i = 0; i < 5; i++) {
        double *e = malloc(sizeof(double) * nsamp); memcpy(e, err[i], sizeof(double) * nsamp); qsort(e, nsamp, sizeof(double), cmpd);
        printf("  %d  %5d   %6.1f%%     %.4f    %.4f   %.4f   %.4f\n", M[i], 2 * M[i], 100.0 * 2 * M[i] / 46, e[nsamp / 2], e[(int)(nsamp * 0.99)], e[(int)(nsamp * 0.999)], e[nsamp - 1]);
        free(e);
    }
    /* оболочки continentalness по множествам биомов */
    const char *sets[][4] = {{"deep_ocean", "deep_cold_ocean", "deep_lukewarm_ocean", "deep_frozen_ocean"}, {"ocean", "cold_ocean", "lukewarm_ocean", "warm_ocean"},
                             {"plains", "forest", NULL, NULL}, {"desert", "savanna", NULL, NULL}, {"river", NULL, NULL, NULL}, {"mushroom_fields", NULL, NULL, NULL}, {"jagged_peaks", "frozen_peaks", "stony_peaks", NULL}};
    const char *setn[] = {"глубокие океаны (4 вида)", "океаны (4 вида)", "plains|forest", "desert|savanna", "river", "mushroom_fields", "горные пики (3 вида)"};
    printf("\nПредфильтр c_partial in [lo-M, hi+M], M = p99.9 |e|. Для каждого множества B: доля seed'ов, у которых биом в случайной точке ∈ B (истина) и доля, прошедшая предфильтр;\n"
           "ожидаемое ускорение = 1/(доля_таблиц_partial + доля_прошедших)   [цена по числу перестраиваемых таблиц; сам поиск по дереву не учтён]\n");
    printf("  m  множество B                          истина P(B)   прошло     отсечено   ускорение(таблицы)   ложных отрицаний (B, не прошло)\n");
    for (int i = 0; i < 5; i++) {
        double *e = malloc(sizeof(double) * nsamp); memcpy(e, err[i], sizeof(double) * nsamp); qsort(e, nsamp, sizeof(double), cmpd);
        double Mg = e[(int)(nsamp * 0.999)]; free(e);
        for (int b = 0; b < 7; b++) {
            double lo = 1e9, hi = -1e9;
            for (int n = 0; n < T->n_nodes; n++) if (T->node[n].count == 0 && T->node[n].biome >= 0) {
                for (int q = 0; q < 4 && sets[b][q]; q++) if (T->node[n].biome == mc_biome_id(sets[b][q])) {
                    double l = T->node[n].box.lo[2] / 10000.0, h = T->node[n].box.hi[2] / 10000.0; if (l < lo) lo = l; if (h > hi) hi = h; }
            }
            int nb = 0, pass = 0, fn = 0;
            for (int s = 0; s < nsamp; s++) {
                int inB = 0; for (int q = 0; q < 4 && sets[b][q]; q++) if (bio[s] == mc_biome_id(sets[b][q])) inB = 1;
                int pp = cpart[i][s] >= lo - Mg && cpart[i][s] <= hi + Mg;
                nb += inB; pass += pp; if (inB && !pp) fn++;
            }
            double frac = 2.0 * M[i] / 46.0, pr = (double)pass / nsamp;
            printf("  %d  %-34s %8.4f   %8.4f   %8.4f   %8.2fx            %d из %d\n", M[i], setn[b], (double)nb / nsamp, pr, 1 - pr, 1.0 / (frac + pr), fn, nb);
        }
    }
    return 0;
}

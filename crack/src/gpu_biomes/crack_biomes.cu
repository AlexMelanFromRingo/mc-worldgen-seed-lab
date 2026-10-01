/* crack-biomes — проверка seed-кандидатов по наблюдаемым биомам (GPU, CUDA; режим --cpu — тот же код на CPU).
 *
 *   crack-biomes --version 26.3 --dim overworld [--preset normal|large_biomes|amplified]
 *                (--candidates FILE | --structure-seed S | --range START COUNT)
 *                --obs FILE  [--cpu | --verify] [--no-sort] [--samples N] [--max N] [--hex] [-v]
 *
 * Файл наблюдений: строки `qx qy qz biome[|biome...]` (quart = 4 блока; биом `minecraft:plains` или `plains`), `#` — комментарий.
 * Выход (stdout): прошедшие ВСЕ наблюдения seed'ы, по одному в строке (знаковые десятичные; --hex — 0x%016llx).
 * Код возврата: 0 — найден хотя бы один; 1 — нет; 2 — ошибка.
 */
#include "gpu_biomes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <numeric>
#include <unistd.h>
#include <errno.h>

static void usage() {
    fprintf(stderr,
        "crack-biomes --version 26.1|26.2|26.3 --dim overworld|nether|end [--preset normal|large_biomes|amplified]\n"
        "             (--candidates FILE | --structure-seed S | --range START COUNT) --obs FILE\n"
        "             [--blocks] [--selftest (быстрая самопроверка GPU против CPU-эталона)] [--biomes (список имён биомов)] [--cpu] [--verify] [--no-sort] [--samples N=4096] [--max N] [--hex] [-v]\n"
        "  --candidates FILE     seed'ы по одному в строке (десятичные со знаком/без знака или 0x...)\n"
        "  --structure-seed S    перебор верхних 16 бит: seed = S | (hi << 48), hi = 0..65535 (S — 48 бит)\n"
        "  --range START COUNT   seed = START + i, i < COUNT\n"
        "  --obs FILE            строки `qx qy qz biome[|biome...]` (кварты)\n"
        "  --blocks              координаты в файле наблюдений — БЛОКОВЫЕ (как в F3): биом берётся как в игре (BiomeManager.getBiome,\n"
        "                        «размытие» по hashed seed = SHA-256(seed)); без флага — шумовой биом ячейки кварта (4 блока)\n"
        "  --ties / --no-ties    (по умолчанию --ties) при равенстве fitness нескольких листьев R-дерева игра выбирает биом недетерминированно\n"
        "                        (~0.1%% точек Overworld); с --ties наблюдение выполнено, если ЛЮБОЙ из равных биомов в маске\n"
        "  --cpu                 выполнить на CPU (тот же код, OpenMP) вместо GPU\n"
        "  --verify              выполнить и на GPU, и на CPU и сравнить результаты\n"
        "  --no-sort             не упорядочивать наблюдения по селективности\n");
}

static bool parse_i64(const char *s, i64 *out) {
    char *e; errno = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return false;
    if (s[0] == '-') { long long v = strtoll(s, &e, 0); if (e == s) return false; *out = (i64)v; return true; }
    unsigned long long v = strtoull(s, &e, 0); if (e == s) return false;
    *out = (i64)v; return true;
}

int main(int argc, char **argv) {
    const char *sver = nullptr, *sdim = nullptr, *spreset = "normal", *cand = nullptr, *obsf = nullptr;
    bool have_ss = false; i64 ss = 0; bool have_range = false; i64 rstart = 0; u64 rcount = 0;
    bool selftest = false, cpu = false, verify = false, nosort = false, hex = false, verbose = false, blocks = false, ties = true; int nsamp = 4096; u32 maxfound = 1 << 20;
    for (int i = 1; i < argc; i++) {
        auto need = [&](int k) { if (i + k >= argc) { usage(); exit(2); } };
        if (!strcmp(argv[i], "--version")) { need(1); sver = argv[++i]; }
        else if (!strcmp(argv[i], "--dim")) { need(1); sdim = argv[++i]; }
        else if (!strcmp(argv[i], "--preset")) { need(1); spreset = argv[++i]; }
        else if (!strcmp(argv[i], "--candidates")) { need(1); cand = argv[++i]; }
        else if (!strcmp(argv[i], "--structure-seed")) { need(1); have_ss = parse_i64(argv[++i], &ss); if (!have_ss) { fprintf(stderr, "плохой seed\n"); return 2; } }
        else if (!strcmp(argv[i], "--range")) { need(2); if (!parse_i64(argv[++i], &rstart)) return 2; rcount = strtoull(argv[++i], nullptr, 0); have_range = true; }
        else if (!strcmp(argv[i], "--obs")) { need(1); obsf = argv[++i]; }
        else if (!strcmp(argv[i], "--cpu")) cpu = true;
        else if (!strcmp(argv[i], "--verify")) verify = true;
        else if (!strcmp(argv[i], "--no-sort")) nosort = true;
        else if (!strcmp(argv[i], "--samples")) { need(1); nsamp = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--max")) { need(1); maxfound = (u32)strtoul(argv[++i], nullptr, 0); }
        else if (!strcmp(argv[i], "--hex")) hex = true;
        else if (!strcmp(argv[i], "--blocks")) blocks = true;
        else if (!strcmp(argv[i], "--no-ties")) ties = false;
        else if (!strcmp(argv[i], "--ties")) ties = true;
        else if (!strcmp(argv[i], "-v")) { verbose = true; gb_progress = true; }
        else if (!strcmp(argv[i], "--selftest")) selftest = true;
        else if (!strcmp(argv[i], "--biomes")) { for (int b = 0; b < mcg_biome_count(); b++) printf("minecraft:%s\n", mcg_biome_name(b)); return 0; }
        else { usage(); return 2; }
    }
    if (selftest && sver && sdim) {}
    else if (!sver || !sdim || !obsf || (!cand && !have_ss && !have_range)) { usage(); return 2; }
    int ver = mcg_version_from_name(sver);
    if (ver < 0) { fprintf(stderr, "версия: 26.1|26.2|26.3\n"); return 2; }
    int dim = !strcmp(sdim, "overworld") || !strcmp(sdim, "ow") ? MC_OVERWORLD : !strcmp(sdim, "nether") ? MC_NETHER : !strcmp(sdim, "end") ? MC_END : -1;
    if (dim < 0) { fprintf(stderr, "измерение: overworld|nether|end\n"); return 2; }
    if (!getenv("MCGEN_ROOT")) {   /* корень проекта: от crack/bin/ */
        static char root[1024]; ssize_t n = readlink("/proc/self/exe", root, sizeof root - 1);
        if (n > 0) { root[n] = 0; char *p = strrchr(root, '/'); if (p) *p = 0; p = strrchr(root, '/'); if (p) *p = 0; p = strrchr(root, '/'); if (p) *p = 0; setenv("MCGEN_ROOT", root, 1); }
    }

    GbCtx c;
    if (gb_open(c, ver, dim, spreset, !cpu)) return 2;
    gb_set_ties(c, ties);
    if (selftest) {      /* быстрая самопроверка: GPU-ядро против CPU-эталона engine/ (бит-в-бит, биомы и климат) на 16 seed'ах x 64 точках */
        int ns = 16, np = 64; std::vector<i64> sd(ns); std::vector<i32> pt(3 * np);
        u64 x = 0x9E3779B97F4A7C15ULL; auto r = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x * 0x2545F4914F6CDD1DULL; };
        for (auto &v : sd) v = (i64)r();
        for (int p = 0; p < np; p++) { int R = dim == MC_END ? 50000 : 20000; pt[3 * p] = (int)(r() % (2 * R + 1)) - R; pt[3 * p + 2] = (int)(r() % (2 * R + 1)) - R; pt[3 * p + 1] = dim == MC_OVERWORLD ? (int)(r() % 97) - 16 : (dim == MC_NETHER ? (int)(r() % 33) : 16); }
        std::vector<u8> bg((size_t)ns * np), br((size_t)ns * np); std::vector<i32> tg((size_t)ns * np * 6), tr((size_t)ns * np * 6);
        if (cpu) gb_points_cpu(c, sd.data(), ns, pt.data(), np, bg.data(), tg.data()); else gb_points_gpu(c, sd.data(), ns, pt.data(), np, bg.data(), tg.data(), 8);
        mcg_ref_points(&c.H, sd.data(), ns, pt.data(), np, br.data(), tr.data());
        size_t bad = 0; for (size_t i = 0; i < bg.size(); i++) if (bg[i] != br[i] || memcmp(&tg[i * 6], &tr[i * 6], 24)) bad++;
        fprintf(stderr, "selftest %s %s %s (%s): %zu точек, расхождений с CPU-эталоном engine/: %zu\n", sver, sdim, c.preset, cpu ? "CPU-тот-же-код" : "GPU", bg.size(), bad);
        return bad ? 2 : 0;
    }

    /* наблюдения */
    std::vector<McgObs> obs;
    {
        FILE *f = fopen(obsf, "r"); if (!f) { perror(obsf); return 2; }
        char line[4096]; int ln = 0;
        while (fgets(line, sizeof line, f)) {
            ln++; char *p = line; while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
            McgObs o; std::string err;
            if (!gb_parse_obs_line(p, o, err, blocks)) { fprintf(stderr, "%s:%d: %s\n", obsf, ln, err.c_str()); return 2; }
            obs.push_back(o);
        }
        fclose(f);
    }
    if (obs.empty()) { fprintf(stderr, "нет наблюдений\n"); return 2; }
    if ((int)obs.size() > MCG_MAX_OBS) { fprintf(stderr, "слишком много наблюдений (макс %d)\n", MCG_MAX_OBS); return 2; }
    gb_finalize_obs(c, obs);

    /* источник seed'ов */
    std::vector<i64> list; GbSource src;
    if (cand) {
        FILE *f = fopen(cand, "r"); if (!f) { perror(cand); return 2; }
        char line[256];
        while (fgets(line, sizeof line, f)) { char *p = line; while (*p == ' ') p++; if (*p == '#' || *p == '\n' || !*p) continue; i64 v; if (parse_i64(p, &v)) list.push_back(v); }
        fclose(f);
        src.kind = 0; src.list = list.data(); src.n = list.size();
    } else if (have_ss) {
        if ((u64)ss >> 48) fprintf(stderr, "предупреждение: --structure-seed шире 48 бит, старшие биты будут перезаписаны перебором\n");
        src.kind = 1; src.base = (u64)ss & ((1ULL << 48) - 1);
        if (dim != MC_OVERWORLD && blocks)
            fprintf(stderr, "note: в Nether/End шумовые биомы не зависят от верхних 16 бит seed; с --blocks они влияют лишь через зум (hashed seed) — биомы почти не различают 2^16 вариантов (тест, 16 наблюдений: медианно проходит 83–100%% вариантов, у отдельных seed — от 8%%)\n");
        if (dim != MC_OVERWORLD && !blocks) {
            /* Nether/End (шум) зависят только от младших 48 бит seed: 2^16 значений hi эквивалентны => достаточно hi = 0.
             * В режиме --blocks это не так: «размытие» BiomeManager берёт hashed seed = SHA-256(ВСЕ 64 бита) => перебор hi нужен. */
            fprintf(stderr, "note: %s зависит только от младших 48 бит seed — верхние 16 бит неразличимы; проверяется один seed (hi=0)\n", sdim);
            src.kind = 2; src.n = 1;
        }
    } else { src.kind = 2; src.base = (u64)rstart; src.n = rcount; }

    /* упорядочение по селективности */
    std::vector<double> p(obs.size(), 1.0);
    if (!nosort && obs.size() > 1) {
        gb_estimate_selectivity(c, obs, nsamp, p);
        std::vector<int> ord(obs.size()); std::iota(ord.begin(), ord.end(), 0);
        std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return p[a] < p[b]; });
        std::vector<McgObs> o2; std::vector<double> p2;
        for (int k : ord) { o2.push_back(obs[k]); p2.push_back(p[k]); }
        obs = o2; p = p2;
    } else if (obs.size() == 1) gb_estimate_selectivity(c, obs, nsamp, p);
    else { std::vector<McgObs> tmp = obs; gb_estimate_selectivity(c, tmp, nsamp, p); }
    double pall = 1.0; for (double x : p) pall *= x;
    if (verbose) {
        fprintf(stderr, "версия %s, измерение %s, пресет %s, режим %s; наблюдений %zu\n", mcg_version_name(ver), sdim, c.preset, c.mode == MC_NOISE_FLOAT ? "FLOAT" : "DOUBLE", obs.size());
        for (size_t j = 0; j < obs.size(); j++) fprintf(stderr, "  obs[%zu] q=(%d,%d,%d) P(случайный seed)≈%.4f %s%s\n", j, obs[j].qx, obs[j].qy, obs[j].qz, p[j],
                                                        gb_mask_str(obs[j]).c_str(), dim == MC_END ? (std::string(" [окно ±") + std::to_string(obs[j].rad & 0xFF) + "]").c_str() : "");
    }

    GbSearch res, res2; int rc;
    if (!cpu) {
        rc = gb_search(c, true, src, obs, res, maxfound);
        if (verify) { rc |= gb_search(c, false, src, obs, res2, maxfound); }
    } else rc = gb_search(c, false, src, obs, res, maxfound);
    if (rc) return 2;
    if (verify) {
        bool same = res.found == res2.found && res.reached == res2.reached;
        fprintf(stderr, "verify GPU vs CPU: %s (GPU %zu, CPU %zu найденных; GPU %.1f мс, CPU %.1f мс)\n", same ? "СОВПАДАЕТ" : "РАСХОЖДЕНИЕ!", res.found.size(), res2.found.size(), res.ms, res2.ms);
        if (!same) return 2;
    }
    for (i64 s : res.found) { if (hex) printf("0x%016llx\n", (unsigned long long)s); else printf("%lld\n", (long long)s); }
    double exp_fp = pall * (double)res.total;
    fprintf(stderr, "кандидатов %llu, прошли все %zu; ожидание ложных по оценке селективностей ≈ %.3g; %s %.2f мс (%.3g кандидатов/с)\n",
            (unsigned long long)res.total, res.found.size(), exp_fp, cpu ? "CPU" : "GPU", res.ms, res.ms > 0 ? res.total / (res.ms / 1e3) : 0.0);
    if (verbose) {
        fprintf(stderr, "прошли ровно j наблюдений:");
        for (size_t j = 0; j < res.reached.size(); j++) fprintf(stderr, " [%zu]=%llu", j, (unsigned long long)res.reached[j]);
        fprintf(stderr, "\n");
    }
    return res.found.empty() ? 1 : 0;
}

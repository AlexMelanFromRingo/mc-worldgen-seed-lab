/* test_gpu_biomes.cu — тест (a): ядро GPU (и тот же код на CPU) против CPU-эталона engine/ БИТ-В-БИТ.
 *   gpu-biomes-test [--versions 26.1 26.2 26.3] [--seeds N=2048] [--points M=512] [--dims ow,ow_large,ow_amp,nether,end] [--cpu-same]
 * Для каждой (версия, измерение/пресет): N seed'ов x M точек; сравниваются id биома и (Overworld/Nether) квантованные t,h,c,e,d,w.
 * Плюс тест решений search-ядра: по случайным маскам (одно наблюдение) GPU-решение == решение по эталонному биому.
 */
#include "gpu_biomes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>
#include <string>

static u64 rs = 0x243F6A8885A308D3ULL;
static u64 rnd64() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs * 0x2545F4914F6CDD1DULL; }
static i32 rrange(i32 lo, i32 hi) { return lo + (i32)(rnd64() % (u64)((i64)hi - lo + 1)); }

static const i64 FIXED[] = {0, 1, -1, 12345, 8675309, 9223372036854775807LL, (i64)0x8000000000000000ULL, 1234567890123456789LL, -4172144997902289642LL};

static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Cfg { const char *name; int dim; const char *preset; };

int main(int argc, char **argv) {
    int ns = 2048, np = 512; std::vector<int> vers = {MC_26_1, MC_26_2, MC_26_3};
    std::string dims = "ow,ow_large,ow_amp,nether,end"; bool cpu_same = false; int end_big = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seeds")) ns = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--points")) np = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dims")) dims = argv[++i];
        else if (!strcmp(argv[i], "--cpu-same")) cpu_same = true;
        else if (!strcmp(argv[i], "--no-end-big")) end_big = 0;
        else if (!strcmp(argv[i], "--versions")) {
            vers.clear();
            while (i + 1 < argc && argv[i + 1][0] != '-') vers.push_back(mcg_version_from_name(argv[++i]));
        }
    }
    std::vector<Cfg> cfgs;
    auto has = [&](const char *t) { std::string d = "," + dims + ","; return d.find(std::string(",") + t + ",") != std::string::npos; };
    if (has("ow")) cfgs.push_back({"overworld/normal", MC_OVERWORLD, "normal"});
    if (has("ow_large")) cfgs.push_back({"overworld/large_biomes", MC_OVERWORLD, "large_biomes"});
    if (has("ow_amp")) cfgs.push_back({"overworld/amplified", MC_OVERWORLD, "amplified"});
    if (has("nether")) cfgs.push_back({"nether", MC_NETHER, "-"});
    if (has("end")) cfgs.push_back({"end", MC_END, "-"});

    u64 tot_pts = 0, tot_bad = 0, tot_dec = 0, tot_dec_bad = 0;
    printf("%-8s %-24s %10s %8s %8s %10s %10s %10s\n", "версия", "конфиг", "точек", "≠биом", "≠клим", "GPU мс", "эталон мс", "решений≠");
    for (int ver : vers) for (const Cfg &cf : cfgs) {
        GbCtx c;
        if (gb_open(c, ver, cf.dim, cf.preset)) { fprintf(stderr, "не удалось открыть %s\n", cf.name); return 2; }
        /* seed'ы */
        std::vector<i64> seeds;
        for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]) && (int)seeds.size() < ns; i++) seeds.push_back(FIXED[i]);
        while ((int)seeds.size() < ns) seeds.push_back((i64)rnd64());
        /* точки */
        std::vector<i32> pts(3 * np);
        for (int p = 0; p < np; p++) {
            int sc = p & 3; int R;
            if (cf.dim == MC_END) { static const int RR[4] = {300, 3000, 40000, 200000}; R = RR[sc]; if (!end_big && sc == 3) R = 40000; }
            else { static const int RR[4] = {64, 2000, 100000, 7500000}; R = RR[sc]; }
            pts[3 * p] = rrange(-R, R); pts[3 * p + 2] = rrange(-R, R);
            pts[3 * p + 1] = cf.dim == MC_OVERWORLD ? ((p & 7) == 7 ? rrange(-200, 200) : rrange(-16, 80)) : (cf.dim == MC_NETHER ? ((p & 7) == 7 ? rrange(-100, 100) : rrange(0, 32)) : 16);
        }
        size_t n = (size_t)ns * np;
        std::vector<u8> bg(n), br(n), bc; std::vector<i32> tg(n * 6), tr(n * 6), tc;
        double t0 = now_ms();
        double gms = gb_points_gpu(c, seeds.data(), ns, pts.data(), np, bg.data(), tg.data(), 64);
        double t1 = now_ms();
        mcg_ref_points(&c.H, seeds.data(), ns, pts.data(), np, br.data(), tr.data());
        double t2 = now_ms();
        u64 bad = 0, badt = 0;
        for (size_t i = 0; i < n; i++) {
            if (bg[i] != br[i]) { if (bad < 4) printf("  ≠биом seed#%zu pt#%zu: gpu=%s ref=%s\n", i / np, i % np, mcg_biome_name(bg[i]), mcg_biome_name(br[i])); bad++; }
            if (memcmp(&tg[i * 6], &tr[i * 6], 24)) { if (badt < 4) printf("  ≠климат seed#%zu pt#%zu\n", i / np, i % np); badt++; }
        }
        u64 badc = 0;
        if (cpu_same) {
            bc.resize(n); tc.resize(n * 6);
            gb_points_cpu(c, seeds.data(), ns, pts.data(), np, bc.data(), tc.data());
            for (size_t i = 0; i < n; i++) if (bc[i] != br[i] || memcmp(&tc[i * 6], &tr[i * 6], 24)) badc++;
            if (badc) printf("  CPU-тот-же-код ≠ эталон: %llu\n", (unsigned long long)badc);
        }
        /* тест решений search-ядра: одна маска -> GPU found == {seed: эталон-биом в маске} */
        u64 dec = 0, decbad = 0;
        int nb = mcg_biome_count();
        std::vector<int> pool;     /* биомы, реально встречающиеся в эталоне для этой конфигурации */
        { std::vector<char> seen(nb, 0); for (size_t i = 0; i < n; i++) seen[br[i]] = 1; for (int b = 0; b < nb; b++) if (seen[b]) pool.push_back(b); }
        int nobs_test = 48; if (nobs_test > np) nobs_test = np;
        for (int j = 0; j < nobs_test; j++) {
            int p = (int)(rnd64() % (u64)np);
            McgObs o; memset(&o, 0, sizeof o); o.qx = pts[3 * p]; o.qy = pts[3 * p + 1]; o.qz = pts[3 * p + 2];
            int s0 = (int)(rnd64() % (u64)ns); int b0 = br[(size_t)s0 * np + p];
            int extra = (int)(rnd64() % 3); bool excl = (rnd64() % 4) == 0;
            if (!excl) o.mask[b0 >> 6] |= 1ULL << (b0 & 63);
            for (int e = 0; e < extra + (excl ? 1 : 0); e++) { int b = pool[rnd64() % pool.size()]; if (excl && b == b0) continue; o.mask[b >> 6] |= 1ULL << (b & 63); }
            if (!o.mask[0] && !o.mask[1]) o.mask[b0 >> 6] |= 1ULL << (b0 & 63);
            std::vector<McgObs> ov = {o}; gb_finalize_obs(c, ov);
            GbSource src; src.kind = 0; src.list = seeds.data(); src.n = ns;
            GbSearch sr; gb_search(c, true, src, ov, sr);
            std::vector<char> pass(ns, 0);
            for (i64 s : sr.found) for (int k = 0; k < ns; k++) if (seeds[k] == s) pass[k] = 1;   /* seed'ы не обязательно уникальны */
            for (int k = 0; k < ns; k++) {
                int want = mcg_in_mask(&ov[0], br[(size_t)k * np + p]);
                /* дубликаты seed'ов дают одно решение — ok */
                if ((int)pass[k] != want) { if (decbad < 4) printf("  ≠решение obs#%d seed#%d: gpu=%d ref=%d\n", j, k, pass[k], want); decbad++; }
                dec++;
            }
            /* CPU-режим того же кода */
            GbSearch sc; gb_search(c, false, src, ov, sc);
            if (sc.found != sr.found) { printf("  CPU/GPU search found различаются obs#%d: %zu vs %zu\n", j, sc.found.size(), sr.found.size()); decbad++; }
        }
        /* тест режима ties (Overworld/Nether): (1) mcg_rt_ties == перебор всех листьев; (2) решение GPU при ties on/off */
        u64 tiebad = 0, nties = 0, tiechk = 0;
        if (cf.dim != MC_END) {
            std::vector<u64> tm(n * 2);
            gb_ties_host(c, tg.data(), (int)n, tm.data());
            std::vector<size_t> tie_idx;
            for (size_t i = 0; i < n; i++) if (__builtin_popcountll(tm[2 * i]) + __builtin_popcountll(tm[2 * i + 1]) > 1) tie_idx.push_back(i);
            nties = tie_idx.size();
            size_t lim = std::min<size_t>(tie_idx.size(), 400);
            for (size_t t = 0; t < lim; t++) {
                size_t i = tie_idx[t];
                u64 ref[2]; mcg_ref_tie_mask(&c.H, &tg[i * 6], ref);
                if (ref[0] != tm[2 * i] || ref[1] != tm[2 * i + 1]) { if (tiebad < 4) printf("  ≠tie-маска seed#%zu pt#%zu\n", i / np, i % np); tiebad++; }
                /* GPU: маска = тай-набор без результата обычного поиска => ties on: проходит, ties off: нет; маска = {вне тай-набора} => не проходит */
                int b0 = br[i]; McgObs o; memset(&o, 0, sizeof o);
                int p = (int)(i % np); size_t s_i = i / np;
                o.qx = pts[3 * p]; o.qy = pts[3 * p + 1]; o.qz = pts[3 * p + 2];
                o.mask[0] = tm[2 * i]; o.mask[1] = tm[2 * i + 1];
                o.mask[b0 >> 6] &= ~(1ULL << (b0 & 63));
                std::vector<McgObs> ov = {o}; gb_finalize_obs(c, ov);
                i64 sd = seeds[s_i]; GbSource src; src.kind = 0; src.list = &sd; src.n = 1;
                GbSearch r_on, r_off, r_cpu;
                gb_set_ties(c, true);  gb_search(c, true, src, ov, r_on); gb_search(c, false, src, ov, r_cpu);
                gb_set_ties(c, false); gb_search(c, true, src, ov, r_off);
                if (r_on.found.size() != 1 || r_off.found.size() != 0 || r_cpu.found.size() != 1) { if (tiebad < 4) printf("  ≠tie-решение seed#%zu pt#%zu on=%zu off=%zu cpu=%zu\n", s_i, (size_t)p, r_on.found.size(), r_off.found.size(), r_cpu.found.size()); tiebad++; }
                tiechk++;
            }
            gb_set_ties(c, false);
        }
        if (nties || tiebad) printf("  ties: точек с тай-набором >1 биома: %llu из %zu; проверено %llu, расхождений %llu\n", (unsigned long long)nties, n, (unsigned long long)tiechk, (unsigned long long)tiebad);
        decbad += tiebad;
        printf("%-8s %-24s %10zu %8llu %8llu %10.1f %10.1f %10llu%s\n", mcg_version_name(ver), cf.name, n, (unsigned long long)bad, (unsigned long long)badt,
               gms, t2 - t1, (unsigned long long)decbad, (bad || badt || badc || decbad) ? "   <-- РАСХОЖДЕНИЕ" : "");
        (void)t0;
        tot_pts += n; tot_bad += bad + badt + badc; tot_dec += dec; tot_dec_bad += decbad;
        gb_close(c);
    }
    printf("ИТОГО: точек %llu, расхождений %llu; решений search %llu, расхождений %llu\n", (unsigned long long)tot_pts, (unsigned long long)tot_bad,
           (unsigned long long)tot_dec, (unsigned long long)tot_dec_bad);
    return (tot_bad || tot_dec_bad) ? 1 : 0;
}

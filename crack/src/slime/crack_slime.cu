/*
 * crack-slime — восстановление 48-битного structure seed по слайм-чанкам (Minecraft 26.x, все версии 26.1-26.3 одинаково).
 *
 * Слайм-чанк:  WorldgenRandom.seedSlimeChunk(x,z,seed,987234911L).nextInt(10) == 0      (WorldgenRandom.java:71-73; Slime.java)
 *   состояние LCG:  s0 = (((W + c(x,z)) mod 2^48) ^ salt) ^ 0x5DEECE66D,   W = seed mod 2^48
 *   nextInt(10) == 0  <=>  r = next(31) = (s0*M+B mod 2^48) >> 17  делится на 10 (и не отклонено: r < 2147483640)
 * Алгоритм (lifting по чётности):
 *   1) r чётно  <=>  бит 17 состояния s1 == 0; он зависит ТОЛЬКО от младших 18 бит W  => для каждого положительного
 *      чанка отсекаем половину из 2^18 вариантов младших бит (допуск ошибок: не более E нарушений).
 *   2) для каждого выжившего низа (в среднем 2^18 / 2^N при N положительных) перебираем 2^30 старших бит,
 *      проверяя r % 10 == 0 с ранним выходом (GPU: CUDA, CPU-fallback: OpenMP).
 *   3) полная проверка всех наблюдений (положительные и отрицательные) с допуском E ошибок.
 *
 * Сборка: см. crack/Makefile.  Запуск: crack-slime [опции] файл_наблюдений     (формат: "chunkX chunkZ slime|noslime")
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cmath>
#include <unistd.h>
#include "../common/mc.h"
#include "../common/util.h"
#include "../common/structdb.h"   /* random_world_seeds() */

#define MAXOBS (1 << 20)
#define RESCAP (1u << 22)
#define HBITS 30
#define LBITS 18
#define SEGBITS 6            /* каждый поток обрабатывает подряд 2^6 значений h при фиксированном низе */

struct SObs { i32 x, z; int pos; };

/* r = next(31) после setSeed для чанка с константой cst, W = (Whi:16 | Wlo:32); всё в 32-битной арифметике */
#define KXLO ((u32)(SLIME_KX))
#define KXHI ((u32)(SLIME_KX >> 32))
#define MLO  ((u32)(LCG_MUL))
#define MHI  ((u32)(LCG_MUL >> 32))
HD u32 slime_r(u32 Wlo, u32 Whi, u64 cst) {
    u32 clo = (u32)cst, chi = (u32)(cst >> 32);
    u32 slo = Wlo + clo;
    u32 shi = (Whi + chi + (slo < Wlo ? 1u : 0u)) & 0xFFFFu;
    slo ^= KXLO; shi ^= KXHI;
    u64 p = (u64)slo * MLO + LCG_ADD;                     /* s1 = s0*M + B  (mod 2^48) */
    u32 plo = (u32)p;
    u32 phi = ((u32)(p >> 32) + shi * MLO + slo * MHI) & 0xFFFFu;
    return (phi << 15) | (plo >> 17);                     /* биты 17..47 состояния = next(31) */
}
HD int slime32(u32 Wlo, u32 Whi, u64 cst) {
    u32 r = slime_r(Wlo, Whi, cst);
    if (r < 2147483640u) return (r % 10u) == 0;          /* без отклонения */
    return slime_from_cst(((u64)Whi << 32) | Wlo, cst);   /* редкий путь: точная семантика nextInt(10) с отклонением */
}

/* полная проверка: не более maxerr несовпадений; cst[0..npos) — положительные, cst[npos..npos+nneg) — отрицательные */
HD bool full_check(u64 W, const u64 *cst, int npos, int nneg, int maxerr) {
    u32 Wlo = (u32)W, Whi = (u32)(W >> 32);
    int err = 0;
    for (int k = 0; k < npos; k++) if (!slime32(Wlo, Whi, cst[k])) { if (++err > maxerr) return false; }
    for (int k = npos; k < npos + nneg; k++) if (slime32(Wlo, Whi, cst[k])) { if (++err > maxerr) return false; }
    return true;
}
HD bool extra_check(u64 W, const Pred *pr, int np) {
    for (int k = 0; k < np; k++) if (!pred_check(W, pr[k])) return false;
    return true;
}

/* ---------- АНАЛИТИЧЕСКИЙ режим (по умолчанию при --max-errors 0): lifting 35 бит + решение старших 13 бит в замкнутом виде ----------
 * W = l + (m<<18) + (g<<35), l (18 бит) — из lifting по чётности, m (17 бит) перебирается, g (13 бит) находится аналитически.
 * Для чанка k (c = c_k mod 2^48 = cl + (ch<<35), cl < 2^35):  u = W + c,  s0 = u ^ KX, KX < 2^35 => XOR трогает только младшие 35 бит u.
 * Пусть v = l + (m<<18) + cl, carry = v>>35, A = ((v mod 2^35 ^ KX)*M + B) mod 2^48, a = A>>17, e = ch + carry, d = e*M mod 2^13, T = g*M mod 2^13.
 * Тогда r_k = (a + 2^18*((T+d) mod 2^13)) mod 2^31 = (b_k + 2^18*T) mod 2^31, b_k = a + 2^18*d: т.е. r_k = rho_k + 2^18*j_k,
 * rho_k = b_k mod 2^18, j_k = (T + beta_k) mod 2^13, beta_k = b_k>>18 (старшие 13 бит r). Условие r_k % 10 == 0 (rho чётно) <=> j_k == lam_k (mod 5), lam_k = rho_k % 5.
 * Для разных чанков j_k = (j_1 + D_k) mod 8192, D_k = beta_k - beta_1; так как 8192 = 2 (mod 5), условие превращается в: q = (lam_1 + D_k - lam_k) mod 5 in {0, 2};
 * q=0 => j_1 < 8192 - D_k, q=2 => j_1 >= 8192 - D_k; иначе пары (l,m) нет решений. В итоге j_1 лежит в интервале [lo,hi) и j_1 == lam_1 (mod 5).
 * Работа на пару (l,m): O(число проверенных чанков) (~2 в среднем) вместо 2^13 значений g. Кандидаты затем проверяются точно (отрицательные чанки, предикаты). */
#define MASK35 ((1ULL << 35) - 1)
#ifdef __CUDACC__
#pragma nv_exec_check_disable
#endif
template <class Emit>
HD void pair_search(u64 l, u32 m, const u64 *cst, int npos, int nneg, const Pred *pr, int np, u32 minv13, Emit &emit) {
    const u64 base = l | ((u64)m << 18);
    u32 lam1 = 0, beta1 = 0, lo = 0, hi = 8192;
    bool slow = false;
    for (int k = 0; k < npos; k++) {
        u64 c = cst[k];
        u64 v = base + (c & MASK35);
        u32 carry = (u32)(v >> 35);
        u64 x = (v & MASK35) ^ SLIME_KX;
        u32 xl = (u32)x, xh = (u32)(x >> 32);
        u64 p = (u64)xl * MLO + LCG_ADD;
        u32 plo = (u32)p;
        u32 phi = ((u32)(p >> 32) + xh * MLO + xl * MHI) & 0xFFFFu;
        u32 a = (phi << 15) | (plo >> 17);
        u32 e = ((u32)(c >> 35) + carry) & 0x1FFFu;
        u32 d = (e * MLO) & 0x1FFFu;
        u32 b = (a + (d << 18)) & 0x7FFFFFFFu;
        u32 rho = b & 0x3FFFFu, be = b >> 18;
        if (rho & 1u) return;                                    /* r нечётно: не слайм-чанк */
        if (rho >= 0x3FFF8u) { slow = true; break; }             /* r может попасть в зону отклонения nextInt(10): точный путь */
        u32 lam = rho % 5u;
        if (k == 0) { lam1 = lam; beta1 = be; }
        else {
            u32 D = (be - beta1) & 0x1FFFu;
            u32 q = (lam1 + D + 5u - lam) % 5u;
            if (q == 0) { u32 t = 8192u - D; if (t < hi) hi = t; }
            else if (q == 2) { u32 t = 8192u - D; if (t > lo) lo = t; }
            else return;
            if (lo >= hi) return;
        }
    }
    if (slow) {
        for (u32 g = 0; g < 8192; g++) { u64 W = base | ((u64)g << 35); if (full_check(W, cst, npos, nneg, 0) && extra_check(W, pr, np)) emit(W); }
        return;
    }
    u32 j = lo + ((lam1 + 5u - lo % 5u) % 5u);
    for (; j < hi; j += 5) {
        u32 T = (j - beta1) & 0x1FFFu;
        u32 g = (T * minv13) & 0x1FFFu;
        u64 W = base | ((u64)g << 35);
        if (full_check(W, cst, npos, nneg, 0) && extra_check(W, pr, np)) emit(W);
    }
}

#ifndef NO_CUDA
struct GpuEmit {
    u64 *res; unsigned long long *nres; unsigned cap;
    __device__ void operator()(u64 W) { unsigned long long p = atomicAdd(nres, 1ULL); if (p < cap) res[p] = W; }
};
__global__ void k_pairs(const u64 *__restrict__ lows, const u64 *__restrict__ cst, int npos, int nneg, const Pred *__restrict__ preds, int np, u32 minv13,
                        u64 start, u64 count, u64 *res, unsigned long long *nres, unsigned cap) {
    u64 stride = (u64)gridDim.x * blockDim.x, end = start + count;
    GpuEmit em{res, nres, cap};
    for (u64 i = start + (u64)blockIdx.x * blockDim.x + threadIdx.x; i < end; i += stride)
        pair_search(lows[i >> 17], (u32)(i & 0x1FFFFu), cst, npos, nneg, preds, np, minv13, em);
}
__global__ void k_slime(const u64 *__restrict__ lows, const u64 *__restrict__ cst, int npos, int nneg, int maxerr,
                        const Pred *__restrict__ preds, int np, u64 seg0, u64 nseg, u64 *res, unsigned long long *nres, unsigned cap) {
    u64 stride = (u64)gridDim.x * blockDim.x;
    u64 end = seg0 + nseg;
    for (u64 sg = seg0 + (u64)blockIdx.x * blockDim.x + threadIdx.x; sg < end; sg += stride) {
        u64 l = lows[sg >> (HBITS - SEGBITS)];
        u64 h0 = (sg & ((1ULL << (HBITS - SEGBITS)) - 1)) << SEGBITS;
        for (int j = 0; j < (1 << SEGBITS); j++) {
            u64 W = ((h0 + j) << LBITS) | l;
            if (full_check(W, cst, npos, nneg, maxerr) && extra_check(W, preds, np)) {
                unsigned long long p = atomicAdd(nres, 1ULL);
                if (p < cap) res[p] = W;
            }
        }
    }
}
#endif

static void usage() {
    fprintf(stderr,
"crack-slime — восстановление 48-битного seed по слайм-чанкам (26.1/26.2/26.3)\n"
"Использование: crack-slime [опции] <файл|->\n"
"  Файл: строки \"chunkX chunkZ slime|noslime\" (также 1/0, s/n, +/-; '#' — комментарий; разделители пробел ; ,)\n"
"Опции:\n"
"  --blocks             координаты в файле — блочные (x z в блоках; чанк = floor(x/16)), а не чанковые\n"
"  --mode pos|mixed     pos: отрицательные наблюдения игнорируются; mixed (по умолчанию): используются все\n"
"  --max-errors N       допустимое число неверных наблюдений (по умолчанию 0)\n"
"  --dev auto|gpu|cpu   устройство (по умолчанию auto: GPU если есть, иначе CPU/OpenMP)\n"
"  --limit N            максимум выводимых seed (по умолчанию 1000000); счётчик полный\n"
"  --int32              оставить только seed, представимые 32-битным целым (набранное число < 2^31 по модулю, String.hashCode текстового seed):\n"
"                       биты 32..47 все 0 или все 1 — почти полностью убирает ложные кандидаты, если мир создан с таким seed\n"
"  --expand-random      для каждого seed вывести 64-бит seed вида RandomSource.create().nextLong() (автосид мира)\n"
"  --out FILE           дополнительно записать seed в файл\n"
"  --threads N          число потоков OpenMP для CPU\n"
"  --with-structs FILE  дополнительные наблюдения (структуры `<structure_set>;cx;cz`, `slime;cx;cz;0|1`) — фильтр на кандидатах;\n"
"                       параметры placement берутся из data/structure_sets-<версия>.json (--version, --data-dir, --sets)\n"
"  --version V          26.1|26.2|26.3 (по умолчанию 26.3); для слайм-чанков формула одинакова во всех версиях\n"
"  --selftest           внутренняя самопроверка 32-битного ядра против точной реализации; выход\n"
"  --gen S,NPOS,NNEG,R[,RNG]  синтетические наблюдения из известного seed S: NPOS положительных и NNEG отрицательных\n"
"                       чанков из окна [-R,R)^2 (RNG — затравка выбора); файл не нужен; --gen-out FILE сохранить\n"
"  --print-map S CX0 CZ0 NX NZ   напечатать карту слайм-чанков в формате oracle (rows[iz][ix]) и выйти\n"
"  --suggest K          при >1 кандидате: предложить K ещё не наблюдённых чанков, лучше всего разделяющих кандидатов (сходите и проверьте)\n"
"  --suggest-window R   окно поиска предложений [-R,R)^2 вокруг центра наблюдений (по умолчанию 32)\n"
"  --algo auto|analytic|brute   analytic (по умолчанию при --max-errors 0): lifting 35 бит + аналитическое решение старших 13 бит (мс);\n"
"                       brute: lifting 18 бит + перебор 2^30 старших (нужен при --max-errors > 0; эталон для сверки)\n"
"  --count-only         не выводить seed, только число кандидатов (stderr)\n"
"  --force              не отказываться от задач с заведомо огромным числом кандидатов (> 5e7)\n"
"  --quiet              без служебных сообщений\n"
"Вывод (stdout): по строке на кандидата: \"<seed48 десятичный> 0x<hex> [64-бит random seed...]\"; служебное — stderr.\n");
}

int main(int argc, char **argv) {
    std::string mode = "mixed", dev = "auto", infile, outfile, genspec, genout, withfile, version = "26.3", setspath, datadir;
    bool selftest = false, countonly = false, force = false, int32only = false, blockcoords = false; int suggest = 0, sugwin = 32; std::string algo = "auto";
    int maxerr = 0, threads = 0; unsigned long long limit = 1000000; bool expand = false, quiet = false;
    bool printmap = false; long long pm[5] = {0};
    unsigned long long pm_seed = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](int n) { if (i + n >= argc) { fprintf(stderr, "опция %s требует %d арг.\n", a.c_str(), n); exit(2); } };
        if (a == "--mode") { need(1); mode = argv[++i]; }
        else if (a == "--max-errors") { need(1); maxerr = atoi(argv[++i]); }
        else if (a == "--dev") { need(1); dev = argv[++i]; }
        else if (a == "--limit") { need(1); limit = strtoull(argv[++i], 0, 0); }
        else if (a == "--expand-random") expand = true;
        else if (a == "--out") { need(1); outfile = argv[++i]; }
        else if (a == "--threads") { need(1); threads = atoi(argv[++i]); }
        else if (a == "--gen") { need(1); genspec = argv[++i]; }
        else if (a == "--gen-out") { need(1); genout = argv[++i]; }
        else if (a == "--quiet") quiet = true;
        else if (a == "--with-structs") { need(1); withfile = argv[++i]; }
        else if (a == "--version") { need(1); version = argv[++i]; }
        else if (a == "--sets") { need(1); setspath = argv[++i]; }
        else if (a == "--data-dir") { need(1); datadir = argv[++i]; }
        else if (a == "--selftest") selftest = true;
        else if (a == "--algo") { need(1); algo = argv[++i]; }
        else if (a == "--count-only") countonly = true;
        else if (a == "--blocks") blockcoords = true;
        else if (a == "--int32") int32only = true;
        else if (a == "--suggest") { need(1); suggest = atoi(argv[++i]); }
        else if (a == "--suggest-window") { need(1); sugwin = atoi(argv[++i]); }
        else if (a == "--force") force = true;
        else if (a == "--print-map") { need(5); printmap = true; parse_u64(argv[++i], pm_seed); for (int k = 0; k < 4; k++) pm[k] = atoll(argv[++i]); }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a[0] == '-' && a != "-") { fprintf(stderr, "неизвестная опция %s\n", a.c_str()); usage(); return 2; }
        else infile = a;
    }
    if (selftest) {
        Rng rng(99); long bad = 0, n = 0;
        for (int it = 0; it < 2000000; it++) {
            u64 W = rng.next() & MASK48; i32 x = (i32)(rng.next() % 4000001) - 2000000, z = (i32)(rng.next() % 4000001) - 2000000;
            if (it % 7 == 0) { x = (i32)rng.next(); z = (i32)rng.next(); }          /* в т.ч. переполнение int */
            u64 c = slime_cst(x, z);
            int a = slime32((u32)W, (u32)(W >> 32), c), b = slime_from_cst(W, c);
            n++; if (a != b) bad++;
        }
        fprintf(stderr, "selftest slime32 vs exact: %ld/%ld расхождений\n", bad, n);
        return bad != 0;
    }
    if (printmap) {
        int cx0 = (int)pm[0], cz0 = (int)pm[1], nx = (int)pm[2], nz = (int)pm[3];
        for (int iz = 0; iz < nz; iz++) { std::string row; for (int ix = 0; ix < nx; ix++) row += is_slime_chunk(pm_seed, cx0 + ix, cz0 + iz) ? '1' : '0'; puts(row.c_str()); }
        return 0;
    }
    if (threads > 0) omp_set_num_threads(threads);
#define LOG(...) do { if (!quiet) fprintf(stderr, __VA_ARGS__); } while (0)

    /* ---------- наблюдения ---------- */
    std::vector<SObs> obs;
    unsigned long long true_seed = 0; bool have_true = false;
    if (!genspec.empty()) {
        unsigned long long S, np, nn, R, rs = 12345;
        char buf[256]; strncpy(buf, genspec.c_str(), 255); buf[255] = 0;
        std::vector<std::string> tk; { char *sv, *t = strtok_r(buf, ",", &sv); while (t) { tk.push_back(t); t = strtok_r(nullptr, ",", &sv); } }
        if (tk.size() < 4 || !parse_u64(tk[0].c_str(), S) || !parse_u64(tk[1].c_str(), np) || !parse_u64(tk[2].c_str(), nn) || !parse_u64(tk[3].c_str(), R)) { fprintf(stderr, "--gen: ожидается S,NPOS,NNEG,R[,RNG]\n"); return 2; }
        if (tk.size() > 4) parse_u64(tk[4].c_str(), rs);
        true_seed = S & MASK48; have_true = true;
        Rng rng(rs ^ (S * 0x9E3779B97F4A7C15ULL));
        int side = (int)(2 * R);
        std::vector<int> order((size_t)side * side);
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        for (size_t i = order.size(); i > 1; i--) std::swap(order[i - 1], order[rng.range((int)i)]);
        unsigned long long gp = 0, gn = 0;
        for (size_t k = 0; k < order.size() && (gp < np || gn < nn); k++) {
            int cx = (int)(order[k] % side) - (int)R, cz = (int)(order[k] / side) - (int)R;
            int sl = is_slime_chunk(S, cx, cz);
            if (sl && gp < np) { obs.push_back({cx, cz, 1}); gp++; }
            else if (!sl && gn < nn) { obs.push_back({cx, cz, 0}); gn++; }
        }
        if (gp < np || gn < nn) { fprintf(stderr, "--gen: в окне R=%llu не хватает чанков (pos %llu/%llu, neg %llu/%llu)\n", R, gp, np, gn, nn); return 2; }
        if (!genout.empty()) { FILE *f = fopen(genout.c_str(), "w"); for (auto &o : obs) fprintf(f, "%d %d %s\n", o.x, o.z, o.pos ? "slime" : "noslime"); fclose(f); }
    } else {
        if (infile.empty()) { usage(); return 2; }
        std::istream *in; std::ifstream fin;
        if (infile == "-") in = &std::cin; else { fin.open(infile); if (!fin) { fprintf(stderr, "не могу открыть %s\n", infile.c_str()); return 2; } in = &fin; }
        std::string line; int ln = 0;
        while (std::getline(*in, line)) {
            ln++;
            auto t = split_tokens(line);
            if (t.empty()) continue;
            if (StructDB::strip(t[0]) == "slime" || StructDB::strip(t[0]) == "struct") t.erase(t.begin());
            long long x, z;
            if (t.size() < 2 || !parse_int(t[0], x) || !parse_int(t[1], z)) { fprintf(stderr, "%s:%d: ожидалось \"chunkX chunkZ slime|noslime\"\n", infile.c_str(), ln); return 2; }
            int pos = 1;
            if (t.size() >= 3) {
                std::string f = StructDB::strip(t[2]);
                if (f == "slime" || f == "1" || f == "s" || f == "+" || f == "yes" || f == "true" || f == "pos" || f == "positive") pos = 1;
                else if (f == "noslime" || f == "0" || f == "n" || f == "-" || f == "no" || f == "false" || f == "neg" || f == "negative" || f == "not_slime") pos = 0;
                else { fprintf(stderr, "%s:%d: неизвестный признак '%s' (ожидалось slime|noslime)\n", infile.c_str(), ln, t[2].c_str()); return 2; }
            }
            if (blockcoords) { x = floordiv_i32((i32)x, 16); z = floordiv_i32((i32)z, 16); }
            obs.push_back({(i32)x, (i32)z, pos});
        }
    }
    if (mode != "pos" && mode != "mixed") { fprintf(stderr, "--mode: pos|mixed\n"); return 2; }
    {   /* дубликаты и противоречия в наблюдениях */
        std::vector<std::pair<std::pair<i32, i32>, int>> v; for (auto &o : obs) v.push_back({{o.x, o.z}, o.pos});
        std::sort(v.begin(), v.end());
        size_t dup = 0, conf = 0;
        for (size_t i = 1; i < v.size(); i++) if (v[i].first == v[i - 1].first) { if (v[i].second == v[i - 1].second) dup++; else conf++; }
        if (conf) fprintf(stderr, "ВНИМАНИЕ: %zu чанков помечены и как слайм, и как не-слайм — без --max-errors кандидатов не будет\n", conf);
        if (dup && !quiet) fprintf(stderr, "# замечание: %zu повторяющихся наблюдений (учитываются повторно)\n", dup);
    }
    std::vector<u64> cst;   /* положительные, затем отрицательные */
    int npos = 0, nneg = 0;
    for (auto &o : obs) if (o.pos) { cst.push_back(slime_cst(o.x, o.z)); npos++; }
    if (mode == "mixed") for (auto &o : obs) if (!o.pos) { cst.push_back(slime_cst(o.x, o.z)); nneg++; }
    int nneg_ignored = (int)std::count_if(obs.begin(), obs.end(), [](const SObs &o) { return !o.pos; }) - nneg;
    if ((int)cst.size() > MAXOBS) { fprintf(stderr, "слишком много наблюдений (макс. %d)\n", MAXOBS); return 2; }
    if (npos == 0) { fprintf(stderr, "нужен хотя бы один положительный (slime) чанк: без них lifting по чётности невозможен\n"); return 2; }
    if (maxerr < 0) maxerr = 0;
    std::vector<Pred> extra;
    if (!withfile.empty()) {
        StructDB db; std::string err;
        if (!load_structdb(db, version, setspath, datadir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
        if (!err.empty()) fprintf(stderr, "%s\n", err.c_str());
        LOG("# таблица placement: %s (%zu наборов)\n", db.source.c_str(), db.sets.size());
        std::string perr;
        if (parse_obs_file(withfile, db, extra, perr)) { fprintf(stderr, "%s", perr.c_str()); return 2; }
        sort_preds(extra);
        LOG("# дополнительных предикатов (--with-structs): %zu (%.1f бит)\n", extra.size(), info_bits(extra));
    }

    /* оценка ожидаемого числа ложных кандидатов: 2^48 * P(число несовпадений <= maxerr) */
    double expect;
    {
        std::vector<double> dp(maxerr + 1, 0.0); dp[0] = 1.0;
        auto add = [&](double pm_) { for (int e = maxerr; e >= 0; e--) { double v = dp[e] * (1 - pm_); if (e > 0) v += dp[e - 1] * pm_; dp[e] = v; } };
        for (int k = 0; k < npos; k++) add(0.9);
        for (int k = 0; k < nneg; k++) add(0.1);
        double pr = 0; for (double v : dp) pr += v;
        expect = 281474976710656.0 * pr;
        for (auto &p : extra) expect *= p.pass;
    }
    double bits = npos * log2(10.0) + nneg * (-log2(0.9));
    LOG("# наблюдений: %d положительных, %d отрицательных (%d игнорируется в режиме pos), допуск ошибок %d; информации %.1f бит из 48\n", npos, nneg, nneg_ignored, maxerr, bits);
    LOG("# ожидаемое число ложных кандидатов ~ %.3g (теория, случайные данные)\n", expect);

    if (expect > 5e7 && !force) {
        fprintf(stderr, "Слишком мало информации: ожидается ~%.3g кандидатов (48 бит - %.1f бит наблюдений). Добавьте наблюдения "
                        "(слайм-чанков нужно десятки, см. docs/22) или --with-structs; --force — всё равно искать.\n", expect, bits);
        return 4;
    }
    double t_start = now_s();
    /* ---------- lifting: младшие 18 бит ---------- */
    std::vector<u64> lows;
    {
        const u64 mask = (1ULL << LBITS) - 1;
        std::vector<u64> pc(npos);
        for (int k = 0; k < npos; k++) pc[k] = cst[k] & mask;
        lows.reserve(1 << 12);
        for (u64 l = 0; l < (1ULL << LBITS); l++) {
            int viol = 0;
            for (int k = 0; k < npos; k++) {
                u64 s = ((l + pc[k]) & mask) ^ (SLIME_KX & mask);
                u64 s1 = (s * LCG_MUL + LCG_ADD) & mask;
                if ((s1 >> 17) & 1) { if (++viol > maxerr) break; }     /* бит 17 == 1 => r нечётно => не слайм */
            }
            if (viol <= maxerr) lows.push_back(l);
        }
    }
    double t_lift = now_s() - t_start;
    u64 nlows = lows.size();
    if (algo != "auto" && algo != "analytic" && algo != "brute") { fprintf(stderr, "--algo: auto|analytic|brute\n"); return 2; }
    bool use_analytic = (algo == "analytic") || (algo == "auto" && maxerr == 0);
    if (use_analytic && maxerr > 0) { fprintf(stderr, "--algo analytic не поддерживает --max-errors > 0 (используйте brute)\n"); return 2; }
    u32 minv13 = 0; { u32 x = (u32)(LCG_MUL & 0x1FFF), y = x; for (int i = 0; i < 5; i++) y = (y * (2 - x * y)) & 0x1FFF; minv13 = y; }
    double work = use_analytic ? (double)nlows * 131072.0 : (double)nlows * (double)(1ULL << HBITS);
    LOG("# lifting (бит 17 состояния, 18 младших бит W): %llu из 262144 вариантов младших бит (%.4f с); алгоритм %s: %.3e %s\n", (unsigned long long)nlows, t_lift,
        use_analytic ? "analytic" : "brute", work, use_analytic ? "пар (l,m) — старшие 13 бит находятся аналитически" : "кандидатов (перебор 2^30 старших бит)");

    std::vector<u64> found;
    unsigned long long total_found = 0;
    bool use_gpu = false; int sms = 0; std::string gpuname;
    if (dev == "gpu" || dev == "auto") use_gpu = have_gpu(&sms, &gpuname);
    if (dev == "gpu" && !use_gpu) { fprintf(stderr, "GPU запрошен, но недоступен (или сборка NO_CUDA)\n"); return 3; }
    double t_search0 = now_s(), t_kernel = 0;
    if (nlows == 0) { LOG("# после lifting кандидатов нет\n"); }
    else if (use_gpu) {
#ifndef NO_CUDA
        { double tg = now_s(); cudaFree(0); LOG("# GPU: %s (%d SM), контекст %.3f с\n", gpuname.c_str(), sms, now_s() - tg); }
        u64 *d_lows, *d_res, *d_cst; unsigned long long *d_nres; Pred *d_preds;
        CK(cudaMalloc(&d_lows, nlows * 8)); CK(cudaMalloc(&d_res, (size_t)RESCAP * 8)); CK(cudaMalloc(&d_nres, 8));
        CK(cudaMalloc(&d_cst, cst.size() * 8)); CK(cudaMalloc(&d_preds, std::max<size_t>(1, extra.size()) * sizeof(Pred)));
        CK(cudaMemcpy(d_lows, lows.data(), nlows * 8, cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_cst, cst.data(), cst.size() * 8, cudaMemcpyHostToDevice));
        if (!extra.empty()) CK(cudaMemcpy(d_preds, extra.data(), extra.size() * sizeof(Pred), cudaMemcpyHostToDevice));
        CK(cudaMemset(d_nres, 0, 8));
        int blocks = sms * 16, threads_ = 256;
        double tw = now_s();
        k_slime<<<1, 1>>>(d_lows, d_cst, npos, nneg, maxerr, d_preds, (int)extra.size(), 0, 0, d_res, d_nres, RESCAP);   /* прогрев: загрузка модуля */
        k_pairs<<<1, 1>>>(d_lows, d_cst, npos, nneg, d_preds, (int)extra.size(), minv13, 0, 0, d_res, d_nres, RESCAP);
        CK(cudaDeviceSynchronize());
        LOG("# GPU: инициализация контекста и модуля %.3f с (не входит во время перебора)\n", now_s() - tw);
        cudaEvent_t e0, e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
        u64 totseg = nlows << (HBITS - SEGBITS), chunkseg = 1ULL << 28;
        CK(cudaEventRecord(e0));
        if (use_analytic) {
            u64 totp = nlows << 17, chunkp = 1ULL << 32;
            for (u64 off = 0; off < totp; off += chunkp) {
                u64 cnt = std::min(chunkp, totp - off);
                k_pairs<<<blocks, threads_>>>(d_lows, d_cst, npos, nneg, d_preds, (int)extra.size(), minv13, off, cnt, d_res, d_nres, RESCAP);
                CK(cudaGetLastError());
                CK(cudaDeviceSynchronize());
            }
        } else
        for (u64 off = 0; off < totseg; off += chunkseg) {
            u64 cnt = std::min(chunkseg, totseg - off);
            k_slime<<<blocks, threads_>>>(d_lows, d_cst, npos, nneg, maxerr, d_preds, (int)extra.size(), off, cnt, d_res, d_nres, RESCAP);
            CK(cudaGetLastError());
            CK(cudaDeviceSynchronize());
            if (!quiet && isatty(2) && totseg > 4 * chunkseg) fprintf(stderr, "# прогресс: %.1f%% (%.1f с)\r", 100.0 * (off + cnt) / totseg, now_s() - t_search0);
        }
        CK(cudaEventRecord(e1)); CK(cudaEventSynchronize(e1));
        float ms; CK(cudaEventElapsedTime(&ms, e0, e1)); t_kernel = ms / 1000.0;
        CK(cudaMemcpy(&total_found, d_nres, 8, cudaMemcpyDeviceToHost));
        size_t nr = std::min<unsigned long long>(total_found, RESCAP);
        found.resize(nr);
        if (nr) CK(cudaMemcpy(found.data(), d_res, nr * 8, cudaMemcpyDeviceToHost));
        cudaFree(d_cst); cudaFree(d_preds);
        cudaFree(d_lows); cudaFree(d_res); cudaFree(d_nres);
#endif
    } else {
        int nt = omp_get_max_threads();
        LOG("# CPU: OpenMP, %d потоков\n", nt);
        u64 nblocks = nlows << (HBITS - 16);
        std::vector<u64> all;
        unsigned long long cnt_total = 0;
        if (use_analytic) {
            struct CpuEmit { std::vector<u64> *v; unsigned long long n; size_t cap; void operator()(u64 W) { n++; if (v->size() < cap) v->push_back(W); } };
            #pragma omp parallel
            {
                std::vector<u64> loc; CpuEmit em{&loc, 0, (size_t)(1u << 19)};
                #pragma omp for schedule(dynamic, 1)
                for (long long b = 0; b < (long long)(nlows << 8); b++) {        /* блоки по 2^9 значений m */
                    u64 l = lows[(u64)b >> 8]; u32 m0 = (u32)(((u64)b & 0xFF) << 9);
                    for (u32 m = m0; m < m0 + 512; m++) pair_search(l, m, cst.data(), npos, nneg, extra.data(), (int)extra.size(), minv13, em);
                }
                #pragma omp critical
                { all.insert(all.end(), loc.begin(), loc.end()); cnt_total += em.n; }
            }
            nblocks = 0;
        }
        #pragma omp parallel
        {
            std::vector<u64> loc; unsigned long long nloc = 0;
            #pragma omp for schedule(dynamic, 4)
            for (long long b = 0; b < (long long)nblocks; b++) {
                u64 l = lows[(u64)b >> (HBITS - 16)];
                u64 hb = ((u64)b & ((1ULL << (HBITS - 16)) - 1)) << 16;
                for (u64 h = hb; h < hb + 65536; h++) {
                    u64 W = (h << LBITS) | l;
                    if (full_check(W, cst.data(), npos, nneg, maxerr) && extra_check(W, extra.data(), (int)extra.size())) { nloc++; if (loc.size() < (1u << 19)) loc.push_back(W); }
                }
            }
            #pragma omp critical
            { all.insert(all.end(), loc.begin(), loc.end()); cnt_total += nloc; }
        }
        total_found = cnt_total; found = all;
        t_kernel = now_s() - t_search0;
    }
    double t_total = now_s() - t_start;
    std::sort(found.begin(), found.end());
    if (int32only) { size_t n0 = found.size(); found.erase(std::remove_if(found.begin(), found.end(), [](u64 w) { return !is_int32_seed(w); }), found.end()); LOG("# --int32: %zu -> %zu кандидатов\n", n0, found.size()); total_found = found.size() < n0 ? found.size() : total_found; }
    /* контрольная проверка на хосте (точная семантика игры) */
    size_t verified = 0;
    for (u64 W : found) {
        int err = 0;
        for (auto &o : obs) { bool use = o.pos || mode == "mixed"; if (!use) continue; if ((bool)is_slime_chunk(W, o.x, o.z) != (bool)o.pos) err++; }
        bool okx = true; for (auto &p : extra) if (!pred_check(W, p)) { okx = false; break; }
        if (err <= maxerr && okx) verified++;
    }
    FILE *fo = outfile.empty() ? nullptr : fopen(outfile.c_str(), "w");
    unsigned long long printed = 0;
    for (u64 W : found) {
        if (printed >= limit || countonly) break;
        printf("%llu 0x%012llx", (unsigned long long)W, (unsigned long long)W);
        if (fo) fprintf(fo, "%llu 0x%012llx", (unsigned long long)W, (unsigned long long)W);
        if (expand) for (i64 w : random_world_seeds(W)) { printf(" %lld", (long long)w); if (fo) fprintf(fo, " %lld", (long long)w); }
        printf("\n"); if (fo) fprintf(fo, "\n");
        printed++;
    }
    if (fo) fclose(fo);
    LOG("# найдено кандидатов: %llu (проверено на хосте: %zu из %zu выведенных/сохранённых)%s\n", total_found, verified, found.size(), total_found > found.size() ? " [ВЫВОД ОБРЕЗАН: превышен буфер]" : "");
    if (total_found > limit) LOG("# выведено первых %llu по --limit\n", limit);
    LOG("# время: lifting %.4f с, перебор %.4f с (%s; %.3e %s/с), всего %.4f с\n", t_lift, t_kernel, use_gpu ? "GPU" : "CPU", t_kernel > 0 ? work / t_kernel : 0.0, use_analytic ? "пар" : "кандидатов", t_total);
    if (suggest > 0 && found.size() > 1 && found.size() <= 200000) {
        /* активный выбор наблюдений: чанки окна, где кандидаты разделяются наиболее равномерно */
        long long sx = 0, sz = 0; for (auto &o : obs) { sx += o.x; sz += o.z; }
        i32 mx = obs.empty() ? 0 : (i32)(sx / (long long)obs.size()), mz = obs.empty() ? 0 : (i32)(sz / (long long)obs.size());
        std::vector<std::pair<i32, i32>> cells;
        for (int dz = -sugwin; dz < sugwin; dz++) for (int dx = -sugwin; dx < sugwin; dx++) {
            i32 x = mx + dx, z = mz + dz; bool seen = false;
            for (auto &o : obs) if (o.x == x && o.z == z) { seen = true; break; }
            if (!seen) cells.push_back({x, z});
        }
        std::vector<long> cnt(cells.size(), 0);
        #pragma omp parallel for schedule(static)
        for (long long ci = 0; ci < (long long)cells.size(); ci++) {
            u64 c = slime_cst(cells[ci].first, cells[ci].second); long n = 0;
            for (u64 W : found) n += slime_from_cst(W, c);
            cnt[ci] = n;
        }
        std::vector<size_t> idx(cells.size()); for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
        long nn = (long)found.size();
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return std::min(cnt[a], nn - cnt[a]) > std::min(cnt[b], nn - cnt[b]); });
        LOG("# подсказки: чанки окна %dx%d вокруг (%d,%d), лучше всего разделяющие %zu кандидатов:\n", 2 * sugwin, 2 * sugwin, (int)mx, (int)mz, found.size());
        if (!idx.empty() && std::min(cnt[idx[0]], nn - cnt[idx[0]]) == 0) LOG("#   ни один чанк окна не разделяет кандидатов (они различаются лишь на редких чанках у границы переполнения) — увеличьте --suggest-window\n");
        for (int k = 0; k < suggest && k < (int)idx.size() && std::min(cnt[idx[k]], nn - cnt[idx[k]]) > 0; k++) LOG("#   чанк (%d,%d): слайм у %ld из %ld кандидатов\n", cells[idx[k]].first, cells[idx[k]].second, cnt[idx[k]], nn);
    }
    if (have_true) {
        bool hit = std::binary_search(found.begin(), found.end(), (u64)true_seed);
        LOG("# тест: истинный seed %llu (0x%012llx) %s среди кандидатов\n", true_seed, true_seed, hit ? "НАЙДЕН" : "НЕ найден");
        if (!hit) return 1;
    }
    return 0;
}

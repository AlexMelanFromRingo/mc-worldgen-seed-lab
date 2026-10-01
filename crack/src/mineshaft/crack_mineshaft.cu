/*
 * crack-mineshaft — восстановление 48-битного structure seed по стартовым чанкам шахт (mineshaft), 26.1 / 26.2 / 26.3.
 *
 * Механика игры: structure_set `mineshafts`: random_spread spacing=1, separation=0 (каждый чанк — свой регион, позиция не несёт
 * информации), frequency=0.004, frequency_reduction_method=legacy_type_3:
 *     setLargeFeatureSeed(seed, cx, cz);  nextDouble() < (double)0.004F            (AbstractSpreadingStructurePlacement.java:107-111)
 * т.е. чанк — старт шахты с вероятностью 0.004 (7.97 бит на чанк); нужно >= 6 (47.8 бит) — практически 7+ стартовых чанков.
 * Стартовый чанк: главная комната шахты (MineshaftStructure.generatePiecesAndAdjust: MineShaftRoom(chunkPos.getBlockX(2), getBlockZ(2)))
 * имеет северо-западный угол в блоке (16*cx+2, 16*cz+2) — отсюда стартовый чанк (формат `block x z` принимает любой блок чанка).
 *
 * Алгоритм: nextDouble()<0.004 зависит от всех 48 бит -> lifting младших бит невозможен; полный перебор 2^48 (GPU) с ранним выходом:
 * первая проверка отсекает 99.6%, остальные — только для выживших. Если есть liftable-предикаты (--with-structs: structure_set'ы с
 * limit не степень двойки, слайм-чанки), их младшие биты используются как lifting и объём перебора падает на порядки.
 * Для тестов: --range BASE,BITS ограничивает перебор диапазоном W in [BASE, BASE+2^BITS).
 *
 * Запуск: crack-mineshaft [опции] файл  (строки: "chunkX chunkZ" | "mineshafts;cx;cz" | "block x z")
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
#include "../common/structdb.h"

#define MAXP 256
#define RESCAP (1u << 22)

#define KLO ((u32)(LCG_MUL))
#define KHI ((u32)(LCG_MUL >> 32))
/* 48-битные числа как (hi:16 | lo:32) */
HD void lcg48(u32 lo, u32 hi, u32 &olo, u32 &ohi) {
    u64 p = (u64)lo * KLO + LCG_ADD;
    olo = (u32)p; ohi = ((u32)(p >> 32) + hi * KLO + lo * KHI) & 0xFFFFu;
}
HD void mul48(u32 alo, u32 ahi, u32 blo, u32 bhi, u32 &rlo, u32 &rhi) {
    u64 p = (u64)alo * blo; rlo = (u32)p; rhi = ((u32)(p >> 32) + alo * bhi + ahi * blo) & 0xFFFFu;
}
struct MsFast { u32 cxlo, cxhi, czlo, czhi; u64 thr53; i32 cx, cz; };

/* mineshaft_start(W, cx, cz) в 32-битной арифметике; thr53: v53 < X <=> v53 <= thr53 (X = (double)0.004f * 2^53) */
HD bool ms_fast(u32 Wlo, u32 Whi, const MsFast &m) {
    u32 s0l = Wlo ^ KLO, s0h = Whi ^ KHI;
    u32 s1l, s1h, s2l, s2h, s3l, s3h, s4l, s4h;
    lcg48(s0l, s0h, s1l, s1h); lcg48(s1l, s1h, s2l, s2h); lcg48(s2l, s2h, s3l, s3h); lcg48(s3l, s3h, s4l, s4h);
    u32 alo = (s2h << 16) | (s2l >> 16), ahi = ((s1l >> 16) - (alo >> 31)) & 0xFFFFu;   /* nextLong() mod 2^48 */
    u32 blo = (s4h << 16) | (s4l >> 16), bhi = ((s3l >> 16) - (blo >> 31)) & 0xFFFFu;
    u32 xl, xh, yl, yh;
    mul48(alo, ahi, m.cxlo, m.cxhi, xl, xh); mul48(blo, bhi, m.czlo, m.czhi, yl, yh);
    u32 rl = xl ^ yl ^ Wlo, rh = (xh ^ yh ^ Whi) & 0xFFFFu;                             /* cx*a ^ cz*b ^ seed */
    u32 t0l = rl ^ KLO, t0h = rh ^ KHI, t1l, t1h, t2l, t2h;
    lcg48(t0l, t0h, t1l, t1h);
    u32 hi26 = (t1h << 10) | (t1l >> 22);
    if (hi26 > 268435u) return false;                                                      /* 99.6% отсекается здесь */
    lcg48(t1l, t1h, t2l, t2h);
    u32 lo27 = (t2h << 11) | (t2l >> 21);
    u64 v53 = ((u64)hi26 << 27) + lo27;
    return v53 <= m.thr53;
}

#ifndef NO_CUDA
__constant__ Pred c_preds[MAXP];
__constant__ int c_np;
__constant__ MsFast c_ms;
__constant__ int c_usefast;

__global__ void k_engine(const u64 *__restrict__ lows, int L, u64 wbase, u64 base, u64 count, u64 *res, unsigned long long *nres, unsigned cap) {
    u64 stride = (u64)gridDim.x * blockDim.x;
    u64 end = base + count;
    u64 hmask = (L >= 48) ? ~0ULL : ((1ULL << (48 - L)) - 1);
    for (u64 i = base + (u64)blockIdx.x * blockDim.x + threadIdx.x; i < end; i += stride) {
        u64 W = wbase + (((i & hmask) << L) | lows[i >> (48 - L)]);
        int k0 = 0;
        if (c_usefast) { if (!ms_fast((u32)W, (u32)(W >> 32), c_ms)) continue; k0 = 1; }
        bool ok = true;
        for (int k = k0; k < c_np && ok; k++) ok = pred_check(W, c_preds[k]);
        if (ok) { unsigned long long p = atomicAdd(nres, 1ULL); if (p < cap) res[p] = W; }
    }
}
#endif

static void usage() {
    fprintf(stderr,
"crack-mineshaft — structure seed (48 бит) по стартовым чанкам шахт (26.1/26.2/26.3)\n"
"Использование: crack-mineshaft [опции] <файл|->\n"
"  Файл: строки \"chunkX chunkZ\" (старт-чанк шахты), либо \"mineshafts;cx;cz\", либо \"block x z\" (любой блок старт-чанка;\n"
"        главная комната шахты начинается в блоке (16cx+2, 16cz+2)). Нужно >= 6-7 стартовых чанков (7.97 бит на чанк).\n"
"Опции:\n"
"  --with-structs FILE  дополнительные предикаты (`<structure_set>;cx;cz`, `slime;cx;cz;0|1`) — структуры дают lifting, резко ускоряя поиск\n"
"  --version V | --data-dir D | --sets F   таблица placement (по умолчанию 26.3)\n"
"  --range BASE,BITS    перебирать только W in [BASE, BASE+2^BITS) (тесты / частичный перебор); по умолчанию весь 2^48\n"
"  --dev auto|gpu|cpu   --threads N   --limit N   --out FILE   --expand-random   --quiet\n"
"  --progress           печатать прогресс (строки, не возврат каретки) примерно каждые 2%% — для долгих запусков в фоне\n"
"  --force              не отказываться от задач с заведомо огромным числом кандидатов (> 5e7; обычно мало шахт: нужно >= 6-7)\n"
"  --int32              оставить только seed, представимые 32-битным целым (биты 32..47 все 0 или 1)\n"
"  --no-fast            выключить специализированную 32-битную проверку mineshaft (использовать общий pred_check)\n"
"  --gen S,N[,R[,RNG]]  синтетические наблюдения: N старт-чанков из окна [-R,R)^2 (R по умолчанию 2048) для seed S; ещё: CRACK_GEN_OUT=файл\n"
"  --gen-sets a+b+c     вместе с --gen: добавить наблюдения этих structure_set'ов (по одному) как --with-structs\n"
"  --selftest           сверка ms_fast с точной реализацией\n"
"Вывод (stdout): \"<seed48> 0x<hex> [random seeds]\"; служебное — stderr.\n");
}

int main(int argc, char **argv) {
    std::string infile, withfile, dev = "auto", version = "26.3", outfile, datadir, setspath, genspec, gensets;
    unsigned long long limit = 1000000, range_base = 0; int range_bits = 48, threads = 0;
    bool progress = false, force = false, int32only = false, expand = false, quiet = false, nofast = false, selftest = false, have_range = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](int n) { if (i + n >= argc) { fprintf(stderr, "опция %s требует %d арг.\n", a.c_str(), n); exit(2); } };
        if (a == "--with-structs") { need(1); withfile = argv[++i]; }
        else if (a == "--version") { need(1); version = argv[++i]; }
        else if (a == "--data-dir") { need(1); datadir = argv[++i]; }
        else if (a == "--sets") { need(1); setspath = argv[++i]; }
        else if (a == "--dev") { need(1); dev = argv[++i]; }
        else if (a == "--limit") { need(1); limit = strtoull(argv[++i], 0, 0); }
        else if (a == "--out") { need(1); outfile = argv[++i]; }
        else if (a == "--threads") { need(1); threads = atoi(argv[++i]); }
        else if (a == "--range") { need(1); std::string r = argv[++i]; size_t c = r.find(','); if (c == std::string::npos || !parse_u64(r.substr(0, c).c_str(), range_base)) { fprintf(stderr, "--range BASE,BITS\n"); return 2; } range_bits = atoi(r.c_str() + c + 1); have_range = true; }
        else if (a == "--gen") { need(1); genspec = argv[++i]; }
        else if (a == "--gen-sets") { need(1); gensets = argv[++i]; }
        else if (a == "--expand-random") expand = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--no-fast") nofast = true;
        else if (a == "--int32") int32only = true;
        else if (a == "--force") force = true;
        else if (a == "--progress") progress = true;
        else if (a == "--selftest") selftest = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a[0] == '-' && a != "-") { fprintf(stderr, "неизвестная опция %s\n", a.c_str()); usage(); return 2; }
        else infile = a;
    }
#define LOG(...) do { if (!quiet) fprintf(stderr, __VA_ARGS__); } while (0)
    if (threads > 0) omp_set_num_threads(threads);
    (void)progress;

    StructDB db; { std::string err; if (!load_structdb(db, version, setspath, datadir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; } if (!err.empty() && !quiet) fprintf(stderr, "%s\n", err.c_str()); }
    const SetDef *msdef = db.find("mineshafts");
    if (!msdef) { fprintf(stderr, "в таблице placement нет набора mineshafts\n"); return 2; }
    MsFast mf; memset(&mf, 0, sizeof mf);
    { double X = ldexp((double)msdef->freq, 53); mf.thr53 = (u64)ceil(X) - 1; }

    if (selftest) {
        Rng rng(7); long bad = 0, n = 0, pass = 0;
        for (int it = 0; it < 4000000; it++) {
            u64 W = rng.next() & MASK48; i32 cx = (i32)(rng.next() % 2001) - 1000, cz = (i32)(rng.next() % 2001) - 1000;
            if (it % 5 == 0) { cx = (i32)rng.next(); cz = (i32)rng.next(); }
            MsFast m = mf; u64 cxx = (u64)(i64)cx & MASK48, czz = (u64)(i64)cz & MASK48;
            m.cxlo = (u32)cxx; m.cxhi = (u32)(cxx >> 32); m.czlo = (u32)czz; m.czhi = (u32)(czz >> 32);
            bool f = ms_fast((u32)W, (u32)(W >> 32), m);
            u64 s = large_feature_seed_state(W, cx, cz); bool ex = lcg_nextDouble(&s) < (double)msdef->freq;
            n++; pass += ex; if (f != ex) bad++;
        }
        /* принудительные «проходящие» чанки: ищем W, для которых mineshaft_start истинно */
        for (int it = 0; it < 2000 ; ) {
            u64 W = rng.next() & MASK48; i32 cx = (i32)(rng.next() % 2001) - 1000, cz = (i32)(rng.next() % 2001) - 1000;
            u64 s = large_feature_seed_state(W, cx, cz); if (!(lcg_nextDouble(&s) < (double)msdef->freq)) continue;
            MsFast m = mf; u64 cxx = (u64)(i64)cx & MASK48, czz = (u64)(i64)cz & MASK48;
            m.cxlo = (u32)cxx; m.cxhi = (u32)(cxx >> 32); m.czlo = (u32)czz; m.czhi = (u32)(czz >> 32);
            n++; pass++; if (!ms_fast((u32)W, (u32)(W >> 32), m)) bad++; it++;
        }
        fprintf(stderr, "selftest ms_fast vs exact: %ld/%ld расхождений (проходящих: %ld), thr53=%llu\n", bad, n, pass, (unsigned long long)mf.thr53);
        return bad != 0;
    }

    std::vector<Pred> preds; std::vector<std::pair<i32, i32>> mschunks;
    bool have_true = false; u64 true_seed = 0;
    auto add_ms = [&](i32 cx, i32 cz) { Pred p; std::string e; if (!make_spread_pred(*msdef, cx, cz, p, e)) { fprintf(stderr, "%s\n", e.c_str()); exit(2); } preds.push_back(p); mschunks.push_back({cx, cz}); };
    if (!genspec.empty()) {
        char buf[256]; strncpy(buf, genspec.c_str(), 255); buf[255] = 0;
        std::vector<std::string> tk; { char *sv, *t = strtok_r(buf, ",", &sv); while (t) { tk.push_back(t); t = strtok_r(nullptr, ",", &sv); } }
        unsigned long long S, N, R = 2048, rs = 777;
        if (tk.size() < 2 || !parse_u64(tk[0].c_str(), S) || !parse_u64(tk[1].c_str(), N)) { fprintf(stderr, "--gen S,N[,R[,RNG]]\n"); return 2; }
        if (tk.size() > 2) parse_u64(tk[2].c_str(), R);
        if (tk.size() > 3) parse_u64(tk[3].c_str(), rs);
        true_seed = S & MASK48; have_true = true;
        Rng rng(rs ^ (S * 0x9E3779B97F4A7C15ULL));
        FILE *gf = nullptr; { const char *g = getenv("CRACK_GEN_OUT"); if (g) gf = fopen(g, "w"); }
        unsigned long long got = 0, tried = 0;
        while (got < N && tried < 200000000ULL) {
            tried++;
            i32 cx = rng.range((int)(2 * R)) - (int)R, cz = rng.range((int)(2 * R)) - (int)R;
            u64 s = large_feature_seed_state(true_seed, cx, cz);
            if (lcg_nextDouble(&s) < (double)msdef->freq) { bool dup = false; for (auto &c : mschunks) if (c.first == cx && c.second == cz) dup = true; if (dup) continue; add_ms(cx, cz); got++; if (gf) fprintf(gf, "mineshafts;%d;%d\n", cx, cz); }
        }
        if (got < N) { fprintf(stderr, "--gen: не хватило чанков\n"); return 2; }
        if (!gensets.empty()) {
            std::string cur; std::string sets = gensets + "+";
            for (char ch : sets) {
                if (ch != '+') { cur += ch; continue; }
                if (cur.empty()) continue;
                std::string nm = cur; cur.clear();
                const SetDef *d = db.find(nm); if (!d || !d->spread) { fprintf(stderr, "--gen-sets: неизвестный набор %s\n", nm.c_str()); return 2; }
                for (int tries = 0; tries < 100000; tries++) {
                    i32 gx = rng.range(60) - 30, gz = rng.range(60) - 30; i32 ox, oz;
                    spread_offset(true_seed, gx, gz, d->spacing, d->sep, d->salt, d->type, &ox, &oz);
                    i32 cx = gx * d->spacing + ox, cz = gz * d->spacing + oz;
                    if (!reducer_pass(true_seed, d->red, d->salt, cx, cz, d->freq, (i32)(1.0f / d->freq))) continue;
                    Pred p; std::string e; if (!make_spread_pred(*d, cx, cz, p, e)) { fprintf(stderr, "%s\n", e.c_str()); return 2; }
                    preds.push_back(p); if (gf) fprintf(gf, "%s;%d;%d\n", d->key.c_str(), cx, cz); break;
                }
            }
        }
        if (gf) fclose(gf);
        LOG("# --gen: seed48=%llu (0x%012llx), %llu старт-чанков шахт (перебрано %llu чанков)\n", (unsigned long long)true_seed, (unsigned long long)true_seed, got, tried);
    } else {
        if (infile.empty()) { usage(); return 2; }
        std::istream *in; std::ifstream fin;
        if (infile == "-") in = &std::cin; else { fin.open(infile); if (!fin) { fprintf(stderr, "не могу открыть %s\n", infile.c_str()); return 2; } in = &fin; }
        std::string line; int ln = 0;
        while (std::getline(*in, line)) {
            ln++; auto t = split_tokens(line); if (t.empty()) continue;
            std::string h = StructDB::strip(t[0]); long long x, z;
            bool blk = (h == "block");
            if (blk || h == "mineshafts" || h == "mineshaft" || h == "chunk") t.erase(t.begin());
            if (t.size() < 2 || !parse_int(t[0], x) || !parse_int(t[1], z)) { fprintf(stderr, "%s:%d: ожидалось \"chunkX chunkZ\"\n", infile.c_str(), ln); return 2; }
            if (blk) { x = floordiv_i32((i32)x, 16); z = floordiv_i32((i32)z, 16); }
            add_ms((i32)x, (i32)z);
        }
    }
    if (!withfile.empty()) {
        std::string perr; size_t n0 = preds.size();
        if (parse_obs_file(withfile, db, preds, perr)) { fprintf(stderr, "%s", perr.c_str()); return 2; }
        LOG("# --with-structs: %zu предикатов\n", preds.size() - n0);
    }
    if (preds.empty()) { fprintf(stderr, "нет наблюдений\n"); return 2; }
    if ((int)preds.size() > MAXP) { fprintf(stderr, "слишком много предикатов (макс. %d)\n", MAXP); return 2; }
    sort_preds(preds);
    double expect = 281474976710656.0; for (auto &p : preds) expect *= p.pass;
    LOG("# предикатов: %zu (из них шахт: %zu); информации %.1f бит из 48; ожидаемое число ложных кандидатов ~ %.3g\n", preds.size(), mschunks.size(), info_bits(preds), expect);

    {
        double ex2 = have_range ? (double)(1ULL << range_bits) : 281474976710656.0; for (auto &p : preds) ex2 *= p.pass;
        if (ex2 > 5e7 && !force) {
            fprintf(stderr, "Слишком мало информации: ожидается ~%.3g ложных кандидатов. Нужно >= 6-7 стартовых чанков шахт (7.97 бит каждый) "
                            "или --with-structs; --force — всё равно искать.\n", ex2);
            return 4;
        }
    }
    /* быстрая проверка шахты применима, если первый предикат — mineshaft-чанк */
    int usefast = 0;
    if (!nofast && preds[0].kind == PK_SPREAD && preds[0].lim == 1 && preds[0].red == RED_TYPE3) {
        usefast = 1;
        u64 cxx = (u64)(i64)preds[0].cx & MASK48, czz = (u64)(i64)preds[0].cz & MASK48;
        mf.cxlo = (u32)cxx; mf.cxhi = (u32)(cxx >> 32); mf.czlo = (u32)czz; mf.czhi = (u32)(czz >> 32); mf.cx = preds[0].cx; mf.cz = preds[0].cz;
    }

    /* ---------- lifting ---------- */
    double t0 = now_s();
    int L = 0; std::vector<u64> lows;
    if (!have_range) lows = lift_lows(preds, L);
    if (L < 18) { L = 0; lows.assign(1, 0); }
    double t_lift = now_s() - t0;
    u64 space = have_range ? (1ULL << range_bits) : (1ULL << (48 - L));
    u64 total = have_range ? space : (lows.size() << (48 - L));
    if (have_range) { L = 0; lows.assign(1, 0); }
    LOG("# lifting: L=%d бит, %zu вариантов младших бит (%.4f с); объём перебора %.3e W (%s)\n", L, lows.size(), t_lift, (double)total, have_range ? "диапазон --range" : (L ? "с lifting" : "полный перебор 2^48"));

    std::vector<u64> found; unsigned long long total_found = 0;
    bool use_gpu = false; int sms = 0; std::string gpuname;
    if (dev == "gpu" || dev == "auto") use_gpu = have_gpu(&sms, &gpuname);
    if (dev == "gpu" && !use_gpu) { fprintf(stderr, "GPU запрошен, но недоступен\n"); return 3; }
    double t_search0 = now_s(), t_kernel = 0;
    if (lows.empty()) LOG("# после lifting кандидатов нет\n");
    else if (use_gpu) {
#ifndef NO_CUDA
        cudaFree(0);
        LOG("# GPU: %s (%d SM), инициализация %.3f с\n", gpuname.c_str(), sms, now_s() - t_search0);
        t_search0 = now_s();
        u64 *d_lows, *d_res; unsigned long long *d_nres;
        CK(cudaMalloc(&d_lows, lows.size() * 8)); CK(cudaMalloc(&d_res, (size_t)RESCAP * 8)); CK(cudaMalloc(&d_nres, 8));
        CK(cudaMemcpy(d_lows, lows.data(), lows.size() * 8, cudaMemcpyHostToDevice)); CK(cudaMemset(d_nres, 0, 8));
        CK(cudaMemcpyToSymbol(c_preds, preds.data(), preds.size() * sizeof(Pred))); int np = (int)preds.size(); CK(cudaMemcpyToSymbol(c_np, &np, 4));
        CK(cudaMemcpyToSymbol(c_ms, &mf, sizeof mf)); CK(cudaMemcpyToSymbol(c_usefast, &usefast, 4));
        int blocks = sms * 16, thr_ = 256;
        u64 chunk = 1ULL << 34;
        k_engine<<<1, 1>>>(d_lows, L, 0, 0, 0, d_res, d_nres, RESCAP); CK(cudaDeviceSynchronize());   /* прогрев: загрузка модуля */
        cudaEvent_t e0, e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1)); CK(cudaEventRecord(e0));
        for (u64 off = 0; off < total; off += chunk) {
            u64 cnt = std::min(chunk, total - off);
            k_engine<<<blocks, thr_>>>(d_lows, L, have_range ? range_base : 0, off, cnt, d_res, d_nres, RESCAP);
            CK(cudaGetLastError()); CK(cudaDeviceSynchronize());
            { static int lastp = -1; int pct = (int)(100.0 * (off + cnt) / total); if (progress && pct / 2 != lastp) { lastp = pct / 2;
                fprintf(stderr, "# прогресс: %d%% (%.0f с, осталось ~%.0f с)\n", pct, now_s() - t_search0, (now_s() - t_search0) * (total - off - cnt) / (double)(off + cnt)); fflush(stderr); } }
            if (!quiet && isatty(2) && total > 4 * chunk) fprintf(stderr, "# прогресс: %.1f%% (%.0f с, ~%.0f с осталось)\r", 100.0 * (off + cnt) / total, now_s() - t_search0, (now_s() - t_search0) * (total - off - cnt) / (double)(off + cnt));
        }
        CK(cudaEventRecord(e1)); CK(cudaEventSynchronize(e1));
        float ms; CK(cudaEventElapsedTime(&ms, e0, e1)); t_kernel = ms / 1000.0;
        CK(cudaMemcpy(&total_found, d_nres, 8, cudaMemcpyDeviceToHost));
        size_t nr = std::min<unsigned long long>(total_found, RESCAP); found.resize(nr);
        if (nr) CK(cudaMemcpy(found.data(), d_res, nr * 8, cudaMemcpyDeviceToHost));
        cudaFree(d_lows); cudaFree(d_res); cudaFree(d_nres);
#endif
    } else {
        LOG("# CPU: OpenMP, %d потоков\n", omp_get_max_threads());
        unsigned long long cnt_total = 0; std::vector<u64> all;
        u64 nblocks = (total + 65535) >> 16;
        u64 hmask = (L >= 48) ? ~0ULL : ((1ULL << (48 - L)) - 1);
        #pragma omp parallel
        {
            std::vector<u64> loc;
            #pragma omp for schedule(dynamic, 16)
            for (long long b = 0; b < (long long)nblocks; b++) {
                u64 i0 = (u64)b << 16, i1 = std::min<u64>(total, i0 + 65536);
                for (u64 i = i0; i < i1; i++) {
                    u64 W = (have_range ? range_base : 0) + (((i & hmask) << L) | lows[i >> (48 - L)]);
                    int k0 = 0;
                    if (usefast) { if (!ms_fast((u32)W, (u32)(W >> 32), mf)) continue; k0 = 1; }
                    bool ok = true; for (size_t k = k0; k < preds.size() && ok; k++) ok = pred_check(W, preds[k]);
                    if (ok) loc.push_back(W);
                }
            }
            #pragma omp critical
            { all.insert(all.end(), loc.begin(), loc.end()); cnt_total += loc.size(); }
        }
        found = all; total_found = cnt_total; t_kernel = now_s() - t_search0;
    }
    double t_total = now_s() - t0;
    std::sort(found.begin(), found.end());
    if (int32only) { size_t n0 = found.size(); found.erase(std::remove_if(found.begin(), found.end(), [](u64 w) { return !is_int32_seed(w); }), found.end()); LOG("# --int32: %zu -> %zu кандидатов\n", n0, found.size()); total_found = found.size(); }
    size_t verified = 0; bool hit = false;
    for (u64 W : found) { bool ok = true; for (auto &p : preds) if (!pred_check(W, p)) { ok = false; break; } if (ok) verified++; if (have_true && W == true_seed) hit = true; }
    FILE *fo = outfile.empty() ? nullptr : fopen(outfile.c_str(), "w");
    unsigned long long printed = 0;
    for (u64 W : found) {
        if (printed >= limit) break;
        printf("%llu 0x%012llx", (unsigned long long)W, (unsigned long long)W); if (fo) fprintf(fo, "%llu 0x%012llx", (unsigned long long)W, (unsigned long long)W);
        if (expand) for (i64 w : random_world_seeds(W)) { printf(" %lld", (long long)w); if (fo) fprintf(fo, " %lld", (long long)w); }
        printf("\n"); if (fo) fprintf(fo, "\n"); printed++;
    }
    if (fo) fclose(fo);
    LOG("# найдено кандидатов: %llu (проверено на хосте: %zu из %zu)\n", total_found, verified, found.size());
    LOG("# время: lifting %.4f с, перебор %.3f с (%s; %.3e W/с), всего %.3f с\n", t_lift, t_kernel, use_gpu ? "GPU" : "CPU", t_kernel > 0 ? (double)total / t_kernel : 0.0, t_total);
    if (have_true) {
        bool inrange = !have_range || (true_seed >= range_base && true_seed < range_base + (1ULL << range_bits));
        LOG("# тест: истинный seed %llu (0x%012llx) %s%s\n", (unsigned long long)true_seed, (unsigned long long)true_seed, hit ? "НАЙДЕН" : "НЕ найден", inrange ? "" : " (вне диапазона --range)");
        if (!hit && inrange) return 1;
    }
    return 0;
}

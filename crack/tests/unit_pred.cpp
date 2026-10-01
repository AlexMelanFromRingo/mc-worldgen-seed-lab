/* Юнит-тесты математики crack-struct (host): fastmod, тест делимости, draw_lim, пороги reducers, pred_eval против эталонной транскрипции Java.
 * Сборка/запуск: g++ -O2 -std=c++17 -I../src -o /tmp/unit_pred unit_pred.cpp && /tmp/unit_pred <data-dir>   (make -C crack unit) */
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../src/sets.h"

static u64 rs = 88172645463325252ULL;
static u64 rnd64() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static long fails = 0;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } } while (0)

static u32 rotr32h(u32 x, int t) { return t ? (x >> t) | (x << (32 - t)) : x; }

int main(int argc, char **argv) {
    std::string data = argc > 1 ? argv[1] : "../../data";
    /* 1. fastmod31 и тест делимости для всех lim 2..4096 */
    for (i32 lim = 2; lim <= 4096; lim++) {
        Pred P; memset(&P, 0, sizeof P); pred_set_lim(P, lim);
        for (int k = 0; k < 300; k++) {
            u32 r = (u32)(rnd64() & 0x7FFFFFFF);
            if (k < 20) r = 0x7FFFFFFFu - (u32)k;         /* граница отбраковки */
            else if (k < 40) r = (u32)(k - 20);
            CHECK(fastmod31(r, P) == r % (u32)lim, "fastmod lim=%d r=%u", lim, r);
            if (P.pow2k < 0) {
                i32 ox = (i32)(rnd64() % (u64)lim);
                if (r >= (u32)ox) {
                    bool div = rotr32h((r - (u32)ox) * P.invq, P.t2) <= P.divbound;
                    CHECK(div == (((r - (u32)ox) % (u32)lim) == 0), "divtest lim=%d r=%u ox=%d", lim, r, ox);
                }
            }
        }
        /* draw_lim против j_nextInt (включая редкий путь отбраковки) */
        for (int k = 0; k < 200; k++) {
            u64 s = rnd64() & K_MASK48;
            if (k < 10) s = ((0x7FFFFFFFULL - (u64)k * 3 - (u64)(rnd64() % (u64)lim)) << 17) & K_MASK48;   /* r у границы */
            u64 a = s, b = s;
            i32 x = draw_lim(&a, P), y = j_nextInt(&b, lim);
            CHECK(x == y && a == b, "draw_lim lim=%d", lim);
        }
    }
    /* 2. пороги reducers: next(24) < T24 <=> (float)r*2^-24 < f ; m53 < T53 <=> double */
    const float fs[] = {0.2f, 0.01f, 0.004f, 0.5f, 0.3f, 0.123456f, 0.9999f, 1.0f / 3};
    for (float f : fs) {
        u32 T24 = (u32)std::ceil((double)f * 16777216.0);
        u64 T53 = (u64)std::ceil((double)f * 9007199254740992.0);
        for (i64 d = -3; d <= 3; d++) {
            i64 r = (i64)T24 + d; if (r < 0 || r >= (1 << 24)) continue;
            CHECK(((float)(i32)r * 5.9604645E-8f < f) == ((u32)r < T24), "T24 f=%g r=%lld", f, (long long)r);
            i64 m = (i64)T53 + d; if (m < 0 || m >= (1LL << 53)) continue;
            CHECK(((double)m * 1.1102230246251565E-16 < (double)f) == ((u64)m < T53), "T53 f=%g m=%lld", f, (long long)m);
        }
    }
    /* 3. pred_eval против host-эталона для всех версий, всех random_spread наборов */
    for (const char *ver : {"26.1", "26.2", "26.3"}) {
        SetTable T; T.load(data + "/structure_sets-" + ver + ".json", ver);
        long nchk = 0;
        for (size_t si = 0; si < T.sets.size(); si++) {
            const StructSet &S = T.sets[si]; if (S.type != "random_spread") continue;
            for (int it = 0; it < 400; it++) {
                u64 W = rnd64() & K_MASK48;
                /* найдём чанк, где набор «стартует» (через эталон) */
                i32 cx = 0, cz = 0; bool ok = false;
                for (int t = 0; t < 4000 && !ok; t++) {
                    if (S.lim() > 1) { i32 rx = (i32)(rnd64() % 200) - 100, rz = (i32)(rnd64() % 200) - 100; host_potential(S, W, rx * S.spacing, rz * S.spacing, &cx, &cz); }
                    else { cx = (i32)(rnd64() % 20000) - 10000; cz = (i32)(rnd64() % 20000) - 10000; }
                    ok = host_is_structure_chunk(T, (int)si, W, cx, cz);
                }
                if (!ok) continue;
                std::vector<Obs> obs = {{S.id, (int)si, cx, cz, 1}}; Problem P; std::string err;
                CHECK(build_problem(T, obs, P, err), "build %s: %s", S.id.c_str(), err.c_str());
                if (P.preds.empty()) continue;      /* nether_fossils: нет информации о seed */
                for (const Pred &p : P.preds) { CHECK(pred_eval(W, p), "%s %s: истинный seed не проходит предикат kind=%d", ver, S.id.c_str(), p.kind); nchk++; }
                /* соседние seed почти всегда не проходят (вероятность 1/lim^2 и пр.): допускаем редкие совпадения */
                int pass = 0; for (int k = 1; k <= 64; k++) { bool all = true; for (const Pred &p : P.preds) all = all && pred_eval(W + (u64)k * 0x9E3779B97F4A7C15ULL, p); pass += all; }
                CHECK(pass <= 12, "%s %s: слишком много ложных совпадений (%d/64)", ver, S.id.c_str(), pass);
                /* согласованность host_check_all с host_is_structure_chunk (с exclusion) */
                CHECK(host_check_all(T, P, W), "%s %s: host_check_all(W) == false для реального чанка", ver, S.id.c_str());
                /* lifting: истинные младшие биты проходят low_ok-ограничения (копия логики low_ok из crack_struct.cu) */
                for (const Pred &p : P.preds) if (p.kind == PK_LIN && p.tz > 0) {
                    int L = 17 + p.tz; u64 mask = (1ULL << L) - 1, lo = W & mask;
                    u64 s = ((lo + p.cst) & mask) ^ (K_MUL & mask), s1 = (s * K_MUL + K_ADD) & mask, s2 = (s1 * K_MUL + K_ADD) & mask;
                    u64 m = (1ULL << p.tz) - 1;
                    CHECK(((s1 >> 17) & m) == ((u64)p.ox & m) && ((s2 >> 17) & m) == ((u64)p.oz & m), "%s %s: lifting low bits не согласованы", ver, S.id.c_str());
                }
            }
        }
        printf("unit_pred %s: проверено предикатов %ld\n", ver, nchk);
    }
    printf(fails ? "unit_pred: ПРОВАЛ (%ld)\n" : "unit_pred: OK (%ld ошибок)\n", fails);
    return fails ? 1 : 0;
}

/*
 * crack-pillars — восстановление 48-битного structure seed по обсидиановым башням Края (End spikes), 26.1 / 26.2 / 26.3.
 *
 * Механика игры (EndSpikeFeature.java:48-52 и SpikeCacheLoader, одинаково во всех трёх версиях):
 *     key   = SingleThreadedRandomSource(levelSeed).nextLong() & 65535                (16 бит, функция seed mod 2^48)
 *     sizes = Util.toShuffledList(IntStream.range(0,10), SingleThreadedRandomSource(key))   (перетасовка Фишера-Йетса)
 *     башня i (центр (42cos, 42sin) — фиксированная раскладка): size = sizes[i], радиус = 2 + size/3,
 *     высота (Y блока бедрока под кристаллом) = 76 + 3*size, клетка из железных решёток: size in {1,2}
 * Шаг 1: перебор 2^16 ключей -> список ключей, согласованных с наблюдёнными башнями (обычно 1; ~1.8% ключей неоднозначны).
 * Шаг 2: nextLong() & 65535 = биты 16..31 второго состояния LCG, то есть зависят лишь от младших 32 бит W:
 *        для данного key ровно 2^16 значений младших 32 бит W (аффинная биекция) x 2^16 свободных старших бит = 2^32 seed.
 *        Перебираем 2^32 (GPU, ~0.03 с без lifting) и фильтруем предикатами (--with-structs: структуры/слайм-чанки).
 *        Lifting: если есть «liftable» структуры, заранее отсекаем младшие биты (необходимое условие на W mod 2^20).
 *
 * Сборка: crack/Makefile.  Запуск: crack-pillars --heights 97,103,91,76,94,100,79,88,85,82 --with-structs obs.txt
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

/* раскладка башен: индекс -> центр (x,z); (x,z)=floor(42*cos/sin(2*(-pi + pi/10*i))); сверено с oracle `pillars` (tests/) */
static const int SPX[10] = {42, 33, 12, -13, -34, -42, -34, -13, 12, 33};
static const int SPZ[10] = {0, 24, 39, 39, 24, -1, -25, -40, -40, -25};

#ifndef NO_CUDA
__constant__ Pred c_preds[MAXP];
__constant__ int c_np;

__global__ void k_pillars(const u32 *__restrict__ xs, u64 base, u64 count, u64 *res, unsigned long long *nres, unsigned cap) {
    u64 stride = (u64)gridDim.x * blockDim.x;
    u64 end = base + count;
    for (u64 i = base + (u64)blockIdx.x * blockDim.x + threadIdx.x; i < end; i += stride) {
        u64 W = ((i & 0xFFFFULL) << 32) | (u64)xs[i >> 16];
        bool ok = true;
        for (int k = 0; k < c_np && ok; k++) ok = pred_check(W, c_preds[k]);
        if (ok) { unsigned long long p = atomicAdd(nres, 1ULL); if (p < cap) res[p] = W; }
    }
}
#endif

/* младшие 32 бита W для key: из nextLong() & 65535 == key. s2 = s0*M^2 + B*(M+1) (mod 2^48); биты 16..31 s2 == key */
static void key_low32_set(u32 key, std::vector<u32> &out) {
    const u64 M2 = (LCG_MUL * LCG_MUL) & MASK48;
    const u64 C2 = (LCG_ADD * (LCG_MUL + 1)) & MASK48;
    const u32 M2inv = [&] { u32 x = (u32)M2; u32 y = x; for (int i = 0; i < 5; i++) y *= 2 - x * y; return y; }();   /* обратный по модулю 2^32 */
    out.clear(); out.reserve(65536);
    for (u32 x = 0; x < 65536; x++) {
        u32 t = (key << 16) | x;                                   /* s2 mod 2^32 с битами 16..31 == key */
        u32 s0 = (t - (u32)C2) * M2inv;                            /* s0 mod 2^32 */
        out.push_back(s0 ^ (u32)LCG_MUL);                          /* W = s0 ^ M (на младших 32 битах) */
    }
}

/* разбор наблюдаемых башен -> маски допустимых size (по 10 бит на башню) */
struct Towers { unsigned mask[10]; int known; Towers() { for (int i = 0; i < 10; i++) mask[i] = 0x3FF; known = 0; } };

static bool tower_token(Towers &T, int idx, const std::string &tok, std::string &err) {
    if (idx < 0 || idx > 9) { err = "индекс башни вне 0..9"; return false; }
    std::string t = StructDB::strip(tok);
    unsigned m = 0x3FF;
    if (t == "?" || t == "-" || t == "unknown" || t == "x") return true;
    if (t == "cage" || t == "guarded" || t == "g") m = (1u << 1) | (1u << 2);
    else if (t == "nocage" || t == "unguarded") m = 0x3FF & ~((1u << 1) | (1u << 2));
    else if (t[0] == 'r' && t.size() >= 2) {
        int r = atoi(t.c_str() + 1); m = 0;
        for (int s = 0; s < 10; s++) if (2 + s / 3 == r) m |= 1u << s;
        if (!m) { err = "радиус вне 2..5"; return false; }
    } else {
        long long h;
        if (!parse_int(t, h)) { err = "не число: " + tok; return false; }
        if (h < 76 || h > 103 || (h - 76) % 3) { err = "высота должна быть 76+3k, k=0..9: " + tok; return false; }
        m = 1u << ((h - 76) / 3);
    }
    T.mask[idx] &= m;
    T.known++;
    if (!T.mask[idx]) { err = "противоречивые наблюдения для башни " + std::to_string(idx); return false; }
    return true;
}

static void usage() {
    fprintf(stderr,
"crack-pillars — structure seed (48 бит) по башням Края и дополнительным наблюдениям (26.1/26.2/26.3)\n"
"Использование: crack-pillars (--heights h0,...,h9 | --obs FILE | --key K | --gen SEED) [опции]\n"
"Наблюдения башен:\n"
"  --heights L      10 значений через запятую в порядке индексов башен (таблица: --print-layout). Значение: высота (76..103, шаг 3;\n"
"                   Y блока бедрока под кристаллом), либо ? (неизвестно), rN (радиус башни 2..5), cage / nocage (железная клетка)\n"
"  --obs FILE       строки \"x z значение\" (центр башни, допуск: манхэттенское расстояние ≤ 16) или \"индекс значение\"; значение как выше\n"
"  --key K          задать pillar key (0..65535) напрямую, пропустив шаг 1\n"
"  --print-layout   напечатать раскладку (индекс -> x z) и выйти;  --print-spikes SEED: башни для seed (для сверки)\n"
"Шаг 2 (2^32 seed на ключ): фильтр-предикаты\n"
"  --with-structs FILE  наблюдения структур `<structure_set>;cx;cz` и `slime;cx;cz;0|1` (параметры — data/structure_sets-<версия>.json)\n"
"  --candidates FILE    вместо перебора: проверить список seed (по одному в строке, 48 младших бит)\n"
"  --version V          26.1|26.2|26.3 (по умолчанию 26.3; для башен результат одинаков); --data-dir D; --sets F\n"
"  --keys-only          только шаг 1;  --count-only: число кандидатов без вывода;  --list N: без предикатов вывести N seed ключа\n"
"  --dev auto|gpu|cpu   (по умолчанию auto);  --threads N;  --limit N (по умолчанию 1000000);  --out FILE;  --expand-random\n"
"  --force              не отказываться от задач, где на ключ ожидается > 5e7 кандидатов (мало предикатов)\n"
"  --int32              оставить только seed, представимые 32-битным целым (биты 32..47 все 0 или 1): набранные числа, String.hashCode\n"
"  --no-lift            не применять lifting младших бит (для сравнения скорости)\n"
"  --max-keys N         максимум ключей для шага 2 (по умолчанию 64)\n"
"  --check-seed S       (отладка) проверить seed S (48 бит) на предикатах из --with-structs: по строке \"PASS|FAIL kind cx cz\"; код 0 если все PASS\n"
"  --scan-set S SET CX0 CZ0 NX NZ   (отладка) перечислить чанки окна, где seed S даёт старт structure_set SET (потенциальный чанк + reducer,\n"
"                       без exclusion_zone и биомов) — для сверки с oracle `structs`\n"
"  --selftest           внутренняя самопроверка (множество младших 32 бит ключа, random seeds, уникальность раскладок)\n"
"  --gen SEED[,набор+набор+...]   синтетический тест из известного seed: башни + по одному наблюдению каждого structure_set (slime:N — N слайм-чанков);\n"
"                       пример: --gen 123456789,desert_pyramids+igloos+villages+shipwrecks+slime:10 (файл наблюдений: CRACK_GEN_OUT=файл)\n"
"  --gen-rng R          затравка выбора регионов при --gen;  --quiet\n"
"Вывод (stdout): \"<seed48 десятичный> 0x<hex> key=<K> [random seeds]\"; служебное — stderr.\n");
}

int main(int argc, char **argv) {
    std::string heights, obsfile, withfile, candfile, dev = "auto", version = "26.3", outfile, datadir, setspath, genspec;
    long long key_given = -1; unsigned long long print_spikes_seed = 0;
    unsigned long long limit = 1000000, listn = 0, gen_rng = 4242; int threads = 0, maxkeys = 64;
    unsigned long long check_seed = 0, scan_seed = 0; bool have_check = false, scan = false; std::string scan_set; int scan_w[4] = {0};
    bool force = false, int32only = false, selftest = false, keysonly = false, countonly = false, expand = false, quiet = false, nolift = false, layout = false, printspikes = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](int n) { if (i + n >= argc) { fprintf(stderr, "опция %s требует %d арг.\n", a.c_str(), n); exit(2); } };
        if (a == "--heights") { need(1); heights = argv[++i]; }
        else if (a == "--obs") { need(1); obsfile = argv[++i]; }
        else if (a == "--key") { need(1); key_given = atoll(argv[++i]); }
        else if (a == "--with-structs") { need(1); withfile = argv[++i]; }
        else if (a == "--candidates") { need(1); candfile = argv[++i]; }
        else if (a == "--dev") { need(1); dev = argv[++i]; }
        else if (a == "--version") { need(1); version = argv[++i]; }
        else if (a == "--data-dir") { need(1); datadir = argv[++i]; }
        else if (a == "--sets") { need(1); setspath = argv[++i]; }
        else if (a == "--out") { need(1); outfile = argv[++i]; }
        else if (a == "--limit") { need(1); limit = strtoull(argv[++i], 0, 0); }
        else if (a == "--list") { need(1); listn = strtoull(argv[++i], 0, 0); }
        else if (a == "--threads") { need(1); threads = atoi(argv[++i]); }
        else if (a == "--max-keys") { need(1); maxkeys = atoi(argv[++i]); }
        else if (a == "--gen") { need(1); genspec = argv[++i]; }
        else if (a == "--gen-rng") { need(1); gen_rng = strtoull(argv[++i], 0, 0); }
        else if (a == "--keys-only") keysonly = true;
        else if (a == "--count-only") countonly = true;
        else if (a == "--expand-random") expand = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--no-lift") nolift = true;
        else if (a == "--int32") int32only = true;
        else if (a == "--force") force = true;
        else if (a == "--print-layout") layout = true;
        else if (a == "--selftest") selftest = true;
        else if (a == "--check-seed") { need(1); parse_u64(argv[++i], check_seed); have_check = true; }
        else if (a == "--scan-set") { need(6); scan = true; parse_u64(argv[++i], scan_seed); scan_set = argv[++i]; for (int q = 0; q < 4; q++) scan_w[q] = atoi(argv[++i]); }
        else if (a == "--print-spikes") { need(1); printspikes = true; parse_u64(argv[++i], print_spikes_seed); }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "неизвестная опция %s\n", a.c_str()); usage(); return 2; }
    }
#define LOG(...) do { if (!quiet) fprintf(stderr, __VA_ARGS__); } while (0)
    if (threads > 0) omp_set_num_threads(threads);
    if (selftest) {
        Rng rng(5); long bad = 0, n = 0;
        for (int it = 0; it < 300; it++) {                      /* 1) key_low32_set: все W этого множества дают нужный key */
            u32 k = (u32)rng.range(65536); std::vector<u32> xs; key_low32_set(k, xs);
            for (int j = 0; j < 400; j++) { u64 W = ((u64)rng.range(65536) << 32) | xs[rng.range(65536)]; n++; if (pillar_key_of(W) != k) bad++; }
        }
        for (int it = 0; it < 200; it++) {                      /* 2) обратное: W случайный => его low32 лежит в множестве ключа */
            u64 W = rng.next() & MASK48; u32 k = pillar_key_of(W); std::vector<u32> xs; key_low32_set(k, xs);
            n++; if (std::find(xs.begin(), xs.end(), (u32)W) == xs.end()) bad++;
        }
        long rbad = 0;                                          /* 3) random_world_seeds: восстанавливает seed вида nextLong() */
        for (int it = 0; it < 300; it++) {
            u64 st0 = lcg_scramble(rng.next()); i64 w2 = lcg_nextLong(&st0);   /* RandomSource.create(x).nextLong() */
            auto v = random_world_seeds((u64)w2 & MASK48); n++; if (std::find(v.begin(), v.end(), w2) == v.end()) rbad++;
        }
        /* 4) число различных раскладок среди 65536 ключей */
        std::vector<u64> perms; for (u32 k = 0; k < 65536; k++) { int sz[10]; pillar_sizes(k, sz); u64 h = 0; for (int i = 0; i < 10; i++) h = h * 10 + sz[i]; perms.push_back(h); }
        std::vector<u64> tmp = perms; std::sort(tmp.begin(), tmp.end()); size_t amb = 0, distinct = 0;
        for (size_t i = 0; i < tmp.size();) { size_t j = i; while (j < tmp.size() && tmp[j] == tmp[i]) j++; distinct++; if (j - i > 1) amb += j - i; i = j; }
        fprintf(stderr, "selftest pillars: key_low32_set %ld/%ld расхождений; random_world_seeds %ld/300; различных раскладок %zu из 65536, ключей с неоднозначной раскладкой: %zu (%.2f%%)\n", bad, n, rbad, distinct, amb, 100.0 * amb / 65536);
        return (bad || rbad) ? 1 : 0;
    }
    if (layout) { for (int i = 0; i < 10; i++) printf("%d %d %d\n", i, SPX[i], SPZ[i]); return 0; }
    if (printspikes) {
        u32 key = pillar_key_of(print_spikes_seed & MASK48); int sz[10]; pillar_sizes(key, sz);
        printf("key=%u\n", key);
        for (int i = 0; i < 10; i++) printf("%d %d %d height=%d radius=%d guarded=%d\n", i, SPX[i], SPZ[i], 76 + 3 * sz[i], 2 + sz[i] / 3, (sz[i] == 1 || sz[i] == 2) ? 1 : 0);
        return 0;
    }

    /* ---------- структуры / синтетика ---------- */
    StructDB db; { std::string err; if (!load_structdb(db, version, setspath, datadir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; } if (!err.empty() && !quiet) fprintf(stderr, "%s\n", err.c_str()); }
    if (scan) {
        const SetDef *d = db.find(scan_set);
        if (!d || !d->spread) { fprintf(stderr, "неизвестный/неподдерживаемый набор %s\n", scan_set.c_str()); return 2; }
        u64 W = scan_seed & MASK48; long n = 0;
        for (int cz = scan_w[1]; cz < scan_w[1] + scan_w[3]; cz++) for (int cx = scan_w[0]; cx < scan_w[0] + scan_w[2]; cx++) {
            i32 gx = floordiv_i32(cx, d->spacing), gz = floordiv_i32(cz, d->spacing), ox, oz;
            if (d->spacing - d->sep > 1) { spread_offset(W, gx, gz, d->spacing, d->sep, d->salt, d->type, &ox, &oz); if (gx * d->spacing + ox != cx || gz * d->spacing + oz != cz) continue; }
            else if (cx != gx * d->spacing || cz != gz * d->spacing) continue;
            if (!reducer_pass(W, d->red, d->salt, cx, cz, d->freq, (i32)(1.0f / d->freq))) continue;
            printf("%d %d\n", cx, cz); n++;
        }
        LOG("# %ld чанков\n", n);
        return 0;
    }
    std::vector<Pred> preds;
    bool have_true = false; u64 true_seed = 0;
    Towers T;
    if (!genspec.empty()) {
        std::string sp = genspec, sets;
        size_t c = sp.find(','); if (c != std::string::npos) { sets = sp.substr(c + 1); sp = sp.substr(0, c); }
        unsigned long long S; if (!parse_u64(sp.c_str(), S)) { fprintf(stderr, "--gen: неверный seed\n"); return 2; }
        true_seed = S & MASK48; have_true = true;
        int sz[10]; pillar_sizes(pillar_key_of(true_seed), sz);
        std::string h; for (int i = 0; i < 10; i++) { std::string tk = std::to_string(76 + 3 * sz[i]); T.mask[i] = 1u << sz[i]; T.known++; h += (i ? "," : "") + tk; }
        LOG("# --gen: seed48=%llu key=%u башни: %s\n", (unsigned long long)true_seed, pillar_key_of(true_seed), h.c_str());
        Rng rng(gen_rng ^ (S * 0x9E3779B97F4A7C15ULL));
        std::string cur; sets += "+";
        FILE *gf = nullptr; { const char *g = getenv("CRACK_GEN_OUT"); if (g) gf = fopen(g, "w"); }
        for (char ch : sets) {
            if (ch != '+') { cur += ch; continue; }
            if (cur.empty()) continue;
            std::string nm = cur; cur.clear();
            if (nm.compare(0, 5, "slime") == 0) {             /* slime или slime:N — N положительных чанков */
                int n = 1; size_t cp = nm.find(':'); if (cp != std::string::npos) n = atoi(nm.c_str() + cp + 1);
                for (int k = 0; k < n; ) { i32 x = rng.range(200) - 100, z = rng.range(200) - 100; if (!is_slime_chunk(true_seed, x, z)) continue;
                    Pred p; make_slime_pred(x, z, 0, p); preds.push_back(p); if (gf) fprintf(gf, "slime;%d;%d;1\n", x, z); k++; }
                continue;
            }
            const SetDef *d = db.find(nm);
            if (!d || !d->spread) { fprintf(stderr, "--gen: неизвестный/неподдерживаемый набор %s\n", nm.c_str()); return 2; }
            /* ищем регион, где потенциальный чанк проходит reducer */
            for (int tries = 0; tries < 100000; tries++) {
                i32 gx = rng.range(60) - 30, gz = rng.range(60) - 30;
                i32 ox, oz; spread_offset(true_seed, gx, gz, d->spacing, d->sep, d->salt, d->type, &ox, &oz);
                i32 cx = gx * d->spacing + ox, cz = gz * d->spacing + oz;
                if (!reducer_pass(true_seed, d->red, d->salt, cx, cz, d->freq, (i32)(1.0f / d->freq))) continue;
                Pred p; std::string e; if (!make_spread_pred(*d, cx, cz, p, e)) { fprintf(stderr, "%s\n", e.c_str()); return 2; }
                preds.push_back(p); if (gf) fprintf(gf, "%s;%d;%d\n", d->key.c_str(), cx, cz);
                LOG("# --gen: %s;%d;%d\n", d->key.c_str(), cx, cz);
                break;
            }
        }
        if (gf) fclose(gf);
    } else {
        if (!heights.empty()) {
            std::vector<std::string> tk; std::string cur;
            for (char ch : heights + ",") { if (ch == ',') { tk.push_back(cur); cur.clear(); } else cur += ch; }
            if (tk.size() != 10) { fprintf(stderr, "--heights: нужно ровно 10 значений (получено %zu)\n", tk.size()); return 2; }
            for (int i = 0; i < 10; i++) { std::string e; if (!tower_token(T, i, tk[i], e)) { fprintf(stderr, "--heights[%d]: %s\n", i, e.c_str()); return 2; } }
        }
        if (!obsfile.empty()) {
            std::ifstream f(obsfile); if (!f) { fprintf(stderr, "не могу открыть %s\n", obsfile.c_str()); return 2; }
            std::string line; int ln = 0;
            while (std::getline(f, line)) {
                ln++; auto t = split_tokens(line); if (t.empty()) continue;
                int idx = -1; std::string tok, e; long long a, b;
                if (t.size() >= 3 && parse_int(t[0], a) && parse_int(t[1], b)) {
                    int best = 1 << 30;
                    for (int i = 0; i < 10; i++) { int d = (int)(llabs(a - SPX[i]) + llabs(b - SPZ[i])); if (d < best) { best = d; idx = i; } }
                    if (best > 16) { fprintf(stderr, "%s:%d: (%lld,%lld) не похоже на центр башни (ближайшая — %d, расстояние %d)\n", obsfile.c_str(), ln, a, b, idx, best); return 2; }
                    tok = t[2];
                } else if (t.size() >= 2 && parse_int(t[0], a)) { idx = (int)a; tok = t[1]; }
                else { fprintf(stderr, "%s:%d: ожидалось \"x z значение\" или \"индекс значение\"\n", obsfile.c_str(), ln); return 2; }
                if (!tower_token(T, idx, tok, e)) { fprintf(stderr, "%s:%d: %s\n", obsfile.c_str(), ln, e.c_str()); return 2; }
            }
        }
        if (!withfile.empty()) {
            std::string perr;
            if (parse_obs_file(withfile, db, preds, perr)) { fprintf(stderr, "%s", perr.c_str()); return 2; }
        }
    }

    if (have_check) {
        int bad = 0; u64 W = check_seed & MASK48;
        for (auto &p : preds) { bool ok = pred_check(W, p); if (!ok) bad++; printf("%s %s %d %d\n", ok ? "PASS" : "FAIL", p.kind == PK_SLIME ? (p.neg ? "noslime" : "slime") : "struct", p.cx, p.cz); }
        LOG("# проверено %zu предикатов, FAIL: %d\n", preds.size(), bad);
        return bad ? 1 : 0;
    }

    /* ---------- шаг 1: ключи ---------- */
    std::vector<u32> keys;
    double t0 = now_s();
    if (key_given >= 0) { if (key_given > 65535) { fprintf(stderr, "--key вне 0..65535\n"); return 2; } keys.push_back((u32)key_given); }
    else {
        if (T.known == 0) { fprintf(stderr, "нет наблюдений башен (--heights/--obs/--key/--gen)\n"); usage(); return 2; }
        for (u32 k = 0; k < 65536; k++) {
            int sz[10]; pillar_sizes(k, sz);
            bool ok = true; for (int i = 0; i < 10 && ok; i++) ok = (T.mask[i] >> sz[i]) & 1;
            if (ok) keys.push_back(k);
        }
    }
    double t_keys = now_s() - t0;
    LOG("# наблюдено башен: %d из 10; шаг 1 (перебор 2^16 ключей): %zu ключ(ей) за %.4f с:", T.known, keys.size(), t_keys);
    for (size_t i = 0; i < keys.size() && i < 16; i++) LOG(" %u", keys[i]);
    LOG("%s\n", keys.size() > 16 ? " ..." : "");
    if (have_true && std::find(keys.begin(), keys.end(), pillar_key_of(true_seed)) == keys.end()) { fprintf(stderr, "ОШИБКА: истинный key не найден\n"); return 1; }
    if (keys.empty()) { fprintf(stderr, "ни один ключ не согласован с наблюдениями\n"); return 1; }
    if (keysonly) { for (u32 k : keys) printf("%u\n", k); return 0; }
    if ((int)keys.size() > maxkeys) { fprintf(stderr, "слишком много ключей (%zu > --max-keys %d): добавьте наблюдения башен\n", keys.size(), maxkeys); return 1; }

    sort_preds(preds);
    LOG("# предикатов шага 2: %zu (информации %.1f бит; ожидаемое число кандидатов на ключ ~ %.3g)\n", preds.size(), info_bits(preds), 4294967296.0 * [&] { double p = 1; for (auto &x : preds) p *= x.pass; return p; }());

    if (candfile.empty() && !preds.empty() && !countonly && !force) {
        double ex2 = 4294967296.0; for (auto &x : preds) ex2 *= x.pass;
        if (ex2 > 5e7) { fprintf(stderr, "Слишком мало информации: на ключ ожидается ~%.3g кандидатов (нужно ~32 бита структур/слайм-чанков сверх ключа). "
                                          "Добавьте наблюдения; --force — всё равно искать, --count-only — только посчитать.\n", ex2); return 4; }
    }
    /* ---------- режим --candidates ---------- */
    std::vector<u64> found; std::vector<u32> found_key; unsigned long long total_found = 0;
    FILE *fo = outfile.empty() ? nullptr : fopen(outfile.c_str(), "w");
    if (!candfile.empty()) {
        std::ifstream f(candfile); if (!f) { fprintf(stderr, "не могу открыть %s\n", candfile.c_str()); return 2; }
        std::string line; long n = 0;
        while (std::getline(f, line)) {
            auto t = split_tokens(line); if (t.empty()) continue;
            unsigned long long v; if (!parse_u64(t[0].c_str(), v)) continue;
            u64 W = v & MASK48; n++;
            u32 k = pillar_key_of(W);
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) continue;
            bool ok = true; for (auto &p : preds) if (!pred_check(W, p)) { ok = false; break; }
            if (ok) { found.push_back(W); found_key.push_back(k); }
        }
        total_found = found.size();
        LOG("# проверено кандидатов из файла: %ld, подошло: %llu\n", n, total_found);
    } else {
        bool use_gpu = false; int sms = 0; std::string gpuname;
        if (dev == "gpu" || dev == "auto") use_gpu = have_gpu(&sms, &gpuname);
        if (dev == "gpu" && !use_gpu) { fprintf(stderr, "GPU запрошен, но недоступен\n"); return 3; }
        if (use_gpu) { double tg = now_s();
#ifndef NO_CUDA
            cudaFree(0);
#endif
            LOG("# GPU: %s (%d SM), инициализация контекста %.3f с\n", gpuname.c_str(), sms, now_s() - tg); } else LOG("# CPU: OpenMP, %d потоков\n", omp_get_max_threads());
        if ((int)preds.size() > MAXP) { fprintf(stderr, "слишком много предикатов (макс. %d)\n", MAXP); return 2; }
        if (preds.empty() && !countonly) {
            /* без предикатов: только список */
            for (u32 k : keys) {
                std::vector<u32> xs; key_low32_set(k, xs);
                LOG("# key %u: 2^32 = 4294967296 seed без дополнительных предикатов (W = hi16<<32 | low32[x], 2^16 x 2^16)\n", k);
                unsigned long long n = 0;
                for (u64 i = 0; i < (u64)listn && i < (1ULL << 32); i++) { u64 W = ((i & 0xFFFF) << 32) | xs[i >> 16]; found.push_back(W); found_key.push_back(k); n++; }
                total_found += (1ULL << 32);
            }
            if (!listn) LOG("# добавьте --with-structs/--candidates либо --list N\n");
        } else {
            double t_lift_tot = 0, t_search_tot = 0, t_kernel_tot = 0;
            for (u32 k : keys) {
                double ta = now_s();
                std::vector<u32> xs; key_low32_set(k, xs);
                size_t n0 = xs.size();
                int L = 0;
                if (!nolift && !preds.empty()) {
                    for (auto &p : preds) { int need = (p.kind == PK_SLIME && !p.neg) ? 18 : (p.kind == PK_SPREAD && p.tz > 0 ? 17 + p.tz : 0); L = std::max(L, need); }
                    L = std::min(L, 20);
                    if (L >= 18) {
                        std::vector<u32> keep; keep.reserve(xs.size());
                        u64 mask = (1ULL << L) - 1;
                        for (u32 w : xs) { bool ok = true; for (auto &p : preds) if (!pred_lowbits_ok((u64)w & mask, L, p)) { ok = false; break; } if (ok) keep.push_back(w); }
                        xs.swap(keep);
                    }
                }
                double tb = now_s(); t_lift_tot += tb - ta;
                u64 nitems = (u64)xs.size() << 16;
                LOG("# key %u: lifting (L=%d): %zu из %zu вариантов младших 32 бит; объём перебора %.3e\n", k, L, xs.size(), n0, (double)nitems);
                std::vector<u64> res; unsigned long long nres = 0;
                if (!xs.empty()) {
                    if (use_gpu) {
#ifndef NO_CUDA
                        u32 *d_xs; u64 *d_res; unsigned long long *d_nres;
                        CK(cudaMalloc(&d_xs, xs.size() * 4)); CK(cudaMalloc(&d_res, (size_t)RESCAP * 8)); CK(cudaMalloc(&d_nres, 8));
                        CK(cudaMemcpy(d_xs, xs.data(), xs.size() * 4, cudaMemcpyHostToDevice)); CK(cudaMemset(d_nres, 0, 8));
                        CK(cudaMemcpyToSymbol(c_preds, preds.data(), preds.size() * sizeof(Pred))); int np = (int)preds.size(); CK(cudaMemcpyToSymbol(c_np, &np, 4));
                        u64 chunk = 1ULL << 32;
                        k_pillars<<<1, 1>>>(d_xs, 0, 0, d_res, d_nres, RESCAP); CK(cudaDeviceSynchronize());   /* прогрев: загрузка модуля */
                        double tk0 = now_s();
                        for (u64 off = 0; off < nitems; off += chunk) {
                            k_pillars<<<sms * 16, 256>>>(d_xs, off, std::min(chunk, nitems - off), d_res, d_nres, RESCAP);
                            CK(cudaGetLastError()); CK(cudaDeviceSynchronize());
                        }
                        CK(cudaMemcpy(&nres, d_nres, 8, cudaMemcpyDeviceToHost));
                        { double tk = now_s() - tk0; t_kernel_tot += tk; LOG("# key %u: ядро GPU %.4f с (%.3e W/с)\n", k, tk, tk > 0 ? (double)nitems / tk : 0.0); }
                        size_t nr = std::min<unsigned long long>(nres, RESCAP); res.resize(nr);
                        if (nr) CK(cudaMemcpy(res.data(), d_res, nr * 8, cudaMemcpyDeviceToHost));
                        cudaFree(d_xs); cudaFree(d_res); cudaFree(d_nres);
#endif
                    } else {
                        #pragma omp parallel
                        {
                            std::vector<u64> loc;
                            #pragma omp for schedule(dynamic, 1)
                            for (long long xi = 0; xi < (long long)xs.size(); xi++)
                                for (u64 hi = 0; hi < 65536; hi++) {
                                    u64 W = (hi << 32) | xs[xi]; bool ok = true;
                                    for (size_t q = 0; q < preds.size() && ok; q++) ok = pred_check(W, preds[q]);
                                    if (ok) loc.push_back(W);
                                }
                            #pragma omp critical
                            res.insert(res.end(), loc.begin(), loc.end());
                        }
                        nres = res.size();
                    }
                }
                double tc = now_s(); t_search_tot += tc - tb;
                LOG("# key %u: найдено %llu кандидатов; перебор %.4f с\n", k, nres, tc - tb);
                total_found += nres;
                for (u64 W : res) { found.push_back(W); found_key.push_back(k); }
            }
            if (use_gpu) LOG("# время: шаг 1 %.4f с, lifting %.4f с, перебор %.4f с (в т.ч. чистое ядро %.4f с) (GPU)\n", t_keys, t_lift_tot, t_search_tot, t_kernel_tot);
            else LOG("# время: шаг 1 %.4f с, lifting %.4f с, перебор %.4f с (CPU)\n", t_keys, t_lift_tot, t_search_tot);
        }
    }
    /* хост-проверка + вывод */
    if (int32only) {
        std::vector<u64> f2; std::vector<u32> k2;
        for (size_t i = 0; i < found.size(); i++) if (is_int32_seed(found[i])) { f2.push_back(found[i]); k2.push_back(found_key[i]); }
        LOG("# --int32: %zu -> %zu кандидатов\n", found.size(), f2.size()); found.swap(f2); found_key.swap(k2); total_found = found.size();
    }
    std::vector<size_t> ord(found.size()); for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return found[a] < found[b]; });
    size_t verified = 0, printed = 0; bool hit = false;
    for (size_t oi : ord) {
        u64 W = found[oi]; u32 k = found_key[oi];
        bool ok = pillar_key_of(W) == k; for (auto &p : preds) if (ok && !pred_check(W, p)) ok = false;
        if (ok) verified++;
        if (have_true && W == true_seed) hit = true;
        if (printed < limit && !countonly) {
            printf("%llu 0x%012llx key=%u", (unsigned long long)W, (unsigned long long)W, k);
            if (fo) fprintf(fo, "%llu 0x%012llx key=%u", (unsigned long long)W, (unsigned long long)W, k);
            if (expand) for (i64 w : random_world_seeds(W)) { printf(" %lld", (long long)w); if (fo) fprintf(fo, " %lld", (long long)w); }
            printf("\n"); if (fo) fprintf(fo, "\n");
            printed++;
        }
    }
    if (fo) fclose(fo);
    LOG("# итого кандидатов: %llu (выведено %zu; проверено на хосте %zu из %zu сохранённых)\n", total_found, printed, verified, found.size());
    if (have_true) {
        LOG("# тест: истинный seed %llu (0x%012llx) %s\n", (unsigned long long)true_seed, (unsigned long long)true_seed, hit ? "НАЙДЕН" : "НЕ найден");
        if (!hit) return 1;
    }
    return 0;
}

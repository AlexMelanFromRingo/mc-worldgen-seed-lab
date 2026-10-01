/*
 * structdb.h — таблица structure_set (placement) из data/structure_sets-<V>.json + разбор файлов наблюдений (host).
 *
 * Формат строки наблюдения (разделители: ';' ',' пробел/таб; '#' — комментарий; регистр не важен):
 *     <structure_set>;<chunkX>;<chunkZ>          например   desert_pyramids;303;364     (префикс minecraft: допускается)
 *     struct <structure_set> <chunkX> <chunkZ>   (то же; слово struct необязательно)
 *     slime;<chunkX>;<chunkZ>[;1|0]              слайм-чанк (по умолчанию 1) / не-слайм (0)
 * <structure_set>: id набора (desert_pyramids, villages, end_cities, mineshafts, ...), либо id структуры из набора
 * (desert_pyramid, village_plains, end_city, ...), либо единственное число (village). Поддерживаются только
 * random_spread (остальные типы — не восстанавливаются по одному чанку).
 * Проверяется необходимое условие: потенциальный чанк региона (+ frequency reducer); exclusion_zone и биомы НЕ проверяются.
 */
#ifndef CRACK_STRUCTDB_H
#define CRACK_STRUCTDB_H
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <limits.h>
#include "json.h"
#include "pred.h"

struct SetDef {
    std::string key;          /* "desert_pyramids" */
    bool spread = false;      /* random_spread */
    i32 spacing = 0, sep = 0, salt = 0, type = 0;
    float freq = 1.0f;
    int red = RED_NONE;
    std::string excl;         /* exclusion_zone.other_set (информационно) */
    std::vector<std::string> structures;   /* короткие id */
};

struct StructDB {
    std::string version, source;
    std::vector<SetDef> sets;
    std::map<std::string, int> alias;

    static std::string strip(std::string s) {
        for (auto &c : s) { c = (char)tolower((unsigned char)c); if (c == '-') c = '_'; }
        if (s.compare(0, 10, "minecraft:") == 0) s = s.substr(10);
        return s;
    }
    void build_alias() {
        alias.clear();
        for (size_t i = 0; i < sets.size(); i++) {
            const std::string &k = sets[i].key;
            alias[k] = (int)i;
            if (k.size() > 3 && k.compare(k.size() - 3, 3, "ies") == 0) alias[k.substr(0, k.size() - 3) + "y"] = (int)i;
            else if (k.size() > 1 && k.back() == 's') alias[k.substr(0, k.size() - 1)] = (int)i;
            for (auto &st : sets[i].structures) if (!alias.count(st)) alias[st] = (int)i;
        }
    }
    const SetDef *find(const std::string &name) const {
        auto it = alias.find(strip(name));
        return it == alias.end() ? nullptr : &sets[it->second];
    }
};

static inline std::string exe_dir() {
    char buf[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0; std::string s(buf); size_t p = s.find_last_of('/');
    return p == std::string::npos ? "." : s.substr(0, p);
}
static inline bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
}

/* встроенная резервная таблица (числа идентичны во всех версиях; abandoned_camp — только 26.3) */
static inline void builtin_sets(StructDB &db, const std::string &ver) {
    struct B { const char *k; int sp, se, salt, type; float f; int red; const char *st; };
    static const B T[] = {
        {"ancient_cities", 24, 8, 20083232, 0, 1.0f, 0, "ancient_city"},
        {"buried_treasures", 1, 0, 0, 0, 0.01f, RED_TYPE2, "buried_treasure"},
        {"desert_pyramids", 32, 8, 14357617, 0, 1.0f, 0, "desert_pyramid"},
        {"end_cities", 20, 11, 10387313, 1, 1.0f, 0, "end_city"},
        {"igloos", 32, 8, 14357618, 0, 1.0f, 0, "igloo"},
        {"jungle_temples", 32, 8, 14357619, 0, 1.0f, 0, "jungle_pyramid"},
        {"mineshafts", 1, 0, 0, 0, 0.004f, RED_TYPE3, "mineshaft"},
        {"nether_complexes", 27, 4, 30084232, 0, 1.0f, 0, "fortress"},
        {"nether_fossils", 2, 1, 14357921, 0, 1.0f, 0, "nether_fossil"},
        {"ocean_monuments", 32, 5, 10387313, 1, 1.0f, 0, "monument"},
        {"ocean_ruins", 20, 8, 14357621, 0, 1.0f, 0, "ocean_ruin_cold"},
        {"pillager_outposts", 32, 8, 165745296, 0, 0.2f, RED_TYPE1, "pillager_outpost"},
        {"ruined_portals", 40, 15, 34222645, 0, 1.0f, 0, "ruined_portal"},
        {"shipwrecks", 24, 4, 165745295, 0, 1.0f, 0, "shipwreck"},
        {"swamp_huts", 32, 8, 14357620, 0, 1.0f, 0, "swamp_hut"},
        {"trail_ruins", 34, 8, 83469867, 0, 1.0f, 0, "trail_ruins"},
        {"trial_chambers", 34, 12, 94251327, 0, 1.0f, 0, "trial_chambers"},
        {"villages", 34, 8, 10387312, 0, 1.0f, 0, "village_plains"},
        {"woodland_mansions", 80, 20, 10387319, 1, 1.0f, 0, "mansion"},
        {"abandoned_camp", 37, 8, 91231127, 0, 1.0f, 0, "abandoned_camp_forest"},
    };
    db.sets.clear();
    for (auto &b : T) {
        if (!strcmp(b.k, "abandoned_camp") && ver != "26.3") continue;
        SetDef d; d.key = b.k; d.spread = true; d.spacing = b.sp; d.sep = b.se; d.salt = b.salt; d.type = b.type;
        d.freq = b.f; d.red = b.red; d.structures.push_back(b.st);
        db.sets.push_back(d);
    }
}

/* загрузка: явный путь sets_path, иначе <data_dir>/structure_sets-<ver>.json, data_dir по умолчанию: CRACK_DATA_DIR или <exe>/../../data */
static inline bool load_structdb(StructDB &db, const std::string &ver, const std::string &sets_path, const std::string &data_dir, std::string &err) {
    db.version = ver;
    std::string path = sets_path;
    if (path.empty()) {
        std::string dd = data_dir;
        if (dd.empty()) { const char *e = getenv("CRACK_DATA_DIR"); dd = e ? e : (exe_dir() + "/../../data"); }
        path = dd + "/structure_sets-" + ver + ".json";
    }
    std::string txt;
    if (!read_file(path, txt)) {
        builtin_sets(db, ver); db.source = "builtin"; db.build_alias();
        err = "предупреждение: нет " + path + " — использую встроенную таблицу placement";
        return true;
    }
    try {
        JVal root = json_parse(txt);
        const JVal *ss = root.get("structure_sets");
        if (!ss) { err = "нет ключа structure_sets в " + path; return false; }
        for (auto &kv : ss->obj) {
            SetDef d; d.key = kv.first;
            const JVal *pl = kv.second.get("placement");
            if (!pl) continue;
            const JVal *ty = pl->get("type");
            d.spread = ty && ty->s == "random_spread";
            if (d.spread) {
                d.spacing = (i32)pl->get("spacing")->num; d.sep = (i32)pl->get("separation")->num;
                d.salt = (i32)pl->get("salt")->num;
                const JVal *sp = pl->get("spread_type"); d.type = (sp && sp->s == "triangular") ? 1 : 0;
                const JVal *fr = pl->get("frequency"); d.freq = fr ? (float)fr->num : 1.0f;
                const JVal *rm = pl->get("frequency_reduction_method");
                std::string rs = rm ? rm->s : "default";
                d.red = RED_NONE;
                if (d.freq < 1.0f) d.red = rs == "legacy_type_1" ? RED_TYPE1 : rs == "legacy_type_2" ? RED_TYPE2 : rs == "legacy_type_3" ? RED_TYPE3 : RED_DEFAULT;
                const JVal *ex = pl->get("exclusion_zone");
                if (ex && ex->t == JVal::OBJ) { const JVal *o = ex->get("other_set"); if (o) d.excl = o->s; }
            }
            const JVal *sts = kv.second.get("structures");
            if (sts) for (auto &s : sts->arr) { const JVal *id = s.get("id"); if (id) d.structures.push_back(StructDB::strip(id->s)); }
            db.sets.push_back(d);
        }
    } catch (std::exception &e) { err = std::string("ошибка разбора ") + path + ": " + e.what(); return false; }
    db.source = path; db.build_alias();
    return true;
}

/* вероятность, что случайный W пройдёт предикат (для порядка проверок и оценки информации) */
static inline double pred_pass_prob(const Pred &p) {
    if (p.kind == PK_SLIME) return p.neg ? 0.9 : 0.1;
    double pp = 1.0;
    if (p.lim > 1) {
        auto pmf = [&](int v) -> double {
            if (p.type == 0) return 1.0 / p.lim;
            long c = 0; for (int a = 0; a < p.lim; a++) for (int b = 0; b < p.lim; b++) if ((a + b) / 2 == v) c++;
            return (double)c / ((double)p.lim * p.lim);
        };
        pp = pmf(p.ox) * pmf(p.oz);
    }
    if (p.red != RED_NONE) pp *= (p.red == RED_TYPE1 ? 1.0 / p.inv1 : (double)p.freq);
    return pp;
}

static inline void pred_finalize(Pred &p) { p.pass = pred_pass_prob(p); }

static inline bool make_spread_pred(const SetDef &d, i32 cx, i32 cz, Pred &p, std::string &err) {
    if (!d.spread) { err = "набор " + d.key + " не random_spread — не поддерживается"; return false; }
    memset(&p, 0, sizeof p);
    p.kind = PK_SPREAD; p.type = d.type; p.lim = d.spacing - d.sep; p.salt = d.salt;
    p.cx = cx; p.cz = cz;
    i32 gx = floordiv_i32(cx, d.spacing), gz = floordiv_i32(cz, d.spacing);
    p.ox = cx - gx * d.spacing; p.oz = cz - gz * d.spacing;
    if (p.ox >= p.lim || p.oz >= p.lim) {
        char b[256]; snprintf(b, sizeof b, "чанк (%d,%d) не может быть стартовым для %s: смещение в регионе (%d,%d) >= limit=%d", cx, cz, d.key.c_str(), p.ox, p.oz, p.lim);
        err = b; return false;
    }
    p.rcst = ((u64)((i64)gx * REGION_A) + (u64)((i64)gz * REGION_B) + (u64)(i64)d.salt) & MASK48;
    p.np2 = (p.lim & (p.lim - 1)) != 0;
    p.magic = p.np2 ? (~0ULL) / (u64)p.lim + 1 : 0;
    p.red = d.red; p.freq = d.freq;
    { float q = 1.0f / d.freq; p.inv1 = (d.freq > 0) ? (i32)q : 1; }
    int tz = 0; while (tz < 3 && ((p.lim >> tz) & 1) == 0) tz++;
    p.tz = (p.type == 0 && p.np2) ? tz : 0;
    pred_finalize(p);
    return true;
}

static inline void make_slime_pred(i32 cx, i32 cz, int neg, Pred &p) {
    memset(&p, 0, sizeof p);
    p.kind = PK_SLIME; p.scst = slime_cst(cx, cz); p.neg = neg; p.cx = cx; p.cz = cz;
    pred_finalize(p);
}

static inline std::vector<std::string> split_tokens(const std::string &line) {
    std::vector<std::string> t; std::string cur;
    for (char c : line) {
        if (c == '#') break;
        if (c == ';' || c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!cur.empty()) { t.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) t.push_back(cur);
    return t;
}
static inline bool parse_int(const std::string &s, long long &v) {
    char *e; errno = 0; v = strtoll(s.c_str(), &e, 10); return !s.empty() && *e == 0 && errno == 0;
}

/* разбор файла наблюдений -> предикаты; возвращает число ошибок (при ошибке строка пропускается, сообщение в err) */
static inline int parse_obs_file(const std::string &path, const StructDB &db, std::vector<Pred> &out, std::string &err) {
    std::ifstream f(path); if (!f) { err += "не могу открыть " + path + "\n"; return 1; }
    std::string line; int ln = 0, bad = 0;
    while (std::getline(f, line)) {
        ln++;
        auto t = split_tokens(line);
        if (t.empty()) continue;
        if (StructDB::strip(t[0]) == "struct") t.erase(t.begin());
        long long x, z;
        char pre[64]; snprintf(pre, sizeof pre, "%s:%d: ", path.c_str(), ln);
        if (t.size() < 3 || !parse_int(t[1], x) || !parse_int(t[2], z)) { err += std::string(pre) + "ожидалось <set>;<cx>;<cz>\n"; bad++; continue; }
        std::string nm = StructDB::strip(t[0]);
        Pred p;
        if (nm == "slime") {
            int neg = 0;
            if (t.size() > 3) { std::string f3 = StructDB::strip(t[3]); neg = (f3 == "0" || f3 == "noslime" || f3 == "no" || f3 == "false" || f3 == "n") ? 1 : 0; }
            make_slime_pred((i32)x, (i32)z, neg, p); out.push_back(p); continue;
        }
        const SetDef *d = db.find(nm);
        if (!d) { err += std::string(pre) + "неизвестный structure_set '" + t[0] + "'\n"; bad++; continue; }
        std::string e2;
        if (!make_spread_pred(*d, (i32)x, (i32)z, p, e2)) { err += std::string(pre) + e2 + "\n"; bad++; continue; }
        out.push_back(p);
    }
    return bad;
}

static inline void sort_preds(std::vector<Pred> &v) {
    std::stable_sort(v.begin(), v.end(), [](const Pred &a, const Pred &b) { return a.pass < b.pass; });
}
static inline double info_bits(const std::vector<Pred> &v) { double b = 0; for (auto &p : v) b += -log2(p.pass); return b; }

/* Все W в [0,2^L), совместимые с lifting-условиями предикатов (инкрементально по битам, начиная с 17) */
static inline std::vector<u64> lift_lows(const std::vector<Pred> &preds, int &Lout, int Lmax = 20) {
    int L = 0;
    for (auto &p : preds) {
        int need = 0;
        if (p.kind == PK_SLIME && !p.neg) need = 18;
        else if (p.kind == PK_SPREAD && p.tz > 0) need = 17 + p.tz;
        L = std::max(L, need);
    }
    L = std::min(L, Lmax);
    Lout = L;
    if (L < 18) return {};
    std::vector<u64> cur; cur.reserve(1 << 17);
    for (u64 v = 0; v < (1ULL << 17); v++) cur.push_back(v);
    for (int l = 17; l < L; l++) {
        std::vector<u64> nxt; nxt.reserve(cur.size());
        for (u64 v : cur) for (int b = 0; b < 2; b++) {
            u64 c = v | ((u64)b << l);
            bool ok = true;
            for (auto &p : preds) if (!pred_lowbits_ok(c, l + 1, p)) { ok = false; break; }
            if (ok) nxt.push_back(c);
        }
        cur.swap(nxt);
    }
    return cur;
}

/* seed, заданный 32-битным целым (набранное число до ±2^31 или String.hashCode() текстового seed): биты 32..47 (48-бит seed) все нули или все единицы */
static inline bool is_int32_seed(u64 W48) { u64 h = (W48 >> 32) & 0xFFFF; return h == 0 || h == 0xFFFF; }

/* "random" world seeds: все 64-бит seed вида RandomSource.create().nextLong() (LCG-пара) с данными младшими 48 битами.
 * Возвращает список (обычно 0..3 штуки, в среднем ~1). */
static inline std::vector<i64> random_world_seeds(u64 W48) {
    std::vector<i64> res;
    const u64 INV = 0xDFE05BCB1365ULL;   /* M^-1 mod 2^48 */
    u64 lo32 = W48 & 0xFFFFFFFFULL;
    for (u64 t = 0; t < 65536; t++) {
        u64 s2 = (lo32 << 16) | t;
        u64 s1 = ((s2 - LCG_ADD) * INV) & MASK48;
        i32 hi = (i32)(s1 >> 16), lo = (i32)(s2 >> 16);
        i64 w = (i64)((u64)(i64)hi << 32) + (i64)lo;
        if (((u64)w & MASK48) == W48) res.push_back(w);
    }
    return res;
}

#endif

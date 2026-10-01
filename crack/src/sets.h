/*
 * sets.h (host) — таблицы structure_set из data/structure_sets-<V>.json, эталонное размещение структур
 * (прямая транскрипция RandomSpreadStructurePlacement / AbstractSpreadingStructurePlacement), разбор файла наблюдений,
 * построение предикатов для поиска.
 */
#ifndef CRACK_SETS_H
#define CRACK_SETS_H

#include <cmath>
#include <algorithm>
#include <set>
#include <sstream>
#include <unistd.h>
#include <limits.h>
#include "mini_json.h"
#include "pred.h"

struct StructSet {
    std::string id;                    /* без "minecraft:" */
    std::string type;                  /* random_spread | concentric_rings | dimension_origin */
    int spacing = 0, separation = 0, salt = 0;
    bool triangular = false;
    float frequency = 1.0f;
    int freq_method = 0;               /* 0 default, 1..3 legacy_type_N */
    std::string excl_set; int excl_chunks = 0;
    std::vector<std::string> structures, dims;
    int rings_distance = 0, rings_spread = 0, rings_count = 0;
    int lim() const { return spacing - separation; }
    bool in_dim(const std::string &d) const { return std::find(dims.begin(), dims.end(), d) != dims.end(); }
};

static inline std::string strip_ns(std::string s) {
    if (s.compare(0, 10, "minecraft:") == 0) s = s.substr(10);
    return s;
}
static inline std::string norm_dim(const std::string &d) {
    if (d == "overworld" || d == "ow") return "overworld";
    if (d == "nether" || d == "the_nether") return "the_nether";
    if (d == "end" || d == "the_end") return "the_end";
    return "";
}

struct SetTable {
    std::string version;
    std::vector<StructSet> sets;

    int find_set(const std::string &id) const {
        for (size_t i = 0; i < sets.size(); i++) if (sets[i].id == id) return (int)i;
        return -1;
    }
    /* имя из файла наблюдений -> индекс набора: id набора, id структуры, типичные синонимы SeedcrackerX/seedfinding */
    int resolve(std::string name) const {
        name = strip_ns(name);
        for (auto &c : name) c = (char)tolower((unsigned char)c);
        int i = find_set(name); if (i >= 0) return i;
        for (size_t k = 0; k < sets.size(); k++)
            for (auto &st : sets[k].structures) if (strip_ns(st) == name) return (int)k;
        static const char *alias[][2] = {
            {"village", "villages"}, {"ocean_ruin", "ocean_ruins"}, {"nether_complex", "nether_complexes"},
            {"desert_temple", "desert_pyramids"}, {"jungle_temple", "jungle_temples"}, {"outpost", "pillager_outposts"},
            {"pillager_outpost", "pillager_outposts"}, {"mansion", "woodland_mansions"}, {"monument", "ocean_monuments"},
            {"ocean_monument", "ocean_monuments"}, {"ruined_portal", "ruined_portals"}, {"trial_chamber", "trial_chambers"},
            {"end_city", "end_cities"}, {"ancient_city", "ancient_cities"}, {"shipwreck", "shipwrecks"}, {"igloo", "igloos"},
            {"swamp_hut", "swamp_huts"}, {"desert_pyramid", "desert_pyramids"}, {"jungle_pyramid", "jungle_temples"},
            {"fortress", "nether_complexes"}, {"bastion_remnant", "nether_complexes"}, {"bastion", "nether_complexes"},
            {"nether_fossil", "nether_fossils"}, {"buried_treasure", "buried_treasures"}, {"mineshaft", "mineshafts"},
            {"trail_ruin", "trail_ruins"}, {"abandoned_camps", "abandoned_camp"}, {"stronghold", "strongholds"}};
        for (auto &a : alias) if (name == a[0]) return find_set(a[1]);
        return -1;
    }

    static int method_from(const std::string &m) {
        if (m == "default") return 0;
        if (m == "legacy_type_1") return 1;
        if (m == "legacy_type_2") return 2;
        if (m == "legacy_type_3") return 3;
        throw std::runtime_error("неизвестный frequency_reduction_method: " + m);
    }

    void load(const std::string &path, const std::string &ver) {
        std::string txt;
        if (!read_file_str(path, txt)) throw std::runtime_error("не могу прочитать " + path);
        JParser jp(txt); JVal root = jp.parse();
        version = ver;
        const JVal *ss = root.get("structure_sets");
        if (!ss || ss->type != JVal::OBJ) throw std::runtime_error("нет structure_sets в " + path);
        for (auto &kv : ss->obj) {
            StructSet s; s.id = strip_ns(kv.first);
            const JVal *pl = kv.second.get("placement");
            if (!pl) continue;
            s.type = pl->get("type") ? strip_ns(pl->get("type")->str) : "random_spread";
            if (auto v = pl->get("salt")) s.salt = (int)v->num;
            if (auto v = pl->get("frequency")) s.frequency = (float)v->num;       /* Java: double JSON -> float */
            if (auto v = pl->get("frequency_reduction_method")) s.freq_method = method_from(v->str);
            if (auto v = pl->get("exclusion_zone")) if (v->type == JVal::OBJ) {
                s.excl_set = strip_ns(v->get("other_set")->str); s.excl_chunks = (int)v->get("chunk_count")->num;
            }
            if (s.type == "random_spread") {
                s.spacing = (int)pl->get("spacing")->num; s.separation = (int)pl->get("separation")->num;
                if (auto v = pl->get("spread_type")) s.triangular = (v->str == "triangular");
            } else if (s.type == "concentric_rings") {
                s.rings_distance = (int)pl->get("distance")->num; s.rings_spread = (int)pl->get("spread")->num; s.rings_count = (int)pl->get("count")->num;
            }
            std::set<std::string> dims;
            if (auto st = kv.second.get("structures")) for (auto &e : st->arr) {
                s.structures.push_back(strip_ns(e.get("id")->str));
                if (auto d = e.get("dimensions")) for (auto &x : d->arr) dims.insert(x.str);
            }
            s.dims.assign(dims.begin(), dims.end());
            sets.push_back(s);
        }
    }
};

/* --------------------------- эталонное размещение (host) --------------------------- */
static inline i32 floor_div(i32 a, i32 b) { i32 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }

/* RandomSpreadStructurePlacement.getPotentialStructureChunk */
static inline void host_potential(const StructSet &S, u64 seed, i32 cx, i32 cz, i32 *px, i32 *pz) {
    i32 gx = floor_div(cx, S.spacing), gz = floor_div(cz, S.spacing);
    u64 s = large_feature_with_salt(seed, gx, gz, S.salt);
    i32 lim = S.lim(), sx, sz;
    if (!S.triangular) { sx = j_nextInt(&s, lim); sz = j_nextInt(&s, lim); }
    else {
        i32 a = j_nextInt(&s, lim), b = j_nextInt(&s, lim); sx = (a + b) / 2;
        a = j_nextInt(&s, lim); b = j_nextInt(&s, lim); sz = (a + b) / 2;
    }
    *px = gx * S.spacing + sx; *pz = gz * S.spacing + sz;
}

/* applyAdditionalChunkRestrictions: frequency reducer */
static inline bool host_reducer(const StructSet &S, u64 seed, i32 cx, i32 cz) {
    if (!(S.frequency < 1.0f)) return true;
    switch (S.freq_method) {
    case 0: { u64 s = large_feature_with_salt(seed, S.salt, cx, cz); return j_nextFloat(&s) < S.frequency; }
    case 1: {
        i32 a = cx >> 4, b = cz >> 4;
        u64 s = j_scramble(((u64)(i64)(a ^ (b << 4)) ^ seed) & K_MASK48);
        j_next(&s, 32);
        return j_nextInt(&s, (i32)(1.0f / S.frequency)) == 0;
    }
    case 2: { u64 s = large_feature_with_salt(seed, cx, cz, 10387320); return j_nextFloat(&s) < S.frequency; }
    case 3: { u64 s = large_feature_seed(seed, cx, cz); return j_nextDouble(&s) < (double)S.frequency; }
    }
    return false;
}

static inline bool host_is_structure_chunk(const SetTable &T, int si, u64 seed, i32 cx, i32 cz, int depth = 0);

static inline bool host_has_chunk_in_range(const SetTable &T, int other, u64 seed, i32 cx, i32 cz, i32 range, int depth) {
    for (i32 x = cx - range; x <= cx + range; x++)
        for (i32 z = cz - range; z <= cz + range; z++)
            if (host_is_structure_chunk(T, other, seed, x, z, depth + 1)) return true;
    return false;
}

/* isStructureChunk = isPlacementChunk && reducer && !exclusion (только random_spread) */
static inline bool host_is_structure_chunk(const SetTable &T, int si, u64 seed, i32 cx, i32 cz, int depth) {
    const StructSet &S = T.sets[si];
    if (S.type != "random_spread" || depth > 4) return false;
    i32 px, pz; host_potential(S, seed, cx, cz, &px, &pz);
    if (px != cx || pz != cz) return false;
    if (!host_reducer(S, seed, cx, cz)) return false;
    if (!S.excl_set.empty()) {
        int o = T.find_set(S.excl_set);
        if (o >= 0 && host_has_chunk_in_range(T, o, seed, cx, cz, S.excl_chunks, depth)) return false;
    }
    return true;
}

/* --------------------------- наблюдения --------------------------- */
struct Obs { std::string name; int set; i32 cx, cz; int line; };

/* Формат: `имя;chunkX;chunkZ` (SeedcrackerX StructureSave), разделители ; , или пробел, `#` — комментарий. */
static inline bool parse_obs_file(const std::string &path, const SetTable &T, bool blocks, std::vector<Obs> &out, std::string &err) {
    std::string txt;
    if (path == "-") { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) txt.append(buf, n); }
    else if (!read_file_str(path, txt)) { err = "не могу прочитать файл наблюдений " + path; return false; }
    std::istringstream in(txt); std::string line; int ln = 0;
    while (std::getline(in, line)) {
        ln++;
        size_t h = line.find('#'); if (h != std::string::npos) line.resize(h);
        for (auto &c : line) if (c == ';' || c == ',' || c == '\t' || c == '\r') c = ' ';
        std::istringstream ls(line); std::string name; std::string sx, sz;
        if (!(ls >> name)) continue;
        if (!(ls >> sx >> sz)) { err = "строка " + std::to_string(ln) + ": ожидалось `имя;chunkX;chunkZ`"; return false; }
        char *e1, *e2; long x = strtol(sx.c_str(), &e1, 10), z = strtol(sz.c_str(), &e2, 10);
        if (*e1 || *e2) { err = "строка " + std::to_string(ln) + ": координаты должны быть целыми"; return false; }
        if (blocks) { x = floor_div((i32)x, 16); z = floor_div((i32)z, 16); }
        int si = T.resolve(name);
        if (si < 0) { err = "строка " + std::to_string(ln) + ": неизвестная структура/набор `" + name + "` для версии " + T.version; return false; }
        out.push_back({name, si, (i32)x, (i32)z, ln});
    }
    return true;
}

/* --------------------------- построение предикатов --------------------------- */
struct Excl { int set; i32 cx, cz; };
struct Problem {
    std::vector<Excl> rings;           /* наблюдения strongholds (concentric_rings): фильтр по геометрии колец */
    std::vector<Pred> preds;
    std::vector<std::string> desc;
    std::vector<double> prob;          /* вероятность случайного прохождения предиката */
    std::vector<Excl> excl;
    double info_bits = 0;
    int used = 0, skipped = 0;
    std::vector<std::string> warnings;
};

static inline double tri_prob(i32 lim, i32 o) {
    long cnt = 0;
    for (i32 a = 0; a < lim; a++) for (i32 b = 0; b < lim; b++) if ((a + b) / 2 == o) cnt++;
    return (double)cnt / ((double)lim * lim);
}

static inline bool build_problem(const SetTable &T, const std::vector<Obs> &obs, Problem &P, std::string &err) {
    std::set<std::pair<int, std::pair<i32, i32>>> seen_chunk;
    std::map<std::pair<int, std::pair<i32, i32>>, std::pair<i32, i32>> region_chunk;   /* (set, region) -> chunk */
    for (const Obs &o : obs) {
        const StructSet &S = T.sets[o.set];
        std::string tag = "`" + o.name + "` (" + S.id + ") chunk (" + std::to_string(o.cx) + "," + std::to_string(o.cz) + ")";
        if (S.type == "concentric_rings") {
            /* позиция зависит от биомов (64 бита); из 48 бит известна геометрия колец: стартовый чанк в пределах ±7 чанков от (ix,iz) */
            if (!seen_chunk.insert({o.set, {o.cx, o.cz}}).second) { P.warnings.push_back("дубликат " + tag + " пропущен"); P.skipped++; continue; }
            P.rings.push_back({o.set, o.cx, o.cz}); P.used++;
            { Pred p; memset(&p, 0, sizeof p); p.kind = PK_RING; p.cx = o.cx; p.cz = o.cz; p.ex_range = 8;     /* device: допуск 8 (надмножество), host: точно 7 */
              p.ring_distance = S.rings_distance; p.ring_spread = S.rings_spread; p.ring_count = S.rings_count;
              P.preds.push_back(p); P.desc.push_back(S.id + " rings chunk(" + std::to_string(o.cx) + "," + std::to_string(o.cz) + ")"); P.prob.push_back(1.0); }
            continue;
        }
        if (S.type != "random_spread") { err = "наблюдение " + tag + ": placement " + S.type + " не зависит от seed / не поддержан"; return false; }
        if (!seen_chunk.insert({o.set, {o.cx, o.cz}}).second) { P.warnings.push_back("дубликат " + tag + " пропущен"); P.skipped++; continue; }
        i32 lim = S.lim();
        i32 rx = floor_div(o.cx, S.spacing), rz = floor_div(o.cz, S.spacing);
        i32 ox = o.cx - rx * S.spacing, oz = o.cz - rz * S.spacing;
        if (ox >= lim || oz >= lim) {
            err = "наблюдение " + tag + " невозможно: смещение в регионе (" + std::to_string(ox) + "," + std::to_string(oz) + ") >= spacing-separation=" + std::to_string(lim) +
                  " (структура не может стартовать в этом чанке; проверьте, что даны координаты ЧАНКА старта)";
            return false;
        }
        auto rk = std::make_pair(o.set, std::make_pair(rx, rz));
        auto it = region_chunk.find(rk);
        if (it != region_chunk.end() && it->second != std::make_pair(o.cx, o.cz)) {
            err = "наблюдение " + tag + " противоречит другому чанку того же набора в том же регионе (" + std::to_string(it->second.first) + "," + std::to_string(it->second.second) + ")";
            return false;
        }
        region_chunk[rk] = {o.cx, o.cz};
        bool any = false;
        if (lim > 1) {
            Pred p; memset(&p, 0, sizeof p);
            p.kind = S.triangular ? PK_TRI : PK_LIN;
            p.ox = ox; p.oz = oz;
            pred_set_lim(p, lim);
            p.cst = (u64)((i64)rx * K_REGION_A + (i64)rz * K_REGION_B + (i64)S.salt) & K_MASK48;
            int tz = 0; while (!((lim >> tz) & 1)) tz++;
            p.tz = (!S.triangular && p.pow2k < 0) ? std::min(tz, 5) : 0;
            P.preds.push_back(p);
            P.desc.push_back(S.id + (S.triangular ? " tri " : " lin ") + "chunk(" + std::to_string(o.cx) + "," + std::to_string(o.cz) + ")");
            P.prob.push_back(S.triangular ? tri_prob(lim, ox) * tri_prob(lim, oz) : 1.0 / ((double)lim * lim));
            any = true;
        }
        if (S.frequency < 1.0f) {
            Pred p; memset(&p, 0, sizeof p);
            double pr = (double)S.frequency;
            switch (S.freq_method) {
            case 0: p.kind = PK_RED_DEF; p.cst = (u64)((i64)S.salt * K_REGION_A + (i64)o.cx * K_REGION_B + (i64)o.cz) & K_MASK48; break;
            case 2: p.kind = PK_RED_L2; p.cst = (u64)((i64)o.cx * K_REGION_A + (i64)o.cz * K_REGION_B + 10387320LL) & K_MASK48; break;
            case 3: p.kind = PK_RED_L3; p.cx = o.cx; p.cz = o.cz; break;
            case 1: p.kind = PK_RED_L1; p.cx = o.cx; p.cz = o.cz; p.nmod = (i32)(1.0f / S.frequency); pr = 1.0 / p.nmod; break;
            }
            double x24 = (double)S.frequency * 16777216.0;
            p.T24 = (u32)std::ceil(x24);
            p.T53 = (u64)std::ceil((double)S.frequency * 9007199254740992.0);
            P.preds.push_back(p);
            static const char *mn[] = {"freq", "legacy1", "legacy2", "legacy3"};
            P.desc.push_back(S.id + " " + mn[S.freq_method] + " chunk(" + std::to_string(o.cx) + "," + std::to_string(o.cz) + ")");
            P.prob.push_back(pr);
            any = true;
        }
        if (!S.excl_set.empty() && T.find_set(S.excl_set) >= 0) {
            P.excl.push_back({o.set, o.cx, o.cz});
            const StructSet &X = T.sets[T.find_set(S.excl_set)];
            if (X.type == "random_spread" && X.frequency >= 1.0f && X.excl_set.empty() && X.lim() > 1) {   /* device-проверка для простого случая (villages) */
                Pred p; memset(&p, 0, sizeof p); p.kind = PK_EXCL; p.cx = o.cx; p.cz = o.cz;
                p.ex_spacing = X.spacing; p.ex_lim = X.lim(); p.ex_salt = X.salt; p.ex_tri = X.triangular; p.ex_range = S.excl_chunks;
                P.preds.push_back(p); P.desc.push_back(S.id + " exclusion(" + X.id + ") chunk(" + std::to_string(o.cx) + "," + std::to_string(o.cz) + ")"); P.prob.push_back(1.0);
            }
        }
        if (any) P.used++; else { P.skipped++; P.warnings.push_back("наблюдение " + tag + " не несёт информации о seed (spacing-separation=1 без frequency) — пропущено"); }
    }
    P.info_bits = 0;
    for (double pr : P.prob) P.info_bits += -std::log2(pr);
    return true;
}

/* concentric_rings (ChunkGeneratorStructureState.generateRingPositions): геометрия колец зависит только от 48 бит seed;
 * итоговый чанк = результат поиска биома в окне ±112 блоков => Chebyshev-расстояние до (ix,iz) не больше 7 чанков.
 * true, если ЛЮБОЕ из count колец-позиций кандидата W лежит в пределах tol чанков от (cx,cz). */
static inline bool host_ring_check(const StructSet &S, u64 W, i32 cx, i32 cz, int tol = 7) {
    u64 s = j_scramble(W & K_MASK48);
    const double PI = 3.141592653589793;
    double angle = j_nextDouble(&s) * PI * 2.0;
    int pic = 0, circle = 0, spread = S.rings_spread, distance = S.rings_distance, count = S.rings_count;
    for (int i = 0; i < count; i++) {
        double dist = (double)(4 * distance + distance * circle * 6) + (j_nextDouble(&s) - 0.5) * (distance * 2.5);
        i32 ix = (i32)std::floor(std::cos(angle) * dist + 0.5), iz = (i32)std::floor(std::sin(angle) * dist + 0.5);
        j_nextLong(&s);                                   /* random.fork() потребляет nextLong */
        if (std::abs(cx - ix) <= tol && std::abs(cz - iz) <= tol) return true;
        angle += (PI * 2) / spread;
        if (++pic == spread) {
            circle++; pic = 0;
            spread += 2 * spread / (circle + 1);
            spread = std::min(spread, count - i);
            angle += j_nextDouble(&s) * PI * 2.0;
        }
    }
    return false;
}

/* хостовая проверка всех предикатов + exclusion zones + кольца strongholds */
static inline bool host_check_all(const SetTable &T, const Problem &P, u64 W) {
    for (const Pred &p : P.preds) if (!pred_eval(W, p)) return false;
    for (const Excl &e : P.excl) {
        const StructSet &S = T.sets[e.set];
        int o = T.find_set(S.excl_set);
        if (host_has_chunk_in_range(T, o, W, e.cx, e.cz, S.excl_chunks, 0)) return false;
    }
    for (const Excl &r : P.rings) if (!host_ring_check(T.sets[r.set], W, r.cx, r.cz)) return false;
    return true;
}

/* корень проекта: $MCGEN_ROOT, либо <каталог exe>/../.. (если там data/), либо cwd */
static inline std::string find_root(const char *argv0) {
    if (const char *r = getenv("MCGEN_ROOT")) return r;
    char buf[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string exe = n > 0 ? std::string(buf, (size_t)n) : std::string(argv0 ? argv0 : ".");
    size_t p = exe.rfind('/');
    std::string dir = p == std::string::npos ? "." : exe.substr(0, p);
    for (const char *rel : {"/../..", "/.."}) {
        std::string cand = dir + rel;
        if (access((cand + "/data").c_str(), F_OK) == 0) return cand;
    }
    return ".";
}

#endif

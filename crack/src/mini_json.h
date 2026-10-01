/*
 * mini_json.h — минимальный разборщик JSON (объекты, массивы, строки, числа, true/false/null).
 * Нужен только для чтения data/structure_sets-<V>.json; зависимостей нет.
 * Числа хранятся и как double, и как исходный текст (чтобы float-значения разбирать так же, как Java: (float)double).
 */
#ifndef CRACK_MINI_JSON_H
#define CRACK_MINI_JSON_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;                       // строка или исходный текст числа
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;   // порядок ключей сохраняется

    const JVal *get(const char *key) const {
        if (type != OBJ) return nullptr;
        for (auto &kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool has(const char *key) const { return get(key) != nullptr; }
    bool is_null() const { return type == NUL; }
};

struct JParser {
    const char *p, *end;
    std::string err;
    explicit JParser(const std::string &s) : p(s.data()), end(s.data() + s.size()) {}

    [[noreturn]] void fail(const char *msg) { throw std::runtime_error(std::string("JSON: ") + msg); }
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }

    JVal parse() { ws(); JVal v = value(); ws(); if (p != end) fail("лишние данные после значения"); return v; }

    JVal value() {
        ws();
        if (p >= end) fail("неожиданный конец");
        char c = *p;
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') { JVal v; v.type = JVal::STR; v.str = string(); return v; }
        if (c == 't' && end - p >= 4 && !strncmp(p, "true", 4)) { p += 4; JVal v; v.type = JVal::BOOL; v.b = true; return v; }
        if (c == 'f' && end - p >= 5 && !strncmp(p, "false", 5)) { p += 5; JVal v; v.type = JVal::BOOL; v.b = false; return v; }
        if (c == 'n' && end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; return JVal(); }
        return number();
    }
    JVal number() {
        const char *s = p;
        while (p < end && (strchr("+-0123456789.eE", *p))) p++;
        if (p == s) fail("ожидалось значение");
        JVal v; v.type = JVal::NUM; v.str.assign(s, p - s); v.num = strtod(v.str.c_str(), nullptr);
        return v;
    }
    std::string string() {
        std::string out; p++;   // "
        while (p < end && *p != '"') {
            if (*p == '\\') {
                p++; if (p >= end) fail("обрыв escape");
                switch (*p) {
                    case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                    case 'u': { if (end - p < 5) fail("обрыв \\u"); unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16); p += 4;
                                if (cp < 0x80) out += (char)cp; else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
                                else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); } break; }
                    default: out += *p;
                }
                p++;
            } else out += *p++;
        }
        if (p >= end) fail("незакрытая строка");
        p++; return out;
    }
    JVal array() {
        JVal v; v.type = JVal::ARR; p++; ws();
        if (p < end && *p == ']') { p++; return v; }
        for (;;) { v.arr.push_back(value()); ws(); if (p < end && *p == ',') { p++; continue; } if (p < end && *p == ']') { p++; break; } fail("ожидалось , или ]"); }
        return v;
    }
    JVal object() {
        JVal v; v.type = JVal::OBJ; p++; ws();
        if (p < end && *p == '}') { p++; return v; }
        for (;;) {
            ws(); if (p >= end || *p != '"') fail("ожидался ключ");
            std::string k = string(); ws();
            if (p >= end || *p != ':') fail("ожидалось :");
            p++;
            v.obj.emplace_back(k, value()); ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; break; }
            fail("ожидалось , или }");
        }
        return v;
    }
};

static inline bool read_file_str(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t r = n ? fread(&out[0], 1, (size_t)n, f) : 0; fclose(f);
    return r == (size_t)n;
}

#endif

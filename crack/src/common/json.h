/* json.h — минимальный JSON-парсер (только host), достаточный для data/structure_sets-<V>.json. */
#ifndef CRACK_JSON_H
#define CRACK_JSON_H
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;   // порядок ключей сохранён

    const JVal *get(const char *k) const {
        for (auto &kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool isnum() const { return t == NUM; }
};

struct JParser {
    const char *p, *e;
    JParser(const std::string &src) : p(src.c_str()), e(src.c_str() + src.size()) {}
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++; }
    [[noreturn]] void fail(const char *m) { throw std::runtime_error(std::string("JSON: ") + m); }
    std::string str() {
        std::string r; p++;   // "
        while (p < e && *p != '"') {
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n': r += '\n'; break; case 't': r += '\t'; break; case 'r': r += '\r'; break;
                case 'b': r += '\b'; break; case 'f': r += '\f'; break;
                case 'u': { unsigned v = (unsigned)strtoul(std::string(p + 1, 4).c_str(), 0, 16); p += 4;
                            if (v < 0x80) r += (char)v; else if (v < 0x800) { r += (char)(0xC0 | (v >> 6)); r += (char)(0x80 | (v & 63)); }
                            else { r += (char)(0xE0 | (v >> 12)); r += (char)(0x80 | ((v >> 6) & 63)); r += (char)(0x80 | (v & 63)); } break; }
                default: r += *p;
                }
                p++;
            } else r += *p++;
        }
        if (p >= e) fail("unterminated string");
        p++;
        return r;
    }
    JVal val() {
        ws();
        JVal v;
        if (p >= e) fail("unexpected end");
        if (*p == '{') {
            v.t = JVal::OBJ; p++; ws();
            if (*p == '}') { p++; return v; }
            for (;;) {
                ws(); if (*p != '"') fail("key expected");
                std::string k = str(); ws();
                if (*p != ':') fail(": expected");
                p++;
                v.obj.emplace_back(k, val()); ws();
                if (*p == ',') { p++; continue; }
                if (*p == '}') { p++; break; }
                fail("} or , expected");
            }
        } else if (*p == '[') {
            v.t = JVal::ARR; p++; ws();
            if (*p == ']') { p++; return v; }
            for (;;) {
                v.arr.push_back(val()); ws();
                if (*p == ',') { p++; continue; }
                if (*p == ']') { p++; break; }
                fail("] or , expected");
            }
        } else if (*p == '"') { v.t = JVal::STR; v.s = str(); }
        else if (!strncmp(p, "true", 4)) { v.t = JVal::BOOL; v.b = true; p += 4; }
        else if (!strncmp(p, "false", 5)) { v.t = JVal::BOOL; v.b = false; p += 5; }
        else if (!strncmp(p, "null", 4)) { v.t = JVal::NUL; p += 4; }
        else { char *q; v.num = strtod(p, &q); if (q == p) fail("bad token"); v.t = JVal::NUM; p = q; }
        return v;
    }
};

inline JVal json_parse(const std::string &src) { JParser jp(src); JVal v = jp.val(); return v; }
#endif

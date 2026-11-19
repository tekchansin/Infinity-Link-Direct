#include "infinitylink/json.h"

#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ilink::json {
namespace {

const Value kNull;

struct Reader {  // recursive descent; deep nesting allowed
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    unsigned hex4() {
        if (i + 4 > s.size()) { ok = false; return 0; }
        unsigned v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else { ok = false; return 0; }
        }
        return v;
    }
    std::string str() {  // at the opening quote
        std::string o;
        for (i++; i < s.size();) {
            char c = s[i++];
            if (c == '"') return o;
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            if (e == 'n') o += '\n'; else if (e == 't') o += '\t'; else if (e == 'r') o += '\r';
            else if (e == 'b') o += '\b'; else if (e == 'f') o += '\f';
            else if (e == 'u') {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && !s.compare(i, 2, "\\u")) {
                    i += 2;
                    unsigned lo = hex4();
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else cp = 0xFFFD;
                } else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;  // a lone surrogate
                if (cp < 0x80) o += (char)cp;
                else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
                else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            } else o += e;  // \" \\ \/
        }
        ok = false;
        return o;
    }
    Value value(int depth) {
        Value j;
        ws();
        if (i >= s.size() || depth > 64) { ok = false; return j; }
        char c = s[i];
        if (c == '{') {
            i++;
            j.type = Value::Obj;
            if (eat('}')) return j;
            do {
                ws();
                if (i >= s.size() || s[i] != '"') { ok = false; return j; }
                std::string k = str();
                if (!eat(':')) { ok = false; return j; }
                j.obj.push_back({k, value(depth + 1)});
            } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (c == '[') {
            i++;
            j.type = Value::Arr;
            if (eat(']')) return j;
            do j.arr.push_back(value(depth + 1)); while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (c == '"') { j.type = Value::Str; j.s = str(); }
        else if (!s.compare(i, 4, "true")) { i += 4; j.type = Value::Bool; j.b = true; }
        else if (!s.compare(i, 5, "false")) { i += 5; j.type = Value::Bool; }
        else if (!s.compare(i, 4, "null")) i += 4;
        else {
            size_t st = i;
            while (i < s.size() && s[i] && strchr("+-0123456789.eE", s[i])) i++;
            if (i == st) ok = false;
            j.type = Value::Num;
            j.s = s.substr(st, i - st);
        }
        return j;
    }
};

void dumpStr(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    o += '"';
}

void dumpTo(std::string& o, const Value& j) {
    switch (j.type) {
        case Value::Null: o += "null"; break;
        case Value::Bool: o += j.b ? "true" : "false"; break;
        case Value::Num: o += j.s.empty() ? "0" : j.s; break;
        case Value::Str: dumpStr(o, j.s); break;
        case Value::Arr:
            o += '[';
            for (size_t i = 0; i < j.arr.size(); i++) { if (i) o += ','; dumpTo(o, j.arr[i]); }
            o += ']';
            break;
        case Value::Obj:
            o += '{';
            for (size_t i = 0; i < j.obj.size(); i++) {
                if (i) o += ',';
                dumpStr(o, j.obj[i].first);
                o += ':';
                dumpTo(o, j.obj[i].second);
            }
            o += '}';
            break;
    }
}

}  // namespace

const Value* Value::find(const std::string& key) const {
    if (type != Obj) return nullptr;
    for (auto& e : obj) if (e.first == key) return &e.second;
    return nullptr;
}
const Value& Value::operator[](const std::string& key) const {
    const Value* v = find(key);
    return v ? *v : kNull;
}
const Value& Value::operator[](size_t i) const { return type == Arr && i < arr.size() ? arr[i] : kNull; }

std::string Value::str(const std::string& def) const {
    if (type == Str || type == Num) return s;
    if (type == Bool) return b ? "true" : "false";
    return def;
}
long long Value::i64(long long def) const {
    if ((type != Num && type != Str) || s.empty()) return def;
    char* end;
    long long v = strtoll(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    if (*end == '.' || *end == 'e' || *end == 'E') {  // a fraction truncates; past long long (1e999 from the LAN) clamps
        const double d = strtod(s.c_str(), nullptr);
        if (d != d) return def;
        return d >= 9.2e18 ? LLONG_MAX : d <= -9.2e18 ? LLONG_MIN : (long long)d;
    }
    return *end ? def : v;
}
double Value::num(double def) const {
    if ((type != Num && type != Str) || s.empty()) return def;
    char* end;
    double v = strtod(s.c_str(), &end);
    return *end || end == s.c_str() ? def : v;
}
bool Value::boolean(bool def) const {
    if (type == Bool) return b;
    if (type == Str) return s == "true" ? true : s == "false" ? false : def;
    return def;
}

Value& Value::set(const std::string& key, Value v) {
    if (type != Obj) { *this = Value(); type = Obj; }
    for (auto& e : obj) if (e.first == key) { e.second = std::move(v); return e.second; }
    obj.push_back({key, std::move(v)});
    return obj.back().second;
}
Value& Value::push(Value v) {
    if (type != Arr) { *this = Value(); type = Arr; }
    arr.push_back(std::move(v));
    return arr.back();
}

Value str(const std::string& s) { Value v; v.type = Value::Str; v.s = s; return v; }
Value num(long long n) { Value v; v.type = Value::Num; v.s = std::to_string(n); return v; }
Value real(double d) {
    Value v;
    v.type = Value::Num;
    char b[32];
    snprintf(b, sizeof b, "%.17g", d);
    v.s = b;
    return v;
}
Value boolean(bool b) { Value v; v.type = Value::Bool; v.b = b; return v; }

bool parse(const std::string& text, Value& out) {
    Reader r{text};
    out = r.value(0);
    r.ws();
    return r.ok && r.i == text.size();
}
std::string dump(const Value& v) {
    std::string o;
    dumpTo(o, v);
    return o;
}
std::string quote(const std::string& s) {
    std::string o;
    dumpStr(o, s);
    return o;
}

}  // namespace ilink::json

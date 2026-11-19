// A small JSON DOM, parser and writer for Infinity Link messages. Numbers keep their text, so 64-bit values survive a
// round trip. Reads are lenient: a missing key or a wrong type gives the default.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace ilink::json {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    std::string s;  // a string, or a number's text
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;  // in document order

    const Value& operator[](const std::string& key) const;  // object member, else a null
    const Value& operator[](size_t i) const;                 // array element, else a null
    const Value* find(const std::string& key) const;         // nullptr if absent
    bool has(const std::string& key) const { return find(key) != nullptr; }
    size_t size() const { return type == Arr ? arr.size() : type == Obj ? obj.size() : 0; }
    bool isNull() const { return type == Null; }

    std::string str(const std::string& def = "") const;  // Str, or a Num's text; Bool → "true"/"false"
    long long i64(long long def = 0) const;               // Num or numeric Str (a fraction truncates)
    double num(double def = 0) const;
    bool boolean(bool def = false) const;                 // Bool, or "true"/"false"

    // building (caches, request bodies)
    Value& set(const std::string& key, Value v);  // replaces or appends; makes this an object
    Value& push(Value v);                         // makes this an array
};
Value str(const std::string& s);
Value num(long long v);  // integers (ids, counts)
Value real(double v);
Value boolean(bool v);

bool parse(const std::string& text, Value& out);  // false = not valid JSON (out holds what was read)
std::string dump(const Value& v);                  // compact
std::string quote(const std::string& s);           // a JSON string literal, for hand-built bodies

}  // namespace ilink::json

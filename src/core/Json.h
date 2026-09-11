#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>

namespace mini {

// Minimal JSON value parser (objects, arrays, strings, numbers, bool, null).
struct JValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JValue> arr;
    struct Member;  // defined below (needs complete JValue)
    std::vector<Member> obj;

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    const JValue* get(const std::string& key) const;
    std::string strOf(const char* key) const;
    long long intOf(const char* key) const;
};

struct JValue::Member {
    std::string key;
    JValue value;
};

inline const JValue* JValue::get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& m : obj)
        if (m.key == key) return &m.value;
    return nullptr;
}
inline std::string JValue::strOf(const char* key) const {
    const JValue* v = get(key);
    if (!v) return {};
    if (v->isString()) return v->str;
    return {};
}
inline long long JValue::intOf(const char* key) const {
    const JValue* v = get(key);
    if (!v) return 0;
    if (v->isNumber()) return static_cast<long long>(v->num);
    return 0;
}

// Returns JValue; on parse failure type stays Null and ok=false.
bool parse(const std::string& text, JValue& out);

} // namespace mini
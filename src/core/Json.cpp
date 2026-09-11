#include "Json.h"

#include <cstdlib>
#include <cstring>

namespace mini {

namespace {

class Parser {
public:
    Parser(const std::string& s) : s_(s), i_(0) {}

    bool parse(JValue& out) {
        skipWs();
        if (!value(out)) return false;
        skipWs();
        return i_ >= s_.size();
    }

private:
    const std::string& s_;
    size_t i_;

    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    void skipWs() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    bool literal(const char* lit) {
        size_t n = strlen(lit);
        if (s_.compare(i_, n, lit) != 0) return false;
        i_ += n;
        return true;
    }

    bool value(JValue& v) {
        char c = peek();
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') {
            v.type = JValue::Type::String;
            return string(v.str);
        }
        if (c == 't') {
            if (!literal("true")) return false;
            v.type = JValue::Type::Bool; v.b = true; return true;
        }
        if (c == 'f') {
            if (!literal("false")) return false;
            v.type = JValue::Type::Bool; v.b = false; return true;
        }
        if (c == 'n') {
            if (!literal("null")) return false;
            v.type = JValue::Type::Null; return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            v.type = JValue::Type::Number;
            return number(v.num);
        }
        return false;
    }

    bool string(std::string& out) {
        if (peek() != '"') return false;
        ++i_;
        out.clear();
        while (true) {
            if (i_ >= s_.size()) return false;
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return false;
                char e = s_[i_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) return false;
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s_[i_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else return false;
                        }
                        // encode UTF-16 code unit (surrogate passthrough is fine for our use)
                        if (cp < 0x80) out.push_back(static_cast<char>(cp));
                        else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp < 0x10000) {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
    }

    bool number(double& out) {
        size_t start = i_;
        if (peek() == '-') ++i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') ++i_;
            else break;
        }
        if (i_ == start) return false;
        out = std::strtod(s_.c_str() + start, nullptr);
        return true;
    }

    bool object(JValue& v) {
        ++i_; // '{'
        v.type = JValue::Type::Object;
        skipWs();
        if (peek() == '}') { ++i_; return true; }
        while (true) {
            skipWs();
            if (peek() != '"') return false;
            std::string key;
            if (!string(key)) return false;
            skipWs();
            if (peek() != ':') return false;
            ++i_;
            skipWs();
            JValue val;
            if (!value(val)) return false;
            v.obj.push_back({ std::move(key), std::move(val) });
            skipWs();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == '}') { ++i_; return true; }
            return false;
        }
    }

    bool array(JValue& v) {
        ++i_; // '['
        v.type = JValue::Type::Array;
        skipWs();
        if (peek() == ']') { ++i_; return true; }
        while (true) {
            skipWs();
            JValue val;
            if (!value(val)) return false;
            v.arr.push_back(std::move(val));
            skipWs();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == ']') { ++i_; return true; }
            return false;
        }
    }
};

} // namespace

bool parse(const std::string& text, JValue& out) {
    Parser p(text);
    return p.parse(out);
}

} // namespace mini
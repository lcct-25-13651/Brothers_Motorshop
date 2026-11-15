// json.h - minimal JSON writer + reader (C++17, no dependencies).
//
// The app stores everything in CSV; JSON is only the wire format for the /api/* endpoints that a
// browser (fetch) or a TypeScript front-end talks to. Two small pieces:
//   * writer helpers (str/num/arr/obj) that build a correctly escaped string;
//   * a tolerant recursive-descent parser into json::Value, used to read request bodies.
// Numbers are emitted with the shortest round-trip form (850.5, not 850.50000).
#pragma once
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace json {

// ------------------------------------------------------------------ writer
inline std::string escape(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);   // UTF-8 passes through untouched
                }
        }
    }
    // A literal U+2028 / U+2029 would break JavaScript parsing of an inline script; escape the bytes
    // that can only ever appear inside those two code points.
    return o;
}

inline std::string str(const std::string& s) { return "\"" + escape(s) + "\""; }
inline std::string boolean(bool b) { return b ? "true" : "false"; }
inline std::string null() { return "null"; }

inline std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    std::ostringstream os;
    os << std::setprecision(15) << v;   // 15 digits: exact for centavo amounts, no 0.10000000000000001
    std::string s = os.str();
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) return s;
    while (s.size() > 1 && s.back() == '0' && s.find('.') != std::string::npos) s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}
inline std::string num(long long v) { return std::to_string(v); }
inline std::string num(int v) { return std::to_string(v); }

// Money: always two decimals, never scientific notation ("1234.50").
inline std::string money(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

inline std::string arr(const std::vector<std::string>& items) {
    std::string o = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) o += ',';
        o += items[i].empty() ? "null" : items[i];
    }
    return o + "]";
}

// Fields whose value is an empty string are dropped: callers build values with json::str/num/arr/obj,
// so "" is the only way to say "omit this field".
inline std::string obj(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string o = "{";
    bool first = true;
    for (const auto& f : fields) {
        if (f.second.empty()) continue;
        if (!first) o += ',';
        first = false;
        o += str(f.first) + ':' + f.second;
    }
    return o + "}";
}

// ------------------------------------------------------------------ reader
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::map<std::string, Value> fields;

    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }

    const Value* find(const std::string& key) const {
        auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    }
    // Tolerant readers: a JSON string is accepted where a number is expected and vice versa, because
    // HTML forms and hand-written fetch calls mix them freely.
    std::string asString(const std::string& dflt = "") const {
        switch (type) {
            case Type::String: return text;
            case Type::Number: return num(number);
            case Type::Bool: return boolean ? "1" : "0";
            default: return dflt;
        }
    }
    double asNumber(double dflt = 0) const {
        if (type == Type::Number) return number;
        if (type == Type::Bool) return boolean ? 1 : 0;
        if (type == Type::String) {
            try {
                size_t pos = 0;
                std::string t = text;
                while (!t.empty() && (t.front() == ' ' || t.front() == ',')) t.erase(t.begin());
                const double v = std::stod(t, &pos);
                if (std::isfinite(v)) return v;
            } catch (const std::exception&) { /* fall through */ }
        }
        return dflt;
    }
    long long asInt(long long dflt = 0) const { return static_cast<long long>(std::llround(asNumber(static_cast<double>(dflt)))); }
    bool asBool(bool dflt = false) const {
        if (type == Type::Bool) return boolean;
        if (type == Type::Number) return number != 0;
        if (type == Type::String) {
            const std::string t = text;
            if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
            if (t == "0" || t == "false" || t == "no" || t == "off" || t.empty()) return false;
        }
        return dflt;
    }
};

namespace detail {

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool bad = false;

    explicit Parser(const std::string& text) : s(text) {}

    void skip() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool literal(const char* word) {
        const size_t n = std::string(word).size();
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    std::string parseString() {
        std::string o;
        if (i >= s.size() || s[i] != '"') { bad = true; return o; }
        ++i;
        while (i < s.size() && s[i] != '"') {
            const char c = s[i];
            if (c == '\\' && i + 1 < s.size()) {
                ++i;
                const char e = s[i++];
                switch (e) {
                    case 'n': o += '\n'; break;
                    case 't': o += '\t'; break;
                    case 'r': o += '\r'; break;
                    case 'b': o += '\b'; break;
                    case 'f': o += '\f'; break;
                    case 'u': {   // \uXXXX - BMP only; a surrogate pair is kept as two escaped bytes (rare here)
                        unsigned code = 0;
                        if (i + 4 <= s.size()) {
                            try {
                                code = static_cast<unsigned>(std::stoul(s.substr(i, 4), nullptr, 16));
                            } catch (const std::exception&) { code = 0xFFFD; }
                            i += 4;
                        }
                        if (code < 0x80) o += static_cast<char>(code);
                        else if (code < 0x800) {
                            o += static_cast<char>(0xC0 | (code >> 6));
                            o += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            o += static_cast<char>(0xE0 | (code >> 12));
                            o += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            o += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: o += e;
                }
            } else {
                o += c;
                ++i;
            }
        }
        if (i < s.size()) ++i;   // closing quote
        else bad = true;
        return o;
    }
    Value parseValue() {
        skip();
        Value v;
        if (i >= s.size()) { bad = true; return v; }
        const char c = s[i];
        if (c == '{') {
            v.type = Value::Type::Object;
            ++i;
            skip();
            if (i < s.size() && s[i] == '}') { ++i; return v; }
            while (i < s.size()) {
                skip();
                if (s[i] != '"') { bad = true; return v; }
                const std::string key = parseString();
                skip();
                if (i >= s.size() || s[i] != ':') { bad = true; return v; }
                ++i;
                Value child = parseValue();
                if (bad) return v;
                v.fields[key] = std::move(child);
                skip();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                bad = true;
                return v;
            }
        } else if (c == '[') {
            v.type = Value::Type::Array;
            ++i;
            skip();
            if (i < s.size() && s[i] == ']') { ++i; return v; }
            while (i < s.size()) {
                Value child = parseValue();
                if (bad) return v;
                v.items.push_back(std::move(child));
                skip();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                bad = true;
                return v;
            }
        } else if (c == '"') {
            v.type = Value::Type::String;
            v.text = parseString();
        } else if (literal("true")) {
            v.type = Value::Type::Bool;
            v.boolean = true;
        } else if (literal("false")) {
            v.type = Value::Type::Bool;
            v.boolean = false;
        } else if (literal("null")) {
            v.type = Value::Type::Null;
        } else {
            const size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                                    s[i] == 'e' || s[i] == 'E'))
                ++i;
            if (i == start) { bad = true; return v; }
            try {
                v.number = std::stod(s.substr(start, i - start));
            } catch (const std::exception&) { bad = true; return v; }
            v.type = Value::Type::Number;
        }
        return v;
    }
};

}  // namespace detail

// Returns nullopt if `text` is not a well-formed JSON document.
inline std::optional<Value> parse(const std::string& text) {
    const std::string t = text.empty() ? std::string("null") : text;
    detail::Parser p(t);
    Value v = p.parseValue();
    if (p.bad) return std::nullopt;
    p.skip();
    if (p.i != t.size()) return std::nullopt;   // trailing garbage
    return v;
}

}  // namespace json

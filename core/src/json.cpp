#include "fixcore/json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace fixcore::json {
namespace {
const Value kNull;
const Array kEmptyArray;
const Object kEmptyObject;

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    std::string err;

    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool fail(const std::string& m) { if (err.empty()) err = m + " at offset " + std::to_string(i); return false; }

    bool lit(std::string_view w) {
        if (s.substr(i, w.size()) != w) return fail("unexpected token");
        i += w.size();
        return true;
    }

    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return fail("bad escape");
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("bad \\u escape");
                    unsigned cp = std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: return fail("bad escape");
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            ++i;
            Object o;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; out = Value(std::move(o)); return true; }
            while (true) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i++] != ':') return fail("expected ':'");
                Value v;
                if (!value(v, depth + 1)) return false;
                o[k] = std::move(v);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}'");
            }
            out = Value(std::move(o));
            return true;
        }
        if (c == '[') {
            ++i;
            Array a;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; out = Value(std::move(a)); return true; }
            while (true) {
                Value v;
                if (!value(v, depth + 1)) return false;
                a.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                return fail("expected ',' or ']'");
            }
            out = Value(std::move(a));
            return true;
        }
        if (c == '"') { std::string v; if (!str(v)) return false; out = Value(std::move(v)); return true; }
        if (c == 't') { if (!lit("true")) return false; out = Value(true); return true; }
        if (c == 'f') { if (!lit("false")) return false; out = Value(false); return true; }
        if (c == 'n') { if (!lit("null")) return false; out = Value(); return true; }
        std::size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' ||
                                s[i] == 'E' || s[i] == '+' || s[i] == '-'))
            ++i;
        if (start == i || (i == start + 1 && s[start] == '-')) return fail("unexpected character");
        out = Value(Number{std::string(s.substr(start, i - start))});
        return true;
    }
};
}  // namespace

long long Value::as_int(long long d) const {
    if (is_number()) return std::atoll(std::get<Number>(v_).text.c_str());
    if (is_string()) return std::atoll(std::get<std::string>(v_).c_str());
    return d;
}

std::string Value::as_string(std::string_view d) const {
    if (is_string()) return std::get<std::string>(v_);
    if (is_number()) return std::get<Number>(v_).text;
    return std::string(d);
}

const Array& Value::as_array() const { return is_array() ? *std::get<std::shared_ptr<Array>>(v_) : kEmptyArray; }
const Object& Value::as_object() const { return is_object() ? *std::get<std::shared_ptr<Object>>(v_) : kEmptyObject; }
Array& Value::as_array() {
    if (!is_array()) v_ = std::make_shared<Array>();
    return *std::get<std::shared_ptr<Array>>(v_);
}
Object& Value::as_object() {
    if (!is_object()) v_ = std::make_shared<Object>();
    return *std::get<std::shared_ptr<Object>>(v_);
}

const Value& Value::operator[](const std::string& key) const {
    if (!is_object()) return kNull;
    const auto& o = as_object();
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}
Value& Value::operator[](const std::string& key) { return as_object()[key]; }
bool Value::has(const std::string& key) const { return is_object() && as_object().count(key); }
std::optional<std::string> Value::get_string(const std::string& key) const {
    const auto& v = (*this)[key];
    if (v.is_string() || v.is_number()) return v.as_string();
    return std::nullopt;
}

std::string escape(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string Value::dump() const {
    if (is_null()) return "null";
    if (is_bool()) return as_bool() ? "true" : "false";
    if (is_number()) return std::get<Number>(v_).text;
    if (is_string()) return escape(std::get<std::string>(v_));
    std::string out;
    if (is_array()) {
        out = "[";
        bool first = true;
        for (const auto& v : as_array()) { if (!first) out += ","; first = false; out += v.dump(); }
        return out + "]";
    }
    out = "{";
    bool first = true;
    for (const auto& [k, v] : as_object()) {
        if (!first) out += ",";
        first = false;
        out += escape(k) + ":" + v.dump();
    }
    return out + "}";
}

std::optional<Value> parse(std::string_view text, std::string* error) {
    Parser p{text, 0, {}};
    Value v;
    if (!p.value(v, 0)) {
        if (error) *error = p.err;
        return std::nullopt;
    }
    p.ws();
    if (p.i != text.size()) {
        if (error) *error = "trailing characters at offset " + std::to_string(p.i);
        return std::nullopt;
    }
    return v;
}

}  // namespace fixcore::json

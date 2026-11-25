#pragma once
// Small JSON value type, parser and serializer: enough for config files and the
// gateway's HTTP API. Numbers are kept as their source text so decimal prices and
// quantities survive a round trip untouched.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fixcore::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Number { std::string text; };

class Value {
public:
    Value() : v_(nullptr) {}
    Value(std::nullptr_t) : v_(nullptr) {}
    Value(bool b) : v_(b) {}
    Value(int n) : v_(Number{std::to_string(n)}) {}
    Value(long n) : v_(Number{std::to_string(n)}) {}
    Value(long long n) : v_(Number{std::to_string(n)}) {}
    Value(unsigned long n) : v_(Number{std::to_string(n)}) {}
    Value(Number n) : v_(std::move(n)) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool is_bool() const { return std::holds_alternative<bool>(v_); }
    bool is_number() const { return std::holds_alternative<Number>(v_); }
    bool is_string() const { return std::holds_alternative<std::string>(v_); }
    bool is_array() const { return std::holds_alternative<std::shared_ptr<Array>>(v_); }
    bool is_object() const { return std::holds_alternative<std::shared_ptr<Object>>(v_); }

    bool as_bool(bool d = false) const { return is_bool() ? std::get<bool>(v_) : d; }
    long long as_int(long long d = 0) const;
    // Strings as-is; numbers as their text; otherwise d.
    std::string as_string(std::string_view d = {}) const;
    const Array& as_array() const;
    const Object& as_object() const;
    Array& as_array();
    Object& as_object();

    // Object access; returns a null Value when missing or not an object.
    const Value& operator[](const std::string& key) const;
    Value& operator[](const std::string& key);  // creates (turns null into an object)
    bool has(const std::string& key) const;
    std::optional<std::string> get_string(const std::string& key) const;

    std::string dump() const;

private:
    std::variant<std::nullptr_t, bool, Number, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_;
};

std::optional<Value> parse(std::string_view text, std::string* error = nullptr);
std::string escape(std::string_view s);  // quoted JSON string literal

}  // namespace fixcore::json

#pragma once
// Minimal test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace t {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
template <class A, class B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream os;
        os << file << ":" << line << ": CHECK_EQ(" << ea << ", " << eb << ")\n    got: " << a << "\n   want: " << b;
        std::cerr << os.str() << "\n";
        ++failures();
    }
}
}  // namespace t

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name) \
    static void name(); \
    static t::Reg TEST_CAT(reg_, name)(#name, name); \
    static void name()
#define CHECK(c) \
    do { if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #c ") failed\n"; ++t::failures(); } } while (0)
#define CHECK_EQ(a, b) t::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

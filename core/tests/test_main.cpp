#include "test.hpp"

int main() {
    int before;
    for (auto& c : t::cases()) {
        before = t::failures();
        c.fn();
        std::cout << (t::failures() == before ? "  ok   " : "  FAIL ") << c.name << "\n";
    }
    std::cout << t::cases().size() << " tests, " << t::failures() << " failed checks\n";
    return t::failures() ? 1 : 0;
}

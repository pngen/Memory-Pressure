#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mp_test {

struct TestCase { std::string name; std::function<void()> fn; };

inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }

struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline void expect(bool cond, const char* expr, const char* file, int line) {
    ++checks();
    if (!cond) {
        ++failures();
        std::printf("  FAIL: %s (%s:%d)\n", expr, file, line);
    }
}

inline int run_all() {
    for (auto& t : registry()) {
        const int before = failures();
        std::printf("[ RUN ] %s\n", t.name.c_str());
        t.fn();
        std::printf(failures() == before ? "[ PASS ] %s\n" : "[ FAIL ] %s\n", t.name.c_str());
    }
    std::printf("\n%d checks, %d failures\n", checks(), failures());
    return failures() == 0 ? 0 : 1;
}

} // namespace mp_test

#define TEST(name)                                        \
    static void mp_test_fn_##name();                     \
    static ::mp_test::Registrar mp_test_reg_##name(#name, mp_test_fn_##name); \
    static void mp_test_fn_##name()

#define EXPECT(cond) ::mp_test::expect((cond), #cond, __FILE__, __LINE__)

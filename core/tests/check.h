#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sve_test {
struct Case { const char *name; void (*fn)(); };
inline std::vector<Case> &registry() { static std::vector<Case> r; return r; }
inline int &failures() { static int n = 0; return n; }
struct Registrar { Registrar(const char *n, void (*f)()) { registry().push_back({n, f}); } };
}  // namespace sve_test

#define TEST(name) \
    static void test_##name(); \
    static sve_test::Registrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++sve_test::failures(); } } while (0)

#define CHECK_EQ(a, b) \
    do { if (!((a) == (b))) { std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); ++sve_test::failures(); } } while (0)

#ifdef SVE_TEST_MAIN
int main() {
    int passed = 0, failed = 0;
    for (auto &c : sve_test::registry()) {
        int before = sve_test::failures();
        c.fn();
        if (sve_test::failures() == before) { ++passed; std::printf("PASS %s\n", c.name); }
        else { ++failed; std::printf("FAIL %s\n", c.name); }
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
#endif

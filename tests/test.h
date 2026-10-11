/* tests/test.h — tiny self-registering test framework.
 *
 *   #include "../../tests/test.h"
 *   TEST(parses_simple_get) {
 *       CHECK(1 + 1 == 2);
 *       CHECK_INT(http_status(...), 200);
 *       CHECK_STR(name, "Adam");
 *   }
 *
 * Put tests next to the code as `<name>_test.c`; the build links every
 * *_test.c into build/test(.exe) and the runner executes them all.
 * A failing CHECK prints the location and returns from the test function. */
#ifndef HEMMA_TEST_H
#define HEMMA_TEST_H

#include "../core/base.h"

typedef void (*TestFn)(void);
void test_register(const char *name, const char *file, TestFn fn);
void test_fail(const char *file, int line, const char *fmt, ...) FMT(3, 4);
const char *test_tmpdir(void); /* fresh empty directory for this test */
extern int test_checks;

#define TEST(name)                                                                                                     \
    static void test_##name(void);                                                                                     \
    __attribute__((constructor)) static void test_reg_##name(void) { test_register(#name, __FILE__, test_##name); }    \
    static void test_##name(void)

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(cond)) {                                                                                                 \
            test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);                                                         \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#define CHECK_INT(a, b)                                                                                                \
    do {                                                                                                               \
        long long a_ = (long long)(a), b_ = (long long)(b);                                                            \
        test_checks++;                                                                                                 \
        if (a_ != b_) {                                                                                                \
            test_fail(__FILE__, __LINE__, "%s == %s  (%lld != %lld)", #a, #b, a_, b_);                                 \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        const char *a_ = (a), *b_ = (b);                                                                               \
        test_checks++;                                                                                                 \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                                                       \
            test_fail(__FILE__, __LINE__, "%s == %s\n      got:      \"%s\"\n      expected: \"%s\"", #a, #b,          \
                      a_ ? a_ : "(null)", b_ ? b_ : "(null)");                                                         \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/* Substring check — handy for rendered HTML / JSON. */
#define CHECK_HAS(hay, needle)                                                                                         \
    do {                                                                                                               \
        const char *h_ = (hay), *n_ = (needle);                                                                        \
        test_checks++;                                                                                                 \
        if (!h_ || !n_ || !strstr(h_, n_)) {                                                                           \
            test_fail(__FILE__, __LINE__, "%s contains \"%s\"\n      in: \"%.300s\"", #hay, n_ ? n_ : "(null)",       \
                      h_ ? h_ : "(null)");                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#endif

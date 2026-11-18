// test_util.hpp - the smallest possible check macro for the host tests.
#ifndef ID6TERM_TEST_UTIL_HPP
#define ID6TERM_TEST_UTIL_HPP

#include <stdio.h>
#include <stdlib.h>
#include <string>

static int g_checks = 0, g_failures = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ_STR(a, b) do { ++g_checks; std::string _a = (a), _b = (b); if (_a != _b) { ++g_failures; fprintf(stderr, "%s:%d: CHECK_EQ_STR failed:\n  got      \"%s\"\n  expected \"%s\"\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { ++g_failures; fprintf(stderr, "%s:%d: CHECK_EQ failed: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static int testSummary(const char* name) {
    printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

#endif

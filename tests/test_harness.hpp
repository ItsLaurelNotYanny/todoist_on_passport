#ifndef TEST_HARNESS_HPP
#define TEST_HARNESS_HPP

#include <iostream>
#include <string>
#include <cstdlib>

static int g_testsRun = 0;
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        std::cerr << "  [FAIL] " << __FILE__ << ":" << __LINE__ << " - Assertion failed: " #cond << std::endl; \
        return false; \
    } \
} while (0)

#define ASSERT_FALSE(cond) do { \
    if (cond) { \
        std::cerr << "  [FAIL] " << __FILE__ << ":" << __LINE__ << " - Assertion failed (expected false): " #cond << std::endl; \
        return false; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "  [FAIL] " << __FILE__ << ":" << __LINE__ << " - Assertion failed: (" #a ") == (" #b ") [" << (a) << " != " << (b) << "]" << std::endl; \
        return false; \
    } \
} while (0)

#define RUN_TEST(testFunc) do { \
    g_testsRun++; \
    std::cout << "[RUN ] " #testFunc << " ... " << std::flush; \
    if (testFunc()) { \
        g_testsPassed++; \
        std::cout << "PASSED" << std::endl; \
    } else { \
        g_testsFailed++; \
        std::cout << "FAILED" << std::endl; \
    } \
} while (0)

#endif // TEST_HARNESS_HPP

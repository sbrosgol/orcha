//
// Assertions.hpp - Always-on test assertions.
//
// Use these instead of <cassert>'s assert(): Release builds define NDEBUG,
// which turns assert() into a no-op that does NOT even evaluate its argument.
// Any assert() wrapping a side-effecting call (e.g. store.create_job(...)) is
// therefore skipped entirely under Release, making such tests silently vacuous
// (and prone to crash when a later line uses the un-created state).
//
// ORCHA_ASSERT always evaluates its expression and throws on failure; the test
// runners catch the exception and report a non-zero exit.
//

#pragma once

#include <stdexcept>
#include <string>

#define ORCHA_ASSERT(expr) \
    do { if (!(expr)) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        " Assertion failed: " #expr); } while(0)

#define ORCHA_ASSERT_EQ(a, b) \
    do { if ((a) != (b)) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        " Expected " #a " == " #b); } while(0)

#define ORCHA_ASSERT_TRUE(expr) ORCHA_ASSERT(expr)
#define ORCHA_ASSERT_FALSE(expr) ORCHA_ASSERT(!(expr))

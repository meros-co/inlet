// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

// The smallest test harness that says where it failed.

#include <cstdio>
#include <cstdlib>

namespace check
{
inline int failures = 0;
}

#define CHECK(cond)                                                                    \
    do                                                                                 \
    {                                                                                  \
        if (!(cond))                                                                   \
        {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++check::failures;                                                         \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do                                                                                 \
    {                                                                                  \
        const auto va_ = (a);                                                          \
        const auto vb_ = (b);                                                          \
        if (!(va_ == vb_))                                                             \
        {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: %lld vs %lld\n", __FILE__, \
                         __LINE__, #a, #b, (long long) va_, (long long) vb_);          \
            ++check::failures;                                                         \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                          \
    do                                                                                 \
    {                                                                                  \
        const double va_ = (double) (a);                                               \
        const double vb_ = (double) (b);                                               \
        if (!(va_ - vb_ <= (tol) && vb_ - va_ <= (tol)))                               \
        {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s) failed: %g vs %g\n", __FILE__, \
                         __LINE__, #a, #b, va_, vb_);                                  \
            ++check::failures;                                                         \
        }                                                                              \
    } while (0)

#define TEST_MAIN_END()                                                                \
    do                                                                                 \
    {                                                                                  \
        if (check::failures)                                                           \
            std::fprintf(stderr, "%d check(s) failed\n", check::failures);             \
        return check::failures ? EXIT_FAILURE : EXIT_SUCCESS;                          \
    } while (0)

#pragma once

#include <cstdio>
#include <cstdlib>

// Invariant checks that stay out of release builds (plan IV.4: "checked by a
// debug-build assertion").
#ifndef NDEBUG
#define LB_DEBUG_ASSERT(condition, message)                                                          \
    do {                                                                                             \
        if (!(condition)) {                                                                          \
            std::fprintf(stderr, "LB_DEBUG_ASSERT failed: %s (%s) at %s:%d\n", message, #condition, \
                         __FILE__, __LINE__);                                                        \
            std::abort();                                                                            \
        }                                                                                            \
    } while (0)
#else
#define LB_DEBUG_ASSERT(condition, message) ((void)0)
#endif

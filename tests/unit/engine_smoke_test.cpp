#include <gtest/gtest.h>

#include "core/clock.h"
#include "engine.h"

static_assert(lb::Clock::is_steady, "engine timing must use a monotonic clock (plan II.8)");

TEST(EngineSmoke, ReportsProjectVersion) {
    EXPECT_EQ(lb::engine_version(), LB_EXPECTED_VERSION);
}

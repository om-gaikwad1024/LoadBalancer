#include <gtest/gtest.h>

#include "health/hysteresis.h"

using lb::health::Hysteresis;
using Change = Hysteresis::Change;

// Plan IV.10: down after N consecutive failures, up after M consecutive successes.

TEST(Hysteresis, GoesDownOnlyAfterNConsecutiveFailures) {
    Hysteresis h(3, 2);
    EXPECT_EQ(h.record(false, true), Change::None);
    EXPECT_EQ(h.record(false, true), Change::None);
    EXPECT_EQ(h.record(false, true), Change::MarkDown);
    EXPECT_EQ(h.failures_in_a_row(), 3u);
}

TEST(Hysteresis, ASingleFailureNeverRemovesABackend) {
    Hysteresis h(3, 2);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(h.record(i % 3 == 0 ? false : true, true), Change::None) << i;
    }
}

TEST(Hysteresis, FlappingJustBelowTheThresholdNeverRemoves) {
    Hysteresis h(3, 2);
    for (int i = 0; i < 30; ++i) {  // two failures, one success, repeated
        EXPECT_EQ(h.record(false, true), Change::None);
        EXPECT_EQ(h.record(false, true), Change::None);
        EXPECT_EQ(h.record(true, true), Change::None);
    }
}

TEST(Hysteresis, ComesBackOnlyAfterMConsecutiveSuccesses) {
    Hysteresis h(3, 2);
    for (int i = 0; i < 3; ++i) h.record(false, true);
    EXPECT_EQ(h.record(true, false), Change::None);
    EXPECT_EQ(h.record(false, false), Change::None);  // a failure resets the success streak
    EXPECT_EQ(h.record(true, false), Change::None);
    EXPECT_EQ(h.record(true, false), Change::MarkUp);
}

TEST(Hysteresis, NoChangeIsReportedForTheStateItIsAlreadyIn) {
    Hysteresis h(1, 1);
    EXPECT_EQ(h.record(true, true), Change::None);     // already up
    EXPECT_EQ(h.record(false, false), Change::None);   // already down
    EXPECT_EQ(h.record(false, true), Change::MarkDown);
    EXPECT_EQ(h.record(true, false), Change::MarkUp);
}

TEST(Hysteresis, StreakCountersSaturate) {
    Hysteresis h(3, 2);
    for (int i = 0; i < 1000; ++i) h.record(false, false);
    EXPECT_EQ(h.failures_in_a_row(), 3u);
    for (int i = 0; i < 1000; ++i) h.record(true, true);
    EXPECT_EQ(h.successes_in_a_row(), 2u);
    EXPECT_EQ(h.failures_in_a_row(), 0u);
}

// Passive checks (step 2.5) can mark a backend down while its probes keep succeeding. The
// successes before that must not count: re-inclusion needs M new ones.
TEST(Hysteresis, MarkDownByAnotherCheckNeedsMFreshSuccesses) {
    Hysteresis h(3, 3);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(h.record(true, true), Change::None);  // streak saturated at 3
    h.restart_successes();  // marked down by real traffic
    EXPECT_EQ(h.record(true, false), Change::None);
    EXPECT_EQ(h.record(true, false), Change::None);
    EXPECT_EQ(h.record(true, false), Change::MarkUp);
}

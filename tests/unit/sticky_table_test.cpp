// Step 2.4 (plan IV.9): the sticky-session table.

#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "affinity/sticky_table.h"

using lb::affinity::StickyTable;
using namespace std::chrono_literals;

namespace {

const lb::TimePoint t0 = lb::Clock::now();
constexpr lb::Duration kTtl = std::chrono::duration_cast<lb::Duration>(10s);

lb::StickyTableConfig table_config(std::uint32_t shards, std::uint32_t max_entries) {
    lb::StickyTableConfig c;
    c.shards = shards;
    c.max_entries = max_entries;
    return c;
}

}  // namespace

TEST(StickyTable, MapsAKeyToABackendPerGroup) {
    StickyTable t(table_config(4, 100));
    EXPECT_FALSE(t.find("web", "abc", t0, kTtl).has_value());
    ASSERT_TRUE(t.assign("web", "abc", "web-2", t0, kTtl));
    EXPECT_EQ(t.find("web", "abc", t0, kTtl), "web-2");
    // The same key in another group is a different session (plan IV.9: per group).
    EXPECT_FALSE(t.find("api", "abc", t0, kTtl).has_value());
    ASSERT_TRUE(t.assign("api", "abc", "api-1", t0, kTtl));
    EXPECT_EQ(t.find("web", "abc", t0, kTtl), "web-2");
    EXPECT_EQ(t.find("api", "abc", t0, kTtl), "api-1");
    // Reassignment replaces the mapping.
    ASSERT_TRUE(t.assign("web", "abc", "web-3", t0, kTtl));
    EXPECT_EQ(t.find("web", "abc", t0, kTtl), "web-3");
    EXPECT_EQ(t.size(), 2u);
}

TEST(StickyTable, TtlSlidesWithEveryUseAndExpiresWhenUnused) {
    StickyTable t(table_config(1, 10));
    ASSERT_TRUE(t.assign("web", "k", "web-1", t0, kTtl));
    // Used just before it would expire: the TTL starts again from then.
    EXPECT_EQ(t.find("web", "k", t0 + kTtl - 1ms, kTtl), "web-1");
    EXPECT_EQ(t.find("web", "k", t0 + 2 * kTtl - 2ms, kTtl), "web-1");
    // Unused for a whole TTL: gone, and removed from the table.
    EXPECT_FALSE(t.find("web", "k", t0 + 3 * kTtl, kTtl).has_value());
    EXPECT_EQ(t.size(), 0u);
}

TEST(StickyTable, SweepRemovesOnlyExpiredMappings) {
    StickyTable t(table_config(8, 1000));
    for (int i = 0; i < 50; ++i) ASSERT_TRUE(t.assign("web", "old" + std::to_string(i), "web-1", t0, kTtl));
    for (int i = 0; i < 30; ++i) ASSERT_TRUE(t.assign("web", "new" + std::to_string(i), "web-1", t0 + kTtl, kTtl));
    EXPECT_EQ(t.sweep(t0 + kTtl), 50u);
    EXPECT_EQ(t.size(), 30u);
    EXPECT_EQ(t.sweep(t0 + kTtl), 0u);
}

TEST(StickyTable, FullTableStoresNoNewSessionsButStillUpdatesExistingOnes) {
    StickyTable t(table_config(1, 2));
    ASSERT_TRUE(t.assign("web", "a", "web-1", t0, kTtl));
    ASSERT_TRUE(t.assign("web", "b", "web-1", t0, kTtl));
    EXPECT_FALSE(t.assign("web", "c", "web-1", t0, kTtl));
    EXPECT_EQ(t.not_stored(), 1u);
    EXPECT_TRUE(t.assign("web", "a", "web-2", t0, kTtl));  // reassigning an existing session
    EXPECT_EQ(t.find("web", "a", t0, kTtl), "web-2");
    // Expired mappings make room again.
    t.sweep(t0 + kTtl);
    EXPECT_TRUE(t.assign("web", "c", "web-1", t0 + kTtl, kTtl));
}

TEST(StickyTable, SessionKeysAreRandomHex) {
    std::set<std::string> keys;
    for (int i = 0; i < 2000; ++i) {
        const std::string k = StickyTable::new_session_key();
        EXPECT_TRUE(StickyTable::is_session_key(k)) << k;
        keys.insert(k);
    }
    EXPECT_EQ(keys.size(), 2000u);
    EXPECT_FALSE(StickyTable::is_session_key(""));
    EXPECT_FALSE(StickyTable::is_session_key("0123456789abcdef0123456789abcde"));    // 31
    EXPECT_FALSE(StickyTable::is_session_key("0123456789ABCDEF0123456789abcdef"));   // upper case
    EXPECT_FALSE(StickyTable::is_session_key("0123456789abcdef0123456789abcdeg"));   // not hex
}

TEST(StickyTable, ConcurrentUseFromManyThreads) {
    StickyTable t(table_config(16, 100000));
    std::vector<std::thread> threads;
    for (int w = 0; w < 8; ++w) {
        threads.emplace_back([&t, w] {
            for (int i = 0; i < 2000; ++i) {
                const std::string key = std::to_string(w) + ":" + std::to_string(i);
                t.assign("web", key, "web-" + std::to_string(w), t0, kTtl);
                const auto found = t.find("web", key, t0, kTtl);
                EXPECT_EQ(found, "web-" + std::to_string(w));
            }
        });
    }
    threads.emplace_back([&t] {
        for (int i = 0; i < 200; ++i) t.sweep(t0);  // nothing has expired yet
    });
    for (auto& th : threads) th.join();
    EXPECT_EQ(t.size(), 16000u);
}

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "config/config_store.h"

namespace {

// Every numeric field carries the same generation number, so a reader can detect a
// snapshot that mixes fields from two generations.
std::shared_ptr<const lb::ConfigSnapshot> make_snapshot(std::uint16_t generation) {
    auto s = std::make_shared<lb::ConfigSnapshot>();
    s->listen = {"127.0.0.1", generation};
    s->workers.threads = generation;
    s->groups.push_back({"g", {{"b", "127.0.0.1", generation, generation}}});
    s->routing.default_group = "g";
    return s;
}

bool is_consistent(const lb::ConfigSnapshot& s) {
    const auto g = s.listen.port;
    return s.workers.threads == g && s.groups.size() == 1 && s.groups[0].backends.size() == 1 &&
           s.groups[0].backends[0].port == g && s.groups[0].backends[0].weight == g;
}

}  // namespace

static_assert(std::is_same_v<decltype(std::declval<lb::ConfigStore&>().current()),
                             std::shared_ptr<const lb::ConfigSnapshot>>,
              "snapshots are handed out read-only (plan II.7)");

TEST(ConfigStore, CurrentReturnsLatestPublished) {
    lb::ConfigStore store(make_snapshot(1));
    EXPECT_EQ(store.current()->listen.port, 1);
    store.publish(make_snapshot(2));
    EXPECT_EQ(store.current()->listen.port, 2);
}

TEST(ConfigStore, CapturedSnapshotIsUnaffectedByLaterPublish) {
    lb::ConfigStore store(make_snapshot(1));
    const auto captured = store.current();  // what a request captures when it starts
    store.publish(make_snapshot(2));

    EXPECT_EQ(captured->listen.port, 1);  // still alive and unchanged
    EXPECT_TRUE(is_consistent(*captured));
    EXPECT_EQ(store.current()->listen.port, 2);
}

TEST(ConfigStore, ConcurrentReadersNeverSeeAHalfAppliedSnapshot) {
    lb::ConfigStore store(make_snapshot(1));
    std::atomic<bool> stop{false};
    std::atomic<int> inconsistent{0};
    std::atomic<long long> reads{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto s = store.current();
                if (!is_consistent(*s)) inconsistent.fetch_add(1);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::uint16_t gen = 2; gen < 20000; ++gen) store.publish(make_snapshot(gen));
    stop = true;
    for (auto& t : readers) t.join();

    EXPECT_EQ(inconsistent.load(), 0);
    EXPECT_GT(reads.load(), 0);
    EXPECT_EQ(store.current()->listen.port, 19999);
}

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "balance/group_balancer.h"

using lb::BackendState;
using lb::Strategy;
using lb::backend::BackendRuntime;
using lb::balance::GroupBalancer;

namespace {

class NoSockets final : public lb::backend::SocketOps {
public:
    bool is_stale(SOCKET) noexcept override { return false; }
    void close(SOCKET) noexcept override {}
};

NoSockets g_ops;

std::shared_ptr<GroupBalancer> group(Strategy strategy, int backends) {
    std::vector<std::shared_ptr<BackendRuntime>> members;
    lb::backend::PoolLimits limits;
    limits.max_connections = 1;
    for (int i = 1; i <= backends; ++i) {
        lb::BackendConfig c{"b" + std::to_string(i), "127.0.0.1", static_cast<std::uint16_t>(9000 + i), 1};
        members.push_back(std::make_shared<BackendRuntime>(c, "g", limits, g_ops));
    }
    return std::make_shared<GroupBalancer>("g", strategy, std::move(members));
}

std::shared_ptr<GroupBalancer> weighted(Strategy strategy, const std::vector<std::uint32_t>& weights) {
    std::vector<std::shared_ptr<BackendRuntime>> members;
    lb::backend::PoolLimits limits;
    limits.max_connections = 1;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        lb::BackendConfig c{"b" + std::to_string(i + 1), "127.0.0.1", static_cast<std::uint16_t>(9001 + i), weights[i]};
        members.push_back(std::make_shared<BackendRuntime>(c, "g", limits, g_ops));
    }
    return std::make_shared<GroupBalancer>("g", strategy, std::move(members));
}

std::map<std::string, int> distribution(GroupBalancer& g, int picks) {
    std::map<std::string, int> counts;
    for (int i = 0; i < picks; ++i) {
        const auto b = g.pick();
        counts[b ? b->id : "none"]++;
    }
    return counts;
}

}  // namespace

// ---- Round robin -------------------------------------------------------------------------

TEST(RoundRobin, SpreadsEvenlyInRotation) {
    auto g = group(Strategy::RoundRobin, 3);
    std::vector<std::string> order;
    for (int i = 0; i < 6; ++i) order.push_back(g->pick()->id);
    EXPECT_EQ(order, (std::vector<std::string>{"b1", "b2", "b3", "b1", "b2", "b3"}));
    const auto d = distribution(*g, 300);
    EXPECT_EQ(d.at("b1"), 100);
    EXPECT_EQ(d.at("b2"), 100);
    EXPECT_EQ(d.at("b3"), 100);
}

TEST(RoundRobin, ExcludedBackendsShareIsSpreadEvenly) {
    auto g = group(Strategy::RoundRobin, 3);
    g->members[1]->state = BackendState::Unhealthy;
    const auto d = distribution(*g, 300);
    EXPECT_EQ(d.count("b2"), 0u);
    EXPECT_EQ(d.at("b1"), 150);
    EXPECT_EQ(d.at("b3"), 150);
}

TEST(RoundRobin, DrainingIsExcludedToo) {
    auto g = group(Strategy::RoundRobin, 2);
    g->members[0]->state = BackendState::Draining;
    const auto d = distribution(*g, 10);
    EXPECT_EQ(d.at("b2"), 10);
}

// ---- Least connections -------------------------------------------------------------------

TEST(LeastConnections, PicksTheBackendWithFewestInFlightRequests) {
    auto g = group(Strategy::LeastConnections, 3);
    g->members[0]->in_flight = 5;
    g->members[1]->in_flight = 1;
    g->members[2]->in_flight = 3;
    for (int i = 0; i < 10; ++i) EXPECT_EQ(g->pick()->id, "b2");
}

TEST(LeastConnections, TiesAreBrokenByRotationNotAlwaysTheFirst) {
    auto g = group(Strategy::LeastConnections, 3);  // all idle: every pick is a tie
    const auto d = distribution(*g, 300);
    EXPECT_EQ(d.at("b1"), 100);
    EXPECT_EQ(d.at("b2"), 100);
    EXPECT_EQ(d.at("b3"), 100);
}

TEST(LeastConnections, IneligibleBackendIsSkippedEvenWhenIdlest) {
    auto g = group(Strategy::LeastConnections, 2);
    g->members[0]->in_flight = 0;
    g->members[0]->state = BackendState::Unhealthy;
    g->members[1]->in_flight = 50;
    EXPECT_EQ(g->pick()->id, "b2");
}

TEST(LeastConnections, ConcurrentLoadSpreadsByInFlightCount) {
    auto g = group(Strategy::LeastConnections, 2);
    // Simulate requests that never finish on b1 (slow) while b2's finish at once.
    std::map<std::string, int> counts;
    for (int i = 0; i < 100; ++i) {
        auto b = g->pick();
        counts[b->id]++;
        if (b->id == "b1") b->in_flight++;  // b1 keeps every request in flight
    }
    EXPECT_EQ(counts["b1"], 1);  // after one slow request, b2 is always less loaded
    EXPECT_EQ(counts["b2"], 99);
}

// ---- No eligible backend -----------------------------------------------------------------

TEST(GroupBalancer, NoEligibleBackendReturnsNothing) {
    for (Strategy s : {Strategy::RoundRobin, Strategy::LeastConnections, Strategy::WeightedRoundRobin,
                       Strategy::LeastResponseTime, Strategy::IpHash}) {
        auto g = group(s, 2);
        g->members[0]->state = BackendState::Unhealthy;
        g->members[1]->state = BackendState::Draining;
        EXPECT_EQ(g->pick(), nullptr);
        g->members[1]->state = BackendState::Healthy;
        EXPECT_EQ(g->pick()->id, "b2");
    }
    EXPECT_EQ(group(Strategy::RoundRobin, 0)->pick(), nullptr);
    EXPECT_EQ(group(Strategy::LeastConnections, 0)->pick(), nullptr);
    EXPECT_EQ(group(Strategy::WeightedRoundRobin, 0)->pick(), nullptr);
    EXPECT_EQ(group(Strategy::LeastResponseTime, 0)->pick(), nullptr);
    EXPECT_EQ(group(Strategy::IpHash, 0)->pick(), nullptr);
}

// ---- Weighted round robin (plan IV.7, phase 2) ------------------------------------------

TEST(WeightedRoundRobin, CycleGivesEachBackendItsWeightInterleaved) {
    const auto g = weighted(Strategy::WeightedRoundRobin, {3, 1});
    EXPECT_EQ(g->weighted_schedule(), (std::vector<std::uint32_t>{0, 0, 1, 0}));
    // Weights are reduced by their common divisor: 300:100 is the same 4-turn cycle.
    EXPECT_EQ(weighted(Strategy::WeightedRoundRobin, {300, 100})->weighted_schedule().size(), 4u);
    // 5:5:1 alternates the heavy backends instead of running each 5 times in a row.
    const auto heavy = weighted(Strategy::WeightedRoundRobin, {5, 5, 1});
    const auto& cycle = heavy->weighted_schedule();
    ASSERT_EQ(cycle.size(), 11u);
    for (std::size_t i = 1; i < cycle.size(); ++i) EXPECT_NE(cycle[i], cycle[i - 1]) << i;
}

TEST(WeightedRoundRobin, WeightsThreeToOneGiveExactlyThreeToOne) {
    const auto g = weighted(Strategy::WeightedRoundRobin, {3, 1});
    const auto counts = distribution(*g, 4000);
    EXPECT_EQ(counts.at("b1"), 3000);
    EXPECT_EQ(counts.at("b2"), 1000);
}

TEST(WeightedRoundRobin, ExcludedBackendTurnsFollowTheRemainingWeights) {
    const auto g = weighted(Strategy::WeightedRoundRobin, {3, 2, 1});
    g->members[0]->state = BackendState::Unhealthy;
    const auto counts = distribution(*g, 6000);
    EXPECT_EQ(counts.count("b1"), 0u);
    EXPECT_EQ(counts.count("none"), 0u);
    EXPECT_EQ(counts.at("b2") + counts.at("b3"), 6000);
    EXPECT_NEAR(static_cast<double>(counts.at("b2")) / counts.at("b3"), 2.0, 0.02);
}

// ---- Least response time (plan IV.7, phase 2) -------------------------------------------

namespace {

const lb::TimePoint t0 = lb::Clock::now();
constexpr auto kDecay = std::chrono::milliseconds(1000);
constexpr auto kExpiry = std::chrono::milliseconds(5000);

lb::balance::PickContext at(lb::TimePoint now) {
    lb::balance::PickContext c;
    c.now = now;
    c.response_time_expiry = kExpiry;
    return c;
}

}  // namespace

TEST(LeastResponseTime, AverageIsTimeWeighted) {
    const auto g = weighted(Strategy::LeastResponseTime, {1});
    auto& b = *g->members[0];
    EXPECT_FALSE(b.response_time_us(t0, kExpiry).has_value());
    EXPECT_EQ(b.stats().response_time_ms, -1);
    b.record_response_time(std::chrono::milliseconds(10), t0, kDecay);
    EXPECT_DOUBLE_EQ(*b.response_time_us(t0, kExpiry), 10'000.0);  // the first sample is the average
    // One time constant later the old average weighs 1/e.
    b.record_response_time(std::chrono::milliseconds(20), t0 + kDecay, kDecay);
    EXPECT_NEAR(*b.response_time_us(t0 + kDecay, kExpiry), 10'000.0 * 0.36788 + 20'000.0 * 0.63212, 1.0);
    // A sample at the same instant as the previous one carries no weight.
    const double before = *b.response_time_us(t0 + kDecay, kExpiry);
    b.record_response_time(std::chrono::milliseconds(500), t0 + kDecay, kDecay);
    EXPECT_DOUBLE_EQ(*b.response_time_us(t0 + kDecay, kExpiry), before);
    // Too old: no average, so the backend is measured again.
    EXPECT_FALSE(b.response_time_us(t0 + kDecay + kExpiry + std::chrono::milliseconds(1), kExpiry).has_value());
    EXPECT_NEAR(b.stats().response_time_ms, before / 1000.0, 1e-9);
}

TEST(LeastResponseTime, PrefersTheFasterBackend) {
    const auto g = weighted(Strategy::LeastResponseTime, {1, 1});
    g->members[0]->record_response_time(std::chrono::milliseconds(10), t0, kDecay);
    g->members[1]->record_response_time(std::chrono::milliseconds(1), t0, kDecay);
    for (int i = 0; i < 10; ++i) EXPECT_EQ(g->pick(at(t0))->id, "b2");
}

TEST(LeastResponseTime, QueuedRequestsOutweighSpeedEventually) {
    const auto g = weighted(Strategy::LeastResponseTime, {1, 1});
    g->members[0]->record_response_time(std::chrono::milliseconds(10), t0, kDecay);
    g->members[1]->record_response_time(std::chrono::milliseconds(1), t0, kDecay);
    g->members[1]->in_flight = 8;  // 1 ms x 9 is still cheaper than 10 ms x 1
    EXPECT_EQ(g->pick(at(t0))->id, "b2");
    g->members[1]->in_flight = 10;  // 1 ms x 11 is not
    EXPECT_EQ(g->pick(at(t0))->id, "b1");
}

TEST(LeastResponseTime, WithoutAveragesItIsLeastConnections) {
    const auto g = weighted(Strategy::LeastResponseTime, {1, 1, 1});
    g->members[0]->in_flight = 2;
    g->members[2]->in_flight = 1;
    EXPECT_EQ(g->pick(at(t0))->id, "b2");
    g->members[1]->in_flight = 1;
    std::map<std::string, int> counts;
    for (int i = 0; i < 100; ++i) counts[g->pick(at(t0))->id]++;  // b2 and b3 tie: rotation
    EXPECT_GT(counts["b2"], 0);
    EXPECT_GT(counts["b3"], 0);
    EXPECT_EQ(counts["b1"], 0);
}

TEST(LeastResponseTime, BackendWithAnExpiredAverageIsTriedAgain) {
    const auto g = weighted(Strategy::LeastResponseTime, {1, 1});
    g->members[0]->record_response_time(std::chrono::milliseconds(100), t0, kDecay);  // slow, then idle
    const auto later = t0 + kExpiry + std::chrono::milliseconds(10);
    g->members[1]->record_response_time(std::chrono::milliseconds(1), later, kDecay);
    EXPECT_EQ(g->pick(at(t0))->id, "b2");
    // The slow average expired: b1 is scored like the best known one and gets a share again.
    std::map<std::string, int> counts;
    for (int i = 0; i < 10; ++i) counts[g->pick(at(later))->id]++;
    EXPECT_GT(counts["b1"], 0);
}

TEST(LeastResponseTime, IneligibleBackendIsSkippedEvenWhenFastest) {
    const auto g = weighted(Strategy::LeastResponseTime, {1, 1});
    g->members[0]->record_response_time(std::chrono::milliseconds(10), t0, kDecay);
    g->members[1]->record_response_time(std::chrono::milliseconds(1), t0, kDecay);
    g->members[1]->state = BackendState::Draining;
    EXPECT_EQ(g->pick(at(t0))->id, "b1");
}

// ---- IP hash (plan IV.7, phase 2) ---------------------------------------------------------

namespace {

lb::balance::PickContext client(int i) {
    lb::balance::PickContext c;
    c.client_hash = lb::balance::hash_key("10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256));
    return c;
}

}  // namespace

TEST(IpHash, SameClientAlwaysGetsTheSameBackend) {
    const auto g = weighted(Strategy::IpHash, {1, 1, 1});
    for (int c = 0; c < 50; ++c) {
        const auto first = g->pick(client(c));
        for (int i = 0; i < 20; ++i) EXPECT_EQ(g->pick(client(c)), first);
    }
    // A rebuilt group (what a reload does) maps every client the same way.
    const auto rebuilt = std::make_shared<GroupBalancer>("g", Strategy::IpHash, g->members);
    for (int c = 0; c < 200; ++c) EXPECT_EQ(rebuilt->pick(client(c)), g->pick(client(c)));
}

TEST(IpHash, ClientsSpreadInProportionToWeights) {
    std::map<std::string, int> even;
    const auto g = weighted(Strategy::IpHash, {1, 1, 1});
    for (int c = 0; c < 30000; ++c) even[g->pick(client(c))->id]++;
    for (const auto& [id, n] : even) EXPECT_NEAR(n, 10000, 500) << id;

    std::map<std::string, int> shares;
    const auto w = weighted(Strategy::IpHash, {3, 1});
    for (int c = 0; c < 20000; ++c) shares[w->pick(client(c))->id]++;
    EXPECT_NEAR(shares["b1"] / 20000.0, 0.75, 0.02);
}

TEST(IpHash, OnlyTheExcludedBackendsClientsMove) {
    const auto g = weighted(Strategy::IpHash, {1, 1, 1, 1});
    std::vector<std::string> before;
    for (int c = 0; c < 4000; ++c) before.push_back(g->pick(client(c))->id);
    g->members[1]->state = BackendState::Unhealthy;
    int moved = 0;
    for (int c = 0; c < 4000; ++c) {
        const std::string now = g->pick(client(c))->id;
        if (before[c] == "b2") {
            EXPECT_NE(now, "b2");
            ++moved;
        } else {
            EXPECT_EQ(now, before[c]) << "client " << c << " moved although its backend stayed eligible";
        }
    }
    EXPECT_GT(moved, 800);
    g->members[1]->state = BackendState::Healthy;  // back: every client returns to its backend
    for (int c = 0; c < 4000; ++c) EXPECT_EQ(g->pick(client(c))->id, before[c]);
}

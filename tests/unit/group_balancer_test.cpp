#include <gtest/gtest.h>

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
    for (Strategy s : {Strategy::RoundRobin, Strategy::LeastConnections}) {
        auto g = group(s, 2);
        g->members[0]->state = BackendState::Unhealthy;
        g->members[1]->state = BackendState::Draining;
        EXPECT_EQ(g->pick(), nullptr);
        g->members[1]->state = BackendState::Healthy;
        EXPECT_EQ(g->pick()->id, "b2");
    }
    EXPECT_EQ(group(Strategy::RoundRobin, 0)->pick(), nullptr);
    EXPECT_EQ(group(Strategy::LeastConnections, 0)->pick(), nullptr);
}

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <vector>

#include "backend/connection_pool.h"
#include "backend/registry.h"

using lb::backend::ConnectionPool;
using lb::backend::PoolLimits;
using lb::backend::PoolTicket;
using Acquire = ConnectionPool::Acquire;

namespace {

class FakeSocketOps final : public lb::backend::SocketOps {
public:
    bool is_stale(SOCKET s) noexcept override { return stale.count(s) > 0; }
    void close(SOCKET s) noexcept override { closed.push_back(s); }
    bool was_closed(SOCKET s) const { return std::find(closed.begin(), closed.end(), s) != closed.end(); }

    std::set<SOCKET> stale;
    std::vector<SOCKET> closed;
};

PoolLimits limits(std::uint32_t max_connections, std::uint32_t max_idle, std::uint32_t max_waiters,
                  std::chrono::milliseconds idle_timeout = std::chrono::seconds(30)) {
    PoolLimits l;
    l.max_connections = max_connections;
    l.max_idle = max_idle;
    l.max_waiters = max_waiters;
    l.idle_timeout = idle_timeout;
    return l;
}

std::shared_ptr<PoolTicket> ticket(int* woken) {
    auto t = std::make_shared<PoolTicket>();
    t->wake = [woken] { ++*woken; };
    return t;
}

const auto t0 = lb::Clock::now();

}  // namespace

TEST(ConnectionPool, OpensUpToTheCapThenQueuesThenRejects) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(2, 2, 1), ops);
    SOCKET s = INVALID_SOCKET;
    int woken = 0;
    EXPECT_EQ(pool.acquire(&s, nullptr), Acquire::Connect);
    EXPECT_EQ(pool.acquire(&s, nullptr), Acquire::Connect);
    EXPECT_EQ(pool.acquire(&s, ticket(&woken)), Acquire::Queued);
    EXPECT_EQ(pool.acquire(&s, ticket(&woken)), Acquire::Rejected);  // queue holds one
    const auto st = pool.stats();
    EXPECT_EQ(st.open, 2u);
    EXPECT_EQ(st.waiting, 1u);
    EXPECT_EQ(st.opened, 2u);
    EXPECT_EQ(woken, 0);
}

TEST(ConnectionPool, ReusableConnectionGoesIdleAndIsReusedMostRecentFirst) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(4, 4, 0), ops);
    SOCKET s = INVALID_SOCKET;
    ASSERT_EQ(pool.acquire(&s, nullptr), Acquire::Connect);
    ASSERT_EQ(pool.acquire(&s, nullptr), Acquire::Connect);
    pool.release(101, true, t0);
    pool.release(102, true, t0);
    EXPECT_EQ(pool.stats().idle, 2u);
    ASSERT_EQ(pool.acquire(&s, nullptr), Acquire::Reused);
    EXPECT_EQ(s, 102u);
    EXPECT_EQ(pool.stats().reused, 1u);
    EXPECT_EQ(pool.stats().open, 2u);  // still two slots: one idle, one in use
}

TEST(ConnectionPool, StaleIdleConnectionsAreDiscardedOnReuse) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(4, 4, 0), ops);
    SOCKET s = INVALID_SOCKET;
    pool.acquire(&s, nullptr);
    pool.acquire(&s, nullptr);
    pool.release(101, true, t0);
    pool.release(102, true, t0);
    ops.stale.insert(102);  // the backend closed it while idle

    ASSERT_EQ(pool.acquire(&s, nullptr), Acquire::Reused);
    EXPECT_EQ(s, 101u);
    EXPECT_TRUE(ops.was_closed(102));
    const auto st = pool.stats();
    EXPECT_EQ(st.stale_discarded, 1u);
    EXPECT_EQ(st.open, 1u);
}

TEST(ConnectionPool, AllStaleMeansANewConnection) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(1, 1, 0), ops);
    SOCKET s = INVALID_SOCKET;
    pool.acquire(&s, nullptr);
    pool.release(101, true, t0);
    ops.stale.insert(101);
    EXPECT_EQ(pool.acquire(&s, nullptr), Acquire::Connect);  // the stale one freed its slot
}

TEST(ConnectionPool, ReleasedConnectionGoesStraightToAWaiter) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(1, 1, 4), ops);
    SOCKET s = INVALID_SOCKET;
    int woken = 0;
    pool.acquire(&s, nullptr);
    const auto t = ticket(&woken);
    ASSERT_EQ(pool.acquire(&s, t), Acquire::Queued);

    pool.release(101, true, t0);
    EXPECT_EQ(woken, 1);
    EXPECT_TRUE(t->settled);
    EXPECT_EQ(t->socket, 101u);
    EXPECT_FALSE(t->may_connect);
    EXPECT_EQ(pool.stats().idle, 0u);
    EXPECT_EQ(pool.stats().open, 1u);
}

TEST(ConnectionPool, ClosedConnectionPassesItsSlotToAWaiter) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(1, 1, 4), ops);
    SOCKET s = INVALID_SOCKET;
    int woken = 0;
    pool.acquire(&s, nullptr);
    const auto t = ticket(&woken);
    pool.acquire(&s, t);

    pool.release(101, false, t0);
    EXPECT_TRUE(ops.was_closed(101));
    EXPECT_EQ(woken, 1);
    EXPECT_TRUE(t->may_connect);
    EXPECT_EQ(pool.stats().open, 1u);  // the slot moved to the waiter
}

TEST(ConnectionPool, FailedConnectPassesItsSlotToAWaiter) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(1, 1, 4), ops);
    SOCKET s = INVALID_SOCKET;
    int woken = 0;
    pool.acquire(&s, nullptr);
    const auto t = ticket(&woken);
    pool.acquire(&s, t);
    pool.abandon_slot();
    EXPECT_EQ(woken, 1);
    EXPECT_TRUE(t->may_connect);
}

TEST(ConnectionPool, CancelSettlesATicketExactlyOnce) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(1, 1, 4), ops);
    SOCKET s = INVALID_SOCKET;
    int woken = 0;
    pool.acquire(&s, nullptr);
    const auto t = ticket(&woken);
    pool.acquire(&s, t);

    EXPECT_TRUE(pool.cancel(t));   // e.g. the wait timed out
    EXPECT_FALSE(pool.cancel(t));  // already settled
    pool.release(101, true, t0);   // nobody is waiting any more
    EXPECT_EQ(woken, 0);           // cancel's caller wakes it, not the pool
    EXPECT_EQ(pool.stats().idle, 1u);
}

TEST(ConnectionPool, SettledTicketsAreSkipped) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(1, 1, 4), ops);
    SOCKET s = INVALID_SOCKET;
    int woken1 = 0;
    int woken2 = 0;
    pool.acquire(&s, nullptr);
    const auto t1 = ticket(&woken1);
    const auto t2 = ticket(&woken2);
    pool.acquire(&s, t1);
    pool.acquire(&s, t2);
    t1->settled = true;  // settled elsewhere (timer) without leaving the queue yet

    pool.release(101, true, t0);
    EXPECT_EQ(woken1, 0);
    EXPECT_EQ(woken2, 1);
    EXPECT_EQ(t2->socket, 101u);
}

TEST(ConnectionPool, IdleListIsCapped) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(4, 1, 0), ops);
    SOCKET s = INVALID_SOCKET;
    pool.acquire(&s, nullptr);
    pool.acquire(&s, nullptr);
    pool.release(101, true, t0);
    pool.release(102, true, t0);
    EXPECT_EQ(pool.stats().idle, 1u);
    EXPECT_TRUE(ops.was_closed(102));
    EXPECT_EQ(pool.stats().open, 1u);
}

TEST(ConnectionPool, MaxIdleZeroDisablesReuse) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(4, 0, 0), ops);
    SOCKET s = INVALID_SOCKET;
    pool.acquire(&s, nullptr);
    pool.release(101, true, t0);
    EXPECT_TRUE(ops.was_closed(101));
    EXPECT_EQ(pool.stats().open, 0u);
}

TEST(ConnectionPool, CloseIdleClosesEveryIdleConnectionButNotThoseInUse) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(3, 3, 0), ops);
    SOCKET s = INVALID_SOCKET;
    pool.acquire(&s, nullptr);
    pool.acquire(&s, nullptr);
    pool.acquire(&s, nullptr);  // stays in use
    pool.release(101, true, t0);
    pool.release(102, true, t0);
    EXPECT_EQ(pool.close_idle(), 2u);
    EXPECT_TRUE(ops.was_closed(101));
    EXPECT_TRUE(ops.was_closed(102));
    EXPECT_EQ(pool.stats().idle, 0u);
    EXPECT_EQ(pool.stats().open, 1u);
}

TEST(ConnectionPool, CloseExpiredClosesOnlyConnectionsPastTheIdleTimeout) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(4, 4, 0, std::chrono::milliseconds(1000)), ops);
    SOCKET s = INVALID_SOCKET;
    pool.acquire(&s, nullptr);
    pool.acquire(&s, nullptr);
    pool.release(101, true, t0);
    pool.release(102, true, t0 + std::chrono::milliseconds(2000));
    EXPECT_EQ(pool.close_expired(t0 + std::chrono::milliseconds(1500)), 1u);
    EXPECT_TRUE(ops.was_closed(101));
    EXPECT_FALSE(ops.was_closed(102));
    EXPECT_EQ(pool.stats().idle, 1u);
    EXPECT_EQ(pool.stats().open, 1u);
}

TEST(ConnectionPool, PreferNewOpensAFreshConnectionWhileUnderTheCap) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(2, 2, 0), ops);
    SOCKET s = INVALID_SOCKET;
    pool.acquire(&s, nullptr);
    pool.release(101, true, t0);
    EXPECT_EQ(pool.acquire(&s, nullptr, /*prefer_new=*/true), Acquire::Connect);
    EXPECT_EQ(pool.stats().idle, 1u);
    EXPECT_EQ(pool.acquire(&s, nullptr, /*prefer_new=*/true), Acquire::Reused);  // at the cap: falls back
}

// ---- Registry (plan IV.4) --------------------------------------------------------------

namespace {

lb::ConfigSnapshot two_group_config() {
    lb::ConfigSnapshot c;
    c.pool.max_connections_per_backend = 4;
    c.pool.max_idle_per_backend = 4;
    c.pool.max_waiters_per_backend = 4;
    c.pool.idle_timeout_ms = 1000;
    c.groups.push_back({"web", {{"w1", "127.0.0.1", 9001, 3}, {"w2", "127.0.0.1", 9002, 1}}});
    c.groups.push_back({"api", {{"a1", "10.0.0.1", 7000, 1}}});
    return c;
}

}  // namespace

TEST(BackendRegistry, LoadsBackendsWithGroupsAndHealthyState) {
    FakeSocketOps ops;
    lb::backend::BackendRegistry registry(ops);
    registry.load(two_group_config());
    ASSERT_EQ(registry.all().size(), 3u);
    const auto web = registry.group("web");
    ASSERT_EQ(web.size(), 2u);
    EXPECT_EQ(web[0]->id, "w1");
    EXPECT_EQ(web[0]->weight, 3u);
    EXPECT_EQ(web[0]->endpoint, "127.0.0.1:9001");
    EXPECT_TRUE(web[0]->eligible());
    EXPECT_EQ(registry.find("a1")->group, "api");
    EXPECT_EQ(registry.find("nope"), nullptr);
    EXPECT_TRUE(registry.group("nope").empty());
}

TEST(BackendRegistry, UnhealthyOrDrainingBackendIsIneligibleAndLosesItsIdleConnections) {
    FakeSocketOps ops;
    lb::backend::BackendRegistry registry(ops);
    registry.load(two_group_config());
    const auto w1 = registry.find("w1");
    SOCKET s = INVALID_SOCKET;
    w1->pool.acquire(&s, nullptr);
    w1->pool.release(101, true, t0);
    ASSERT_EQ(w1->pool.stats().idle, 1u);

    ASSERT_TRUE(registry.set_state("w1", lb::BackendState::Unhealthy));
    EXPECT_FALSE(w1->eligible());
    EXPECT_TRUE(ops.was_closed(101));
    EXPECT_EQ(w1->pool.stats().idle, 0u);

    ASSERT_TRUE(registry.set_state("w1", lb::BackendState::Healthy));
    EXPECT_TRUE(w1->eligible());
    ASSERT_TRUE(registry.set_state("w2", lb::BackendState::Draining));
    EXPECT_FALSE(registry.find("w2")->eligible());
    EXPECT_FALSE(registry.set_state("nope", lb::BackendState::Healthy));
}

TEST(BackendRegistry, StatsAreACopiedSnapshot) {
    FakeSocketOps ops;
    lb::backend::BackendRegistry registry(ops);
    registry.load(two_group_config());
    const auto w1 = registry.find("w1");
    w1->in_flight = 2;
    w1->requests = 10;
    w1->failures = 1;
    const auto s = w1->stats();
    EXPECT_EQ(s.id, "w1");
    EXPECT_EQ(s.group, "web");
    EXPECT_EQ(s.in_flight, 2u);
    EXPECT_EQ(s.requests, 10u);
    EXPECT_EQ(s.failures, 1u);
    EXPECT_EQ(s.state, lb::BackendState::Healthy);
}

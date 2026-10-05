// Step 2.1 (plan IV.14): the pieces of a hot reload that need no sockets: which fields
// are restart-only, the content hash, reconciling the registry by backend id, new pool
// limits, and metrics series for backends added later.

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "backend/registry.h"
#include "config/reload_rules.h"
#include "metrics/metrics.h"

namespace {

using lb::backend::BackendRegistry;
using lb::backend::ConnectionPool;
using lb::backend::PoolLimits;
using lb::backend::PoolTicket;

const lb::TimePoint t0 = lb::Clock::now();

class FakeSocketOps final : public lb::backend::SocketOps {
public:
    bool is_stale(SOCKET) noexcept override { return false; }
    void close(SOCKET s) noexcept override { closed.push_back(s); }
    bool was_closed(SOCKET s) const { return std::find(closed.begin(), closed.end(), s) != closed.end(); }
    std::vector<SOCKET> closed;
};

PoolLimits limits(std::uint32_t max_connections, std::uint32_t max_idle, std::uint32_t max_waiters) {
    PoolLimits l;
    l.max_connections = max_connections;
    l.max_idle = max_idle;
    l.max_waiters = max_waiters;
    l.idle_timeout = std::chrono::seconds(30);
    return l;
}

std::shared_ptr<PoolTicket> ticket(int* woken) {
    auto t = std::make_shared<PoolTicket>();
    t->wake = [woken] { ++*woken; };
    return t;
}

lb::ConfigSnapshot base_config() {
    lb::ConfigSnapshot c;
    c.listen.address = "127.0.0.1";
    c.listen.port = 8080;
    c.pool.max_connections_per_backend = 4;
    c.pool.max_idle_per_backend = 4;
    c.pool.max_waiters_per_backend = 4;
    c.pool.idle_timeout_ms = 1000;
    c.metrics.slice_ms = 1000;
    c.metrics.window_slices = 10;
    c.metrics.max_backend_series = 8;
    c.groups.push_back({"web", {{"w1", "127.0.0.1", 9001, 1}, {"w2", "127.0.0.1", 9002, 1}}});
    return c;
}

std::shared_ptr<const lb::ConfigSnapshot> snapshot(const std::function<void(lb::ConfigSnapshot&)>& edit = {}) {
    auto c = base_config();
    if (edit) edit(c);
    return std::make_shared<const lb::ConfigSnapshot>(std::move(c));
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

// --- restart-only fields ------------------------------------------------------------

TEST(ReloadRules, IdenticalConfigsHaveNoRestartOnlyChanges) {
    EXPECT_TRUE(lb::restart_only_changes(base_config(), base_config()).empty());
}

TEST(ReloadRules, EachRestartOnlyFieldIsNamed) {
    struct Case {
        const char* field;
        std::function<void(lb::ConfigSnapshot&)> edit;
    };
    const std::vector<Case> cases = {
        {"listen.address", [](auto& c) { c.listen.address = "0.0.0.0"; }},
        {"listen.port", [](auto& c) { c.listen.port = 8081; }},
        {"listen.backlog", [](auto& c) { c.listen.backlog = 7; }},
        {"listen.pending_accepts", [](auto& c) { c.listen.pending_accepts = 3; }},
        {"workers.threads", [](auto& c) { c.workers.threads = 3u; }},
        {"maintenance.interval_ms", [](auto& c) { c.maintenance.interval_ms = 77; }},
        {"metrics.slice_ms", [](auto& c) { c.metrics.slice_ms = 500; }},
        {"metrics.window_slices", [](auto& c) { c.metrics.window_slices = 5; }},
        {"metrics.max_backend_series", [](auto& c) { c.metrics.max_backend_series = 9; }},
        {"event_log.path", [](auto& c) { c.event_log.path = "other.jsonl"; }},
        {"event_log.max_file_bytes", [](auto& c) { c.event_log.max_file_bytes = 5; }},
        {"event_log.max_files", [](auto& c) { c.event_log.max_files = 5; }},
        {"event_log.max_queue", [](auto& c) { c.event_log.max_queue = 5; }},
        {"event_log.recent_events", [](auto& c) { c.event_log.recent_events = 5; }},
        {"event_log.trace_requests", [](auto& c) { c.event_log.trace_requests = !c.event_log.trace_requests; }},
        {"dashboard.publish_interval_ms", [](auto& c) { c.dashboard.publish_interval_ms = 5; }},
        {"dashboard.event_rows", [](auto& c) { c.dashboard.event_rows = 5; }},
        {"dashboard.graph_points", [](auto& c) { c.dashboard.graph_points = 5; }},
        {"config_reload.watch_file", [](auto& c) { c.config_reload.watch_file = !c.config_reload.watch_file; }},
        {"config_reload.debounce_ms", [](auto& c) { c.config_reload.debounce_ms = 5; }},
        {"sticky_table.shards", [](auto& c) { c.sticky_table.shards = 5; }},
        {"sticky_table.max_entries", [](auto& c) { c.sticky_table.max_entries = 5; }},
    };
    for (const auto& tc : cases) {
        auto next = base_config();
        tc.edit(next);
        const auto changed = lb::restart_only_changes(base_config(), next);
        ASSERT_EQ(changed.size(), 1u) << tc.field;
        EXPECT_EQ(changed[0], tc.field);
    }
}

TEST(ReloadRules, LiveFieldsAreNotRestartOnly) {
    auto next = base_config();
    next.groups[0].backends.push_back({"w3", "127.0.0.1", 9003, 5});  // backends (plan IV.14)
    next.groups[0].backends[0].weight = 9;                             // weights
    next.groups[0].strategy = lb::Strategy::LeastConnections;
    next.groups[0].health.interval_ms = 123;  // health-check settings
    next.pool.max_connections_per_backend = 99;  // pool settings
    next.timeouts.backend_response_ms = 42;
    next.limits.max_client_connections = 3;
    next.buffers.client_read_bytes = 1024;
    next.routing.default_group = "web";
    next.trusted_proxies.push_back({0x0A000000u, 0xFF000000u});
    next.groups[0].sticky.mode = lb::StickyConfig::Mode::InsertedCookie;  // sticky settings per group
    next.groups[0].sticky.cookie = "lb";
    next.groups[0].sticky.ttl_ms = 5000;
    EXPECT_TRUE(lb::restart_only_changes(base_config(), next).empty());
}

TEST(ReloadRules, ContentHashIsFnv1a64) {
    EXPECT_EQ(lb::config_content_hash(""), 0xCBF29CE484222325ull);
    EXPECT_EQ(lb::config_content_hash("a"), 0xAF63DC4C8601EC8Cull);
    EXPECT_EQ(lb::config_content_hash("{\"a\":1}"), lb::config_content_hash("{\"a\":1}"));
    EXPECT_NE(lb::config_content_hash("{\"a\":1}"), lb::config_content_hash("{\"a\":2}"));
}

// --- registry reconcile ---------------------------------------------------------------

TEST(RegistryReconcile, UnchangedBackendKeepsItsRuntimeStateAndCounters) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    registry.load(snapshot());
    const auto w1 = registry.find("w1");
    w1->requests = 41;
    w1->state = lb::BackendState::Unhealthy;
    registry.find("w2")->state = lb::BackendState::Draining;

    const auto r = registry.reconcile(snapshot([](auto& c) { c.groups[0].backends[0].weight = 7; }));
    EXPECT_EQ(registry.find("w1"), w1);  // the same object: nothing reset
    EXPECT_EQ(w1->requests.load(), 41u);
    EXPECT_EQ(w1->weight.load(), 7u);
    EXPECT_EQ(w1->state.load(), lb::BackendState::Unhealthy);
    // Plan IV.12: a reload never silently un-drains.
    EXPECT_EQ(registry.find("w2")->state.load(), lb::BackendState::Draining);
    EXPECT_TRUE(r.added.empty());
    EXPECT_TRUE(r.removed.empty());
    EXPECT_EQ(r.reweighted, std::vector<std::string>{"w1"});
}

TEST(RegistryReconcile, AddedAndRemovedBackendsAreReported) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    registry.load(snapshot());
    const auto r = registry.reconcile(snapshot([](auto& c) {
        c.groups[0].backends.erase(c.groups[0].backends.begin());  // w1 leaves
        c.groups[0].backends.push_back({"w3", "127.0.0.1", 9003, 1});
    }));
    EXPECT_EQ(r.added, std::vector<std::string>{"w3"});
    EXPECT_EQ(r.removed, std::vector<std::string>{"w1"});
    EXPECT_EQ(registry.find("w1"), nullptr);
    ASSERT_NE(registry.find("w3"), nullptr);
    EXPECT_TRUE(registry.find("w3")->eligible());
    ASSERT_EQ(registry.group("web").size(), 2u);
    EXPECT_EQ(registry.group("web")[0]->id, "w2");
    EXPECT_EQ(registry.group("web")[1]->id, "w3");
}

TEST(RegistryReconcile, ChangedAddressGetsAFreshRuntimeThatStaysDrained) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    registry.load(snapshot());
    const auto old_w1 = registry.find("w1");
    old_w1->requests = 5;
    old_w1->state = lb::BackendState::Draining;
    const auto r = registry.reconcile(snapshot([](auto& c) { c.groups[0].backends[0].port = 9101; }));
    const auto w1 = registry.find("w1");
    ASSERT_NE(w1, old_w1);
    EXPECT_EQ(w1->endpoint, "127.0.0.1:9101");
    EXPECT_EQ(w1->requests.load(), 0u);
    EXPECT_EQ(w1->state.load(), lb::BackendState::Draining);
    EXPECT_TRUE(contains(r.added, "w1"));
    EXPECT_TRUE(contains(r.removed, "w1"));
}

TEST(RegistryReconcile, BackendMovedToAnotherGroupGetsAFreshRuntime) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    registry.load(snapshot());
    const auto old_w2 = registry.find("w2");
    registry.reconcile(snapshot([](auto& c) {
        const auto w2 = c.groups[0].backends[1];
        c.groups[0].backends.pop_back();
        c.groups.push_back({"api", {w2}});
    }));
    ASSERT_NE(registry.find("w2"), old_w2);
    EXPECT_EQ(registry.find("w2")->group, "api");
    EXPECT_EQ(registry.group("api").size(), 1u);
    EXPECT_EQ(registry.group("web").size(), 1u);
}

TEST(RegistryReconcile, CapturedTopologyIsUnchangedByALaterReload) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    const auto first = snapshot();
    registry.load(first);
    const auto captured = registry.topology();  // what an in-flight request holds
    registry.reconcile(snapshot([](auto& c) { c.groups[0].backends.pop_back(); }));
    EXPECT_EQ(captured->config, first);
    ASSERT_EQ(captured->backends.size(), 2u);
    ASSERT_NE(captured->find_group("web"), nullptr);
    EXPECT_EQ(captured->find_group("web")->members.size(), 2u);
    EXPECT_EQ(registry.topology()->backends.size(), 1u);
    EXPECT_NE(registry.topology()->config, first);
}

TEST(RegistryReconcile, RemovedBackendClosesIdleConnectionsAndLaterReleases) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    registry.load(snapshot());
    const auto w2 = registry.find("w2");  // a request still holds it
    SOCKET a = INVALID_SOCKET;
    SOCKET b = INVALID_SOCKET;
    ASSERT_EQ(w2->pool.acquire(&a, nullptr), ConnectionPool::Acquire::Connect);
    ASSERT_EQ(w2->pool.acquire(&b, nullptr), ConnectionPool::Acquire::Connect);
    w2->pool.release(201, true, t0);  // idle
    registry.reconcile(snapshot([](auto& c) { c.groups[0].backends.pop_back(); }));
    EXPECT_TRUE(ops.was_closed(201));
    w2->pool.release(202, true, t0);  // the in-flight request finishes: closed, not pooled
    EXPECT_TRUE(ops.was_closed(202));
    EXPECT_EQ(w2->pool.stats().open, 0u);
}

TEST(RegistryReconcile, PoolLimitsOfKeptBackendsAreUpdated) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    registry.load(snapshot());
    const auto w1 = registry.find("w1");
    registry.reconcile(snapshot([](auto& c) { c.pool.max_connections_per_backend = 1; }));
    SOCKET s = INVALID_SOCKET;
    ASSERT_EQ(w1->pool.acquire(&s, nullptr), ConnectionPool::Acquire::Connect);
    EXPECT_EQ(w1->pool.acquire(&s, nullptr), ConnectionPool::Acquire::Rejected);  // no ticket, cap 1
}

TEST(RegistryReconcile, MetricsSeriesComeFromTheCallback) {
    FakeSocketOps ops;
    BackendRegistry registry(ops);
    std::vector<std::string> asked;
    const auto series_for = [&](const std::string& id) {
        asked.push_back(id);
        return asked.size() + 10;
    };
    registry.load(snapshot(), series_for);
    EXPECT_EQ(registry.find("w1")->metrics_series, 11u);
    EXPECT_EQ(registry.find("w2")->metrics_series, 12u);
    registry.reconcile(snapshot([](auto& c) { c.groups[0].backends.push_back({"w3", "127.0.0.1", 9003, 1}); }),
                       series_for);
    EXPECT_EQ(registry.find("w3")->metrics_series, 13u);
    EXPECT_EQ(registry.find("w1")->metrics_series, 11u);  // kept backends keep their series
    EXPECT_EQ(asked.size(), 3u);
}

// --- pool limits ----------------------------------------------------------------------

TEST(PoolSetLimits, LowerMaxIdleClosesTheOldestIdleConnections) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(4, 4, 4), ops);
    SOCKET s = INVALID_SOCKET;
    for (int i = 0; i < 3; ++i) pool.acquire(&s, nullptr);
    pool.release(1, true, t0);
    pool.release(2, true, t0);
    pool.release(3, true, t0);
    EXPECT_EQ(pool.set_limits(limits(4, 1, 4)), 2u);
    EXPECT_TRUE(ops.was_closed(1));
    EXPECT_TRUE(ops.was_closed(2));
    EXPECT_FALSE(ops.was_closed(3));  // most recently used survives
    EXPECT_EQ(pool.stats().idle, 1u);
    EXPECT_EQ(pool.stats().open, 1u);
}

TEST(PoolSetLimits, HigherCapAdmitsQueuedRequests) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(1, 1, 4), ops);
    SOCKET s = INVALID_SOCKET;
    int woken = 0;
    ASSERT_EQ(pool.acquire(&s, nullptr), ConnectionPool::Acquire::Connect);
    const auto waiting = ticket(&woken);
    ASSERT_EQ(pool.acquire(&s, waiting), ConnectionPool::Acquire::Queued);
    pool.set_limits(limits(2, 1, 4));
    EXPECT_EQ(woken, 1);
    EXPECT_TRUE(waiting->may_connect);
    EXPECT_EQ(pool.stats().open, 2u);
}

TEST(PoolSetLimits, LowerCapShrinksAsConnectionsAreReleased) {
    FakeSocketOps ops;
    ConnectionPool pool(limits(3, 3, 4), ops);
    SOCKET s = INVALID_SOCKET;
    for (int i = 0; i < 3; ++i) pool.acquire(&s, nullptr);
    pool.set_limits(limits(1, 3, 4));
    EXPECT_EQ(pool.stats().open, 3u);  // busy connections are never cut
    pool.release(11, true, t0);
    pool.release(12, true, t0);
    EXPECT_TRUE(ops.was_closed(11));
    EXPECT_TRUE(ops.was_closed(12));
    pool.release(13, true, t0);  // at the cap again: pooled
    EXPECT_FALSE(ops.was_closed(13));
    EXPECT_EQ(pool.stats().idle, 1u);
}

// --- metrics series -------------------------------------------------------------------

TEST(MetricsSeries, BackendsAddedLaterGetTheirOwnSeriesUpToTheCapacity) {
    lb::MetricsConfig mc;
    mc.slice_ms = 1000;
    mc.window_slices = 10;
    mc.max_backend_series = 3;
    lb::metrics::Metrics m({"a", "b"}, mc, t0);
    EXPECT_EQ(m.register_series("a"), 1u);  // existing ids keep their number
    EXPECT_EQ(m.register_series("b"), 2u);
    EXPECT_TRUE(m.has_room_for({"a", "b", "c"}));
    EXPECT_FALSE(m.has_room_for({"c", "d"}));
    EXPECT_EQ(m.register_series("c"), 3u);
    EXPECT_EQ(m.register_series("d"), 0u);  // full
    EXPECT_TRUE(m.has_room_for({"a", "c"}));

    m.record(3, t0 + std::chrono::milliseconds(10), std::chrono::milliseconds(2), std::chrono::milliseconds(1),
             lb::k2xx);
    const auto snap = m.snapshot(t0 + std::chrono::milliseconds(20));
    ASSERT_EQ(snap.backends.size(), 3u);
    EXPECT_EQ(snap.backends[2].id, "c");
    EXPECT_EQ(snap.backends[2].total_since_start.count, 1u);
    EXPECT_EQ(snap.backends[0].total_since_start.count, 0u);
    EXPECT_EQ(snap.system.total_since_start.count, 1u);
}

// Step 2.6 (plan IV.12): graceful drain. A draining backend gets no new requests, its
// in-flight requests finish, and it is taken out when its in-flight count reaches zero or
// the drain timeout expires (then the rest are aborted with 502).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::ClientResponse;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using namespace std::chrono_literals;

namespace {

const lb::BackendStats& stats_of(const std::vector<lb::BackendStats>& all, const std::string& id) {
    for (const auto& b : all) {
        if (b.id == id) return b;
    }
    static lb::BackendStats none;
    ADD_FAILURE() << "no backend " << id;
    return none;
}

BackendState state_of(lb::Engine& engine, const std::string& id) { return stats_of(engine.backend_stats(), id).state; }

const lb::LoggedEvent* find_event(const std::vector<lb::LoggedEvent>& events, const std::string& type,
                                  const std::string& backend = {}) {
    for (const auto& e : events) {
        if (e.type == type && (backend.empty() || e.backend == backend)) return &e;
    }
    return nullptr;
}

bool has_event(lb::Engine& engine, const std::string& type, const std::string& backend = {}) {
    const auto events = engine.recent_events();
    return find_event(events, type, backend) != nullptr;
}

void drain_directive(nlohmann::json& j, int backend, const char* directive) {
    j["groups"][0]["backends"][backend]["drain"] = directive;
}

// Keep-alive clients sending requests back to back; anything but a complete 200 is a failure.
class Load {
public:
    Load(std::uint16_t port, int clients) {
        for (int i = 0; i < clients; ++i) {
            threads_.emplace_back([this, port] {
                auto c = std::make_unique<TestClient>(port);
                while (!stop_) {
                    ClientResponse r;
                    if (c->connected() && c->send(get_request("/load"))) r = c->read_response();
                    if (r.status == 200 && r.end == ClientResponse::End::Complete) {
                        ++ok_;
                    } else {
                        ++failed_;
                        c = std::make_unique<TestClient>(port);
                    }
                }
            });
        }
    }
    ~Load() { stop(); }
    void stop() {
        stop_ = true;
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }
    int ok() const { return ok_; }
    int failed() const { return failed_; }

private:
    std::atomic<bool> stop_{false};
    std::atomic<int> ok_{0};
    std::atomic<int> failed_{0};
    std::vector<std::thread> threads_;
};

}  // namespace

// Plan IV.12 done: draining a backend under active load results in zero new requests to it,
// zero interrupted in-flight requests, and removal once its in-flight count reaches zero.
TEST_F(ProxyTest, DrainUnderLoadSendsNothingNewAndInterruptsNothing) {
    mock::MockFaults slow;
    slow.latency_ms = 100;  // so requests are in flight when the drain starts
    const auto p1 = start_backend(slow, "b1");
    const auto p2 = start_backend(slow, "b2");
    const auto p3 = start_backend(slow, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) { j["workers"]["threads"] = 4; });
    Load load(proxy_port(), 12);
    ASSERT_TRUE(eventually([&] { return stats_of(engine().backend_stats(), "b1").in_flight > 0 && load.ok() > 50; }));

    ASSERT_TRUE(engine().drain_backend("b1"));
    const auto selected_at_drain = stats_of(engine().backend_stats(), "b1").requests;
    ASSERT_TRUE(eventually([&] { return state_of(engine(), "b1") == BackendState::Drained; }));
    // Everything b1 received was selected before the drain began: it got no new request.
    EXPECT_EQ(backend(0).stats().requests, selected_at_drain);
    const int ok_at_drained = load.ok();
    ASSERT_TRUE(eventually([&] { return load.ok() > ok_at_drained + 100; }));  // load carries on
    EXPECT_EQ(backend(0).stats().requests, selected_at_drain);
    load.stop();

    EXPECT_EQ(load.failed(), 0);  // no in-flight request was interrupted
    EXPECT_EQ(engine().stats().error_responses, 0u);
    const auto events = engine().recent_events();
    const auto* started = find_event(events, "drain_started", "b1");
    const auto* completed = find_event(events, "drain_completed", "b1");
    ASSERT_NE(started, nullptr);
    ASSERT_NE(completed, nullptr);
    EXPECT_EQ(started->json.find("\"in_flight\":0"), std::string::npos) << started->message;  // it had work in flight
    EXPECT_NE(completed->message.find("no requests in flight, out of service"), std::string::npos);
    const auto s = engine().stats();
    EXPECT_EQ(s.drains_started, 1u);
    EXPECT_EQ(s.drains_completed, 1u);
    EXPECT_EQ(s.drains_timed_out, 0u);
    EXPECT_EQ(stats_of(engine().backend_stats(), "b1").open_connections, 0u);  // pooled connections closed
}

// Plan IV.12 / VI: when the drain timeout expires, the remaining requests are aborted (502),
// the backend is taken out, and it is logged.
TEST_F(ProxyTest, DrainTimeoutAbortsTheRemainingRequestsWith502) {
    mock::MockFaults stuck;
    stuck.latency_ms = 5000;
    const auto p1 = start_backend(stuck, "b1");
    start_proxy({p1}, [](nlohmann::json& j) {
        j["timeouts"]["drain_ms"] = 200;
        j["groups"][0]["passive_health"]["enabled"] = true;
        j["groups"][0]["passive_health"]["consecutive_failures"] = 1;
    });
    ClientResponse r;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread t([&] { r = fetch(proxy_port(), "/stuck"); });
    ASSERT_TRUE(eventually([&] { return stats_of(engine().backend_stats(), "b1").in_flight == 1; }));
    ASSERT_TRUE(engine().drain_backend("b1"));
    t.join();
    EXPECT_EQ(r.status, 502);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);  // not the backend's 5 s
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Drained);
    ASSERT_TRUE(eventually([&] { return has_event(engine(), "drain_timed_out", "b1"); }));
    const auto events = engine().recent_events();
    const auto* timed_out = find_event(events, "drain_timed_out", "b1");
    EXPECT_EQ(timed_out->message, "b1 drain timed out after 200 ms: 1 requests aborted, out of service");
    const auto* aborted = find_event(events, "drain_aborted", "b1");
    ASSERT_NE(aborted, nullptr);
    EXPECT_EQ(aborted->request_id, r.header("x-request-id"));
    const auto s = engine().stats();
    EXPECT_EQ(s.drains_timed_out, 1u);
    EXPECT_EQ(s.drain_aborted_requests, 1u);
    EXPECT_EQ(s.backends_marked_down, 0u);  // an abort by the drain is not the backend's failure
}

// A request still waiting in the pool's queue for a connection to the draining backend is
// aborted by the timeout too.
TEST_F(ProxyTest, DrainTimeoutAlsoAbortsRequestsWaitingForAPooledConnection) {
    mock::MockFaults stuck;
    stuck.latency_ms = 5000;
    const auto p1 = start_backend(stuck, "b1");
    start_proxy({p1}, [](nlohmann::json& j) {
        j["timeouts"]["drain_ms"] = 200;
        j["pool"]["max_connections_per_backend"] = 1;
        j["pool"]["max_idle_per_backend"] = 1;
        j["pool"]["wait_timeout_ms"] = 10000;
    });
    ClientResponse a;
    ClientResponse b;
    std::thread ta([&] { a = fetch(proxy_port(), "/a"); });
    ASSERT_TRUE(eventually([&] { return stats_of(engine().backend_stats(), "b1").in_flight == 1; }));
    std::thread tb([&] { b = fetch(proxy_port(), "/b"); });
    ASSERT_TRUE(eventually([&] { return stats_of(engine().backend_stats(), "b1").waiting_requests == 1; }));
    ASSERT_TRUE(engine().drain_backend("b1"));
    ta.join();
    tb.join();
    EXPECT_EQ(a.status, 502);
    EXPECT_EQ(b.status, 502);
    EXPECT_EQ(engine().stats().drain_aborted_requests, 2u);
    EXPECT_TRUE(eventually([&] {
        const auto st = stats_of(engine().backend_stats(), "b1");
        return st.in_flight == 0 && st.waiting_requests == 0 && st.open_connections == 0;
    }));
}

TEST_F(ProxyTest, IdleBackendDrainsAtOnceAndReturnsOnlyWhenUndrained) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) {
        auto& h = j["groups"][0]["health"];
        h["interval_ms"] = 50;
        h["timeout_ms"] = 40;
    });
    ASSERT_TRUE(engine().drain_backend("b1"));
    EXPECT_FALSE(engine().drain_backend("b1"));  // already draining
    ASSERT_TRUE(eventually([&] { return state_of(engine(), "b1") == BackendState::Drained; }));
    EXPECT_FALSE(engine().drain_backend("b1"));  // already drained
    EXPECT_FALSE(engine().drain_backend("nope"));

    // Drained: no requests, no probes (it is out of service on purpose).
    const auto probes = stats_of(engine().backend_stats(), "b1").health_probes;
    for (int i = 0; i < 6; ++i) EXPECT_EQ(fetch(proxy_port(), "/").header("x-backend-id"), "b2");
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(stats_of(engine().backend_stats(), "b1").health_probes, probes);
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Drained);

    ASSERT_TRUE(engine().undrain_backend("b1"));
    EXPECT_FALSE(engine().undrain_backend("b1"));
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Healthy);
    EXPECT_TRUE(has_event(engine(), "drain_cancelled", "b1"));
    int to_b1 = 0;
    for (int i = 0; i < 6; ++i) to_b1 += fetch(proxy_port(), "/").header("x-backend-id") == "b1" ? 1 : 0;
    EXPECT_EQ(to_b1, 3);
}

// Plan IV.12 / VI precedence: a reload never un-drains a backend unless its config explicitly
// says so.
TEST_F(ProxyTest, ReloadDrainPrecedence) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto config = [&](const char* b1_drain, int b2_weight = 1) {
        return lbtest::make_proxy_config({p1, p2}, [=](nlohmann::json& j) {
            drain_directive(j, 0, b1_drain);
            j["groups"][0]["backends"][1]["weight"] = b2_weight;
        });
    };
    start_proxy({p1, p2});
    ASSERT_TRUE(engine().drain_backend("b1"));  // operator drain
    ASSERT_TRUE(eventually([&] { return state_of(engine(), "b1") == BackendState::Drained; }));

    // "keep": a reload that changes something else leaves the drain alone.
    ASSERT_TRUE(engine().reload(config("keep", 5)).accepted);
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Drained);

    // "cancel": explicitly back in service.
    ASSERT_TRUE(engine().reload(config("cancel")).accepted);
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Healthy);
    const auto events = engine().recent_events();
    const auto* cancelled = find_event(events, "drain_cancelled", "b1");
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->message, "b1 back in service (config), was drained");

    // "start": the config drains it.
    ASSERT_TRUE(engine().reload(config("start")).accepted);
    EXPECT_NE(state_of(engine(), "b1"), BackendState::Healthy);
    ASSERT_TRUE(eventually([&] { return state_of(engine(), "b1") == BackendState::Drained; }));
    // Re-asserting "start" on every later reload changes nothing.
    ASSERT_TRUE(engine().reload(config("start", 2)).accepted);
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Drained);
    EXPECT_EQ(engine().stats().drains_started, 2u);
}

// A backend added by a reload with "drain": "start" never takes a request.
TEST_F(ProxyTest, BackendAddedAsDrainingNeverGetsARequest) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1});
    Load load(proxy_port(), 4);
    ASSERT_TRUE(eventually([&] { return load.ok() > 50; }));
    ASSERT_TRUE(engine()
                    .reload(lbtest::make_proxy_config({p1, p2}, [](nlohmann::json& j) { drain_directive(j, 1, "start"); }))
                    .accepted);
    ASSERT_TRUE(eventually([&] { return state_of(engine(), "b2") == BackendState::Drained; }));
    const int ok_then = load.ok();
    ASSERT_TRUE(eventually([&] { return load.ok() > ok_then + 100; }));
    load.stop();
    EXPECT_EQ(backend(1).stats().requests, 0u);
    EXPECT_EQ(load.failed(), 0);
}

// The config can also start a drain at startup, and a backend removed from the config while
// draining ends its drain.
TEST_F(ProxyTest, DrainFromStartupConfigAndRemovalWhileDraining) {
    mock::MockFaults slow;
    slow.latency_ms = 500;
    const auto p1 = start_backend(slow, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) { drain_directive(j, 1, "start"); });
    EXPECT_TRUE(eventually([&] { return state_of(engine(), "b2") == BackendState::Drained; }));
    EXPECT_EQ(fetch(proxy_port(), "/").header("x-backend-id"), "b1");

    // b1 draining with a request in flight, then removed from the config.
    ASSERT_TRUE(engine().reload(lbtest::make_proxy_config({p1, p2})).accepted);  // b2 "keep": stays drained
    EXPECT_EQ(state_of(engine(), "b2"), BackendState::Drained);
    ClientResponse r;
    std::thread t([&] { r = fetch(proxy_port(), "/slow"); });
    ASSERT_TRUE(eventually([&] { return stats_of(engine().backend_stats(), "b1").in_flight == 1; }));
    ASSERT_TRUE(engine().drain_backend("b1"));
    ASSERT_TRUE(engine()
                    .reload(lbtest::make_proxy_config({p2}, [](nlohmann::json& j) {
                        j["groups"][0]["backends"][0]["id"] = "b2";
                        drain_directive(j, 0, "cancel");
                    }))
                    .accepted);
    t.join();
    EXPECT_EQ(r.status, 200);  // the in-flight request still finished
    EXPECT_TRUE(eventually([&] { return has_event(engine(), "drain_ended", "b1"); }));
    EXPECT_EQ(fetch(proxy_port(), "/").header("x-backend-id"), "b2");
}

// Step 2.5 (plan IV.10, phase 2): passive health checks. Real request outcomes (connection
// refused, timeouts, and optionally 5xx) count toward a backend's health.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using namespace std::chrono_literals;

namespace {

void passive(nlohmann::json& j, bool enabled, int consecutive_failures, bool count_5xx) {
    j["groups"][0]["passive_health"] = {
        {"enabled", enabled}, {"consecutive_failures", consecutive_failures}, {"count_5xx", count_5xx}};
}

BackendState state_of(lb::Engine& engine, const std::string& id) {
    for (const auto& b : engine.backend_stats()) {
        if (b.id == id) return b.state;
    }
    ADD_FAILURE() << "no backend " << id;
    return BackendState::Healthy;
}

const lb::LoggedEvent* find_event(const std::vector<lb::LoggedEvent>& events, const std::string& type,
                                  const std::string& backend) {
    for (const auto& e : events) {
        if (e.type == type && e.backend == backend) return &e;
    }
    return nullptr;
}

}  // namespace

// Refused connections mark a backend down after N failed requests in a row, before any
// probe notices (active probes here run once a minute); afterwards nothing fails.
TEST_F(ProxyTest, RefusedConnectionsMarkABackendDownAfterNFailedRequests) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) { passive(j, true, 3, false); });
    backend(0).stop();  // b1 dies

    int errors = 0;
    for (int i = 0; i < 40; ++i) {
        const auto r = fetch(proxy_port(), "/");
        if (r.status != 200) {
            ++errors;
            EXPECT_EQ(r.status, 502);
        } else {
            EXPECT_EQ(r.header("x-backend-id"), "b2");
        }
    }
    // Round robin alternates, and b2's successes do not reset b1's count: exactly 3 failures.
    EXPECT_EQ(errors, 3);
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Unhealthy);
    EXPECT_EQ(engine().stats().backends_marked_down, 1u);
    const auto events = engine().recent_events();
    const auto* e = find_event(events, "backend_marked_unhealthy", "b1");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->message.rfind("b1 marked unhealthy after 3 failed requests in a row: connect: ", 0), 0u) << e->message;
    EXPECT_NE(e->json.find("\"check\":\"passive\""), std::string::npos);
    EXPECT_FALSE(e->request_id.empty());  // the request that tipped it over
}

TEST_F(ProxyTest, BackendTimeoutsCountAsFailures) {
    mock::MockFaults slow;
    slow.latency_ms = 400;
    const auto p1 = start_backend(slow, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) {
        passive(j, true, 2, false);
        j["timeouts"]["backend_response_ms"] = 100;
    });
    int timeouts = 0;
    for (int i = 0; i < 12; ++i) timeouts += fetch(proxy_port(), "/").status == 504 ? 1 : 0;
    EXPECT_EQ(timeouts, 2);
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Unhealthy);
}

TEST_F(ProxyTest, FiveHundredsCountOnlyWhenTheGroupSaysSo) {
    mock::MockFaults failing;
    failing.error_rate = 1.0;
    failing.error_status = 503;
    const auto p1 = start_backend(failing, "b1");
    start_proxy({p1}, [](nlohmann::json& j) { passive(j, true, 3, false); });
    for (int i = 0; i < 10; ++i) EXPECT_EQ(fetch(proxy_port(), "/").status, 503);  // the backend's own 503s
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Healthy);

    // Turned on by a reload, it applies from the next request.
    ASSERT_TRUE(engine().reload(lbtest::make_proxy_config({p1}, [](nlohmann::json& j) { passive(j, true, 3, true); }))
                    .accepted);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(fetch(proxy_port(), "/").status, 503);
    // The outcome is recorded when the response has been written, just after the client read it.
    ASSERT_TRUE(eventually([&] { return state_of(engine(), "b1") == BackendState::Unhealthy; }));
    ASSERT_TRUE(eventually([&] {
        const auto all = engine().recent_events();
        return find_event(all, "backend_marked_unhealthy", "b1") != nullptr;
    }));
    const auto events = engine().recent_events();
    const auto* e = find_event(events, "backend_marked_unhealthy", "b1");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->message, "b1 marked unhealthy after 3 failed requests in a row: HTTP 503");
    // Now the group has no eligible backend: the proxy answers 503 itself.
    EXPECT_EQ(engine().stats().no_backend_available, 0u);
    EXPECT_EQ(fetch(proxy_port(), "/").status, 503);
    EXPECT_EQ(engine().stats().no_backend_available, 1u);
}

// Plan IV.10 hysteresis for real traffic: failures must be consecutive; one success in
// between starts the count again, so a blip never removes a backend.
TEST_F(ProxyTest, ASuccessfulRequestResetsThePassiveCount) {
    const auto p1 = start_backend({}, "b1");
    start_proxy({p1}, [](nlohmann::json& j) { passive(j, true, 3, true); });
    mock::MockFaults failing;
    failing.error_rate = 1.0;
    for (int round = 0; round < 5; ++round) {
        backend().set_faults(failing);
        EXPECT_EQ(fetch(proxy_port(), "/").status, 500);
        EXPECT_EQ(fetch(proxy_port(), "/").status, 500);
        backend().set_faults({});
        EXPECT_EQ(fetch(proxy_port(), "/").status, 200);
    }
    EXPECT_TRUE(eventually([&] { return engine().backend_stats()[0].passive_failures_in_a_row == 0; }));
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Healthy);
}

// A backend taken out by real traffic while its health endpoint still answers is brought
// back only by M successful probes counted after the mark-down, not by earlier ones.
TEST_F(ProxyTest, PassivelyExcludedBackendNeedsMFreshProbesToReturn) {
    mock::MockFaults failing;
    failing.error_rate = 1.0;  // every request fails; /health stays 200
    const auto p1 = start_backend(failing, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) {
        passive(j, true, 2, true);
        auto& h = j["groups"][0]["health"];
        h["interval_ms"] = 100;
        h["timeout_ms"] = 80;
        h["unhealthy_threshold"] = 2;
        h["healthy_threshold"] = 3;
    });
    // Let probes build a full success streak first.
    ASSERT_TRUE(eventually([&] { return engine().backend_stats()[0].probe_successes_in_a_row == 3; }));
    while (state_of(engine(), "b1") == BackendState::Healthy) fetch(proxy_port(), "/");
    ASSERT_TRUE(eventually([&] { return state_of(engine(), "b1") == BackendState::Healthy; }));

    const auto events = engine().recent_events();
    const auto* down = find_event(events, "backend_marked_unhealthy", "b1");
    const auto* up = find_event(events, "backend_marked_healthy", "b1");
    ASSERT_NE(down, nullptr);
    ASSERT_NE(up, nullptr);
    EXPECT_NE(down->json.find("\"check\":\"passive\""), std::string::npos);
    EXPECT_NE(up->json.find("\"check\":\"active\""), std::string::npos);
    EXPECT_NE(up->json.find("\"successes\":3"), std::string::npos);
    // Three probe intervals: at least two full intervals after the mark-down (the first probe
    // may come at once). With the old streak it would have returned at the very next probe.
    EXPECT_GE(up->mono_ms - down->mono_ms, 190u) << "re-included too early";
}

TEST_F(ProxyTest, PassiveChecksOffMeansRealFailuresNeverMarkDown) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) { passive(j, false, 1, true); });
    backend(0).stop();
    int errors = 0;
    for (int i = 0; i < 20; ++i) errors += fetch(proxy_port(), "/").status == 502 ? 1 : 0;
    EXPECT_EQ(errors, 10);  // every request round robin sends to b1
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Healthy);
}

// A client that gives up is not the backend's fault.
TEST_F(ProxyTest, ClientAbortsDoNotCountAgainstTheBackend) {
    mock::MockFaults slow;
    slow.latency_ms = 300;
    const auto p1 = start_backend(slow, "b1");
    start_proxy({p1}, [](nlohmann::json& j) { passive(j, true, 1, true); });
    for (int i = 0; i < 3; ++i) {
        TestClient c(proxy_port());
        ASSERT_TRUE(c.send(get_request("/")));
        std::this_thread::sleep_for(50ms);
    }  // each client closes while its request waits on the backend
    ASSERT_TRUE(eventually([&] { return engine().backend_stats()[0].in_flight == 0; }));
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Healthy);
    EXPECT_EQ(fetch(proxy_port(), "/").status, 200);
}

// Passive checks only move a healthy backend to unhealthy: a draining backend's failing
// in-flight requests leave it draining (plan VIII: health never un-drains or re-drains).
TEST_F(ProxyTest, PassiveChecksLeaveADrainingBackendDraining) {
    mock::MockFaults slow;
    slow.latency_ms = 300;
    const auto p1 = start_backend(slow, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) {
        passive(j, true, 1, false);
        j["timeouts"]["backend_response_ms"] = 150;
    });
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/")));  // round robin: to b1, which will time out
    ASSERT_TRUE(eventually([&] { return engine().backend_stats()[0].in_flight == 1; }));
    ASSERT_TRUE(engine().set_backend_state("b1", BackendState::Draining));
    EXPECT_EQ(c.read_response().status, 504);
    EXPECT_EQ(state_of(engine(), "b1"), BackendState::Draining);
}

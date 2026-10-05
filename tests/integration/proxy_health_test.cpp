#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::ClientResponse;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kInterval = 100;
constexpr int kTimeout = 100;
constexpr int kDownAfter = 3;
constexpr int kUpAfter = 2;
// Plan IV.10: excluded within probe interval x N, plus the probe timeout.
constexpr double kDetectionWindowMs = kInterval * kDownAfter + kTimeout;
constexpr double kSchedulingSlackMs = 150;

std::function<void(nlohmann::json&)> fast_health(const char* type = "http", int interval = kInterval,
                                                 int timeout = kTimeout) {
    return [=](nlohmann::json& j) {
        auto& h = j["groups"][0]["health"];
        h["type"] = type;
        h["interval_ms"] = interval;
        h["timeout_ms"] = timeout;
        h["unhealthy_threshold"] = kDownAfter;
        h["healthy_threshold"] = kUpAfter;
    };
}

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

lb::BackendStats stats_of(lb::Engine& e, const std::string& id) {
    for (auto& s : e.backend_stats()) {
        if (s.id == id) return s;
    }
    ADD_FAILURE() << "no backend " << id;
    return {};
}

}  // namespace

// Plan IV.10 Done: a killed backend is excluded within the detection window, requests
// arriving after exclusion see zero errors, and a restarted backend is re-included
// after M successful probes.
TEST_F(ProxyTest, KilledBackendIsExcludedInTimeAndReincludedAfterRestart) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, fast_health());
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").health_probes >= 2; }));

    const auto killed_at = Clock::now();
    backend(0).stop();
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").state == BackendState::Unhealthy; }));
    const double detection_ms = ms_since(killed_at);
    const auto b1 = stats_of(engine(), "b1");
    std::printf("[ health ] killed backend excluded after %.0f ms (window %.0f ms), last probe: %s\n", detection_ms,
                kDetectionWindowMs, b1.last_probe_error.c_str());
    EXPECT_LE(detection_ms, kDetectionWindowMs + kSchedulingSlackMs);
    EXPECT_EQ(engine().stats().backends_marked_down, 1u);
    EXPECT_FALSE(b1.last_probe_error.empty());

    // After exclusion: zero errors, everything on b2.
    TestClient c(proxy_port());
    int errors = 0;
    for (int i = 0; i < 100; ++i) {
        if (!c.send(get_request("/after")) || c.read_response().status != 200) ++errors;
    }
    EXPECT_EQ(errors, 0);
    EXPECT_EQ(backend(1).stats().requests, 100u);

    // Restart on the same port: back after M successful probes.
    const auto restarted_at = Clock::now();
    start_backend({}, "b1", p1);
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").state == BackendState::Healthy; }));
    const double recovery_ms = ms_since(restarted_at);
    std::printf("[ health ] restarted backend re-included after %.0f ms (%d probes)\n", recovery_ms, kUpAfter);
    EXPECT_LE(recovery_ms, kInterval * kUpAfter + kTimeout + kSchedulingSlackMs);
    EXPECT_EQ(engine().stats().backends_marked_up, 1u);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(c.send(get_request("/back")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    EXPECT_EQ(backend(2).stats().requests, 5u);  // round robin includes it again
}

TEST_F(ProxyTest, ASingleFailedProbeDoesNotRemoveABackend) {
    start_proxy({start_backend()}, fast_health());
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").health_probes >= 1; }));
    mock::MockFaults f;
    f.health_status = 503;
    backend().set_faults(f);
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").probe_failures_in_a_row >= 1; }));
    backend().set_faults({});  // healthy again before the third failure
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").probe_successes_in_a_row >= 2; }));
    EXPECT_EQ(engine().stats().backends_marked_down, 0u);
    EXPECT_EQ(stats_of(engine(), "b1").state, BackendState::Healthy);
}

TEST_F(ProxyTest, ReachableButFailingHealthEndpointMarksTheBackendDown) {
    start_proxy({start_backend()}, fast_health());
    mock::MockFaults f;
    f.health_status = 503;
    backend().set_faults(f);
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").state == BackendState::Unhealthy; }));
    EXPECT_EQ(stats_of(engine(), "b1").last_probe_error, "HTTP 503");
    EXPECT_GE(backend().stats().health_requests, static_cast<std::uint64_t>(kDownAfter));  // GET /health
    EXPECT_EQ(fetch(proxy_port(), "/").status, 503);  // its only backend is out

    backend().set_faults({});
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").state == BackendState::Healthy; }));
    EXPECT_EQ(fetch(proxy_port(), "/").status, 200);
}

TEST_F(ProxyTest, TcpProbeOnlyChecksThatTheBackendAcceptsConnections) {
    mock::MockFaults f;
    f.health_status = 503;  // an HTTP probe would fail; a TCP probe never asks
    start_proxy({start_backend(f)}, fast_health("tcp"));
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").health_probes >= 5; }));
    EXPECT_EQ(stats_of(engine(), "b1").state, BackendState::Healthy);
    EXPECT_EQ(backend().stats().health_requests, 0u);

    backend().stop();
    EXPECT_TRUE(eventually([&] { return stats_of(engine(), "b1").state == BackendState::Unhealthy; }));
}

// Plan IV.10: each probe has its own timeout and a slow probe never blocks live traffic.
TEST_F(ProxyTest, SlowProbeTimesOutWithoutDelayingOtherProbesOrTraffic) {
    mock::MockFaults slow_health;
    slow_health.health_latency_ms = 2000;
    const auto p1 = start_backend(slow_health, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, fast_health("http", 150, 100));

    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").state == BackendState::Unhealthy; }));
    EXPECT_EQ(stats_of(engine(), "b1").last_probe_error, "timeout");

    const auto b2_before = stats_of(engine(), "b2").health_probes;
    double worst_ms = 0;
    const auto t0 = Clock::now();
    TestClient c(proxy_port());
    while (ms_since(t0) < 1200.0) {
        const auto r0 = Clock::now();
        ASSERT_TRUE(c.send(get_request("/live")));
        ASSERT_EQ(c.read_response().status, 200);
        worst_ms = std::max(worst_ms, ms_since(r0));
    }
    const auto b2_probes = stats_of(engine(), "b2").health_probes - b2_before;
    std::printf("[ health ] while b1's probes hang: b2 probed %llu times in 1.2 s, worst live request %.1f ms\n",
                static_cast<unsigned long long>(b2_probes), worst_ms);
    EXPECT_GE(b2_probes, 6u);  // every 150 ms, undisturbed
    EXPECT_LT(worst_ms, 100.0);
    EXPECT_EQ(stats_of(engine(), "b2").state, BackendState::Healthy);
}

TEST_F(ProxyTest, HealthChecksNeverOverrideDraining) {
    // A slow request keeps the backend draining (a drain ends when nothing is in flight).
    mock::MockFaults slow;
    slow.latency_ms = 3000;
    start_proxy({start_backend(slow)}, fast_health());
    ClientResponse in_flight;
    std::thread t([&] { in_flight = fetch(proxy_port(), "/slow"); });
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").in_flight == 1; }));
    ASSERT_TRUE(engine().set_backend_state("b1", BackendState::Draining));
    mock::MockFaults f = slow;
    f.health_status = 503;
    backend().set_faults(f);
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").probe_failures_in_a_row >= kDownAfter; }));
    EXPECT_EQ(stats_of(engine(), "b1").state, BackendState::Draining);
    backend().set_faults(slow);
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").probe_successes_in_a_row >= kUpAfter; }));
    EXPECT_EQ(stats_of(engine(), "b1").state, BackendState::Draining);
    EXPECT_EQ(engine().stats().backends_marked_down + engine().stats().backends_marked_up, 0u);
    t.join();
    EXPECT_EQ(in_flight.status, 200);  // the in-flight request finished normally (plan IV.12)
    EXPECT_TRUE(eventually([&] { return stats_of(engine(), "b1").state == BackendState::Drained; }));
}

// Plan IV.10 Done: requests already in flight when the backend dies fail (phase 3 retries
// idempotent ones elsewhere).
TEST_F(ProxyTest, InFlightRequestToADyingBackendFails) {
    mock::MockFaults f;
    f.latency_ms = 1000;
    start_proxy({start_backend(f)});
    ClientResponse r;
    std::thread t([&] { r = fetch(proxy_port(), "/in-flight"); });
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").in_flight == 1; }));
    backend().stop();
    t.join();
    EXPECT_EQ(r.status, 502);
}

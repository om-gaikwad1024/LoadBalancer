#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size()))) - 1;
    return v[std::min(idx, v.size() - 1)];
}

std::vector<lb::LoggedEvent> events_of_request(lb::Engine& e, const std::string& request_id) {
    std::vector<lb::LoggedEvent> out;
    for (auto& ev : e.recent_events()) {
        if (ev.request_id == request_id) out.push_back(ev);
    }
    return out;
}

std::vector<std::string> steps_of(const std::vector<lb::LoggedEvent>& events) {
    std::vector<std::string> steps;
    for (const auto& ev : events) {
        if (ev.type == "request_step") steps.push_back(nlohmann::json::parse(ev.json)["step"]);
    }
    return steps;
}

bool has_event(lb::Engine& e, const std::string& type, const std::string& request_id = {}) {
    for (auto& ev : e.recent_events()) {
        if (ev.type == type && (request_id.empty() || ev.request_id == request_id)) return true;
    }
    return false;
}

}  // namespace

// Plan IV.15 Done: one slow tail visibly moves p99 and max while barely moving the
// average, and the proxy's percentiles agree with the client's own measurements.
TEST_F(ProxyTest, SlowTailMovesP99AndMaxButBarelyTheMean) {
    const auto measure = [&](const mock::MockFaults& faults, const char* label) {
        backend().set_faults(faults);
        lb::Engine engine(lbtest::make_proxy_config({backend().port()}));
        std::string error;
        EXPECT_TRUE(engine.start(&error)) << error;
        TestClient c(engine.listen_port());
        std::vector<double> client_ms;
        for (int i = 0; i < 500; ++i) {
            const auto t0 = Clock::now();
            EXPECT_TRUE(c.send(get_request("/m")));
            EXPECT_EQ(c.read_response().status, 200);
            client_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        }
        EXPECT_TRUE(ProxyTest::eventually([&] { return engine.metrics().system.total_since_start.count == 500; }));
        const auto m = engine.metrics();
        engine.stop();
        const auto& t = m.system.total_since_start;
        std::printf("[ metrics ] %-9s proxy: mean %6.2f p50 %6.2f p95 %6.2f p99 %7.2f max %7.2f ms | "
                    "client: p50 %6.2f p99 %7.2f max %7.2f ms | backend-only p99 %7.2f ms\n",
                    label, t.mean_ms, t.p50_ms, t.p95_ms, t.p99_ms, t.max_ms, percentile(client_ms, 0.5),
                    percentile(client_ms, 0.99), percentile(client_ms, 1.0), m.system.backend_since_start.p99_ms);
        return std::make_pair(m, client_ms);
    };

    start_backend();
    const auto [base, base_client] = measure({}, "baseline");
    mock::MockFaults tail;
    tail.latency_ms = 200;
    tail.latency_rate = 0.03;  // 3% of requests are slow
    const auto [slow, slow_client] = measure(tail, "slow tail");

    const auto& b = base.system.total_since_start;
    const auto& s = slow.system.total_since_start;
    EXPECT_GT(s.p99_ms, 150.0);
    EXPECT_GT(s.max_ms, 190.0);
    EXPECT_LT(s.mean_ms, 15.0);
    EXPECT_GT(s.p99_ms - b.p99_ms, 20.0 * (s.mean_ms - b.mean_ms));  // the tail moves far more than the mean

    // Agreement with the client's measurements: stated tolerance 10% + 2 ms.
    for (const auto& [proxy_ms, client] : {std::pair{s.p50_ms, percentile(slow_client, 0.50)},
                                          std::pair{s.p99_ms, percentile(slow_client, 0.99)},
                                          std::pair{s.max_ms, percentile(slow_client, 1.0)}}) {
        EXPECT_NEAR(proxy_ms, client, 0.10 * client + 2.0);
    }
    // Backend time alone is reported separately, so proxy overhead is visible.
    EXPECT_EQ(slow.system.backend_since_start.count, 500u);
    EXPECT_LE(slow.system.backend_since_start.p50_ms, s.p50_ms);
    ASSERT_EQ(slow.backends.size(), 1u);
    EXPECT_EQ(slow.backends[0].total_since_start.count, 500u);
}

TEST_F(ProxyTest, MetricsCountStatusClassesPerBackend) {
    mock::MockFaults failing;
    failing.error_rate = 1.0;
    failing.error_status = 503;
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend(failing, "b2");
    start_proxy({p1, p2});
    TestClient c(proxy_port());
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        c.read_response();
    }
    ASSERT_TRUE(eventually([&] { return engine().metrics().system.total_since_start.count == 20; }));
    const auto m = engine().metrics();
    EXPECT_EQ(m.system.status_window[lb::k2xx], 10u);
    EXPECT_EQ(m.system.status_window[lb::k5xx], 10u);
    EXPECT_DOUBLE_EQ(m.backends[0].error_rate, 0.0);
    EXPECT_DOUBLE_EQ(m.backends[1].error_rate, 1.0);
    EXPECT_NEAR(m.system.error_rate, 0.5, 1e-9);
    EXPECT_GT(m.system.requests_per_second, 0.0);
}

// Plan IV.16 Done: after a backend-kill test, the log alone gives a clear chronological
// account of detection, exclusion, recovery and re-inclusion.
TEST_F(ProxyTest, EventLogAloneTellsTheKillAndRecoveryStory) {
    const fs::path dir = fs::temp_directory_path() / "lb-kill-story";
    fs::remove_all(dir);
    const fs::path file = dir / "events.jsonl";

    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [&](nlohmann::json& j) {
        j["event_log"]["path"] = file.string();
        auto& h = j["groups"][0]["health"];
        h["interval_ms"] = 100;
        h["timeout_ms"] = 100;
    });
    const auto healthy = [&](bool want) {
        return eventually([&] {
            for (auto& b : engine().backend_stats()) {
                if (b.id == "b1") return (b.state == BackendState::Healthy) == want;
            }
            return false;
        });
    };
    ASSERT_TRUE(eventually([&] { return engine().backend_stats()[0].health_probes >= 2; }));
    backend(0).stop();
    ASSERT_TRUE(healthy(false));
    start_backend({}, "b1", p1);
    ASSERT_TRUE(healthy(true));
    engine().stop();

    std::vector<nlohmann::json> lines;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) lines.push_back(nlohmann::json::parse(line));
    std::vector<std::string> story;
    for (const auto& l : lines) {
        story.push_back(l["event"].get<std::string>() + (l.contains("backend") ? ":" + l["backend"].get<std::string>() : ""));
        std::printf("[ log ] %s\n", l.dump().c_str());
    }
    const std::vector<std::string> expected = {"engine_started", "backend_marked_unhealthy:b1",
                                               "backend_marked_healthy:b1", "engine_stopped"};
    EXPECT_EQ(story, expected);
    for (std::size_t i = 1; i < lines.size(); ++i) {
        EXPECT_GT(lines[i]["seq"].get<std::uint64_t>(), lines[i - 1]["seq"].get<std::uint64_t>());
        EXPECT_GE(lines[i]["mono_ms"].get<std::uint64_t>(), lines[i - 1]["mono_ms"].get<std::uint64_t>());
    }
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_NE(lines[1]["reason"].get<std::string>().find("refused"), std::string::npos) << lines[1].dump();
    EXPECT_EQ(lines[1]["failures"], 3);
    EXPECT_EQ(lines[2]["successes"], 2);
}

// Plan IV.18 Done: in debug logging mode, the event log filtered by X-Request-Id shows
// every step in the fixed order, including on error paths.
TEST_F(ProxyTest, DebugTraceShowsEveryStepOfARequestInOrder) {
    const auto port = start_backend();
    start_proxy({port}, [](nlohmann::json& j) { j["event_log"]["trace_requests"] = true; });
    const auto ok = fetch(proxy_port(), "/traced");
    ASSERT_EQ(ok.status, 200);
    const std::vector<std::string> success = {"request_received",  "group_routed",      "backend_selected",
                                              "backend_connected", "request_forwarded", "response_received",
                                              "response_completed"};
    EXPECT_TRUE(eventually([&] { return steps_of(events_of_request(engine(), ok.header("x-request-id"))) == success; }));

    backend().stop();
    const auto failed = fetch(proxy_port(), "/traced-error");
    ASSERT_EQ(failed.status, 502);
    const std::vector<std::string> error_path = {"request_received", "group_routed", "backend_selected",
                                                 "error_response", "response_completed"};
    const std::string id = failed.header("x-request-id");
    EXPECT_TRUE(eventually([&] { return steps_of(events_of_request(engine(), id)) == error_path; }));
    EXPECT_TRUE(has_event(engine(), "backend_error", id));
}

TEST_F(ProxyTest, NoEligibleBackendIsLoggedLoudlyWithTheRequestId) {
    start_proxy({start_backend()});
    engine().set_backend_state("b1", BackendState::Unhealthy);
    const auto r = fetch(proxy_port(), "/");
    ASSERT_EQ(r.status, 503);
    EXPECT_TRUE(eventually([&] { return has_event(engine(), "no_backend_available", r.header("x-request-id")); }));
}

TEST_F(ProxyTest, RefusedConnectionsAndBackendTimeoutsAreLogged) {
    mock::MockFaults slow;
    slow.latency_ms = 2000;
    start_proxy({start_backend(slow)}, [](nlohmann::json& j) {
        j["limits"]["max_client_connections"] = 1;
        j["timeouts"]["backend_response_ms"] = 200;
    });
    {
        TestClient first(proxy_port());
        ASSERT_TRUE(eventually([&] { return engine().stats().connections_active == 1; }));
        TestClient second(proxy_port());
        EXPECT_EQ(second.read_response().status, 503);
        EXPECT_TRUE(eventually([&] { return has_event(engine(), "connection_rejected"); }));
        ASSERT_TRUE(first.send(get_request("/slow")));
        const auto r = first.read_response();
        EXPECT_EQ(r.status, 504);
        EXPECT_TRUE(eventually([&] { return has_event(engine(), "backend_error", r.header("x-request-id")); }));
    }
    for (auto& ev : engine().recent_events()) {
        if (ev.type == "backend_error") EXPECT_NE(ev.message.find("no response headers within 200 ms"), std::string::npos);
    }
}

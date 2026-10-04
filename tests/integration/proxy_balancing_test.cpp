#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "test_http_client.h"

using lb::BackendState;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using Clock = std::chrono::steady_clock;

namespace {

void use_strategy(nlohmann::json& j, const char* strategy) { j["groups"][0]["strategy"] = strategy; }

// `clients` keep-alive clients send requests back to back for `duration`.
int run_load(std::uint16_t port, int clients, std::chrono::milliseconds duration) {
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    const auto end = Clock::now() + duration;
    for (int t = 0; t < clients; ++t) {
        threads.emplace_back([&] {
            TestClient c(port);
            while (Clock::now() < end) {
                if (!c.send(get_request("/load"))) return;
                if (c.read_response().status == 200) ++ok;
            }
        });
    }
    for (auto& t : threads) t.join();
    return ok.load();
}

}  // namespace

TEST_F(ProxyTest, RoundRobinSpreadsRequestsEvenly) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) { use_strategy(j, "round_robin"); });
    TestClient c(proxy_port());
    for (int i = 0; i < 300; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    EXPECT_EQ(backend(0).stats().requests, 100u);
    EXPECT_EQ(backend(1).stats().requests, 100u);
    EXPECT_EQ(backend(2).stats().requests, 100u);
}

TEST_F(ProxyTest, RoundRobinSkipsAnIneligibleBackend) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) { use_strategy(j, "round_robin"); });
    ASSERT_TRUE(engine().set_backend_state("b2", BackendState::Unhealthy));
    TestClient c(proxy_port());
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    EXPECT_EQ(backend(0).stats().requests, 100u);
    EXPECT_EQ(backend(1).stats().requests, 0u);
    EXPECT_EQ(backend(2).stats().requests, 100u);
}

TEST_F(ProxyTest, LeastConnectionsSpreadsIdleTrafficAcrossAllBackends) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) { use_strategy(j, "least_connections"); });
    TestClient c(proxy_port());
    for (int i = 0; i < 300; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    // Ties (all idle) rotate, so no backend is starved and none takes everything.
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_GE(backend(i).stats().requests, 60u) << "b" << i + 1;
        EXPECT_LE(backend(i).stats().requests, 140u) << "b" << i + 1;
    }
}

// Plan IV.7 / requirement 1: the strategy measurably changes the distribution under load.
// Same load, same backends (b1 slow, b2 fast): round robin splits requests evenly,
// least connections steers them away from the backend that holds them longer.
TEST_F(ProxyTest, LeastConnectionsSendsLessTrafficToASlowBackendThanRoundRobin) {
    mock::MockFaults slow;
    slow.latency_ms = 100;
    const auto p_slow = start_backend(slow, "b1");
    const auto p_fast = start_backend({}, "b2");

    const auto measure = [&](const char* strategy) {
        backend(0).reset_stats();
        backend(1).reset_stats();
        lb::Engine engine(lbtest::make_proxy_config({p_slow, p_fast}, [&](nlohmann::json& j) {
            use_strategy(j, strategy);
            j["workers"]["threads"] = 4;
        }));
        std::string error;
        EXPECT_TRUE(engine.start(&error)) << error;
        const int ok = run_load(engine.listen_port(), 8, std::chrono::milliseconds(1500));
        engine.stop();
        const double slow_n = static_cast<double>(backend(0).stats().requests);
        const double fast_n = static_cast<double>(backend(1).stats().requests);
        const double share = slow_n / (slow_n + fast_n);
        std::printf("[ lb ] %-17s %5d requests: slow backend %4.0f (%.1f%%), fast backend %5.0f\n", strategy, ok, slow_n,
                    100.0 * share, fast_n);
        return share;
    };

    const double rr_share = measure("round_robin");
    const double lc_share = measure("least_connections");
    EXPECT_GT(rr_share, 0.40);
    EXPECT_LT(rr_share, 0.60);
    EXPECT_LT(lc_share, 0.25);
    EXPECT_LT(lc_share, rr_share / 2);
}

TEST_F(ProxyTest, NoEligibleBackendGives503AndIsCounted) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) { use_strategy(j, "least_connections"); });
    engine().set_backend_state("b1", BackendState::Unhealthy);
    engine().set_backend_state("b2", BackendState::Draining);
    const auto t0 = Clock::now();
    EXPECT_EQ(fetch(proxy_port(), "/").status, 503);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    EXPECT_LT(elapsed_ms, 500.0);  // never a hang
    EXPECT_EQ(engine().stats().no_backend_available, 1u);
    EXPECT_EQ(backend(0).stats().requests + backend(1).stats().requests, 0u);
}

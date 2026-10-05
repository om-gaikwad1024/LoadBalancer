#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
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

// ---- Phase 2 strategies (plan IV.7) -------------------------------------------------------

// Plan IV.7 done: under synthetic load an artificially slow backend receives measurably
// less traffic under least response time (compared with round robin and least connections).
TEST_F(ProxyTest, LeastResponseTimeSendsMeasurablyLessTrafficToASlowBackend) {
    mock::MockFaults slow;
    slow.latency_ms = 20;
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
        double slow_ewma = -1;
        double fast_ewma = -1;
        for (const auto& b : engine.backend_stats()) (b.id == "b1" ? slow_ewma : fast_ewma) = b.response_time_ms;
        engine.stop();
        const double slow_n = static_cast<double>(backend(0).stats().requests);
        const double fast_n = static_cast<double>(backend(1).stats().requests);
        const double share = slow_n / (slow_n + fast_n);
        std::printf("[ lb ] %-20s %5d requests: slow backend %4.0f (%.2f%%), fast backend %5.0f; "
                    "EWMA slow %.2f ms, fast %.3f ms\n",
                    strategy, ok, slow_n, 100.0 * share, fast_n, slow_ewma, fast_ewma);
        if (std::string(strategy) == "least_response_time") {
            EXPECT_GT(slow_ewma, 15.0);
            EXPECT_GE(fast_ewma, 0.0);
            EXPECT_LT(fast_ewma, slow_ewma / 5);
        }
        return share;
    };

    const double rr_share = measure("round_robin");
    const double lc_share = measure("least_connections");
    const double lrt_share = measure("least_response_time");
    EXPECT_GT(rr_share, 0.40);
    EXPECT_LT(lrt_share, 0.02);
    EXPECT_LT(lrt_share, lc_share);
    EXPECT_LT(lrt_share, rr_share / 20);
}

// A backend that fails at once must not look fast: a failure counts as taking the whole
// backend_response_ms.
TEST_F(ProxyTest, LeastResponseTimeAvoidsABackendThatFailsFast) {
    mock::MockFaults broken;
    broken.close_rate = 1.0;  // every request: connection reset, no response
    const auto p_broken = start_backend(broken, "b1");
    const auto p_ok = start_backend({}, "b2");
    start_proxy({p_broken, p_ok}, [](nlohmann::json& j) {
        use_strategy(j, "least_response_time");
        j["workers"]["threads"] = 4;
    });
    const int ok = run_load(proxy_port(), 4, std::chrono::milliseconds(1000));
    const auto broken_n = backend(0).stats().requests;
    std::printf("[ lb ] %d good responses; the failing backend got %llu requests\n", ok,
                static_cast<unsigned long long>(broken_n));
    EXPECT_GT(ok, 500);
    EXPECT_LE(broken_n, 8u);  // cold start only: once it has failed, it is avoided
}

// Plan IV.7 done: backends weighted 3:1 receive close to a 3:1 share.
TEST_F(ProxyTest, WeightedRoundRobinGivesThreeToOneUnderLoad) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2}, [](nlohmann::json& j) {
        use_strategy(j, "weighted_round_robin");
        j["groups"][0]["backends"][0]["weight"] = 3;
        j["groups"][0]["backends"][1]["weight"] = 1;
        j["workers"]["threads"] = 4;
    });
    const int ok = run_load(proxy_port(), 8, std::chrono::milliseconds(1000));
    const double a = static_cast<double>(backend(0).stats().requests);
    const double b = static_cast<double>(backend(1).stats().requests);
    std::printf("[ lb ] %d requests: weight 3 got %.0f (%.2f%%), weight 1 got %.0f\n", ok, a, 100.0 * a / (a + b), b);
    EXPECT_GT(ok, 1000);
    EXPECT_NEAR(a / (a + b), 0.75, 0.01);
}

TEST_F(ProxyTest, WeightedRoundRobinWeightChangeAppliesAfterReload) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto tweak = [](int w1, int w2) {
        return [w1, w2](nlohmann::json& j) {
            use_strategy(j, "weighted_round_robin");
            j["groups"][0]["backends"][0]["weight"] = w1;
            j["groups"][0]["backends"][1]["weight"] = w2;
        };
    };
    start_proxy({p1, p2}, tweak(1, 1));
    ASSERT_TRUE(engine().reload(lbtest::make_proxy_config({p1, p2}, tweak(1, 4))).accepted);
    TestClient c(proxy_port());
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    EXPECT_EQ(backend(0).stats().requests, 100u);
    EXPECT_EQ(backend(1).stats().requests, 400u);
}

// IP hash keys on client_identity(): with the test client as a trusted proxy, the
// X-Forwarded-For address is the client.
TEST_F(ProxyTest, IpHashKeepsEachClientOnOneBackendAndSpreadsClients) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) {
        use_strategy(j, "ip_hash");
        j["trusted_proxies"] = nlohmann::json::array({"127.0.0.1/32"});
    });
    std::map<std::string, int> clients_per_backend;
    for (int client = 0; client < 90; ++client) {
        const std::string xff = "X-Forwarded-For: 198.51.100." + std::to_string(client) + "\r\n";
        TestClient c(proxy_port());
        std::string first;
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(c.send(get_request("/", xff)));
            const auto r = c.read_response();
            ASSERT_EQ(r.status, 200);
            if (i == 0) first = r.header("x-backend-id");
            EXPECT_EQ(r.header("x-backend-id"), first) << "client " << client << " request " << i;
        }
        // The same client on a new connection lands on the same backend too.
        EXPECT_EQ(fetch(proxy_port(), "/", xff).header("x-backend-id"), first);
        clients_per_backend[first]++;
    }
    for (const char* id : {"b1", "b2", "b3"}) EXPECT_GT(clients_per_backend[id], 15) << id;

    // A drained backend's clients move; everyone else stays put.
    ASSERT_TRUE(engine().set_backend_state("b2", BackendState::Draining));
    for (int client = 0; client < 90; ++client) {
        const std::string xff = "X-Forwarded-For: 198.51.100." + std::to_string(client) + "\r\n";
        EXPECT_NE(fetch(proxy_port(), "/", xff).header("x-backend-id"), "b2");
    }
}

TEST_F(ProxyTest, IpHashIgnoresForwardedForFromUntrustedPeers) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    const auto p3 = start_backend({}, "b3");
    start_proxy({p1, p2, p3}, [](nlohmann::json& j) { use_strategy(j, "ip_hash"); });  // no trusted proxies
    std::set<std::string> seen;
    for (int client = 0; client < 30; ++client) {
        const std::string xff = "X-Forwarded-For: 198.51.100." + std::to_string(client) + "\r\n";
        seen.insert(fetch(proxy_port(), "/", xff).header("x-backend-id"));
    }
    EXPECT_EQ(seen.size(), 1u);  // every request is the same client: 127.0.0.1
}

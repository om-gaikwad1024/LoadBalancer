#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "scripted_backend.h"
#include "test_http_client.h"
#include "test_process.h"

using lb::BackendState;
using lbtest::ClientResponse;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::ScriptedBackend;
using lbtest::TestClient;
using End = ClientResponse::End;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

lb::BackendStats stats_of(lb::Engine& e, const std::string& id) {
    for (auto& s : e.backend_stats()) {
        if (s.id == id) return s;
    }
    ADD_FAILURE() << "no backend " << id;
    return {};
}

}  // namespace

// Plan IV.5 Done: new backend connections per second far below requests per second.
TEST_F(ProxyTest, KeepAliveBackendConnectionsAreReused) {
    start_proxy({start_backend()});
    TestClient c(proxy_port());
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(c.send(get_request("/r")));
        ASSERT_EQ(c.read_response().status, 200);
    }
    ASSERT_TRUE(eventually([&] { return engine().stats().requests_completed == 200; }));
    const auto s = engine().stats();
    EXPECT_EQ(s.backend_connections_opened, 1u);
    EXPECT_EQ(s.backend_connections_reused, 199u);
    EXPECT_EQ(backend().stats().connections, 1u);
    const auto b = stats_of(engine(), "b1");
    EXPECT_EQ(b.requests, 200u);
    EXPECT_EQ(b.successes, 200u);
    EXPECT_EQ(b.in_flight, 0u);
    EXPECT_EQ(b.idle_connections, 1u);
}

TEST_F(ProxyTest, ConcurrentClientsShareASmallPool) {
    start_proxy({start_backend()});
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < 8; ++t) {
        clients.emplace_back([&] {
            TestClient c(proxy_port());
            for (int i = 0; i < 50; ++i) {
                if (c.send(get_request("/c")) && c.read_response().status == 200) ++ok;
            }
        });
    }
    for (auto& t : clients) t.join();
    EXPECT_EQ(ok.load(), 400);
    const auto s = engine().stats();
    std::printf("[ pool ] 400 requests from 8 clients used %llu backend connections\n",
                static_cast<unsigned long long>(s.backend_connections_opened));
    EXPECT_LE(s.backend_connections_opened, 8u);
    EXPECT_LE(backend().stats().connections, 8u);
}

// Plan IV.5: at the cap, new requests wait in a bounded queue, then fail with 503.
TEST_F(ProxyTest, RequestsWaitForAFreeConnectionAtTheCap) {
    mock::MockFaults f;
    f.latency_ms = 300;
    start_proxy({start_backend(f)}, [](nlohmann::json& j) { j["pool"]["max_connections_per_backend"] = 1;
        j["pool"]["max_idle_per_backend"] = 1; });
    ClientResponse first;
    ClientResponse second;
    const auto t0 = Clock::now();
    std::thread a([&] { first = fetch(proxy_port(), "/a"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::thread b([&] { second = fetch(proxy_port(), "/b"); });
    a.join();
    b.join();
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(second.status, 200);
    EXPECT_GE(ms_since(t0), 550.0);  // served one after the other on the single connection
    EXPECT_EQ(engine().stats().backend_connections_opened, 1u);
    EXPECT_EQ(backend().stats().connections, 1u);
}

TEST_F(ProxyTest, QueuedRequestGets503WhenTheWaitTimesOut) {
    mock::MockFaults f;
    f.latency_ms = 800;
    start_proxy({start_backend(f)}, [](nlohmann::json& j) {
        j["pool"]["max_connections_per_backend"] = 1;
        j["pool"]["max_idle_per_backend"] = 1;
        j["pool"]["wait_timeout_ms"] = 100;
    });
    std::thread slow([&] { fetch(proxy_port(), "/slow"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = Clock::now();
    const auto r = fetch(proxy_port(), "/waits");
    const double waited = ms_since(t0);
    slow.join();
    EXPECT_EQ(r.status, 503);
    EXPECT_GE(waited, 90.0);
    EXPECT_LT(waited, 600.0);
    EXPECT_EQ(engine().stats().pool_rejections, 1u);
    EXPECT_EQ(stats_of(engine(), "b1").failures, 0u);  // overload, not a backend failure
}

TEST_F(ProxyTest, FullWaitQueueGets503Immediately) {
    mock::MockFaults f;
    f.latency_ms = 500;
    start_proxy({start_backend(f)}, [](nlohmann::json& j) {
        j["pool"]["max_connections_per_backend"] = 1;
        j["pool"]["max_idle_per_backend"] = 1;
        j["pool"]["max_waiters_per_backend"] = 0;
    });
    std::thread slow([&] { fetch(proxy_port(), "/slow"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = Clock::now();
    EXPECT_EQ(fetch(proxy_port(), "/rejected").status, 503);
    EXPECT_LT(ms_since(t0), 300.0);
    slow.join();
}

// Plan IV.5: a backend that goes unhealthy or starts draining has its idle connections
// closed immediately; plan IV.4: it gets no new requests.
TEST_F(ProxyTest, IneligibleBackendLosesIdleConnectionsAndGetsNoRequests) {
    start_proxy({start_backend()});
    ASSERT_EQ(fetch(proxy_port(), "/").status, 200);
    ASSERT_TRUE(eventually([&] { return stats_of(engine(), "b1").idle_connections == 1; }));
    ASSERT_EQ(backend().stats().active_connections, 1u);

    for (BackendState state : {BackendState::Unhealthy, BackendState::Draining}) {
        ASSERT_TRUE(engine().set_backend_state("b1", state));
        EXPECT_EQ(stats_of(engine(), "b1").idle_connections, 0u);
        EXPECT_TRUE(eventually([&] { return backend().stats().active_connections == 0; }));
        const auto before = backend().stats().requests;
        EXPECT_EQ(fetch(proxy_port(), "/").status, 503);  // its only backend is ineligible
        EXPECT_EQ(backend().stats().requests, before);

        ASSERT_TRUE(engine().set_backend_state("b1", BackendState::Healthy));
        EXPECT_EQ(fetch(proxy_port(), "/").status, 200);
    }
    EXPECT_FALSE(engine().set_backend_state("unknown", BackendState::Healthy));
}

TEST_F(ProxyTest, TrafficSkipsIneligibleBackends) {
    const auto p1 = start_backend({}, "b1");
    const auto p2 = start_backend({}, "b2");
    start_proxy({p1, p2});
    EXPECT_EQ(fetch(proxy_port(), "/").header("x-backend-id"), "b1");
    engine().set_backend_state("b1", BackendState::Unhealthy);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(fetch(proxy_port(), "/").header("x-backend-id"), "b2");
    EXPECT_EQ(backend(0).stats().requests, 1u);
}

TEST_F(ProxyTest, IdleConnectionsAreClosedAfterTheIdleTimeout) {
    start_proxy({start_backend()}, [](nlohmann::json& j) {
        j["pool"]["idle_timeout_ms"] = 100;
        j["maintenance"]["interval_ms"] = 20;
    });
    ASSERT_EQ(fetch(proxy_port(), "/").status, 200);
    EXPECT_TRUE(eventually([&] { return stats_of(engine(), "b1").open_connections == 0; }));
    EXPECT_TRUE(eventually([&] { return backend().stats().active_connections == 0; }));
}

TEST_F(ProxyTest, ConnectionCloseResponseIsNotPooled) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok");
    start_proxy({b.port()});
    ASSERT_EQ(fetch(proxy_port(), "/").status, 200);
    EXPECT_TRUE(eventually([&] { return stats_of(engine(), "b1").open_connections == 0; }));
    EXPECT_EQ(stats_of(engine(), "b1").idle_connections, 0u);
}

TEST_F(ProxyTest, InFlightCountsRequestsNotSockets) {
    mock::MockFaults f;
    f.latency_ms = 400;
    start_proxy({start_backend(f)});
    std::thread t([&] { fetch(proxy_port(), "/slow"); });
    EXPECT_TRUE(eventually([&] { return stats_of(engine(), "b1").in_flight == 1; }));
    t.join();
    EXPECT_TRUE(eventually([&] { return stats_of(engine(), "b1").in_flight == 0; }));
    EXPECT_EQ(stats_of(engine(), "b1").open_connections, 1u);  // the socket stays pooled
}

// Plan VI: a pooled connection the backend closed is detected on reuse; an idempotent
// request goes out again on a fresh connection, anything else gets 502.
TEST_F(ProxyTest, StaleConnectionIsRetriedOnAFreshOneForIdempotentRequests) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", /*close_after=*/false,
                      /*answers_per_connection=*/1);
    start_proxy({b.port()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/one")));
    ASSERT_EQ(c.read_response().status, 200);
    ASSERT_TRUE(c.send(get_request("/two")));  // its pooled connection dies on use
    const auto r = c.read_response();
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(engine().stats().stale_retries, 1u);
    EXPECT_EQ(b.connections(), 2);
}

TEST_F(ProxyTest, StaleConnectionGives502ForRequestsWithABody) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", /*close_after=*/false,
                      /*answers_per_connection=*/1);
    start_proxy({b.port()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/one")));
    ASSERT_EQ(c.read_response().status, 200);
    ASSERT_TRUE(c.send("POST /two HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nabc"));
    EXPECT_EQ(c.read_response().status, 502);
    EXPECT_EQ(engine().stats().stale_retries, 0u);
}

TEST_F(ProxyTest, PooledConnectionClosedByTheBackendIsDiscardedBeforeUse) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", /*close_after=*/true);
    start_proxy({b.port()});
    for (int i = 0; i < 3; ++i) EXPECT_EQ(fetch(proxy_port(), "/").status, 200);
    // Each pooled connection already had the backend's FIN when it was taken.
    EXPECT_TRUE(eventually([&] { return stats_of(engine(), "b1").stale_discarded >= 1; }));
    EXPECT_EQ(engine().stats().error_responses, 0u);
}

// Plan IV.5 Done: a killed backend does not leave stale pooled connections that cause
// repeated errors. The backend is a real process, killed hard and restarted on its port.
TEST(ProxyPool, KilledBackendLeavesNoStalePooledConnections) {
    lbtest::ChildProcess mock;
    ASSERT_TRUE(mock.start(LB_MOCK_BACKEND_EXE, L"--port 0"));
    std::string line;
    ASSERT_TRUE(mock.read_line(&line, std::chrono::seconds(10)));
    const auto port = static_cast<std::uint16_t>(std::stoi(line.substr(line.find("127.0.0.1:") + 10)));

    lb::Engine engine(lbtest::make_proxy_config({port}));
    std::string error;
    ASSERT_TRUE(engine.start(&error)) << error;

    // Fill the pool with several idle connections.
    {
        std::vector<std::thread> warm;
        for (int i = 0; i < 4; ++i) warm.emplace_back([&] { fetch(engine.listen_port(), "/warm"); });
        for (auto& t : warm) t.join();
    }
    ASSERT_GE(stats_of(engine, "b1").idle_connections, 1u);

    mock.kill();
    lbtest::ChildProcess restarted;
    ASSERT_TRUE(restarted.start(LB_MOCK_BACKEND_EXE, L"--port " + std::to_wstring(port)));
    ASSERT_TRUE(restarted.read_line(&line, std::chrono::seconds(10))) << "restart failed";

    int errors = 0;
    for (int i = 0; i < 20; ++i) {
        if (fetch(engine.listen_port(), "/after").status != 200) ++errors;
    }
    EXPECT_EQ(errors, 0);
    const auto b = stats_of(engine, "b1");
    std::printf("[ pool ] after kill+restart: %llu stale discarded, %llu stale retries\n",
                static_cast<unsigned long long>(b.stale_discarded),
                static_cast<unsigned long long>(engine.stats().stale_retries));
    EXPECT_GE(b.stale_discarded + engine.stats().stale_retries, 1u);
    engine.stop();
}

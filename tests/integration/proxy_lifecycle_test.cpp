#include <gtest/gtest.h>

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "proxy_test_fixture.h"
#include "test_http_client.h"
#include "test_process.h"

using lbtest::ClientResponse;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::TestClient;
using End = ClientResponse::End;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size()))) - 1;
    return v[std::min(idx, v.size() - 1)];
}

DWORD process_handle_count() {
    DWORD n = 0;
    ::GetProcessHandleCount(::GetCurrentProcess(), &n);
    return n;
}

std::size_t process_thread_count() {
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    std::size_t n = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = ::Thread32First(snap, &te); ok; ok = ::Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID == ::GetCurrentProcessId()) ++n;
    }
    ::CloseHandle(snap);
    return n;
}

// Keeps Winsock loaded for a whole test, so handle counts do not move with WSAStartup refcounts.
struct WinsockHold {
    WinsockHold() {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockHold() { ::WSACleanup(); }
};

}  // namespace

// Plan IV.1 Done: a deliberately slow client does not raise other clients' p99.
TEST_F(ProxyTest, SlowClientsDoNotRaiseOtherClientsP99) {
    start_proxy({start_backend()}, [](nlohmann::json& j) { j["workers"]["threads"] = 2; });

    const auto measure = [&](int requests) {
        TestClient c(proxy_port());
        std::vector<double> latencies;
        for (int i = 0; i < requests; ++i) {
            const auto t0 = Clock::now();
            EXPECT_TRUE(c.send(get_request("/fast")));
            EXPECT_EQ(c.read_response().status, 200);
            latencies.push_back(ms_since(t0));
        }
        return percentile(latencies, 0.99);
    };

    measure(50);  // warm-up
    const double baseline_p99 = measure(400);

    // 50 slowloris clients, each trickling a request head one byte every 50 ms.
    std::vector<std::unique_ptr<TestClient>> slow;
    for (int i = 0; i < 50; ++i) slow.push_back(std::make_unique<TestClient>(proxy_port()));
    const std::string slow_request = "GET /slow HTTP/1.1\r\nHost: t\r\nX-Pad: " + std::string(400, 'p') + "\r\n\r\n";
    std::atomic<bool> stop{false};
    std::thread driver([&] {
        for (std::size_t i = 0; i + 1 < slow_request.size() && !stop; ++i) {
            for (auto& s : slow) s->send(std::string_view(slow_request).substr(i, 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double loaded_p99 = measure(400);
    stop = true;
    driver.join();

    RecordProperty("baseline_p99_ms", std::to_string(baseline_p99));
    RecordProperty("with_slow_clients_p99_ms", std::to_string(loaded_p99));
    std::printf("[ p99 ] baseline %.3f ms, with 50 slow clients %.3f ms (2 workers)\n", baseline_p99, loaded_p99);
    EXPECT_LE(loaded_p99, baseline_p99 * 3.0 + 5.0);
    EXPECT_EQ(backend().stats().requests, 850u);  // slow clients never completed a request
}

TEST_F(ProxyTest, ShutdownLetsInFlightRequestsFinish) {
    mock::MockFaults f;
    f.latency_ms = 500;
    start_proxy({start_backend(f)});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/slow")));
    ClientResponse r;
    std::thread reader([&] { r = c.read_response(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto t0 = Clock::now();
    engine().stop();
    const double stop_ms = ms_since(t0);
    reader.join();

    EXPECT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.header("connection"), "close");  // no further requests during shutdown
    EXPECT_GE(stop_ms, 250.0);
    EXPECT_LT(stop_ms, 4000.0);
}

TEST_F(ProxyTest, ShutdownClosesIdleConnectionsAtOnce) {
    start_proxy({start_backend()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/")));
    ASSERT_EQ(c.read_response().status, 200);

    const auto t0 = Clock::now();
    engine().stop();
    EXPECT_LT(ms_since(t0), 1000.0);
    EXPECT_EQ(c.wait_for_close(), End::Closed);
}

TEST_F(ProxyTest, ShutdownForceClosesWhenGraceExpires) {
    mock::MockFaults f;
    f.latency_ms = 5000;
    start_proxy({start_backend(f)}, [](nlohmann::json& j) { j["timeouts"]["shutdown_grace_ms"] = 300; });
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/stuck")));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto t0 = Clock::now();
    engine().stop();
    EXPECT_LT(ms_since(t0), 2500.0);
    EXPECT_NE(c.read_response().end, End::Complete);
}

// Plan IV.1 Done: after shutdown the process holds no open sockets (handle count), and
// no engine thread is left behind. The backend runs in a child process so only the
// proxy's handles are counted here.
TEST(ProxyLifecycle, ShutdownReleasesEverySocketAndThread) {
    WinsockHold winsock;
    lbtest::ChildProcess mock;
    ASSERT_TRUE(mock.start(LB_MOCK_BACKEND_EXE, L"--port 0"));
    std::string line;
    ASSERT_TRUE(mock.read_line(&line, std::chrono::seconds(10)));
    const auto port = static_cast<std::uint16_t>(std::stoi(line.substr(line.find("127.0.0.1:") + 10)));

    const auto cycle = [&] {
        lb::Engine engine(lbtest::make_proxy_config({port}, [](nlohmann::json& j) {
            j["workers"]["threads"] = 4;
            j["timeouts"]["shutdown_grace_ms"] = 200;
        }));
        std::string error;
        ASSERT_TRUE(engine.start(&error)) << error;
        std::vector<std::unique_ptr<TestClient>> clients;
        for (int i = 0; i < 10; ++i) {  // idle keep-alive connections
            clients.push_back(std::make_unique<TestClient>(engine.listen_port()));
            ASSERT_TRUE(clients.back()->send(get_request("/")));
            ASSERT_EQ(clients.back()->read_response().status, 200);
        }
        for (int i = 0; i < 5; ++i) {  // connections stuck mid-head
            clients.push_back(std::make_unique<TestClient>(engine.listen_port()));
            ASSERT_TRUE(clients.back()->send("GET / HTTP/1.1\r\nHost: x\r\n"));
        }
        engine.stop();
        clients.clear();
    };

    cycle();  // warm-up: lazily created system handles and threads appear here
    const DWORD baseline_handles = process_handle_count();
    const std::size_t baseline_threads = process_thread_count();
    for (int i = 0; i < 3; ++i) {
        cycle();
        const DWORD handles = process_handle_count();
        const std::size_t threads = process_thread_count();
        std::printf("[ handles ] cycle %d: %lu handles (baseline %lu), %zu threads (baseline %zu)\n", i + 1, handles,
                    baseline_handles, threads, baseline_threads);
        EXPECT_LE(handles, baseline_handles + 2) << "cycle " << i + 1;
        EXPECT_LE(threads, baseline_threads + 1) << "cycle " << i + 1;
    }
}

TEST(ProxyLifecycle, BindConflictIsReported) {
    lb::Engine first(lbtest::make_proxy_config({9}));
    std::string error;
    ASSERT_TRUE(first.start(&error)) << error;
    const auto taken = first.listen_port();
    lb::Engine second(lbtest::make_proxy_config({9}, [&](nlohmann::json& j) { j["listen"]["port"] = taken; }));
    EXPECT_FALSE(second.start(&error));
    EXPECT_NE(error.find("bind"), std::string::npos) << error;
}

TEST(ProxyLifecycle, AutoWorkerCountIsOnePerLogicalProcessor) {
    lb::Engine engine(lbtest::make_proxy_config({9}, [](nlohmann::json& j) { j["workers"]["threads"] = "auto"; }));
    std::string error;
    ASSERT_TRUE(engine.start(&error)) << error;
    EXPECT_EQ(engine.worker_threads(), ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
}

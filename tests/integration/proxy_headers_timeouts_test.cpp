#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>

#include "proxy_test_fixture.h"
#include "scripted_backend.h"
#include "test_http_client.h"

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

bool has_line(const std::string& head, const std::string& line) {
    return head.find("\r\n" + line + "\r\n") != std::string::npos;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (auto p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++n;
    return n;
}

mock::MockFaults echo() {
    mock::MockFaults f;
    f.echo_headers = true;
    return f;
}

}  // namespace

// ---- Forwarding headers (plan IV.6 Done: an echo backend shows them) ------------------

TEST_F(ProxyTest, EchoBackendSeesForwardingHeadersAndRequestId) {
    start_proxy({start_backend(echo())});
    const auto r = fetch(proxy_port(), "/p", "");  // Host: test
    ASSERT_EQ(r.status, 200);
    const std::string id = r.header("x-request-id");
    ASSERT_EQ(id.size(), 32u) << r.head;
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-For: 127.0.0.1")) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-Proto: http")) << r.body;
    EXPECT_TRUE(has_line(r.body, "Host: test")) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-Host: test")) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Request-Id: " + id)) << r.body;
}

TEST_F(ProxyTest, SpoofedForwardingHeadersAreAppendedToOrReplaced) {
    start_proxy({start_backend(echo())});
    const auto r = fetch(proxy_port(), "/",
                         "X-Forwarded-For: 6.6.6.6\r\nX-Forwarded-Proto: https\r\n"
                         "X-Forwarded-Host: evil.example\r\nX-Request-Id: spoofed-id\r\n");
    ASSERT_EQ(r.status, 200);
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-For: 6.6.6.6, 127.0.0.1")) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-Proto: http")) << r.body;
    EXPECT_EQ(count_of(r.body, "X-Forwarded-Proto"), 1u);
    EXPECT_EQ(r.body.find("evil.example"), std::string::npos) << r.body;
    EXPECT_EQ(r.body.find("spoofed-id"), std::string::npos) << r.body;
    EXPECT_NE(r.header("x-request-id"), "spoofed-id");
}

TEST_F(ProxyTest, TrustedProxyKeepsUpstreamForwardingHeaders) {
    start_proxy({start_backend(echo())}, [](nlohmann::json& j) { j["trusted_proxies"] = {"127.0.0.0/8"}; });
    const auto r = fetch(proxy_port(), "/",
                         "X-Forwarded-For: 198.51.100.7\r\nX-Forwarded-Proto: https\r\n"
                         "X-Forwarded-Host: www.example\r\nX-Request-Id: edge-1234\r\n");
    ASSERT_EQ(r.status, 200);
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-For: 198.51.100.7, 127.0.0.1")) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-Proto: https")) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-Host: www.example")) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Request-Id: edge-1234")) << r.body;
    EXPECT_EQ(r.header("x-request-id"), "edge-1234");
}

TEST_F(ProxyTest, HostIsRewrittenToTheBackendWhenTheGroupSaysSo) {
    const auto port = start_backend(echo());
    start_proxy({port}, [](nlohmann::json& j) { j["groups"][0]["host_header"] = "backend"; });
    const auto r = fetch(proxy_port(), "/");
    EXPECT_TRUE(has_line(r.body, "Host: 127.0.0.1:" + std::to_string(port))) << r.body;
    EXPECT_TRUE(has_line(r.body, "X-Forwarded-Host: test")) << r.body;
}

TEST_F(ProxyTest, EveryRequestGetsItsOwnRequestId) {
    start_proxy({start_backend()});
    TestClient c(proxy_port());
    std::set<std::string> ids;
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        ids.insert(c.read_response().header("x-request-id"));
    }
    EXPECT_EQ(ids.size(), 20u);
}

TEST_F(ProxyTest, ProxyErrorResponsesCarryTheRequestId) {
    const auto dead = start_backend();
    backend().stop();
    start_proxy({dead});
    const auto r = fetch(proxy_port(), "/");
    EXPECT_EQ(r.status, 502);
    EXPECT_EQ(r.header("x-request-id").size(), 32u) << r.head;
}

// ---- Timeouts (plan VI table) -----------------------------------------------------------

TEST_F(ProxyTest, ClientHeaderTimeoutClosesAConnectionThatSendsNothing) {
    start_proxy({start_backend()}, [](nlohmann::json& j) { j["timeouts"]["client_header_ms"] = 300; });
    TestClient c(proxy_port());
    const auto t0 = Clock::now();
    EXPECT_EQ(c.wait_for_close(), End::Closed);
    const double ms = ms_since(t0);
    EXPECT_GE(ms, 250.0);
    EXPECT_LT(ms, 1500.0);
    EXPECT_EQ(engine().stats().client_timeouts, 1u);
}

TEST_F(ProxyTest, ClientHeaderTimeoutIsAbsoluteAgainstSlowloris) {
    start_proxy({start_backend()}, [](nlohmann::json& j) { j["timeouts"]["client_header_ms"] = 400; });
    TestClient c(proxy_port());
    const std::string head = "GET / HTTP/1.1\r\nHost: t\r\nX-Pad: " + std::string(200, 'p') + "\r\n\r\n";
    const auto t0 = Clock::now();
    // A byte every 30 ms: never idle, but the head is not complete before the 400 ms deadline.
    for (std::size_t i = 0; i + 1 < head.size() && ms_since(t0) < 700.0; ++i) {
        if (!c.send(std::string_view(head).substr(i, 1))) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const End end = c.wait_for_close();
    EXPECT_TRUE(end == End::Closed || end == End::Reset);
    EXPECT_LT(ms_since(t0), 2500.0);
    EXPECT_EQ(engine().stats().client_timeouts, 1u);
    EXPECT_EQ(backend().stats().requests, 0u);
}

TEST_F(ProxyTest, KeepAliveIdleTimeoutClosesAnIdleConnection) {
    start_proxy({start_backend()}, [](nlohmann::json& j) { j["timeouts"]["client_keepalive_idle_ms"] = 300; });
    TestClient c(proxy_port());
    for (int i = 0; i < 3; ++i) {  // traffic within the timeout keeps the connection
        ASSERT_TRUE(c.send(get_request("/")));
        ASSERT_EQ(c.read_response().status, 200);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    const auto t0 = Clock::now();
    EXPECT_EQ(c.wait_for_close(), End::Closed);
    EXPECT_LT(ms_since(t0), 1500.0);
    EXPECT_EQ(engine().stats().client_timeouts, 1u);
}

TEST_F(ProxyTest, FirstByteOfTheNextRequestSwitchesToTheHeaderTimeout) {
    start_proxy({start_backend()}, [](nlohmann::json& j) {
        j["timeouts"]["client_keepalive_idle_ms"] = 300;
        j["timeouts"]["client_header_ms"] = 800;
    });
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/")));
    ASSERT_EQ(c.read_response().status, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_TRUE(c.send("GE"));  // started, never finished
    const auto t0 = Clock::now();
    EXPECT_EQ(c.wait_for_close(), End::Closed);
    EXPECT_GE(ms_since(t0), 700.0);  // the header deadline, not the 300 ms keep-alive one
}

TEST_F(ProxyTest, ClientBodyTimeoutGives408) {
    start_proxy({start_backend()}, [](nlohmann::json& j) { j["timeouts"]["client_body_idle_ms"] = 300; });
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nabc"));
    const auto t0 = Clock::now();
    const auto r = c.read_response();
    EXPECT_EQ(r.status, 408);
    EXPECT_EQ(r.header("connection"), "close");
    EXPECT_GE(ms_since(t0), 250.0);
    EXPECT_EQ(c.wait_for_close(), End::Closed);
}

TEST_F(ProxyTest, BackendResponseTimeoutGives504AndCountsAsAFailure) {
    mock::MockFaults f;
    f.latency_ms = 3000;
    start_proxy({start_backend(f)}, [](nlohmann::json& j) { j["timeouts"]["backend_response_ms"] = 300; });
    const auto t0 = Clock::now();
    const auto r = fetch(proxy_port(), "/");
    EXPECT_EQ(r.status, 504);
    EXPECT_GE(ms_since(t0), 250.0);
    EXPECT_LT(ms_since(t0), 2000.0);
    EXPECT_EQ(engine().stats().backend_timeouts, 1u);
    EXPECT_TRUE(eventually([&] { return engine().backend_stats()[0].failures == 1; }));
    EXPECT_EQ(engine().backend_stats()[0].idle_connections, 0u);  // a timed-out connection is not reused
}

TEST_F(ProxyTest, BackendStallMidBodyCutsOffTheClient) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + std::string(10, 'x'), /*close_after=*/false);
    start_proxy({b.port()}, [](nlohmann::json& j) { j["timeouts"]["backend_idle_ms"] = 300; });
    const auto t0 = Clock::now();
    const auto r = fetch(proxy_port(), "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.end, End::Complete);
    EXPECT_LT(r.body.size(), 100u);
    EXPECT_LT(ms_since(t0), 1500.0);
    EXPECT_EQ(engine().stats().backend_timeouts, 1u);
}

TEST_F(ProxyTest, ClientWriteTimeoutClosesAClientThatStopsReading) {
    mock::MockFaults f;
    f.body_bytes = 32 * 1024 * 1024;
    start_proxy({start_backend(f)}, [](nlohmann::json& j) { j["timeouts"]["client_write_idle_ms"] = 300; });
    TestClient c(proxy_port(), 5000, /*receive_buffer=*/4096);
    ASSERT_TRUE(c.send(get_request("/huge")));  // and never read
    EXPECT_TRUE(eventually([&] { return engine().stats().client_timeouts == 1; }, std::chrono::seconds(10)));
    EXPECT_TRUE(eventually([&] { return engine().stats().connections_active == 0; }));
}

TEST_F(ProxyTest, BackendConnectTimeoutGives502) {
    start_proxy({9}, [](nlohmann::json& j) {
        j["groups"][0]["backends"][0]["address"] = "192.0.2.1";  // TEST-NET-1: packets go nowhere
        j["timeouts"]["backend_connect_ms"] = 300;
    });
    const auto t0 = Clock::now();
    const auto r = fetch(proxy_port(), "/");
    const double ms = ms_since(t0);
    EXPECT_EQ(r.status, 502);
    if (ms < 200.0) GTEST_SKIP() << "this network refuses 192.0.2.1 at once; the connect timeout cannot be exercised";
    EXPECT_LT(ms, 1500.0);
    EXPECT_EQ(engine().stats().backend_timeouts, 1u);
}

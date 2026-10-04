#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mock_server.h"
#include "test_http_client.h"
#include "test_process.h"

using lbtest::ClientResponse;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::TestClient;
using End = lbtest::ClientResponse::End;

namespace {

class MockBackend : public ::testing::Test {
protected:
    void start(mock::MockOptions options = {}) {
        server_ = std::make_unique<mock::MockServer>(std::move(options));
        std::string error;
        ASSERT_TRUE(server_->start(&error)) << error;
    }

    void start_with(const mock::MockFaults& faults) {
        mock::MockOptions o;
        o.faults = faults;
        start(std::move(o));
    }

    std::uint16_t port() const { return server_->port(); }
    mock::MockServer& server() { return *server_; }

    void TearDown() override {
        if (server_) server_->stop();
    }

private:
    std::unique_ptr<mock::MockServer> server_;
};

double elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

// ---- Basic behavior -------------------------------------------------------------------

TEST_F(MockBackend, AnswersOkWithBackendId) {
    mock::MockOptions o;
    o.id = "b1";
    start(std::move(o));
    const auto r = fetch(port(), "/");
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "ok");
    EXPECT_EQ(r.header("x-backend-id"), "b1");
    const auto s = server().stats();
    EXPECT_EQ(s.requests, 1u);
    EXPECT_EQ(s.status_counts.at(200), 1u);
}

TEST_F(MockBackend, DefaultIdIsThePort) {
    start();
    EXPECT_EQ(fetch(port(), "/").header("x-backend-id"), std::to_string(port()));
}

TEST_F(MockBackend, KeepAliveServesManyRequestsOnOneConnection) {
    start();
    TestClient c(port());
    ASSERT_TRUE(c.connected());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(c.send(get_request("/k")));
        const auto r = c.read_response();
        ASSERT_EQ(r.end, End::Complete);
        EXPECT_EQ(r.status, 200);
        EXPECT_TRUE(r.header("connection").empty());
    }
    const auto s = server().stats();
    EXPECT_EQ(s.connections, 1u);
    EXPECT_EQ(s.requests, 3u);
}

TEST_F(MockBackend, ConnectionCloseIsHonored) {
    start();
    TestClient c(port());
    ASSERT_TRUE(c.send(get_request("/", "Connection: close\r\n")));
    const auto r = c.read_response();
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.header("connection"), "close");
    EXPECT_EQ(c.wait_for_close(), End::Closed);
}

TEST_F(MockBackend, Http10ClosesByDefault) {
    start();
    TestClient c(port());
    ASSERT_TRUE(c.send("GET / HTTP/1.0\r\n\r\n"));
    EXPECT_EQ(c.read_response().header("connection"), "close");
    EXPECT_EQ(c.wait_for_close(), End::Closed);
}

TEST_F(MockBackend, ContentLengthAndChunkedRequestBodiesAreConsumed) {
    start();
    TestClient c(port());
    ASSERT_TRUE(c.send("POST /a HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhello"));
    EXPECT_EQ(c.read_response().status, 200);
    ASSERT_TRUE(c.send("POST /b HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nT: x\r\n\r\n"));
    EXPECT_EQ(c.read_response().status, 200);
    ASSERT_TRUE(c.send(get_request("/c")));  // still in sync after both bodies
    EXPECT_EQ(c.read_response().status, 200);
    EXPECT_EQ(server().stats().requests, 3u);
}

TEST_F(MockBackend, HeadRequestGetsHeadersOnly) {
    start();
    TestClient c(port());
    ASSERT_TRUE(c.send("HEAD / HTTP/1.1\r\nHost: t\r\n\r\n"));
    const auto r = c.read_response(/*head_request=*/true);
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.content_length, 2u);
    EXPECT_TRUE(r.body.empty());
    ASSERT_TRUE(c.send(get_request("/")));
    EXPECT_EQ(c.read_response().body, "ok");  // no stray body bytes on the connection
}

TEST_F(MockBackend, BodyBytesSetsResponseSize) {
    mock::MockFaults f;
    f.body_bytes = 1000;
    start_with(f);
    const auto r = fetch(port(), "/");
    ASSERT_EQ(r.body.size(), 1000u);
    EXPECT_EQ(r.body.substr(0, 2), "ok");
}

TEST_F(MockBackend, MalformedRequestGets400) {
    start();
    TestClient c(port());
    ASSERT_TRUE(c.send("NOT-A-REQUEST\r\n\r\n"));
    EXPECT_EQ(c.read_response().status, 400);
}

// ---- Fault switches (plan IX) ---------------------------------------------------------

TEST_F(MockBackend, LatencySwitchDelaysAndCanBeTurnedOff) {
    mock::MockFaults f;
    f.latency_ms = 300;
    start_with(f);
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(fetch(port(), "/").status, 200);
    EXPECT_GE(elapsed_ms(t0), 290.0);

    f.latency_ms = 0;
    server().set_faults(f);
    t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(fetch(port(), "/").status, 200);
    EXPECT_LT(elapsed_ms(t0), 250.0);
}

TEST_F(MockBackend, ErrorRateOneAlwaysAnswersErrorStatus) {
    mock::MockFaults f;
    f.error_rate = 1.0;
    f.error_status = 503;
    start_with(f);
    TestClient c(port());
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        EXPECT_EQ(c.read_response().status, 503);
    }
    EXPECT_EQ(server().stats().status_counts.at(503), 5u);
}

TEST_F(MockBackend, ErrorRateIsHonoredStatistically) {
    mock::MockFaults f;
    f.error_rate = 0.25;
    start_with(f);
    TestClient c(port());
    int errors = 0;
    for (int i = 0; i < 400; ++i) {
        ASSERT_TRUE(c.send(get_request("/")));
        if (c.read_response().status == 500) ++errors;
    }
    EXPECT_GT(errors, 70);
    EXPECT_LT(errors, 130);
}

TEST_F(MockBackend, AbruptCloseResetsWithoutSendingBytes) {
    mock::MockFaults f;
    f.close_rate = 1.0;
    start_with(f);
    TestClient c(port());
    ASSERT_TRUE(c.send(get_request("/")));
    const auto r = c.read_response();
    EXPECT_EQ(r.end, End::Reset);
    EXPECT_TRUE(r.head.empty());
    EXPECT_EQ(server().stats().aborted, 1u);
}

TEST_F(MockBackend, PartialResponseSendsHalfTheBodyThenCloses) {
    mock::MockFaults f;
    f.partial_rate = 1.0;
    f.body_bytes = 100;
    start_with(f);
    const auto r = fetch(port(), "/");
    EXPECT_EQ(r.end, End::Closed);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.content_length, 100u);
    EXPECT_EQ(r.body.size(), 50u);
    EXPECT_EQ(server().stats().partial, 1u);
}

TEST_F(MockBackend, EchoHeadersReturnsTheReceivedRequestHead) {
    mock::MockFaults f;
    f.echo_headers = true;
    start_with(f);
    const auto r = fetch(port(), "/echo?x=1", "X-Forwarded-For: 10.0.0.1\r\nX-Test: 123\r\n");
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.body.rfind("GET /echo?x=1 HTTP/1.1\r\n", 0), 0u) << r.body;
    EXPECT_NE(r.body.find("\r\nX-Forwarded-For: 10.0.0.1\r\n"), std::string::npos);
    EXPECT_NE(r.body.find("\r\nX-Test: 123\r\n"), std::string::npos);
}

TEST_F(MockBackend, HealthPathHasItsOwnStatusAndIgnoresOtherFaults) {
    mock::MockFaults f;
    f.error_rate = 1.0;
    f.close_rate = 1.0;
    start_with(f);
    EXPECT_EQ(fetch(port(), "/health").status, 200);
    f.health_status = 503;
    server().set_faults(f);
    EXPECT_EQ(fetch(port(), "/health").status, 503);
    const auto s = server().stats();
    EXPECT_EQ(s.health_requests, 2u);
    EXPECT_EQ(s.requests, 0u);
}

TEST_F(MockBackend, ControlEndpointChangesSwitchesAtRuntime) {
    start();
    auto r = fetch(port(), "/__mock/set?error_rate=1&error_status=418&latency_ms=0");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("\"error_status\":418"), std::string::npos) << r.body;
    EXPECT_EQ(fetch(port(), "/").status, 418);

    EXPECT_EQ(fetch(port(), "/__mock/set?nope=1").status, 400);
    EXPECT_EQ(fetch(port(), "/__mock/set?error_rate=2").status, 400);
    EXPECT_EQ(fetch(port(), "/__mock/set?error_rate").status, 400);
    EXPECT_EQ(fetch(port(), "/__mock/unknown").status, 404);
    EXPECT_EQ(server().faults().error_status, 418);  // rejected changes left the switches alone

    r = fetch(port(), "/__mock/stats");
    EXPECT_NE(r.body.find("\"requests\":1"), std::string::npos) << r.body;
    EXPECT_NE(r.body.find("\"418\":1"), std::string::npos) << r.body;
    EXPECT_EQ(fetch(port(), "/__mock/reset").status, 200);
    EXPECT_EQ(server().stats().requests, 0u);
}

TEST_F(MockBackend, ConnectionCapRefusesWith503) {
    mock::MockOptions o;
    o.max_connections = 2;
    start(std::move(o));
    TestClient a(port()), b(port());
    ASSERT_TRUE(a.send(get_request("/")));
    ASSERT_TRUE(b.send(get_request("/")));
    ASSERT_EQ(a.read_response().status, 200);  // both are now active connections
    ASSERT_EQ(b.read_response().status, 200);

    TestClient c(port());
    const auto r = c.read_response();
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(server().stats().refused_connections, 1u);
}

TEST_F(MockBackend, StopIsPromptEvenWhileARequestSleeps) {
    mock::MockFaults f;
    f.latency_ms = 10'000;
    start_with(f);
    TestClient c(port());
    ASSERT_TRUE(c.send(get_request("/")));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let the request start sleeping
    const auto t0 = std::chrono::steady_clock::now();
    server().stop();
    EXPECT_LT(elapsed_ms(t0), 2000.0);
    EXPECT_NE(c.read_response().end, End::Complete);
}

TEST_F(MockBackend, CanRestartOnTheSamePort) {
    start();
    const std::uint16_t p = port();
    EXPECT_EQ(fetch(p, "/").status, 200);
    server().stop();

    mock::MockOptions o;
    o.port = p;
    start(std::move(o));
    EXPECT_EQ(port(), p);
    EXPECT_EQ(fetch(p, "/").status, 200);
}

TEST(MockFaultSettings, RejectsBadValuesAndAppliesGoodOnes) {
    mock::MockFaults f;
    EXPECT_FALSE(mock::apply_fault_setting(f, "latency_ms", "-1").empty());
    EXPECT_FALSE(mock::apply_fault_setting(f, "error_rate", "abc").empty());
    EXPECT_FALSE(mock::apply_fault_setting(f, "close_rate", "1.5").empty());
    EXPECT_FALSE(mock::apply_fault_setting(f, "error_status", "99").empty());
    EXPECT_FALSE(mock::apply_fault_setting(f, "echo_headers", "yes").empty());
    EXPECT_FALSE(mock::apply_fault_setting(f, "bogus", "1").empty());

    EXPECT_TRUE(mock::apply_fault_setting(f, "latency_ms", "25").empty());
    EXPECT_TRUE(mock::apply_fault_setting(f, "partial_rate", "0.5").empty());
    EXPECT_TRUE(mock::apply_fault_setting(f, "echo_headers", "true").empty());
    EXPECT_EQ(f.latency_ms, 25u);
    EXPECT_DOUBLE_EQ(f.partial_rate, 0.5);
    EXPECT_TRUE(f.echo_headers);
}

// ---- The real executable: CLI switches and a hard kill --------------------------------

TEST(MockBackendExe, StartsWithCliSwitchesAndDiesOnKill) {
    lbtest::ChildProcess proc;
    ASSERT_TRUE(proc.start(LB_MOCK_BACKEND_EXE, L"--port 0 --id cli-1 --error-rate 1 --error-status 502"));
    std::string line;
    ASSERT_TRUE(proc.read_line(&line, std::chrono::seconds(10))) << "no startup line";
    const auto colon = line.find("127.0.0.1:");
    ASSERT_NE(colon, std::string::npos) << line;
    const auto port = static_cast<std::uint16_t>(std::stoi(line.substr(colon + 10)));

    const auto r = fetch(port, "/");
    EXPECT_EQ(r.status, 502);
    EXPECT_EQ(r.header("x-backend-id"), "cli-1");

    proc.kill();
    TestClient after(port);
    EXPECT_FALSE(after.connected());
}

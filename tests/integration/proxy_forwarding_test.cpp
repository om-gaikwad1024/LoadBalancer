#include <gtest/gtest.h>

#include <string>

#include "proxy_test_fixture.h"
#include "scripted_backend.h"
#include "test_http_client.h"

using lb::TraceStep;
using lbtest::ClientResponse;
using lbtest::fetch;
using lbtest::get_request;
using lbtest::ProxyTest;
using lbtest::ScriptedBackend;
using lbtest::TestClient;
using End = ClientResponse::End;
using Framing = ClientResponse::Framing;

// ---- Forwarding ------------------------------------------------------------------------

TEST_F(ProxyTest, ForwardsAGetToTheBackend) {
    start_proxy({start_backend()});
    const auto r = fetch(proxy_port(), "/hello");
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "ok");
    EXPECT_EQ(r.header("x-backend-id"), "b1");
    EXPECT_TRUE(eventually([&] { return engine().stats().requests_completed == 1; }));
    EXPECT_EQ(backend().stats().requests, 1u);
}

TEST_F(ProxyTest, ClientKeepAliveCarriesManyRequests) {
    start_proxy({start_backend()});
    TestClient c(proxy_port());
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(c.send(get_request("/k" + std::to_string(i))));
        const auto r = c.read_response();
        ASSERT_EQ(r.end, End::Complete);
        EXPECT_EQ(r.status, 200);
        EXPECT_TRUE(r.header("connection").empty());
    }
    // The counter moves when the proxy sees its last send complete, just after the client has the bytes.
    EXPECT_TRUE(eventually([&] { return engine().stats().requests_completed == 5; }));
    const auto s = engine().stats();
    EXPECT_EQ(s.connections_accepted, 1u);
    EXPECT_EQ(s.backend_connections_opened, 1u);  // pooled keep-alive connection (plan IV.5)
    EXPECT_EQ(s.backend_connections_reused, 4u);
}

TEST_F(ProxyTest, ContentLengthBodyIsForwarded) {
    mock::MockFaults f;
    f.echo_body = true;
    start_proxy({start_backend(f)});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 11\r\n\r\nhello world"));
    EXPECT_EQ(c.read_response().body, "hello world");
}

TEST_F(ProxyTest, ChunkedRequestIsReframedAsCleanChunks) {
    mock::MockFaults f;
    f.echo_headers = true;
    f.echo_body = true;
    start_proxy({start_backend(f)});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("POST /c HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: t\r\n\r\n"));
    const auto r = c.read_response();
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_NE(r.body.find("\r\nTransfer-Encoding: chunked\r\n"), std::string::npos) << r.body;
    EXPECT_EQ(r.body.find("Content-Length"), std::string::npos) << r.body;
    EXPECT_EQ(r.body.substr(r.body.size() - 11), "hello world");
}

TEST_F(ProxyTest, LargeBodiesStreamThroughSmallBuffers) {
    mock::MockFaults f;
    f.echo_body = true;
    start_proxy({start_backend(f)}, [](nlohmann::json& j) {
        j["buffers"]["client_read_bytes"] = 1024;
        j["buffers"]["backend_read_bytes"] = 1024;
    });
    std::string body(1024 * 1024, 'x');
    for (std::size_t i = 0; i < body.size(); i += 997) body[i] = static_cast<char>('a' + (i % 26));
    TestClient c(proxy_port(), 20000);
    ASSERT_TRUE(c.send("POST /big HTTP/1.1\r\nHost: t\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body));
    const auto r = c.read_response();
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.body.size(), body.size());
    EXPECT_TRUE(r.body == body);
}

TEST_F(ProxyTest, HopByHopHeadersAreNotForwarded) {
    mock::MockFaults f;
    f.echo_headers = true;
    start_proxy({start_backend(f)});
    const auto r = fetch(proxy_port(), "/h",
                         "Connection: keep-alive, X-Secret\r\nX-Secret: s\r\nKeep-Alive: timeout=5\r\n"
                         "Upgrade: h2c\r\nTE: trailers\r\nProxy-Connection: keep-alive\r\nX-Kept: yes\r\n");
    ASSERT_EQ(r.end, End::Complete);
    for (const char* dropped : {"X-Secret", "Keep-Alive", "Upgrade", "TE:", "Proxy-Connection"}) {
        EXPECT_EQ(r.body.find(dropped), std::string::npos) << dropped << " forwarded:\n" << r.body;
    }
    EXPECT_NE(r.body.find("\r\nX-Kept: yes\r\n"), std::string::npos);
    EXPECT_NE(r.body.find("\r\nHost: test\r\n"), std::string::npos);
}

TEST_F(ProxyTest, ExpectContinueIsAnsweredByTheProxy) {
    mock::MockFaults f;
    f.echo_headers = true;
    f.echo_body = true;
    start_proxy({start_backend(f)});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("PUT /e HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n"));
    const auto interim = c.read_response();
    ASSERT_EQ(interim.status, 100);
    ASSERT_TRUE(c.send("data"));
    const auto r = c.read_response();
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body.find("Expect"), std::string::npos) << r.body;
    EXPECT_EQ(r.body.substr(r.body.size() - 4), "data");
}

TEST_F(ProxyTest, PipelinedRequestsAreAnsweredInOrder) {
    mock::MockFaults f;
    f.echo_headers = true;
    start_proxy({start_backend(f)});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/first") + get_request("/second")));
    EXPECT_EQ(c.read_response().body.rfind("GET /first ", 0), 0u);
    EXPECT_EQ(c.read_response().body.rfind("GET /second ", 0), 0u);
}

TEST_F(ProxyTest, HeadResponseKeepsContentLengthWithoutBody) {
    start_proxy({start_backend()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("HEAD / HTTP/1.1\r\nHost: t\r\n\r\n"));
    const auto r = c.read_response(/*head_request=*/true);
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.header("content-length"), "2");
    ASSERT_TRUE(c.send(get_request("/")));
    EXPECT_EQ(c.read_response().body, "ok");  // the connection is still in sync
}

TEST_F(ProxyTest, Http10ClientWithoutKeepAliveIsClosed) {
    start_proxy({start_backend()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("GET / HTTP/1.0\r\n\r\n"));
    const auto r = c.read_response();
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.header("connection"), "close");
    EXPECT_EQ(c.wait_for_close(), End::Closed);
}

// ---- Response framing toward the client (plan IV.3, IV.6 re-framing) -----------------

TEST_F(ProxyTest, ChunkedBackendResponseStaysChunked) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    start_proxy({b.port()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/")));
    const auto r = c.read_response();
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.framing, Framing::Chunked);
    EXPECT_EQ(r.body, "hello");
    ASSERT_TRUE(c.send(get_request("/again")));  // keep-alive survives
    EXPECT_EQ(c.read_response().body, "hello");
}

TEST_F(ProxyTest, CloseDelimitedResponseIsRechunkedForHttp11) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil the backend closes");
    start_proxy({b.port()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send(get_request("/")));
    const auto r = c.read_response();
    ASSERT_EQ(r.end, End::Complete);
    EXPECT_EQ(r.framing, Framing::Chunked);
    EXPECT_EQ(r.body, "until the backend closes");
    EXPECT_TRUE(r.header("connection").empty());
}

TEST_F(ProxyTest, CloseDelimitedResponseToHttp10ClientIsCloseDelimited) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\n\r\nraw body");
    start_proxy({b.port()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"));
    const auto r = c.read_response();
    EXPECT_EQ(r.framing, Framing::UntilClose);
    EXPECT_EQ(r.end, End::Closed);
    EXPECT_EQ(r.body, "raw body");
}

TEST_F(ProxyTest, InterimResponsesAreNotForwarded) {
    ScriptedBackend b("HTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    start_proxy({b.port()});
    const auto r = fetch(proxy_port(), "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "ok");
}

// ---- Error paths (plan VI) -------------------------------------------------------------

TEST_F(ProxyTest, BackendDownGives502) {
    const std::uint16_t dead_port = start_backend();
    backend().stop();
    start_proxy({dead_port});
    const auto r = fetch(proxy_port(), "/");
    EXPECT_EQ(r.status, 502);
    EXPECT_EQ(r.header("connection"), "close");
    const std::vector<TraceStep> expected = {TraceStep::RequestReceived, TraceStep::GroupRouted,
                                             TraceStep::BackendSelected, TraceStep::ErrorResponse,
                                             TraceStep::ResponseCompleted};
    EXPECT_TRUE(eventually([&] { return trace().steps_of_last_request() == expected; }));
}

// pool.fail_fast_connect: a refused connect is answered at once instead of after
// Windows' SYN retries.
TEST_F(ProxyTest, RefusedBackendConnectFailsFastOnlyWhenConfigured) {
    const std::uint16_t dead_port = start_backend();
    backend().stop();
    const auto time_502 = [&](bool fail_fast) {
        lb::Engine engine(lbtest::make_proxy_config({dead_port}, [&](nlohmann::json& j) {
            j["pool"]["fail_fast_connect"] = fail_fast;
        }));
        std::string error;
        EXPECT_TRUE(engine.start(&error)) << error;
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(fetch(engine.listen_port(), "/").status, 502);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    const double fast_ms = time_502(true);
    const double default_ms = time_502(false);
    std::printf("[ connect ] refused backend -> 502 after %.0f ms (fail-fast) vs %.0f ms (Windows retries)\n", fast_ms,
                default_ms);
    EXPECT_LT(fast_ms, 300.0);
    if (default_ms < 300.0) GTEST_SKIP() << "this system does not retry refused connects; nothing to compare";
    EXPECT_GT(default_ms, fast_ms * 2);
}

TEST_F(ProxyTest, GarbageFromBackendGives502) {
    ScriptedBackend b("this is not http\r\n\r\n");
    start_proxy({b.port()});
    EXPECT_EQ(fetch(proxy_port(), "/").status, 502);
}

TEST_F(ProxyTest, BackendCutOffMidBodyCutsOffTheClient) {
    ScriptedBackend b("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + std::string(50, 'p'));
    start_proxy({b.port()});
    const auto r = fetch(proxy_port(), "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.end, End::Complete);  // incomplete, never a corrupted "complete" response
    EXPECT_LT(r.body.size(), 100u);
}

TEST_F(ProxyTest, MalformedRequestGets400AndNeverReachesTheBackend) {
    start_proxy({start_backend()});
    TestClient c(proxy_port());
    ASSERT_TRUE(c.send("GET / HTTP/1.1\r\n\r\n"));  // no Host
    const auto r = c.read_response();
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.header("connection"), "close");
    EXPECT_EQ(c.wait_for_close(), End::Closed);
    EXPECT_EQ(backend().stats().requests, 0u);
}

TEST_F(ProxyTest, OversizedHeadersGet431) {
    start_proxy({start_backend()}, [](nlohmann::json& j) { j["limits"]["max_request_header_bytes"] = 1024; });
    const auto r = fetch(proxy_port(), "/", "X-Big: " + std::string(2000, 'v') + "\r\n");
    EXPECT_EQ(r.status, 431);
}

TEST_F(ProxyTest, ConnectionsOverTheLimitGet503) {
    start_proxy({start_backend()}, [](nlohmann::json& j) { j["limits"]["max_client_connections"] = 2; });
    TestClient a(proxy_port()), b(proxy_port());
    ASSERT_TRUE(eventually([&] { return engine().stats().connections_active == 2; }));
    TestClient c(proxy_port());
    const auto r = c.read_response();
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(c.wait_for_close(), End::Closed);
    EXPECT_EQ(engine().stats().connections_rejected, 1u);
    // The two admitted clients still work.
    ASSERT_TRUE(a.send(get_request("/")));
    EXPECT_EQ(a.read_response().status, 200);
}

TEST_F(ProxyTest, TraceShowsTheFixedStepOrder) {
    start_proxy({start_backend()});
    ASSERT_EQ(fetch(proxy_port(), "/").status, 200);
    const std::vector<TraceStep> expected = {TraceStep::RequestReceived,  TraceStep::GroupRouted,
                                             TraceStep::BackendSelected,  TraceStep::BackendConnected,
                                             TraceStep::RequestForwarded, TraceStep::ResponseReceived,
                                             TraceStep::ResponseCompleted};
    EXPECT_TRUE(eventually([&] { return trace().steps_of_last_request() == expected; }));
}

#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

#include "mock_server.h"

namespace {

// Blocking test client: send one request, read until the server closes.
std::string round_trip(std::uint16_t port, const std::string& request) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    EXPECT_NE(s, INVALID_SOCKET);
    const DWORD recv_timeout_ms = 5000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout_ms),
                 sizeof(recv_timeout_ms));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    EXPECT_EQ(::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    ::send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string response;
    char buf[1024];
    for (int n; (n = ::recv(s, buf, sizeof(buf), 0)) > 0;) response.append(buf, static_cast<size_t>(n));
    ::closesocket(s);
    return response;
}

}  // namespace

TEST(MockBackendSmoke, AnswersFixed200) {
    mock::MockServer server({"127.0.0.1", 0});
    std::string error;
    ASSERT_TRUE(server.start(&error)) << error;
    ASSERT_NE(server.port(), 0);

    const std::string response = round_trip(server.port(), "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
    EXPECT_EQ(response.rfind("HTTP/1.1 200 OK\r\n", 0), 0u) << response;
    EXPECT_TRUE(response.ends_with("\r\n\r\nok")) << response;

    server.stop();
}

#include "mock_server.h"

#include <ws2tcpip.h>

#include <string_view>
#include <utility>

namespace mock {

namespace {

constexpr std::string_view kHeaderTerminator = "\r\n\r\n";
constexpr std::string_view kFixedResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "ok";

bool send_all(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const int sent = ::send(s, data.data(), static_cast<int>(data.size()), 0);
        if (sent == SOCKET_ERROR) return false;
        data.remove_prefix(static_cast<size_t>(sent));
    }
    return true;
}

}  // namespace

MockServer::MockServer(MockOptions options) : options_(std::move(options)) {}

MockServer::~MockServer() { stop(); }

bool MockServer::start(std::string* error) {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        *error = "WSAStartup failed";
        return false;
    }
    wsa_started_ = true;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr) != 1) {
        *error = "invalid bind address: " + options_.bind_address;
        return false;
    }

    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) {
        *error = "socket() failed: " + std::to_string(::WSAGetLastError());
        return false;
    }
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(listener_, SOMAXCONN) == SOCKET_ERROR) {
        *error = "bind/listen failed: " + std::to_string(::WSAGetLastError());
        ::closesocket(listener_);
        listener_ = INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound{};
    int len = sizeof(bound);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &len);
    bound_port_ = ::ntohs(bound.sin_port);

    accept_thread_ = std::thread([this, listener = listener_] { accept_loop(listener); });
    return true;
}

void MockServer::stop() {
    if (stopping_.exchange(true)) return;

    if (listener_ != INVALID_SOCKET) {
        ::closesocket(listener_);  // unblocks accept()
        listener_ = INVALID_SOCKET;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    std::list<std::unique_ptr<Connection>> remaining;
    {
        std::lock_guard lock(connections_mutex_);
        remaining.swap(connections_);
        for (auto& c : remaining) ::shutdown(c->socket, SD_BOTH);  // unblocks recv()
    }
    for (auto& c : remaining) {
        if (c->thread.joinable()) c->thread.join();
        ::closesocket(c->socket);
    }

    if (wsa_started_) {
        ::WSACleanup();
        wsa_started_ = false;
    }
}

void MockServer::accept_loop(SOCKET listener) {
    while (!stopping_) {
        SOCKET s = ::accept(listener, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (stopping_) return;
            continue;
        }
        std::lock_guard lock(connections_mutex_);
        reap_finished_locked();
        auto conn = std::make_unique<Connection>();
        conn->socket = s;
        Connection* raw = conn.get();
        connections_.push_back(std::move(conn));
        raw->thread = std::thread([this, raw] { serve(raw); });
    }
}

void MockServer::reap_finished_locked() {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if ((*it)->done) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            ::closesocket((*it)->socket);
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void MockServer::serve(Connection* conn) {
    std::string received;
    char buf[4096];
    while (received.find(kHeaderTerminator) == std::string::npos) {
        const int n = ::recv(conn->socket, buf, sizeof(buf), 0);
        if (n <= 0) break;
        received.append(buf, static_cast<size_t>(n));
    }
    if (received.find(kHeaderTerminator) != std::string::npos) {
        send_all(conn->socket, kFixedResponse);
        ::shutdown(conn->socket, SD_SEND);
    }
    // The owner closes the socket after joining this thread (reap or stop), so
    // stop() never calls shutdown() on a handle that was already closed and reused.
    conn->done = true;
}

}  // namespace mock

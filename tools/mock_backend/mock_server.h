#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mock {

struct MockOptions {
    std::string bind_address;  // IPv4 literal, e.g. 127.0.0.1
    std::uint16_t port = 0;    // 0 = ephemeral; read the real port with MockServer::port()
};

// Configurable mock HTTP backend. Blocking Winsock, one thread per connection:
// it is a test tool, not the proxy, so plan VIII's threading rules do not apply.
// Skeleton (step 1.0): answers every request with a fixed 200 and closes.
// Step 1.3 adds keep-alive and the fault switches from plan IX.
class MockServer {
public:
    explicit MockServer(MockOptions options);
    ~MockServer();

    MockServer(const MockServer&) = delete;
    MockServer& operator=(const MockServer&) = delete;

    // Binds and starts the accept thread. On failure returns false and fills *error.
    bool start(std::string* error);
    // Closes the listener and every open connection, then joins all threads.
    void stop();

    std::uint16_t port() const noexcept { return bound_port_; }

private:
    struct Connection {
        SOCKET socket = INVALID_SOCKET;
        std::thread thread;
        std::atomic<bool> done{false};
    };

    void accept_loop(SOCKET listener);
    void serve(Connection* conn);
    void reap_finished_locked();

    MockOptions options_;
    bool wsa_started_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;

    std::mutex connections_mutex_;
    std::list<std::unique_ptr<Connection>> connections_;
};

}  // namespace mock

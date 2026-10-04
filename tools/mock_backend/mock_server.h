#pragma once

#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace mock {

// Fault switches (plan IX). All can be changed while the server runs: in-process with
// MockServer::set_faults(), or over HTTP with GET /__mock/set?key=value&...
struct MockFaults {
    std::uint32_t latency_ms = 0;  // sleep before answering
    double latency_rate = 1.0;     // share of requests that get latency_ms (the rest answer at once)
    double error_rate = 0.0;       // share of requests answered with error_status
    int error_status = 500;
    double close_rate = 0.0;    // share of requests answered by an abrupt close (RST), no bytes sent
    double partial_rate = 0.0;  // share answered with the head and half the body, then RST
    bool echo_headers = false;  // response body = the received request line and headers
    bool echo_body = false;     // response body = the received (de-chunked) request body; after the head if both
    int health_status = 200;    // status of the health path; other faults never apply to it
    std::uint32_t health_latency_ms = 0;  // sleep before answering the health path (slow probe)
    std::uint32_t body_bytes = 2;  // size of a normal 200 body ("ok", padded with '.')
};

struct MockOptions {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0;  // 0 = ephemeral; read the real port with MockServer::port()
    std::string id;          // X-Backend-Id response header; defaults to the bound port
    std::uint64_t seed = 1;  // fault decisions are a deterministic function of (seed, request number)
    std::uint32_t max_connections = 1024;  // over the cap: 503 and close
    std::string health_path = "/health";
    MockFaults faults;
};

struct MockStats {
    std::uint64_t requests = 0;         // excludes health and control requests
    std::uint64_t health_requests = 0;
    std::uint64_t connections = 0;      // accepted (including refused over the cap)
    std::uint64_t active_connections = 0;
    std::uint64_t refused_connections = 0;
    std::uint64_t aborted = 0;          // close_rate hits
    std::uint64_t partial = 0;          // partial_rate hits
    std::map<int, std::uint64_t> status_counts;  // responses sent, by status (non-control)
};

// Applies one "key=value" switch to `faults`. Returns an error message, or empty on success.
// Keys match MockFaults member names (latency_ms, error_rate, ...).
std::string apply_fault_setting(MockFaults& faults, std::string_view key, std::string_view value);
std::string faults_to_json(const MockFaults& faults);

// Configurable mock HTTP backend. Blocking Winsock, one thread per connection: it is a
// test tool, not the proxy, so plan VIII's threading rules do not apply. It has its own
// minimal HTTP parsing and never uses the engine's parser.
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
    const std::string& id() const noexcept { return id_; }

    MockFaults faults() const;
    void set_faults(const MockFaults& faults);

    MockStats stats() const;
    void reset_stats();

private:
    struct Connection {
        std::mutex socket_mutex;  // guards `socket` between the serving thread and stop()
        SOCKET socket = INVALID_SOCKET;
        std::thread thread;
        std::atomic<bool> done{false};
    };

    enum class Outcome { KeepOpen, Close, Abort };

    void accept_loop(SOCKET listener);
    void serve(Connection* conn);
    void reap_finished_locked();
    Outcome handle_request(SOCKET s, std::string_view method, std::string_view target, std::string_view raw_head,
                           std::string_view body, bool keep_alive);
    void count_status(int status);
    double draw(std::uint64_t request_number, int which) const noexcept;
    bool sleep_unless_stopping(std::uint32_t ms);
    std::string stats_json() const;

    MockOptions options_;
    std::string id_;
    bool wsa_started_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    mutable std::mutex faults_mutex_;
    MockFaults faults_;

    std::atomic<std::uint64_t> request_counter_{0};  // numbers every request for draw()
    mutable std::mutex stats_mutex_;
    MockStats stats_;

    std::mutex connections_mutex_;
    std::list<std::unique_ptr<Connection>> connections_;
};

}  // namespace mock

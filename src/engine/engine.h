#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "backend/backend_types.h"
#include "config/config.h"
#include "proxy/trace.h"

namespace lb {

std::string_view engine_version() noexcept;

struct EngineStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_active = 0;
    std::uint64_t connections_rejected = 0;  // over limits.max_client_connections
    std::uint64_t requests_completed = 0;
    std::uint64_t error_responses = 0;       // proxy-generated 4xx/5xx
    std::uint64_t backend_connections_opened = 0;
    std::uint64_t backend_connections_reused = 0;  // requests sent on a pooled keep-alive connection
    std::uint64_t stale_retries = 0;    // bodiless idempotent requests resent after a dead pooled connection
    std::uint64_t pool_rejections = 0;  // 503 because a backend's pool was full and its wait queue full or timed out
    std::uint64_t no_backend_available = 0;  // 503 because no backend in the group was eligible
    std::uint64_t client_timeouts = 0;   // plan VI: header, body, keep-alive idle, write
    std::uint64_t backend_timeouts = 0;  // plan VI: connect, response headers, idle
};

class EngineImpl;

// The proxy engine: IOCP worker pool, listener and request pipeline (plan II.2, IV.1,
// IV.18). This header is safe to include from MFC code: no Winsock or MFC types.
class Engine {
public:
    explicit Engine(std::shared_ptr<const ConfigSnapshot> config);
    ~Engine();  // stop()

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Binds the listener and starts the workers. On failure returns false and fills *error.
    bool start(std::string* error);

    // Graceful shutdown: stop accepting, close idle connections, let in-flight requests
    // finish for up to timeouts.shutdown_grace_ms, force-close the rest, then join every
    // engine thread. Afterwards the engine holds no sockets, threads or handles.
    void stop();

    std::uint16_t listen_port() const noexcept;
    std::uint32_t worker_threads() const noexcept;
    EngineStats stats() const noexcept;

    // Copied per-backend state for the dashboard (plan IV.4, IV.5).
    std::vector<BackendStats> backend_stats() const;

    // Operator/health-check entry point: unhealthy or draining backends stop receiving new
    // requests and their idle pooled connections are closed at once. False if the id is unknown.
    bool set_backend_state(std::string_view backend_id, BackendState state);

    // Optional per-step trace (plan IV.18). Set before start(); the sink must outlive the engine run.
    void set_trace_sink(TraceSink* sink) noexcept;

private:
    std::unique_ptr<EngineImpl> impl_;
};

}  // namespace lb

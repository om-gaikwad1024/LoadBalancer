#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "affinity/sticky_table.h"
#include "backend/registry.h"
#include "config/config_store.h"
#include "core/timer_service.h"
#include "log/event_log.h"
#include "metrics/metrics.h"
#include "net/iocp.h"
#include "net/winsock.h"
#include "proxy/forwarding.h"
#include "proxy/trace.h"

namespace lb::proxy {

class ClientSession;

// Hot counters: atomics, never a lock (plan V).
struct EngineCounters {
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> connections_active{0};
    std::atomic<std::uint64_t> connections_rejected{0};
    std::atomic<std::uint64_t> requests_completed{0};
    std::atomic<std::uint64_t> error_responses{0};
    std::atomic<std::uint64_t> backend_connections_opened{0};
    std::atomic<std::uint64_t> backend_connections_reused{0};  // requests sent on a pooled connection
    std::atomic<std::uint64_t> stale_retries{0};  // idempotent requests resent after a dead pooled connection
    std::atomic<std::uint64_t> stale_retry_successes{0};
    std::atomic<std::uint64_t> pool_rejections{0};  // 503: wait queue full or wait timed out
    std::atomic<std::uint64_t> no_backend_available{0};  // 503: no eligible backend in the group
    std::atomic<std::uint64_t> backends_marked_down{0};  // health checks (plan IV.10)
    std::atomic<std::uint64_t> backends_marked_up{0};
    std::atomic<std::uint64_t> client_timeouts{0};   // header, body, keep-alive idle, write
    std::atomic<std::uint64_t> backend_timeouts{0};  // connect, response, idle
    std::atomic<std::uint64_t> sticky_hits{0};          // request went to its session's backend
    std::atomic<std::uint64_t> sticky_assignments{0};   // a new session was mapped to a backend
    std::atomic<std::uint64_t> sticky_reassignments{0};  // its backend was ineligible or gone: moved
};

// Live sessions, so shutdown can reach them. Touched only when a connection opens or
// closes, never per request.
class SessionRegistry {
public:
    void add(const std::shared_ptr<ClientSession>& session);
    void remove(ClientSession* session) noexcept;
    std::vector<std::shared_ptr<ClientSession>> snapshot();
    // Returns true if the registry became empty within `timeout`.
    bool wait_empty(std::chrono::milliseconds timeout);

private:
    std::mutex mutex_;
    std::condition_variable empty_;
    std::unordered_map<ClientSession*, std::weak_ptr<ClientSession>> sessions_;
};

// Everything a session needs from the engine. Owned by the engine, outlives every session.
struct SessionContext {
    net::CompletionPort* port = nullptr;
    const net::SocketExtensions* ext = nullptr;
    ConfigStore* config = nullptr;
    backend::BackendRegistry* backends = nullptr;
    TimerService* timers = nullptr;
    EngineCounters* counters = nullptr;
    SessionRegistry* registry = nullptr;
    std::atomic<TraceSink*>* trace = nullptr;
    std::atomic<bool>* stopping = nullptr;
    std::atomic<std::uint64_t>* next_request_id = nullptr;
    const RequestIdGenerator* request_ids = nullptr;
    metrics::Metrics* metrics = nullptr;
    log::EventLog* events = nullptr;
    affinity::StickyTable* sticky = nullptr;
};

}  // namespace lb::proxy

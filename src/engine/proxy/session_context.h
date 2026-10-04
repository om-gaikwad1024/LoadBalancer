#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config/config_store.h"
#include "net/iocp.h"
#include "net/winsock.h"
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
    EngineCounters* counters = nullptr;
    SessionRegistry* registry = nullptr;
    std::atomic<TraceSink*>* trace = nullptr;
    std::atomic<bool>* stopping = nullptr;
    std::atomic<std::uint64_t>* next_request_id = nullptr;
};

}  // namespace lb::proxy

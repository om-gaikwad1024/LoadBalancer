#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "backend/backend_types.h"
#include "backend/connection_pool.h"
#include "config/config.h"

namespace lb::backend {

PoolLimits pool_limits(const PoolConfig& config) noexcept;

// Live state of one backend (plan IV.4). Requests hold a shared_ptr, so an entry stays
// valid for in-flight requests even after a reload removes it from the registry.
class BackendRuntime {
public:
    BackendRuntime(const BackendConfig& config, std::string group, const PoolLimits& limits, SocketOps& ops);

    const std::string id;
    const std::string group;
    const std::string endpoint;  // address:port, also the fallback Host header
    const sockaddr_in address;
    std::atomic<std::uint32_t> weight;
    std::atomic<BackendState> state{BackendState::Healthy};

    // Hot counters: atomics only (plan V).
    std::atomic<std::uint64_t> in_flight{0};
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> successes{0};
    std::atomic<std::uint64_t> failures{0};

    ConnectionPool pool;

    // Healthy and not draining (circuit-open joins this check in phase 3).
    bool eligible() const noexcept { return state.load(std::memory_order_acquire) == BackendState::Healthy; }

    BackendStats stats() const;
};

// Single source of truth for which backends exist and their state (plan IV.4). The
// structure is guarded by an SRWLOCK: many concurrent readers (every request), rare
// writers (health checks, drain, reload).
class BackendRegistry {
public:
    explicit BackendRegistry(SocketOps& ops = system_socket_ops()) noexcept : ops_(ops) {}
    BackendRegistry(const BackendRegistry&) = delete;
    BackendRegistry& operator=(const BackendRegistry&) = delete;

    // Initial load from a validated snapshot. (Hot reload reconciles by id in step 2.1.)
    void load(const ConfigSnapshot& config);

    std::shared_ptr<BackendRuntime> find(std::string_view id) const;
    std::vector<std::shared_ptr<BackendRuntime>> group(std::string_view name) const;
    std::vector<std::shared_ptr<BackendRuntime>> all() const;

    // Unhealthy or draining: idle pooled connections are closed at once (plan IV.5).
    bool set_state(std::string_view id, BackendState state);

    // Maintenance thread: closes pooled connections past their idle timeout.
    void sweep(TimePoint now);
    void close_all_idle();

private:
    SocketOps& ops_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<std::shared_ptr<BackendRuntime>> backends_;
};

}  // namespace lb::backend

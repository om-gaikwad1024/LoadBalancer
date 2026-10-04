#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <string>

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

}  // namespace lb::backend

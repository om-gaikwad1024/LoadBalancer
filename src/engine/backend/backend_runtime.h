#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <mutex>
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

    // Series number in the latency metrics (1-based; 0 is the whole proxy). Set at load.
    std::size_t metrics_series = 0;

    // Healthy and not draining (circuit-open joins this check in phase 3).
    bool eligible() const noexcept { return state.load(std::memory_order_acquire) == BackendState::Healthy; }

    // Health checks only move a backend between healthy and unhealthy: a draining backend
    // is being removed on purpose and stays draining (plan VIII). True if the state changed.
    // Marking down closes the idle pooled connections at once (plan IV.5).
    bool mark_unhealthy() noexcept;
    bool mark_healthy() noexcept;

    // Active health-check results (written by the health thread, read for the dashboard).
    std::atomic<std::uint64_t> probes{0};
    std::atomic<std::uint32_t> probe_failures_in_a_row{0};
    std::atomic<std::uint32_t> probe_successes_in_a_row{0};
    void set_last_probe_error(std::string error);
    std::string last_probe_error() const;

    BackendStats stats() const;

private:
    mutable std::mutex probe_error_mutex_;
    std::string last_probe_error_;
};

}  // namespace lb::backend

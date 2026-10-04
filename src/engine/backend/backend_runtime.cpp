#include "backend/backend_runtime.h"

#include <ws2tcpip.h>

#include <chrono>

namespace lb {

std::string_view to_string(BackendState state) noexcept {
    switch (state) {
        case BackendState::Healthy: return "healthy";
        case BackendState::Unhealthy: return "unhealthy";
        case BackendState::Draining: return "draining";
    }
    return "unknown";
}

}  // namespace lb

namespace lb::backend {

namespace {

sockaddr_in to_sockaddr(const BackendConfig& c) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = ::htons(c.port);
    ::inet_pton(AF_INET, c.address.c_str(), &a.sin_addr);  // validated as an IPv4 literal by the loader
    return a;
}

}  // namespace

PoolLimits pool_limits(const PoolConfig& config) noexcept {
    PoolLimits l;
    l.max_connections = config.max_connections_per_backend;
    l.max_idle = config.max_idle_per_backend;
    l.max_waiters = config.max_waiters_per_backend;
    l.idle_timeout = std::chrono::milliseconds(config.idle_timeout_ms);
    return l;
}

BackendRuntime::BackendRuntime(const BackendConfig& config, std::string group_name, const PoolLimits& limits,
                               SocketOps& ops)
    : id(config.id),
      group(std::move(group_name)),
      endpoint(config.address + ":" + std::to_string(config.port)),
      address(to_sockaddr(config)),
      weight(config.weight),
      pool(limits, ops) {}

bool BackendRuntime::mark_unhealthy() noexcept {
    BackendState expected = BackendState::Healthy;
    if (!state.compare_exchange_strong(expected, BackendState::Unhealthy, std::memory_order_acq_rel)) return false;
    pool.close_idle();
    return true;
}

bool BackendRuntime::mark_healthy() noexcept {
    BackendState expected = BackendState::Unhealthy;
    return state.compare_exchange_strong(expected, BackendState::Healthy, std::memory_order_acq_rel);
}

void BackendRuntime::set_last_probe_error(std::string error) {
    std::lock_guard lock(probe_error_mutex_);
    last_probe_error_ = std::move(error);
}

std::string BackendRuntime::last_probe_error() const {
    std::lock_guard lock(probe_error_mutex_);
    return last_probe_error_;
}

BackendStats BackendRuntime::stats() const {
    BackendStats s;
    s.id = id;
    s.group = group;
    s.endpoint = endpoint;
    s.weight = weight.load();
    s.state = state.load();
    s.in_flight = in_flight.load();
    s.requests = requests.load();
    s.successes = successes.load();
    s.failures = failures.load();
    const PoolStats p = pool.stats();
    s.open_connections = p.open;
    s.idle_connections = p.idle;
    s.waiting_requests = p.waiting;
    s.connections_opened = p.opened;
    s.connections_reused = p.reused;
    s.stale_discarded = p.stale_discarded;
    s.health_probes = probes.load();
    s.probe_failures_in_a_row = probe_failures_in_a_row.load();
    s.probe_successes_in_a_row = probe_successes_in_a_row.load();
    s.last_probe_error = last_probe_error();
    return s;
}

}  // namespace lb::backend

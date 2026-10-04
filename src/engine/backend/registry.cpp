#include "backend/registry.h"

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

class SharedLock {
public:
    explicit SharedLock(SRWLOCK& l) noexcept : l_(l) { ::AcquireSRWLockShared(&l_); }
    ~SharedLock() { ::ReleaseSRWLockShared(&l_); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    SRWLOCK& l_;
};

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK& l) noexcept : l_(l) { ::AcquireSRWLockExclusive(&l_); }
    ~ExclusiveLock() { ::ReleaseSRWLockExclusive(&l_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

private:
    SRWLOCK& l_;
};

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
    return s;
}

void BackendRegistry::load(const ConfigSnapshot& config) {
    const PoolLimits limits = pool_limits(config.pool);
    std::vector<std::shared_ptr<BackendRuntime>> fresh;
    for (const auto& g : config.groups) {
        for (const auto& b : g.backends) fresh.push_back(std::make_shared<BackendRuntime>(b, g.name, limits, ops_));
    }
    ExclusiveLock lock(lock_);
    backends_ = std::move(fresh);
}

std::shared_ptr<BackendRuntime> BackendRegistry::find(std::string_view id) const {
    SharedLock lock(lock_);
    for (const auto& b : backends_) {
        if (b->id == id) return b;
    }
    return nullptr;
}

std::vector<std::shared_ptr<BackendRuntime>> BackendRegistry::group(std::string_view name) const {
    std::vector<std::shared_ptr<BackendRuntime>> out;
    SharedLock lock(lock_);
    for (const auto& b : backends_) {
        if (b->group == name) out.push_back(b);
    }
    return out;
}

std::vector<std::shared_ptr<BackendRuntime>> BackendRegistry::all() const {
    SharedLock lock(lock_);
    return backends_;
}

bool BackendRegistry::set_state(std::string_view id, BackendState state) {
    const auto b = find(id);
    if (!b) return false;
    b->state.store(state, std::memory_order_release);
    if (state != BackendState::Healthy) b->pool.close_idle();
    return true;
}

void BackendRegistry::sweep(TimePoint now) {
    for (const auto& b : all()) b->pool.close_expired(now);
}

void BackendRegistry::close_all_idle() {
    for (const auto& b : all()) b->pool.close_idle();
}

}  // namespace lb::backend

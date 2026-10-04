#include "backend/registry.h"

namespace lb::backend {

namespace {

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

void BackendRegistry::load(const ConfigSnapshot& config) {
    const PoolLimits limits = pool_limits(config.pool);
    std::vector<std::shared_ptr<BackendRuntime>> backends;
    std::vector<std::shared_ptr<balance::GroupBalancer>> groups;
    for (const auto& g : config.groups) {
        std::vector<std::shared_ptr<BackendRuntime>> members;
        for (const auto& b : g.backends) {
            members.push_back(std::make_shared<BackendRuntime>(b, g.name, limits, ops_));
            members.back()->metrics_series = backends.size() + 1;
            backends.push_back(members.back());
        }
        groups.push_back(std::make_shared<balance::GroupBalancer>(g.name, g.strategy, std::move(members)));
    }
    ExclusiveLock lock(lock_);
    backends_ = std::move(backends);
    groups_ = std::move(groups);
}

std::shared_ptr<BackendRuntime> BackendRegistry::find(std::string_view id) const {
    SharedLock lock(lock_);
    for (const auto& b : backends_) {
        if (b->id == id) return b;
    }
    return nullptr;
}

std::shared_ptr<balance::GroupBalancer> BackendRegistry::find_group(std::string_view name) const {
    SharedLock lock(lock_);
    for (const auto& g : groups_) {
        if (g->name == name) return g;
    }
    return nullptr;
}

std::vector<std::shared_ptr<BackendRuntime>> BackendRegistry::group(std::string_view name) const {
    const auto g = find_group(name);
    return g ? g->members : std::vector<std::shared_ptr<BackendRuntime>>{};
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

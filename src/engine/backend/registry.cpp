#include "backend/registry.h"

#include <algorithm>

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

bool same_endpoint(const BackendRuntime& rt, const BackendConfig& b) {
    return rt.endpoint == b.address + ":" + std::to_string(b.port);
}

}  // namespace

std::shared_ptr<balance::GroupBalancer> Topology::find_group(std::string_view name) const {
    for (const auto& g : groups) {
        if (g->name == name) return g;
    }
    return nullptr;
}

std::shared_ptr<BackendRuntime> Topology::find(std::string_view id) const {
    for (const auto& b : backends) {
        if (b->id == id) return b;
    }
    return nullptr;
}

void BackendRegistry::load(std::shared_ptr<const ConfigSnapshot> config, const SeriesFor& series_for) {
    const PoolLimits limits = pool_limits(config->pool);
    auto next = std::make_shared<Topology>();
    for (const auto& g : config->groups) {
        std::vector<std::shared_ptr<BackendRuntime>> members;
        for (const auto& b : g.backends) {
            members.push_back(std::make_shared<BackendRuntime>(b, g.name, limits, ops_));
            // "drain": "start" on a new backend: it never takes a request (the engine tracks the drain).
            if (b.drain == DrainDirective::Start) members.back()->state.store(BackendState::Draining);
            members.back()->metrics_series = series_for ? series_for(b.id) : next->backends.size() + 1;
            next->backends.push_back(members.back());
        }
        next->groups.push_back(std::make_shared<balance::GroupBalancer>(g.name, g.strategy, std::move(members)));
    }
    next->config = std::move(config);
    publish(std::move(next));
}

ReconcileResult BackendRegistry::reconcile(std::shared_ptr<const ConfigSnapshot> config, const SeriesFor& series_for) {
    const auto active = topology();
    const PoolLimits limits = pool_limits(config->pool);
    ReconcileResult result;
    auto next = std::make_shared<Topology>();
    std::vector<std::shared_ptr<BackendRuntime>> kept;

    for (const auto& g : config->groups) {
        std::vector<std::shared_ptr<BackendRuntime>> members;
        for (const auto& b : g.backends) {
            std::shared_ptr<BackendRuntime> rt;
            const auto old = active->find(b.id);
            if (old && old->group == g.name && same_endpoint(*old, b)) {
                rt = old;  // same backend: live state carries over
                if (rt->weight.exchange(b.weight) != b.weight) result.reweighted.push_back(b.id);
                rt->pool.set_limits(limits);
                kept.push_back(rt);
            } else {
                rt = std::make_shared<BackendRuntime>(b, g.name, limits, ops_);
                if (b.drain == DrainDirective::Start) rt->state.store(BackendState::Draining);
                rt->metrics_series = series_for ? series_for(b.id) : 0;
                // A draining or drained backend stays so when it moves (plan IV.12).
                if (old) {
                    const BackendState s = old->state.load();
                    if (s == BackendState::Draining || s == BackendState::Drained) rt->state.store(s);
                }
                result.added.push_back(b.id);
            }
            members.push_back(rt);
            next->backends.push_back(rt);
        }
        next->groups.push_back(std::make_shared<balance::GroupBalancer>(g.name, g.strategy, std::move(members)));
    }
    next->config = std::move(config);

    std::vector<std::shared_ptr<BackendRuntime>> retired;
    for (const auto& b : active->backends) {
        if (std::find(kept.begin(), kept.end(), b) == kept.end()) {
            retired.push_back(b);
            result.removed.push_back(b->id);
        }
    }

    publish(std::move(next));

    // New requests can no longer pick a retired runtime. Requests that captured the old
    // topology finish on it; with max_idle 0 their connections close on release.
    for (const auto& b : retired) {
        PoolLimits drain = limits;
        drain.max_idle = 0;
        b->pool.set_limits(drain);
    }
    return result;
}

void BackendRegistry::publish(std::shared_ptr<const Topology> next) {
    std::shared_ptr<const Topology> old;
    {
        ExclusiveLock lock(lock_);
        old = std::exchange(topology_, std::move(next));
    }
    // `old` is released here, outside the lock (it may destroy pools and close sockets).
}

std::shared_ptr<const Topology> BackendRegistry::topology() const {
    SharedLock lock(lock_);
    return topology_;
}

std::shared_ptr<BackendRuntime> BackendRegistry::find(std::string_view id) const { return topology()->find(id); }

std::shared_ptr<balance::GroupBalancer> BackendRegistry::find_group(std::string_view name) const {
    return topology()->find_group(name);
}

std::vector<std::shared_ptr<BackendRuntime>> BackendRegistry::group(std::string_view name) const {
    const auto g = find_group(name);
    return g ? g->members : std::vector<std::shared_ptr<BackendRuntime>>{};
}

std::vector<std::shared_ptr<BackendRuntime>> BackendRegistry::all() const { return topology()->backends; }

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

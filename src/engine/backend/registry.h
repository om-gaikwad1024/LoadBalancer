#pragma once

#include <winsock2.h>
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "backend/backend_runtime.h"
#include "balance/group_balancer.h"
#include "config/config.h"

namespace lb::backend {

// One consistent view of the backends: the config they came from, every backend in config
// order, and one balancer per group. Immutable once published; a request captures it once
// at its start (plan II.7, IV.14), so it never sees a half-applied reload.
struct Topology {
    std::shared_ptr<const ConfigSnapshot> config;
    std::vector<std::shared_ptr<BackendRuntime>> backends;
    std::vector<std::shared_ptr<balance::GroupBalancer>> groups;

    std::shared_ptr<balance::GroupBalancer> find_group(std::string_view name) const;
    std::shared_ptr<BackendRuntime> find(std::string_view id) const;
};

// What a reload changed, for the event log.
struct ReconcileResult {
    std::vector<std::string> added;     // new ids, or ids whose address or group changed
    std::vector<std::string> removed;   // ids no longer configured (or replaced, see added)
    std::vector<std::string> reweighted;
};

// Single source of truth for which backends exist and their state (plan IV.4). The
// topology pointer is guarded by an SRWLOCK: many concurrent readers (every request),
// rare writers (reload). Backend state itself lives in atomics on each runtime.
class BackendRegistry {
public:
    // Metrics series for a backend id (1-based; 0 = none). Without one, load() numbers
    // backends by position and reconcile() leaves new ones at 0.
    using SeriesFor = std::function<std::size_t(const std::string& id)>;

    explicit BackendRegistry(SocketOps& ops = system_socket_ops()) noexcept : ops_(ops) {}
    BackendRegistry(const BackendRegistry&) = delete;
    BackendRegistry& operator=(const BackendRegistry&) = delete;

    // Initial load from a validated snapshot.
    void load(std::shared_ptr<const ConfigSnapshot> config, const SeriesFor& series_for = {});
    void load(const ConfigSnapshot& config) { load(std::make_shared<const ConfigSnapshot>(config)); }

    // Hot reload (plan IV.14), matched by backend id. A backend with the same id, address
    // and group keeps its runtime: health, draining, counters and pooled connections carry
    // over, so a reload never un-drains (plan IV.12); its weight and pool limits are
    // updated. A changed address or group gets a fresh runtime that inherits draining.
    // Removed runtimes stay valid for requests still using them; their idle connections
    // are closed and later ones are closed on release.
    ReconcileResult reconcile(std::shared_ptr<const ConfigSnapshot> config, const SeriesFor& series_for = {});

    std::shared_ptr<const Topology> topology() const;
    std::shared_ptr<BackendRuntime> find(std::string_view id) const;
    std::shared_ptr<balance::GroupBalancer> find_group(std::string_view name) const;
    std::vector<std::shared_ptr<BackendRuntime>> group(std::string_view name) const;
    std::vector<std::shared_ptr<BackendRuntime>> all() const;

    // Unhealthy or draining: idle pooled connections are closed at once (plan IV.5).
    bool set_state(std::string_view id, BackendState state);

    // Maintenance thread: closes pooled connections past their idle timeout.
    void sweep(TimePoint now);
    void close_all_idle();

private:
    void publish(std::shared_ptr<const Topology> next);

    SocketOps& ops_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::shared_ptr<const Topology> topology_ = std::make_shared<const Topology>();
};

}  // namespace lb::backend

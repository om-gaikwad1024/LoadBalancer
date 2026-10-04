#pragma once

#include <winsock2.h>
#include <windows.h>

#include <memory>
#include <string_view>
#include <vector>

#include "backend/backend_runtime.h"
#include "balance/group_balancer.h"
#include "config/config.h"

namespace lb::backend {

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
    std::shared_ptr<balance::GroupBalancer> find_group(std::string_view name) const;
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
    std::vector<std::shared_ptr<balance::GroupBalancer>> groups_;
};

}  // namespace lb::backend

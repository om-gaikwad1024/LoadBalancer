#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend/backend_runtime.h"
#include "config/config.h"

namespace lb::balance {

// Picks the backend for a request within one group when there is no valid sticky backend
// (plan IV.7). Members are fixed when the group is built (a reload builds a new group), so
// picking takes no lock: only atomics are read and one rotation counter is advanced.
class GroupBalancer {
public:
    GroupBalancer(std::string name, Strategy strategy, std::vector<std::shared_ptr<backend::BackendRuntime>> members);

    const std::string name;
    const Strategy strategy;
    const std::vector<std::shared_ptr<backend::BackendRuntime>> members;

    // An eligible backend (healthy, not draining), or nullptr if none is: the caller
    // answers 503 and logs it loudly.
    std::shared_ptr<backend::BackendRuntime> pick() noexcept;

private:
    std::shared_ptr<backend::BackendRuntime> pick_round_robin() noexcept;
    std::shared_ptr<backend::BackendRuntime> pick_least_connections() noexcept;

    std::atomic<std::uint64_t> rotation_{0};
};

}  // namespace lb::balance

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "backend/backend_runtime.h"
#include "config/config.h"
#include "core/clock.h"

namespace lb::balance {

// What a strategy may need besides the backends' own state.
struct PickContext {
    TimePoint now{};                   // least response time: for the age of each average
    Duration response_time_expiry{};   // least response time: older averages are ignored
    std::uint64_t client_hash = 0;     // IP hash: hash_key(client_identity(...))
};

// 64-bit hash of a key (the client address for IP hash): FNV-1a, then a full mix.
std::uint64_t hash_key(std::string_view key) noexcept;

// Picks the backend for a request within one group when there is no valid sticky backend
// (plan IV.7). Members and weights are fixed when the group is built (a reload builds a
// new group), so picking takes no lock: only atomics are read and a rotation counter is
// advanced. Only eligible backends (healthy, not draining) are ever chosen.
class GroupBalancer {
public:
    GroupBalancer(std::string name, Strategy strategy, std::vector<std::shared_ptr<backend::BackendRuntime>> members);

    const std::string name;
    const Strategy strategy;
    const std::vector<std::shared_ptr<backend::BackendRuntime>> members;

    // An eligible backend, or nullptr if none is: the caller answers 503 and logs it loudly.
    std::shared_ptr<backend::BackendRuntime> pick(const PickContext& context = {}) noexcept;

    // Weighted round robin's cycle: member indexes, each weight/gcd times, interleaved.
    const std::vector<std::uint32_t>& weighted_schedule() const noexcept { return schedule_; }

private:
    std::shared_ptr<backend::BackendRuntime> pick_round_robin() noexcept;
    std::shared_ptr<backend::BackendRuntime> pick_least_connections() noexcept;
    std::shared_ptr<backend::BackendRuntime> pick_weighted_round_robin() noexcept;
    std::shared_ptr<backend::BackendRuntime> pick_least_response_time(const PickContext& context) noexcept;
    std::shared_ptr<backend::BackendRuntime> pick_ip_hash(const PickContext& context) noexcept;

    std::vector<std::uint32_t> weights_;      // fixed at construction (members' weights then)
    std::vector<std::uint64_t> member_hash_;  // hash_key(id): IP hash
    std::vector<std::uint32_t> schedule_;     // weighted round robin
    std::atomic<std::uint64_t> rotation_{0};
    std::atomic<std::uint64_t> fallback_rotation_{0};  // weighted round robin with excluded backends
};

}  // namespace lb::balance

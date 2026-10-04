#include "balance/group_balancer.h"

#include <limits>

#include "core/debug_assert.h"

namespace lb::balance {

GroupBalancer::GroupBalancer(std::string group_name, Strategy group_strategy,
                             std::vector<std::shared_ptr<backend::BackendRuntime>> group_members)
    : name(std::move(group_name)), strategy(group_strategy), members(std::move(group_members)) {}

std::shared_ptr<backend::BackendRuntime> GroupBalancer::pick() noexcept {
    std::shared_ptr<backend::BackendRuntime> chosen;
    switch (strategy) {
        case Strategy::RoundRobin: chosen = pick_round_robin(); break;
        case Strategy::LeastConnections: chosen = pick_least_connections(); break;
    }
    LB_DEBUG_ASSERT(!chosen || chosen->eligible(), "the balancer may only choose an eligible backend (IV.4, IV.7)");
    return chosen;
}

// Rotates over the eligible members only, so an excluded backend's share is spread evenly
// instead of falling entirely on its neighbor.
std::shared_ptr<backend::BackendRuntime> GroupBalancer::pick_round_robin() noexcept {
    std::size_t eligible = 0;
    for (const auto& b : members) eligible += b->eligible() ? 1 : 0;
    if (eligible == 0) return nullptr;

    std::size_t target = static_cast<std::size_t>(rotation_.fetch_add(1, std::memory_order_relaxed) % eligible);
    for (const auto& b : members) {
        if (!b->eligible()) continue;
        if (target-- == 0) return b;
    }
    // A backend became ineligible between the two passes: take any eligible one.
    for (const auto& b : members) {
        if (b->eligible()) return b;
    }
    return nullptr;
}

// Fewest in-flight requests. The scan starts at a rotating offset, so ties go to a
// different backend each time rather than always the first.
std::shared_ptr<backend::BackendRuntime> GroupBalancer::pick_least_connections() noexcept {
    const std::size_t n = members.size();
    if (n == 0) return nullptr;
    const std::size_t start = static_cast<std::size_t>(rotation_.fetch_add(1, std::memory_order_relaxed) % n);
    std::shared_ptr<backend::BackendRuntime> best;
    std::uint64_t best_in_flight = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& b = members[(start + i) % n];
        if (!b->eligible()) continue;
        const std::uint64_t in_flight = b->in_flight.load(std::memory_order_relaxed);
        if (in_flight < best_in_flight) {
            best = b;
            best_in_flight = in_flight;
        }
    }
    return best;
}

}  // namespace lb::balance

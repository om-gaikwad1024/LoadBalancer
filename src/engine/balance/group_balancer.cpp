#include "balance/group_balancer.h"

#include <cmath>
#include <limits>
#include <numeric>
#include <queue>

#include "core/debug_assert.h"

namespace lb::balance {

namespace {

std::uint64_t mix(std::uint64_t x) noexcept {  // splitmix64 finalizer
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

// Smooth weighted cycle by stride scheduling: member i's k-th turn is due at (k + 1/2)/w_i,
// and turns are taken in due order. Each member gets exactly w_i turns per cycle, spread
// evenly instead of in runs (weights 3:1 give A A B A, not A A A B).
std::vector<std::uint32_t> build_schedule(const std::vector<std::uint32_t>& weights) {
    std::uint32_t g = 0;
    for (auto w : weights) g = std::gcd(g, w);
    if (g == 0) return {};
    struct Turn {
        std::uint64_t k;  // turns taken so far
        std::uint32_t w;
        std::uint32_t index;
    };
    // Due (2k+1)/w, compared by cross-multiplying; ties go to the lower index.
    const auto later = [](const Turn& a, const Turn& b) {
        const std::uint64_t da = (2 * a.k + 1) * b.w;
        const std::uint64_t db = (2 * b.k + 1) * a.w;
        return da != db ? da > db : a.index > b.index;
    };
    std::priority_queue<Turn, std::vector<Turn>, decltype(later)> due(later);
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < weights.size(); ++i) {
        const std::uint32_t w = weights[i] / g;
        total += w;
        due.push({0, w, i});
    }
    std::vector<std::uint32_t> schedule;
    schedule.reserve(static_cast<std::size_t>(total));
    while (schedule.size() < total) {
        Turn t = due.top();
        due.pop();
        schedule.push_back(t.index);
        ++t.k;
        due.push(t);
    }
    return schedule;
}

}  // namespace

std::uint64_t hash_key(std::string_view key) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const char c : key) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ull;
    }
    return mix(h);
}

GroupBalancer::GroupBalancer(std::string group_name, Strategy group_strategy,
                             std::vector<std::shared_ptr<backend::BackendRuntime>> group_members)
    : name(std::move(group_name)), strategy(group_strategy), members(std::move(group_members)) {
    for (const auto& b : members) {
        weights_.push_back(b->weight.load());
        member_hash_.push_back(hash_key(b->id));
    }
    if (strategy == Strategy::WeightedRoundRobin) schedule_ = build_schedule(weights_);
}

std::shared_ptr<backend::BackendRuntime> GroupBalancer::pick(const PickContext& context) noexcept {
    std::shared_ptr<backend::BackendRuntime> chosen;
    switch (strategy) {
        case Strategy::RoundRobin: chosen = pick_round_robin(); break;
        case Strategy::LeastConnections: chosen = pick_least_connections(); break;
        case Strategy::WeightedRoundRobin: chosen = pick_weighted_round_robin(); break;
        case Strategy::LeastResponseTime: chosen = pick_least_response_time(context); break;
        case Strategy::IpHash: chosen = pick_ip_hash(context); break;
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

// Next turn of the precomputed cycle. A turn that falls on an excluded backend goes to the
// eligible ones in proportion to their weights (a weighted rotation of its own), so the
// shares among the remaining backends stay weight-proportional.
std::shared_ptr<backend::BackendRuntime> GroupBalancer::pick_weighted_round_robin() noexcept {
    if (schedule_.empty()) return nullptr;
    const auto turn = rotation_.fetch_add(1, std::memory_order_relaxed);
    const auto& scheduled = members[schedule_[static_cast<std::size_t>(turn % schedule_.size())]];
    if (scheduled->eligible()) return scheduled;

    std::uint64_t total = 0;
    for (std::size_t i = 0; i < members.size(); ++i) total += members[i]->eligible() ? weights_[i] : 0;
    if (total == 0) return nullptr;
    std::uint64_t target = fallback_rotation_.fetch_add(1, std::memory_order_relaxed) % total;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (!members[i]->eligible()) continue;
        if (target < weights_[i]) return members[i];
        target -= weights_[i];
    }
    for (const auto& b : members) {  // eligibility changed during the scan
        if (b->eligible()) return b;
    }
    return nullptr;
}

// Lowest expected wait: response-time average × (in-flight + 1), so a fast backend is
// preferred until its queue makes it slower than the others. A backend without a recent
// average (new, or idle past the expiry) is scored with the best known average, so it
// gets traffic and is measured; with no averages at all this is least connections. Ties
// rotate like least connections.
std::shared_ptr<backend::BackendRuntime> GroupBalancer::pick_least_response_time(const PickContext& context) noexcept {
    const std::size_t n = members.size();
    if (n == 0) return nullptr;
    double best_known = std::numeric_limits<double>::infinity();
    for (const auto& b : members) {
        if (!b->eligible()) continue;
        if (const auto rt = b->response_time_us(context.now, context.response_time_expiry)) {
            best_known = std::min(best_known, *rt);
        }
    }
    if (best_known == std::numeric_limits<double>::infinity()) best_known = 0;

    const std::size_t start = static_cast<std::size_t>(rotation_.fetch_add(1, std::memory_order_relaxed) % n);
    std::shared_ptr<backend::BackendRuntime> best;
    double best_score = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& b = members[(start + i) % n];
        if (!b->eligible()) continue;
        const double rt = b->response_time_us(context.now, context.response_time_expiry).value_or(best_known);
        // +1 µs keeps a sub-microsecond average from zeroing the in-flight term.
        const double score = (rt + 1.0) * static_cast<double>(b->in_flight.load(std::memory_order_relaxed) + 1);
        if (score < best_score) {
            best = b;
            best_score = score;
        }
    }
    return best;
}

// Weighted rendezvous hashing: every eligible backend draws a score from hash(client,
// backend) and the lowest -ln(u)/weight wins. A client keeps its backend while that
// backend is eligible; when one is excluded or added, only the clients it gains or loses
// move. Shares follow the weights.
std::shared_ptr<backend::BackendRuntime> GroupBalancer::pick_ip_hash(const PickContext& context) noexcept {
    std::shared_ptr<backend::BackendRuntime> best;
    double best_score = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (!members[i]->eligible()) continue;
        const std::uint64_t h = mix(context.client_hash ^ member_hash_[i]);
        const double u = (static_cast<double>(h >> 11) + 0.5) * 0x1.0p-53;  // uniform in (0, 1)
        const double score = -std::log(u) / static_cast<double>(weights_[i]);
        if (score < best_score) {
            best = members[i];
            best_score = score;
        }
    }
    return best;
}

}  // namespace lb::balance

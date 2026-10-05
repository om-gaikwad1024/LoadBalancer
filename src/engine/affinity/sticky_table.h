#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config/config.h"
#include "core/clock.h"

namespace lb::affinity {

// Session affinity map (plan IV.9): (group, session key) -> backend id, with a sliding TTL.
// Sharded locks (plan V): a request locks one shard briefly; the maintenance thread sweeps
// expired entries shard by shard. In memory only: a restart reassigns sticky clients.
class StickyTable {
public:
    explicit StickyTable(const StickyTableConfig& config);
    StickyTable(const StickyTable&) = delete;
    StickyTable& operator=(const StickyTable&) = delete;

    // The backend id mapped to (group, key), with its TTL restarted; nothing if there is
    // no mapping or it expired.
    std::optional<std::string> find(std::string_view group, std::string_view key, TimePoint now, Duration ttl);

    // Maps (group, key) to `backend` (replacing any previous mapping). False if the key is
    // new and its shard is full: the session then simply isn't sticky.
    bool assign(std::string_view group, std::string_view key, std::string_view backend, TimePoint now, Duration ttl);

    // Maintenance thread: removes expired mappings. Returns how many.
    std::size_t sweep(TimePoint now);

    std::size_t size() const;
    std::uint64_t not_stored() const noexcept { return not_stored_.load(std::memory_order_relaxed); }

    // A new proxy session key: 32 hex characters from 128 random bits.
    static std::string new_session_key();
    static bool is_session_key(std::string_view key) noexcept;

private:
    struct Entry {
        std::string backend;
        TimePoint expires;
    };
    struct Shard {
        mutable std::mutex mutex;
        std::unordered_map<std::string, Entry> map;
    };

    Shard& shard_for(const std::string& composite) const noexcept;

    std::vector<std::unique_ptr<Shard>> shards_;
    std::size_t per_shard_capacity_;
    std::atomic<std::uint64_t> not_stored_{0};
};

}  // namespace lb::affinity

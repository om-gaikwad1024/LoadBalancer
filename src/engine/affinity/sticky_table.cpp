#include "affinity/sticky_table.h"

#include <random>

namespace lb::affinity {

namespace {

// Group names cannot contain '\n' (config name rules), so the composite key is unambiguous.
std::string composite_key(std::string_view group, std::string_view key) {
    std::string k;
    k.reserve(group.size() + 1 + key.size());
    k.append(group).push_back('\n');
    k.append(key);
    return k;
}

}  // namespace

StickyTable::StickyTable(const StickyTableConfig& config)
    : per_shard_capacity_(std::max<std::size_t>(1, config.max_entries / std::max<std::uint32_t>(1, config.shards))) {
    const std::uint32_t n = std::max<std::uint32_t>(1, config.shards);
    for (std::uint32_t i = 0; i < n; ++i) shards_.push_back(std::make_unique<Shard>());
}

StickyTable::Shard& StickyTable::shard_for(const std::string& composite) const noexcept {
    return *shards_[std::hash<std::string>{}(composite) % shards_.size()];
}

std::optional<std::string> StickyTable::find(std::string_view group, std::string_view key, TimePoint now,
                                             Duration ttl) {
    const std::string k = composite_key(group, key);
    Shard& s = shard_for(k);
    std::lock_guard lock(s.mutex);
    const auto it = s.map.find(k);
    if (it == s.map.end()) return std::nullopt;
    if (it->second.expires <= now) {
        s.map.erase(it);
        return std::nullopt;
    }
    it->second.expires = now + ttl;  // sliding: every use restarts the TTL
    return it->second.backend;
}

bool StickyTable::assign(std::string_view group, std::string_view key, std::string_view backend, TimePoint now,
                         Duration ttl) {
    std::string k = composite_key(group, key);
    Shard& s = shard_for(k);
    std::lock_guard lock(s.mutex);
    const auto it = s.map.find(k);
    if (it != s.map.end()) {
        it->second.backend.assign(backend);
        it->second.expires = now + ttl;
        return true;
    }
    if (s.map.size() >= per_shard_capacity_) {
        not_stored_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    s.map.emplace(std::move(k), Entry{std::string(backend), now + ttl});
    return true;
}

std::size_t StickyTable::sweep(TimePoint now) {
    std::size_t removed = 0;
    for (const auto& s : shards_) {
        std::lock_guard lock(s->mutex);
        removed += std::erase_if(s->map, [now](const auto& item) { return item.second.expires <= now; });
    }
    return removed;
}

std::size_t StickyTable::size() const {
    std::size_t n = 0;
    for (const auto& s : shards_) {
        std::lock_guard lock(s->mutex);
        n += s->map.size();
    }
    return n;
}

std::string StickyTable::new_session_key() {
    // MSVC's random_device draws from the OS cryptographic generator (rand_s), so keys
    // cannot be predicted from earlier ones.
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string key;
    key.reserve(32);
    for (int word = 0; word < 4; ++word) {
        std::uint32_t r = rd();
        for (int i = 0; i < 8; ++i, r >>= 4) key.push_back(kHex[r & 0xF]);
    }
    return key;
}

bool StickyTable::is_session_key(std::string_view key) noexcept {
    if (key.size() != 32) return false;
    for (const char c : key) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace lb::affinity

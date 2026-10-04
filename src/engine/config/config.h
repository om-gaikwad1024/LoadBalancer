#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lb {

// Immutable configuration snapshot (plan II.7, IV.14). Built once by the loader,
// never modified afterwards, and shared as std::shared_ptr<const ConfigSnapshot>.
// Field meanings and valid ranges: docs/config-reference.md.

struct ListenConfig {
    std::string address;  // IPv4 literal
    std::uint16_t port = 0;
};

struct WorkersConfig {
    // IOCP worker thread count. nullopt means "auto": one per logical processor,
    // resolved when the engine starts (plan II.2).
    std::optional<std::uint32_t> threads;
};

struct BackendConfig {
    std::string id;       // unique across all groups
    std::string address;  // IPv4 literal
    std::uint16_t port = 0;
    std::uint32_t weight = 0;
};

struct GroupConfig {
    std::string name;
    std::vector<BackendConfig> backends;
};

struct RoutingConfig {
    std::string default_group;
};

struct ConfigSnapshot {
    ListenConfig listen;
    WorkersConfig workers;
    std::vector<GroupConfig> groups;
    RoutingConfig routing;

    const GroupConfig* find_group(std::string_view name) const noexcept;
};

}  // namespace lb

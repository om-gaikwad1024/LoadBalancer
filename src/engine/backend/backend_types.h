#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lb {

// Per-backend state (plan IV.4). Unhealthy and draining are separate states (plan VIII):
// an unhealthy backend keeps receiving health probes; a draining one is being removed.
// Circuit-open arrives with phase 3.
enum class BackendState : std::uint8_t { Healthy, Unhealthy, Draining };

std::string_view to_string(BackendState state) noexcept;

// Copied snapshot of one backend's live state, safe to hand to the UI thread.
struct BackendStats {
    std::string id;
    std::string group;
    std::string endpoint;  // address:port
    std::uint32_t weight = 0;
    BackendState state = BackendState::Healthy;
    std::uint64_t in_flight = 0;  // what "least connections" counts (plan IV.5 decision)
    std::uint64_t requests = 0;
    std::uint64_t successes = 0;
    std::uint64_t failures = 0;
    // Connection pool (plan IV.5).
    std::uint64_t open_connections = 0;  // in use + connecting + idle
    std::uint64_t idle_connections = 0;
    std::uint64_t waiting_requests = 0;
    std::uint64_t connections_opened = 0;
    std::uint64_t connections_reused = 0;
    std::uint64_t stale_discarded = 0;
};

}  // namespace lb

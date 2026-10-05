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
    double response_time_ms = -1;  // EWMA used by least response time (plan IV.7); -1 = no sample yet
    // Connection pool (plan IV.5).
    std::uint64_t open_connections = 0;  // in use + connecting + idle
    std::uint64_t idle_connections = 0;
    std::uint64_t waiting_requests = 0;
    std::uint64_t connections_opened = 0;
    std::uint64_t connections_reused = 0;
    std::uint64_t stale_discarded = 0;
    // Active health checks (plan IV.10).
    std::uint64_t health_probes = 0;
    std::uint32_t probe_failures_in_a_row = 0;
    std::uint32_t probe_successes_in_a_row = 0;
    std::uint32_t passive_failures_in_a_row = 0;  // failed real requests in a row (plan IV.10, phase 2)
    std::string last_probe_error;  // empty after a successful probe
};

}  // namespace lb

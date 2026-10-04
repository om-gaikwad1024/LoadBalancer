#pragma once

#include <cstdint>
#include <string>

namespace lb {

// One event-log entry (plan IV.16), copied for the dashboard's live event list.
struct LoggedEvent {
    std::uint64_t sequence = 0;
    std::string wall_time;     // UTC, ISO 8601 with milliseconds: for people and for correlating with k6
    std::uint64_t mono_ms = 0;  // monotonic milliseconds since engine start: for ordering
    std::string type;          // e.g. "backend_marked_unhealthy"
    std::string backend;       // empty if no backend is involved
    std::string request_id;    // X-Request-Id, empty if no request is involved
    std::string message;       // one human-readable line
    std::string json;          // the full line as written to the log file
};

}  // namespace lb

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "log/event_types.h"

namespace lb::log {

enum class Severity : std::uint8_t { Info, Warning, Error };

// How serious an event type is, for the log view's colors and its "problems only" filter.
// Error: a client got an error or a backend was taken out. Warning: something degraded
// or was refused that an operator should notice. Everything else is informational.
Severity event_severity(std::string_view type) noexcept;
std::string_view to_string(Severity severity) noexcept;

// The dashboard's log filter (plan IV.17: searchable, filterable). Empty fields match all.
struct EventFilter {
    std::string type;     // exact event type
    std::string backend;  // exact backend id
    std::string text;     // case-insensitive substring of the message, type, backend or request id
    bool problems_only = false;  // warnings and errors
};

bool matches(const LoggedEvent& event, const EventFilter& filter) noexcept;

}  // namespace lb::log

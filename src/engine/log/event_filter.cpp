#include "log/event_filter.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace lb::log {

namespace {

bool contains_ci(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

}  // namespace

Severity event_severity(std::string_view type) noexcept {
    static constexpr std::array<std::string_view, 8> kErrors = {
        "backend_marked_unhealthy", "backend_error", "no_backend_available", "response_aborted",
        "drain_aborted",            "drain_timed_out", "pool_rejected",      "connection_rejected",
    };
    static constexpr std::array<std::string_view, 7> kWarnings = {
        "timeout",           "config_reload_rejected", "sticky_reassigned", "sticky_table_full",
        "health_checks_not_restarted", "retry",        "drain_ended",
    };
    if (std::find(kErrors.begin(), kErrors.end(), type) != kErrors.end()) return Severity::Error;
    if (std::find(kWarnings.begin(), kWarnings.end(), type) != kWarnings.end()) return Severity::Warning;
    return Severity::Info;
}

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "info";
}

bool matches(const LoggedEvent& e, const EventFilter& f) noexcept {
    if (!f.type.empty() && e.type != f.type) return false;
    if (!f.backend.empty() && e.backend != f.backend) return false;
    if (f.problems_only && event_severity(e.type) == Severity::Info) return false;
    if (f.text.empty()) return true;
    return contains_ci(e.message, f.text) || contains_ci(e.type, f.text) || contains_ci(e.backend, f.text) ||
           contains_ci(e.request_id, f.text);
}

}  // namespace lb::log

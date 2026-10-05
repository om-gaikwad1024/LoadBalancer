#include "config/reload_rules.h"

namespace lb {

std::vector<std::string> restart_only_changes(const ConfigSnapshot& a, const ConfigSnapshot& b) {
    std::vector<std::string> changed;
    const auto check = [&](bool differs, const char* field) {
        if (differs) changed.emplace_back(field);
    };
    check(a.listen.address != b.listen.address, "listen.address");
    check(a.listen.port != b.listen.port, "listen.port");
    check(a.listen.backlog != b.listen.backlog, "listen.backlog");
    check(a.listen.pending_accepts != b.listen.pending_accepts, "listen.pending_accepts");
    check(a.workers.threads != b.workers.threads, "workers.threads");
    check(a.maintenance.interval_ms != b.maintenance.interval_ms, "maintenance.interval_ms");
    check(a.metrics.slice_ms != b.metrics.slice_ms, "metrics.slice_ms");
    check(a.metrics.window_slices != b.metrics.window_slices, "metrics.window_slices");
    check(a.metrics.max_backend_series != b.metrics.max_backend_series, "metrics.max_backend_series");
    check(a.event_log.path != b.event_log.path, "event_log.path");
    check(a.event_log.max_file_bytes != b.event_log.max_file_bytes, "event_log.max_file_bytes");
    check(a.event_log.max_files != b.event_log.max_files, "event_log.max_files");
    check(a.event_log.max_queue != b.event_log.max_queue, "event_log.max_queue");
    check(a.event_log.recent_events != b.event_log.recent_events, "event_log.recent_events");
    check(a.event_log.trace_requests != b.event_log.trace_requests, "event_log.trace_requests");
    check(a.dashboard.publish_interval_ms != b.dashboard.publish_interval_ms, "dashboard.publish_interval_ms");
    check(a.dashboard.event_rows != b.dashboard.event_rows, "dashboard.event_rows");
    check(a.dashboard.graph_points != b.dashboard.graph_points, "dashboard.graph_points");
    check(a.config_reload.watch_file != b.config_reload.watch_file, "config_reload.watch_file");
    check(a.config_reload.debounce_ms != b.config_reload.debounce_ms, "config_reload.debounce_ms");
    check(a.sticky_table.shards != b.sticky_table.shards, "sticky_table.shards");
    check(a.sticky_table.max_entries != b.sticky_table.max_entries, "sticky_table.max_entries");
    return changed;
}

std::uint64_t config_content_hash(std::string_view bytes) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

}  // namespace lb

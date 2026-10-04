#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "backend/backend_types.h"
#include "config/config.h"
#include "log/event_types.h"
#include "metrics/metrics_types.h"
#include "proxy/trace.h"

namespace lb {

std::string_view engine_version() noexcept;

struct EngineStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_active = 0;
    std::uint64_t connections_rejected = 0;  // over limits.max_client_connections
    std::uint64_t requests_completed = 0;
    std::uint64_t error_responses = 0;       // proxy-generated 4xx/5xx
    std::uint64_t backend_connections_opened = 0;
    std::uint64_t backend_connections_reused = 0;  // requests sent on a pooled keep-alive connection
    std::uint64_t stale_retries = 0;    // bodiless idempotent requests resent after a dead pooled connection
    std::uint64_t pool_rejections = 0;  // 503 because a backend's pool was full and its wait queue full or timed out
    std::uint64_t no_backend_available = 0;  // 503 because no backend in the group was eligible
    std::uint64_t backends_marked_down = 0;  // active health checks (plan IV.10)
    std::uint64_t backends_marked_up = 0;
    std::uint64_t client_timeouts = 0;   // plan VI: header, body, keep-alive idle, write
    std::uint64_t backend_timeouts = 0;  // plan VI: connect, response headers, idle
    std::uint64_t stale_retry_successes = 0;  // stale retries that got a response
    std::uint64_t events_dropped = 0;    // event-log entries lost because the writer fell behind
    std::uint64_t reloads_accepted = 0;  // plan IV.14: configs swapped in while running
    std::uint64_t reloads_rejected = 0;  // invalid or restart-only changes; the old config stays
};

// Outcome of a hot reload (plan IV.14). A rejected reload changes nothing.
struct ReloadResult {
    bool accepted = false;
    bool unchanged = false;           // the file's content equals the active config: skipped
    std::vector<std::string> errors;  // why it was rejected
    std::string summary;              // what an accepted reload changed
};

// Everything the dashboard shows, copied on an engine thread (plan IV.17, V). The UI
// owns it once delivered and never reads engine state directly.
struct DashboardSnapshot {
    std::uint64_t sequence = 0;
    std::string listen_address;
    std::uint16_t listen_port = 0;
    std::uint32_t workers = 0;
    double uptime_seconds = 0;
    EngineStats stats;
    std::vector<BackendStats> backends;
    MetricsSnapshot metrics;
    std::vector<LoggedEvent> new_events;  // event-log entries added since the previous snapshot
};

// Receives snapshots on the engine's publisher thread. Implementations must only hand the
// snapshot over (e.g. PostMessage to a window) and return at once.
class SnapshotSink {
public:
    virtual void on_snapshot(std::unique_ptr<DashboardSnapshot> snapshot) noexcept = 0;

protected:
    ~SnapshotSink() = default;
};

class EngineImpl;

// The proxy engine: IOCP worker pool, listener and request pipeline (plan II.2, IV.1,
// IV.18). This header is safe to include from MFC code: no Winsock or MFC types.
class Engine {
public:
    explicit Engine(std::shared_ptr<const ConfigSnapshot> config);
    ~Engine();  // stop()

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Binds the listener and starts the workers. On failure returns false and fills *error.
    bool start(std::string* error);

    // Graceful shutdown: stop accepting, close idle connections, let in-flight requests
    // finish for up to timeouts.shutdown_grace_ms, force-close the rest, then join every
    // engine thread. Afterwards the engine holds no sockets, threads or handles.
    void stop();

    std::uint16_t listen_port() const noexcept;
    std::uint32_t worker_threads() const noexcept;
    EngineStats stats() const noexcept;

    // Copied per-backend state for the dashboard (plan IV.4, IV.5).
    std::vector<BackendStats> backend_stats() const;

    // Latency percentiles and rates, merged from every worker's histograms (plan IV.15).
    MetricsSnapshot metrics() const;

    // The newest event-log entries (plan IV.16), oldest first.
    std::vector<LoggedEvent> recent_events() const;

    // Operator/health-check entry point: unhealthy or draining backends stop receiving new
    // requests and their idle pooled connections are closed at once. False if the id is unknown.
    bool set_backend_state(std::string_view backend_id, BackendState state);

    // Hot reload (plan IV.14): validates everything first, then swaps the config and the
    // backend set atomically. In-flight requests finish on the snapshot they started with;
    // new requests see the complete new one. A change to a restart-only field (listener,
    // worker count, metrics window, event log, dashboard, config_reload) rejects the whole
    // reload. Every outcome is logged (config_reload_accepted / config_reload_rejected).
    // `source` names the trigger in the log ("file", "api", later "gui").
    ReloadResult reload(std::shared_ptr<const ConfigSnapshot> next, std::string_view source = "api");
    // Parses and validates `json_text`, then reloads. Skipped as unchanged if its content
    // hash equals the active config's (so a GUI save is not reloaded again by the watcher).
    ReloadResult reload_from_text(std::string_view json_text, std::string_view source);
    ReloadResult reload_from_file(const std::filesystem::path& path);

    // After start(): when config_reload.watch_file is true, reloads `path` whenever it
    // changes, debounced by config_reload.debounce_ms. The file's current content is taken
    // as the active config's. Returns false (and fills *error) if the file cannot be watched.
    bool watch_config_file(const std::filesystem::path& path, std::string* error);

    // Optional per-step trace (plan IV.18). Set before start(); the sink must outlive the engine run.
    void set_trace_sink(TraceSink* sink) noexcept;

    // Dashboard feed (plan IV.17): with a sink set before start(), a snapshot is published
    // every dashboard.publish_interval_ms until stop() begins. The sink must outlive stop().
    void set_snapshot_sink(SnapshotSink* sink) noexcept;

private:
    std::unique_ptr<EngineImpl> impl_;
};

}  // namespace lb

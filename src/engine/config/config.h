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
    std::uint32_t backlog = 0;          // listen() backlog
    std::uint32_t pending_accepts = 0;  // AcceptEx calls kept outstanding
};

struct BuffersConfig {
    std::uint32_t client_read_bytes = 0;   // per-connection receive buffer, client side
    std::uint32_t backend_read_bytes = 0;  // per-connection receive buffer, backend side
};

// Backend connection pool, per backend (plan IV.5).
struct PoolConfig {
    std::uint32_t max_connections_per_backend = 0;  // open + connecting + in use
    std::uint32_t max_idle_per_backend = 0;         // kept for reuse; 0 disables pooling
    std::uint32_t idle_timeout_ms = 0;              // idle longer than this: closed by the maintenance thread
    std::uint32_t max_waiters_per_backend = 0;      // queue length when the cap is reached; full queue: 503
    std::uint32_t wait_timeout_ms = 0;              // queued longer than this: 503
    // true: a refused backend connect fails at once (502) instead of after Windows' ~2 s of
    // SYN retries; a SYN lost on the network then also fails at once.
    bool fail_fast_connect = true;
};

// Latency histograms (plan IV.15): "live" percentiles cover a rolling window of
// window_slices x slice_ms; "since start" percentiles cover everything.
struct MetricsConfig {
    std::uint32_t slice_ms = 0;
    std::uint32_t window_slices = 0;
    // Distinct backend ids tracked since start (removed ones keep their series), so
    // backends added by hot reload get a series without reallocating live histograms.
    std::uint32_t max_backend_series = 0;
};

// Least response time (plan IV.7): each backend keeps an exponentially weighted moving
// average of its response time. Shared by every group using that strategy.
struct BalancingConfig {
    std::uint32_t response_time_decay_ms = 0;   // EWMA time constant: a sample this old weighs 1/e
    std::uint32_t response_time_expiry_ms = 0;  // an average older than this is dropped and re-measured
};

// Hot reload (plan IV.14). Both fields need a restart to change.
struct ConfigReloadConfig {
    bool watch_file = false;        // watch the config file and apply valid changes
    std::uint32_t debounce_ms = 0;  // quiet time after the last write before reading the file
};

// Event log (plan IV.16): JSON lines written by a background thread, size-based rotation.
struct EventLogConfig {
    std::string path;                 // empty: in-memory only (recent events), no file
    std::uint32_t max_file_bytes = 0;  // rotate when the file would grow past this
    std::uint32_t max_files = 0;       // rotated files kept: path.1 ... path.N
    std::uint32_t max_queue = 0;       // events waiting for the writer; beyond it events are dropped and counted
    std::uint32_t recent_events = 0;   // kept in memory for the dashboard
    bool trace_requests = false;       // debug: log every pipeline step of every request (plan IV.18)
};

// Operator dashboard (plan IV.17).
struct DashboardConfig {
    std::uint32_t publish_interval_ms = 0;  // engine publishes a copied snapshot this often; the UI repaints at the same rate
    std::uint32_t event_rows = 0;           // rows kept in the live event list
    std::uint32_t graph_points = 0;         // metric slices each graph shows (its time span = graph_points x slice_ms)
};

// Maintenance thread (plan V): idle-connection sweeps, later stale sticky/rate-limit entries.
struct MaintenanceConfig {
    std::uint32_t interval_ms = 0;
};

// Plan VI timeouts table, plus the two waits it implies ("every wait has a limit"):
// a client that stops reading and a backend that stalls mid-transfer. All monotonic.
struct TimeoutsConfig {
    std::uint32_t client_header_ms = 0;          // whole request head (slowloris): close
    std::uint32_t client_body_idle_ms = 0;       // gap between request body reads: 408, close
    std::uint32_t client_keepalive_idle_ms = 0;  // idle between requests: close
    std::uint32_t client_write_idle_ms = 0;      // client not reading the response: close
    std::uint32_t backend_connect_ms = 0;        // opening a backend connection: failure, 502
    std::uint32_t backend_response_ms = 0;       // request sent, waiting for response headers: failure, 504
    std::uint32_t backend_idle_ms = 0;           // backend stalls while sending or receiving a body
    // On shutdown, in-flight requests get this long to finish before connections are forced closed.
    std::uint32_t shutdown_grace_ms = 0;
    // A draining backend's in-flight requests get this long; then they are aborted (502) and
    // the backend is taken out (plan IV.12, VI).
    std::uint32_t drain_ms = 0;
};

// IPv4 network in host byte order, e.g. 10.0.0.0/8.
struct Ipv4Cidr {
    std::uint32_t network = 0;
    std::uint32_t mask = 0;

    bool contains(std::uint32_t address) const noexcept { return (address & mask) == network; }
};

// Load-balancing strategy, selectable per group (plan IV.7).
enum class Strategy : std::uint8_t {
    RoundRobin,
    LeastConnections,    // fewest in-flight requests (plan IV.5 decision)
    WeightedRoundRobin,  // shares proportional to backend weights, interleaved
    LeastResponseTime,   // lowest EWMA of backend response time, scaled by in-flight requests
    IpHash,              // client address picks the backend (weighted rendezvous hashing)
};

// Active health checks for one group's backends (plan IV.10).
struct HealthConfig {
    enum class Type : std::uint8_t { Http, Tcp };
    Type type = Type::Http;
    std::string path;  // HTTP probes: GET path; status 200-399 = healthy
    std::uint32_t interval_ms = 0;          // time between probe starts, per backend
    std::uint32_t timeout_ms = 0;           // per probe, <= interval_ms
    std::uint32_t unhealthy_threshold = 0;  // N consecutive failures: marked down
    std::uint32_t healthy_threshold = 0;    // M consecutive successes: marked up again

    bool operator==(const HealthConfig&) const = default;
};

enum class HostHeaderMode : std::uint8_t {
    Preserve,  // forward the client's Host (default in plan IV.6)
    Backend,   // rewrite Host to the backend's address:port
};

struct WorkersConfig {
    // IOCP worker thread count. nullopt means "auto": one per logical processor,
    // resolved when the engine starts (plan II.2).
    std::optional<std::uint32_t> threads;
};

// HTTP parser input limits (plan IV.3, VII). Exceeding a request limit answers 400
// (request line) or 431 (header section); a response over its limits is a 502.
struct LimitsConfig {
    std::uint32_t max_request_line_bytes = 0;
    std::uint32_t max_request_header_bytes = 0;  // header fields, excluding the request line
    std::uint32_t max_request_header_count = 0;
    std::uint32_t max_response_header_bytes = 0;  // whole response head, status line included
    std::uint32_t max_response_header_count = 0;
    std::uint32_t max_chunk_line_bytes = 0;  // one chunk-size line including extensions
    std::uint32_t max_client_connections = 0;  // global cap; connections over it get 503 (plan IV.1)
};

// What a config (load or reload) says about a backend's drain state (plan IV.12). A reload
// never un-drains a backend unless its config explicitly says "cancel".
enum class DrainDirective : std::uint8_t {
    Keep,    // leave the running drain state alone (a new backend starts in service)
    Start,   // drain it (nothing changes if it is already draining or drained)
    Cancel,  // return a draining or drained backend to service
};

struct BackendConfig {
    std::string id;       // unique across all groups
    std::string address;  // IPv4 literal
    std::uint16_t port = 0;
    std::uint32_t weight = 0;
    DrainDirective drain = DrainDirective::Keep;
};

// Passive health checks for one group (plan IV.10, phase 2): real request outcomes count
// toward health. Consecutive failed requests mark a backend unhealthy; only active probes
// bring it back (it gets no traffic while unhealthy).
struct PassiveHealthConfig {
    bool enabled = false;
    std::uint32_t consecutive_failures = 0;  // failed requests in a row, with no success between them
    bool count_5xx = false;                  // a 5xx response from the backend counts as a failure
};

// Session affinity for one group (plan IV.9): the session key is a cookie, mapped to a
// backend id in the sticky table for ttl_ms after its last use.
struct StickyConfig {
    enum class Mode : std::uint8_t {
        Off,
        ApplicationCookie,  // the application's own session cookie (learned from its Set-Cookie)
        InsertedCookie,     // a cookie the proxy sets itself
    };
    Mode mode = Mode::Off;
    std::string cookie;        // cookie name; empty when off
    std::uint32_t ttl_ms = 0;  // a mapping unused this long expires
};

struct GroupConfig {
    std::string name;
    std::vector<BackendConfig> backends;
    Strategy strategy = Strategy::RoundRobin;
    HostHeaderMode host_header = HostHeaderMode::Preserve;
    HealthConfig health;
    PassiveHealthConfig passive_health;
    StickyConfig sticky;
};

// The sticky-session table (plan IV.9, V): sharded locks, bounded size.
struct StickyTableConfig {
    std::uint32_t shards = 0;       // independent locks
    std::uint32_t max_entries = 0;  // across all shards; when full, new sessions are not stored
};

// One content-routing rule (plan IV.8). Rules are tried in order; the first match picks
// the group, and a request no rule matches goes to default_group.
struct RouteRule {
    enum class Type : std::uint8_t {
        PathPrefix,  // the path is `value` or continues it with a new segment
        PathGlob,    // the whole path matches `value`; '*' matches any run of characters
        Header,      // header `field` is present (value null) or has exactly `value`
        Cookie,      // cookie `field` is present (value null) or has exactly `value`
    };
    std::string id;
    Type type = Type::PathPrefix;
    std::string field;                 // header or cookie name; empty for path rules
    std::optional<std::string> value;  // empty optional: presence only (header and cookie rules)
    std::string group;
};

struct RoutingConfig {
    std::string default_group;
    std::vector<RouteRule> rules;  // priority order: first match wins
};

struct ConfigSnapshot {
    ListenConfig listen;
    WorkersConfig workers;
    LimitsConfig limits;
    BuffersConfig buffers;
    PoolConfig pool;
    MaintenanceConfig maintenance;
    MetricsConfig metrics;
    EventLogConfig event_log;
    DashboardConfig dashboard;
    ConfigReloadConfig config_reload;
    BalancingConfig balancing;
    StickyTableConfig sticky_table;
    TimeoutsConfig timeouts;
    std::vector<GroupConfig> groups;
    RoutingConfig routing;
    // Upstream proxies whose X-Forwarded-* headers are believed (plan IV.6, VII). Empty: none.
    std::vector<Ipv4Cidr> trusted_proxies;

    const GroupConfig* find_group(std::string_view name) const noexcept;
    bool is_trusted_proxy(std::uint32_t address_host_order) const noexcept;
};

}  // namespace lb

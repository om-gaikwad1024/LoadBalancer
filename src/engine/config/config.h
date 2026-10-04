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
};

// IPv4 network in host byte order, e.g. 10.0.0.0/8.
struct Ipv4Cidr {
    std::uint32_t network = 0;
    std::uint32_t mask = 0;

    bool contains(std::uint32_t address) const noexcept { return (address & mask) == network; }
};

// Load-balancing strategy, selectable per group (plan IV.7). Phase 2 adds weighted round
// robin, least response time and IP hash.
enum class Strategy : std::uint8_t {
    RoundRobin,
    LeastConnections,  // fewest in-flight requests (plan IV.5 decision)
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

struct BackendConfig {
    std::string id;       // unique across all groups
    std::string address;  // IPv4 literal
    std::uint16_t port = 0;
    std::uint32_t weight = 0;
};

struct GroupConfig {
    std::string name;
    std::vector<BackendConfig> backends;
    Strategy strategy = Strategy::RoundRobin;
    HostHeaderMode host_header = HostHeaderMode::Preserve;
    HealthConfig health;
};

struct RoutingConfig {
    std::string default_group;
};

struct ConfigSnapshot {
    ListenConfig listen;
    WorkersConfig workers;
    LimitsConfig limits;
    BuffersConfig buffers;
    PoolConfig pool;
    MaintenanceConfig maintenance;
    TimeoutsConfig timeouts;
    std::vector<GroupConfig> groups;
    RoutingConfig routing;
    // Upstream proxies whose X-Forwarded-* headers are believed (plan IV.6, VII). Empty: none.
    std::vector<Ipv4Cidr> trusted_proxies;

    const GroupConfig* find_group(std::string_view name) const noexcept;
    bool is_trusted_proxy(std::uint32_t address_host_order) const noexcept;
};

}  // namespace lb

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

struct TimeoutsConfig {
    // On shutdown, in-flight requests get this long to finish before connections are forced closed.
    std::uint32_t shutdown_grace_ms = 0;
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
};

struct RoutingConfig {
    std::string default_group;
};

struct ConfigSnapshot {
    ListenConfig listen;
    WorkersConfig workers;
    LimitsConfig limits;
    BuffersConfig buffers;
    TimeoutsConfig timeouts;
    std::vector<GroupConfig> groups;
    RoutingConfig routing;

    const GroupConfig* find_group(std::string_view name) const noexcept;
};

}  // namespace lb

#pragma once

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config/config_loader.h"

namespace lbtest {

// A complete, valid proxy config for tests as JSON: listener on an ephemeral port, the
// given backends in group "web". `tweak` edits it.
inline nlohmann::json make_proxy_config_json(const std::vector<std::uint16_t>& backend_ports,
                                             const std::function<void(nlohmann::json&)>& tweak = {}) {
    nlohmann::json backends = nlohmann::json::array();
    for (std::size_t i = 0; i < backend_ports.size(); ++i) {
        backends.push_back({{"id", "b" + std::to_string(i + 1)},
                            {"address", "127.0.0.1"},
                            {"port", backend_ports[i]},
                            {"weight", 1}});
    }
    nlohmann::json j = {
        {"listen", {{"address", "127.0.0.1"}, {"port", 0}, {"backlog", 256}, {"pending_accepts", 8}}},
        {"workers", {{"threads", 2}}},
        {"limits",
         {{"max_request_line_bytes", 8192},
          {"max_request_header_bytes", 32768},
          {"max_request_header_count", 100},
          {"max_response_header_bytes", 65536},
          {"max_response_header_count", 200},
          {"max_chunk_line_bytes", 4096},
          {"max_client_connections", 1000}}},
        {"buffers", {{"client_read_bytes", 16384}, {"backend_read_bytes", 16384}}},
        {"pool",
         {{"max_connections_per_backend", 64},
          {"max_idle_per_backend", 16},
          {"idle_timeout_ms", 30000},
          {"max_waiters_per_backend", 256},
          {"wait_timeout_ms", 2000},
          {"fail_fast_connect", true}}},
        {"maintenance", {{"interval_ms", 50}}},
        {"metrics", {{"slice_ms", 1000}, {"window_slices", 10}, {"max_backend_series", 64}}},
        {"config_reload", {{"watch_file", false}, {"debounce_ms", 200}}},
        {"balancing", {{"response_time_decay_ms", 2000}, {"response_time_expiry_ms", 10000}}},
        {"dashboard", {{"publish_interval_ms", 500}, {"event_rows", 500}}},
        {"event_log",
         {{"path", ""},  // in memory only unless a test asks for a file
          {"max_file_bytes", 1048576},
          {"max_files", 3},
          {"max_queue", 100000},
          {"recent_events", 1000},
          {"trace_requests", false}}},
        {"timeouts",
         {{"client_header_ms", 10000},
          {"client_body_idle_ms", 10000},
          {"client_keepalive_idle_ms", 30000},
          {"client_write_idle_ms", 10000},
          {"backend_connect_ms", 3000},
          {"backend_response_ms", 10000},
          {"backend_idle_ms", 10000},
          {"shutdown_grace_ms", 5000}}},
        {"trusted_proxies", nlohmann::json::array()},
        {"groups",
         {{{"name", "web"},
           {"strategy", "round_robin"},
           {"host_header", "preserve"},
           // First probes are staggered over the first interval, so with 60 s no probe
           // touches tests that are not about health checks; those set short intervals.
           {"health",
            {{"type", "http"},
             {"path", "/health"},
             {"interval_ms", 60000},
             {"timeout_ms", 1000},
             {"unhealthy_threshold", 3},
             {"healthy_threshold", 2}}},
           {"backends", backends}}}},
        {"routing", {{"default_group", "web"}, {"rules", nlohmann::json::array()}}},
    };
    if (tweak) tweak(j);
    return j;
}

// The same config through the real loader.
inline std::shared_ptr<const lb::ConfigSnapshot> make_proxy_config(
    const std::vector<std::uint16_t>& backend_ports, const std::function<void(nlohmann::json&)>& tweak = {}) {
    auto loaded = lb::parse_config(make_proxy_config_json(backend_ports, tweak).dump());
    for (const auto& e : loaded.errors) ADD_FAILURE() << "test config rejected: " << lb::to_string(e);
    return loaded.snapshot;
}

}  // namespace lbtest

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

// A complete, valid proxy config for tests: listener on an ephemeral port, the given
// backends in group "web". `tweak` edits the JSON before it goes through the real loader.
inline std::shared_ptr<const lb::ConfigSnapshot> make_proxy_config(
    const std::vector<std::uint16_t>& backend_ports, const std::function<void(nlohmann::json&)>& tweak = {}) {
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
          {"wait_timeout_ms", 2000}}},
        {"maintenance", {{"interval_ms", 50}}},
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
        {"groups", {{{"name", "web"}, {"host_header", "preserve"}, {"backends", backends}}}},
        {"routing", {{"default_group", "web"}}},
    };
    if (tweak) tweak(j);
    auto loaded = lb::parse_config(j.dump());
    for (const auto& e : loaded.errors) ADD_FAILURE() << "test config rejected: " << lb::to_string(e);
    return loaded.snapshot;
}

}  // namespace lbtest
